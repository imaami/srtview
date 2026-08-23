#!/usr/bin/env bash
set -euo pipefail

readonly api="${GITHUB_API_URL:-https://api.github.com}"
readonly openai="${OPENAI_BASE_URL:-https://api.openai.com}"
readonly repo="${GITHUB_REPOSITORY:?GITHUB_REPOSITORY is unset}"
readonly repo_owner="${GITHUB_REPOSITORY_OWNER:-${repo%%/*}}"
readonly event="${GITHUB_EVENT_PATH:?GITHUB_EVENT_PATH is unset}"
readonly prompt_file=".github/gpt-review/prompt.md"
readonly max_diff_bytes="${GPT_REVIEW_MAX_DIFF_BYTES:-600000}"
readonly max_bot_bytes="${GPT_REVIEW_MAX_BOT_BYTES:-120000}"
# The Responses API counts reasoning tokens against this cap along
# with the visible ones, and a reasoning model at xhigh spends tens
# of thousands before it writes a word: a cap sized for the review
# text alone ends the call before the review, as "incomplete" with
# no message at all.
readonly max_output_tokens="${GPT_REVIEW_MAX_OUTPUT_TOKENS:-64000}"

umask 077
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

need()
{
	command -v "$1" >/dev/null || {
		printf 'gpt-review: missing command: %s\n' "$1" >&2
		exit 2
	}
}

need curl
need jq
need awk

[[ -r $prompt_file ]] || {
	printf 'gpt-review: cannot read %s\n' "$prompt_file" >&2
	exit 2
}

# A refused call says why in its body; that goes to the log, where
# the status alone said nothing about a token short of a permission.
gh_curl()
{
	local status
	curl --silent --show-error --fail-with-body \
		-H "Authorization: Bearer ${GH_TOKEN:?GH_TOKEN is unset}" \
		-H 'Accept: application/vnd.github+json' \
		-H 'X-GitHub-Api-Version: 2022-11-28' \
		"$@" > "$tmp/gh.out" || {
		status=$?
		printf 'gpt-review: GitHub API refused %s:\n' "${*: -1}" >&2
		head -c 2000 "$tmp/gh.out" >&2
		printf '\n' >&2
		return "$status"
	}
	cat "$tmp/gh.out"
}

