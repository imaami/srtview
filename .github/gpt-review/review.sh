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
# The reviewer reads the repository itself, through one read-only
# git tool; each tool reply and the number of tool rounds are
# bounded so a curious model cannot flood its own context.
readonly max_tool_bytes="${GPT_REVIEW_MAX_TOOL_BYTES:-50000}"
readonly max_tool_calls="${GPT_REVIEW_MAX_TOOL_CALLS:-40}"

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
# prods every bot, each on a line of its own.  The focus is what
# the comment says besides -- the rest of the first command line,
# kept whole even when it opens with / or @, and every other line
# that is not a command to some bot, which is to say not one that
# opens with / or @.  A note written for the reviewer reaches it,
# the other bots' orders do not, and each ORIGINAL line is judged
# exactly once: a filter run on stripped text mistook a focus
# opening with / for a bot command, and a substitution run on
# every line turned a second command line, or a /gpt reviewer,
# into focus.
printf '%s\n' "$comment_body" | tr -d '\r' |
	grep -q -E '^/gpt[[:space:]]+review([[:space:]]|$)' || {
	printf 'gpt-review: comment carries no /gpt review command line\n' >&2
	exit 0
}

focus=$(printf '%s\n' "$comment_body" | tr -d '\r' | LC_ALL=C awk '
	{
		if (!found && match($0, /^\/gpt[ \t]+review([ \t]+|$)/)) {
			found = 1
			$0 = substr($0, RLENGTH + 1)
		} else if ($0 ~ /^[\/@]/)
			next
		if ($0 == "") {
			if (out)
				gap = gap "\n"
			next
		}
		printf "%s%s\n", gap, $0
		gap = ""
		out = 1
	}
')

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

# The PR head and base as refs in the trusted clone: git OBJECTS
# only, data the reviewer's read-only tool can browse.  Nothing of
# the PR is ever checked out, built, sourced or executed -- this
# job holds OPENAI_API_KEY -- and the working tree stays the
# default branch's.
auth=$(printf 'x-access-token:%s' "${GH_TOKEN:?}" | base64 -w0)
git -c "http.${GITHUB_SERVER_URL:-https://github.com}/.extraheader=AUTHORIZATION: basic $auth" \
	fetch --quiet --no-tags "${GITHUB_SERVER_URL:-https://github.com}/$repo" \
	"pull/$pr/head:refs/gpt-review/head" \
	"+refs/heads/$base_ref:refs/gpt-review/base" ||
	fail "could not fetch the PR head and base as git refs."

# Fetch the diff as data only.  GitHub
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

# The reviewer's one tool: git, read-only, over the trusted clone
# and the fetched refs.  The subcommand whitelist keeps it off the
# network and the argument denylist off the flags that execute a
# command or write through a path (-O runs a pager, --output writes
# a file); everything else -- log, show, diff, grep, blame -- is
# object reads.  A refused call returns its reason as tool output:
# the model corrects course, the job does not care.
run_git()
{
	local sub=${1:-}
	case $sub in
	log|show|diff|grep|blame|ls-tree|ls-files|rev-list|rev-parse|shortlog|cat-file|describe) ;;
	*)
		printf 'gpt-review tool: git %q is not allowed; use: log show diff grep blame ls-tree ls-files rev-list rev-parse shortlog cat-file describe\n' "$sub"
		return 0
		;;
	esac
	local a
	for a in "$@"; do
		case $a in
		-O*|--open-files-in-pager*|--output*|--ext-diff|--textconv)
			printf 'gpt-review tool: argument %q is not allowed\n' "$a"
			return 0
			;;
		esac
	done
	timeout 30 git --no-pager "$@" 2>&1 | head -c "$max_tool_bytes" || :
}

jq -n \
	--rawfile instructions "$prompt_file" \
	--arg metadata "$metadata" \
	--arg pr_body "$pr_body" \
	--arg focus "$focus" \
	--rawfile policy "$tmp/policy.txt" \
	--rawfile bots "$tmp/bots.txt" \
	--rawfile diff "$tmp/diff.txt" \
	'[
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
	]' > "$tmp/input.json"

# The tool loop, stateless on purpose (store: false): every round
# resends the whole transcript, reasoning items riding along
# encrypted, and each function call runs here and returns as a
# function_call_output item.  When the round budget is out the
# model is told to write with what it has.
tool_calls=0
input_total=0
output_total=0
while :; do
	choice=auto
	if (( tool_calls >= max_tool_calls )); then
		choice=none
		jq '. + [{role:"user", content:[{type:"input_text",
			text:"Tool budget spent. Write the review from what you have seen."}]}]' \
			"$tmp/input.json" > "$tmp/input.next" &&
			mv "$tmp/input.next" "$tmp/input.json"
	fi
	jq -n \
		--arg model "${OPENAI_MODEL:-gpt-5.6-sol}" \
		--arg effort "${OPENAI_REASONING_EFFORT:-xhigh}" \
		--argjson max_out "$max_output_tokens" \
		--arg choice "$choice" \
		--slurpfile input "$tmp/input.json" \
		'{
			model: $model,
			store: false,
			include: ["reasoning.encrypted_content"],
			reasoning: {effort: $effort},
			max_output_tokens: $max_out,
			tool_choice: $choice,
			tools: [{
				type: "function",
				name: "git",
				description: "Run one read-only git command in the repository clone. args is argv after git, e.g. [\"log\",\"--oneline\",\"-20\",\"refs/gpt-review/head\"]. Allowed subcommands: log show diff grep blame ls-tree ls-files rev-list rev-parse shortlog cat-file describe.",
				strict: true,
				parameters: {
					type: "object",
					properties: {
						args: {type: "array", items: {type: "string"}}
					},
					required: ["args"],
					additionalProperties: false
				}
			}],
			input: $input[0]
		}' > "$tmp/request.json"

	# A transport failure leaves no HTTP code; curl's own exit is
	# the report then, and it reaches the PR like any other.
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

	input_total=$((input_total + $(jq -r '.usage.input_tokens // 0' "$tmp/response.json")))
	output_total=$((output_total + $(jq -r '.usage.output_tokens // 0' "$tmp/response.json")))

	jq -s '.[0] + [.[1].output[]?]' "$tmp/input.json" "$tmp/response.json" \
		> "$tmp/input.next" && mv "$tmp/input.next" "$tmp/input.json"

	jq -c '.output[]? | select(.type == "function_call")' \
		"$tmp/response.json" > "$tmp/calls.jsonl"
	[[ -s $tmp/calls.jsonl ]] || break

	while IFS= read -r call; do
		call_id=$(jq -r '.call_id' <<< "$call")
		mapfile -t args < <(jq -r '(.arguments | fromjson).args[]?' <<< "$call")
		tool_calls=$((tool_calls + 1))
		run_git "${args[@]}" > "$tmp/tool.out"
		jq --arg id "$call_id" --rawfile out "$tmp/tool.out" \
			'. + [{type:"function_call_output", call_id:$id, output:$out}]' \
			"$tmp/input.json" > "$tmp/input.next" &&
			mv "$tmp/input.next" "$tmp/input.json"
	done < "$tmp/calls.jsonl"
done

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
usage="$tool_calls git call(s) · input $input_total, output $output_total tokens"

post_comment "$(cat <<EOF
## GPT review

$model · $usage

$review
EOF
)"
