#!/usr/bin/env bash
# Deletes this workflow's skipped runs.  Every comment on every PR
# wakes an issue_comment workflow; the job's gate skips the ones
# that are not commands, and each skip still leaves a run in the
# Actions list forever.  The runs are noise by construction --
# nothing executed -- so the real runs sweep them: the review job
# ends with this, whatever became of the review, and the
# review-bots tooling can run it by hand.  Only skipped runs of
# this one workflow; failures, cancellations and successes are
# history and stay.
set -euo pipefail

readonly api="${GITHUB_API_URL:-https://api.github.com}"
readonly repo="${GITHUB_REPOSITORY:?GITHUB_REPOSITORY is unset}"
readonly workflow="${GPT_REVIEW_WORKFLOW:-gpt-review.yml}"

gh_curl()
{
	curl --silent --show-error --fail-with-body \
		-H "Authorization: Bearer ${GH_TOKEN:?GH_TOKEN is unset}" \
		-H 'Accept: application/vnd.github+json' \
		-H 'X-GitHub-Api-Version: 2022-11-28' \
		"$@"
}

swept=0
for id in $(gh_curl "$api/repos/$repo/actions/workflows/$workflow/runs?status=skipped&per_page=100" |
	jq -r '.workflow_runs[].id'); do
	gh_curl -X DELETE "$api/repos/$repo/actions/runs/$id" > /dev/null
	printf 'gpt-review: swept skipped run %s\n' "$id"
	swept=$((swept + 1))
done
printf 'gpt-review: %d skipped run(s) swept\n' "$swept"