# Every comment the job writes opens with a marker naming the PR and
# the head it looked at, so tooling can find the job's own comments
# among a PR's -- the failures too, which is how a reader learns why
# nothing came.  GitHub refuses a comment past 65536 characters; a
# review that long is cut and still posted.
post_comment()
{
	local body=$1

	if (( ${#body} > 65000 )); then
		body="${body:0:65000}"$'\n\n[... truncated by gpt-review ...]'
	fi
	body="<!-- gpt-review: pr=$pr sha=${head_sha:-} -->"$'\n'"$body"
	printf '%s' "$body" | jq -Rs '{body: .}' > "$tmp/comment.json"
	gh_curl \
		-X POST \
		-H 'Content-Type: application/json' \
		--data-binary "@$tmp/comment.json" \
		"$api/repos/$repo/issues/$pr/comments" >/dev/null
}

fail()
{
	post_comment "**GPT review failed:** $1"
	exit 1
}

# The eyes on the command are the first write the job makes, with
# the permission the review comment will need: a token short of it
# fails here, before the paid call, with the refusal in the log --
# nothing can reach the PR then, which is the point of knowing.
react_eyes()
{
	printf '%s\n' '{"content":"eyes"}' > "$tmp/reaction.json"
	gh_curl \
		-X POST \
		-H 'Content-Type: application/json' \
		--data-binary "@$tmp/reaction.json" \
		"$api/repos/$repo/issues/comments/$comment_id/reactions" \
		>/dev/null || {
		printf 'gpt-review: the token cannot write to the pull request; no review was asked\n' >&2
		exit 1
	}
}

# Cuts at a line boundary and then drains the rest: exiting early
# would close the pipe under the producer, and with pipefail a curl
# or jq killed by EPIPE fails the pipeline -- the whole review died
# on exactly the PRs large enough to need the cut.
truncate_lines()
{
	local limit=$1

	LC_ALL=C awk -v limit="$limit" '
		cut { next }
		{
			n += length($0) + 1
			if (n > limit) {
				print ""
				print "[... truncated by gpt-review ...]"
				cut = 1
				next
			}
			print
		}
	'
}

pr=$(jq -r '.issue.number // empty' "$event")
comment_id=$(jq -r '.comment.id // empty' "$event")
commenter=$(jq -r '.comment.user.login // empty' "$event")
comment_body=$(jq -r '.comment.body // ""' "$event")

[[ $pr =~ ^[0-9]+$ && $comment_id =~ ^[0-9]+$ ]] || {
	printf 'gpt-review: event is not a PR issue_comment\n' >&2
	exit 2
}

if [[ $commenter != "$repo_owner" || ${GITHUB_TRIGGERING_ACTOR:-} != "$repo_owner" ]]; then
	printf 'gpt-review: refusing command or rerun from %q (repository owner is %q)\n' \
		"${GITHUB_TRIGGERING_ACTOR:-$commenter}" "$repo_owner" >&2
	exit 0
fi

# The command stands at the start of a line, any line: one comment
# prods every bot, each on a line of its own.  The focus is what the
# comment says besides -- the rest of the command's line and every
# line that is not a command to some bot, which is to say not one
# that opens with / or @ -- so a note written for the reviewer
# reaches it and the other bots' orders do not.
command_line=$(printf '%s\n' "$comment_body" | tr -d '\r' |
	grep -m1 -E '^/gpt[[:space:]]+review([[:space:]]|$)' || :)

if [[ -z $command_line ]]; then
	printf 'gpt-review: comment carries no /gpt review command line\n' >&2
	exit 0
fi

focus=$(printf '%s\n' "$comment_body" | tr -d '\r' |
	sed -E 's@^/gpt[[:space:]]+review[[:space:]]*@@; /^[/@]/d' |
	awk 'NF { printf "%s", gap; gap = ""; seen = 1; print; next }
	     seen { gap = gap "\n" }')

react_eyes

[[ -n ${OPENAI_API_KEY:-} ]] ||
	fail 'repository secret `OPENAI_API_KEY` is not configured.'

gh_curl "$api/repos/$repo/pulls/$pr" > "$tmp/pr.json" ||
	fail "could not read the pull request through the GitHub API."

title=$(jq -r '.title' "$tmp/pr.json")
pr_body=$(jq -r '.body // ""' "$tmp/pr.json")
author=$(jq -r '.user.login' "$tmp/pr.json")
base_ref=$(jq -r '.base.ref' "$tmp/pr.json")
head_ref=$(jq -r '.head.ref' "$tmp/pr.json")
head_sha=$(jq -r '.head.sha' "$tmp/pr.json")
changed=$(jq -r '.changed_files' "$tmp/pr.json")
additions=$(jq -r '.additions' "$tmp/pr.json")
deletions=$(jq -r '.deletions' "$tmp/pr.json")

# Fetch the diff as data only. We deliberately do not check out or execute
# the PR head because this job has access to OPENAI_API_KEY.  GitHub
# serves no diff past 300 files or 20000 lines (406); that is a
# review that cannot happen, and the PR is told so.
curl --silent --show-error --fail-with-body \
	-H "Authorization: Bearer ${GH_TOKEN:?}" \
	-H 'Accept: application/vnd.github.v3.diff' \
	-H 'X-GitHub-Api-Version: 2022-11-28' \
	"$api/repos/$repo/pulls/$pr" |
	truncate_lines "$max_diff_bytes" > "$tmp/diff.txt" ||
	fail "could not fetch the diff; GitHub serves none for a PR past 300 files or 20000 diff lines."

# Existing AI-review feedback is useful mainly as a deduplication/adversarial
# signal. Restrict it to the reviewers on this repository -- CodeRabbit,
# Copilot, Codex -- and cap both per-comment and total size so a noisy
# review cannot consume the model context.  The
# feedback is context, not the subject: a page that will not come
# leaves the review to go without it.
for f in "issues.json:issues/$pr/comments" \
         "review-comments.json:pulls/$pr/comments" \
         "reviews.json:pulls/$pr/reviews"; do
	gh_curl "$api/repos/$repo/${f#*:}?per_page=100" > "$tmp/${f%%:*}" ||
		printf '[]' > "$tmp/${f%%:*}"
done

{
	jq -r '
		.[]
		| select((.user.login // "") | test("coderabbit|copilot|codex"; "i"))
		| "TOP-LEVEL [\(.user.login)]:\n\((.body // "")[0:5000])\n"
	' "$tmp/issues.json"

	jq -r '
		.[]
		| select((.user.login // "") | test("coderabbit|copilot|codex"; "i"))
		| "INLINE [\(.user.login)] \(.path // "?"):\(.line // .original_line // "?"):\n\((.body // "")[0:5000])\n"
	' "$tmp/review-comments.json"

	jq -r '
		.[]
		| select((.user.login // "") | test("coderabbit|copilot|codex"; "i"))
		| select((.body // "") != "")
		| "REVIEW [\(.user.login)] state=\(.state // "?"):\n\((.body // "")[0:5000])\n"
	' "$tmp/reviews.json"
} | truncate_lines "$max_bot_bytes" > "$tmp/bots.txt"

# The review policy comes from the trusted default branch. CLAUDE.md carries
# architecture and house rules; .coderabbit.yaml carries the concise
# reviewer-specific exceptions; AGENTS.md carries repository mechanics.
for f in CLAUDE.md .coderabbit.yaml AGENTS.md; do
	[[ -r $f ]] || continue
	{
		printf '\n===== %s =====\n' "$f"
		cat "$f"
	} >> "$tmp/policy.txt"
done
: > "${tmp}/empty"
[[ -e $tmp/policy.txt ]] || cp "$tmp/empty" "$tmp/policy.txt"

metadata=$(jq -n \
	--arg repo "$repo" \
	--argjson pr "$pr" \
	--arg title "$title" \
	--arg author "$author" \
	--arg base "$base_ref" \
	--arg head "$head_ref" \
	--arg sha "$head_sha" \
	--argjson changed "$changed" \
	--argjson additions "$additions" \
	--argjson deletions "$deletions" \
	'{repo:$repo, pr:$pr, title:$title, author:$author,
	  base:$base, head:$head, head_sha:$sha, changed_files:$changed,
	  additions:$additions, deletions:$deletions}')

jq -n \
	--arg model "${OPENAI_MODEL:-gpt-5.6-sol}" \
	--arg effort "${OPENAI_REASONING_EFFORT:-xhigh}" \
	--argjson max_out "$max_output_tokens" \
	--rawfile instructions "$prompt_file" \
	--arg metadata "$metadata" \
	--arg pr_body "$pr_body" \
	--arg focus "$focus" \
	--rawfile policy "$tmp/policy.txt" \
	--rawfile bots "$tmp/bots.txt" \
	--rawfile diff "$tmp/diff.txt" \
	'{
		model: $model,
		store: false,
		reasoning: {effort: $effort},
		max_output_tokens: $max_out,
		input: [
			{
				role: "developer",
				content: [{type:"input_text", text:$instructions}]
			},
			{
				role: "user",
				content: [{
					type:"input_text",
					text: (
						"<PR_METADATA>\n" + $metadata + "\n</PR_METADATA>\n\n" +
						"<PR_DESCRIPTION>\n" + $pr_body + "\n</PR_DESCRIPTION>\n\n" +
						"<OPTIONAL_REVIEW_FOCUS>\n" + $focus + "\n</OPTIONAL_REVIEW_FOCUS>\n\n" +
						"<REPOSITORY_POLICY>\n" + $policy + "\n</REPOSITORY_POLICY>\n\n" +
						"<EXISTING_AI_REVIEWS>\n" + $bots + "\n</EXISTING_AI_REVIEWS>\n\n" +
						"<PR_DIFF>\n" + $diff + "\n</PR_DIFF>"
					)
				}]
			}
		]
	}' > "$tmp/request.json"

# A transport failure leaves no HTTP code; curl's own exit is the
# report then, and it reaches the PR like any other.
http=$(curl --silent --show-error \
	-o "$tmp/response.json" \
	-w '%{http_code}' \
	-X POST \
	-H "Authorization: Bearer $OPENAI_API_KEY" \
	-H 'Content-Type: application/json' \
	--data-binary "@$tmp/request.json" \
	"$openai/v1/responses") || http="curl exit $?"

if [[ ! $http =~ ^[0-9]+$ ]]; then
	fail "OpenAI API unreachable ($http)."
elif [[ ! $http =~ ^2 ]]; then
	error=$(jq -r '.error.message // "unknown OpenAI API error"' \
		"$tmp/response.json" 2>/dev/null || printf 'unparseable OpenAI API error')
	fail "$(printf 'OpenAI API HTTP %s: %s' "$http" "$error")"
fi

review=$(jq -r '
	[
		.output[]?
		| select(.type == "message")
		| .content[]?
		| select(.type == "output_text")
		| .text
	] | join("\n")
' "$tmp/response.json")

# A response the cap or the model cut short says so in its status;
# one with text is still posted, marked, and one without is a
# failure with its reason, not "no text output".
status=$(jq -r '.status // "completed"' "$tmp/response.json")
reason=$(jq -r '.incomplete_details.reason // .status // "unknown"' \
	"$tmp/response.json")

if [[ -z $review || $review == null ]]; then
	[[ $status == completed ]] ||
		fail "$(printf 'OpenAI response %s (%s) before any text was written.' \
			"$status" "$reason")"
	fail 'OpenAI returned no text output.'
fi

if [[ $status != completed ]]; then
	review+=$(printf '\n\n*Review cut short: response %s (%s).*' \
		"$status" "$reason")
fi

model=$(jq -r '.model // empty' "$tmp/response.json")
usage=$(jq -r '
	if .usage then
		"input " + ((.usage.input_tokens // 0) | tostring) +
		", output " + ((.usage.output_tokens // 0) | tostring) +
		" tokens"
	else
		"usage unavailable"
	end
' "$tmp/response.json")

post_comment "$(cat <<EOF
## GPT review

$model · $usage

$review
EOF
)"
