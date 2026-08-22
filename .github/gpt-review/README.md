# GPT PR review for srtview

A deliberately small, comment-triggered OpenAI reviewer.

## What it does

On a pull request, a trusted maintainer can write:

```text
/gpt review
```

or add a focus, on the same line:

```text
/gpt review concurrency, ownership, UB, and error paths
```

The lines below the command are the commenter's own and are not sent
to the model.

The workflow:

1. runs the trusted reviewer code from the repository's default branch;
2. reads PR metadata and the unified diff through the GitHub API;
3. reads existing CodeRabbit/Copilot/Codex review text as deduplication
   and disagreement context;
4. adds the repository's existing `CLAUDE.md`, `.coderabbit.yaml`, and
   `AGENTS.md` as house-policy context;
5. makes one OpenAI Responses API request;
6. posts the resulting review as a normal PR conversation comment,
   opening with `<!-- gpt-review: pr=N sha=H -->` so tooling can find
   the job's comments (failures carry the marker too).

It never starts Codex, never runs a persistent agent, and never executes
the PR branch.

## Install

Extract this archive at the repository root:

```bash
tar -xzf srtview-gpt-review.tar.gz
git add .github
git commit -m 'Add comment-triggered GPT PR review'
git push
```

The `issue_comment` workflow must exist on the default branch before
`/gpt review` can trigger it.

## Configure

Create a repository Actions secret named:

```text
OPENAI_API_KEY
```

The API is billed separately from a ChatGPT Plus subscription.

Optional repository Actions variables:

```text
OPENAI_REVIEW_MODEL=gpt-5.6-sol
OPENAI_REVIEW_REASONING=xhigh
```

The workflow defaults to those values if the variables do not exist.

Those are also the workflow defaults. The model and reasoning variables remain
configurable so changing either does not require editing the workflow.

## Authorization

Only the repository owner's GitHub login can trigger a paid review:

```text
comment author == repository owner
workflow triggering actor == repository owner
```

Checking both values also prevents collaborators from spending credits by
rerunning an earlier owner-triggered workflow. Other users' `/gpt review`
comments do not start the job.

## Security model

This is an `issue_comment` workflow, so it runs from the default branch.
That is intentional.

The job has a repository secret. Therefore it must not check out, source,
build, test, or execute the PR head. The PR diff, PR text, and bot comments
are fetched only as untrusted data and passed to the model.

Do not "improve" this by checking out `${{ github.event.pull_request... }}`
or a `refs/pull/...` head and then running repository scripts.

## Files

- `.github/workflows/gpt-review.yml` — trigger and least-privilege token
  permissions.
- `.github/gpt-review/review.sh` — GitHub/OpenAI API plumbing.
- `.github/gpt-review/prompt.md` — review policy.

## Limits

The default caps are:

```text
GPT_REVIEW_MAX_DIFF_BYTES=600000
GPT_REVIEW_MAX_BOT_BYTES=120000
GPT_REVIEW_MAX_OUTPUT_TOKENS=64000
```

They can be set in the workflow environment if a future PR needs
different limits. Truncation happens on line boundaries, and each
existing-review surface is read one page (100 entries) deep. The output
cap counts the model's reasoning tokens as well as the review text, so
it is sized for `xhigh` reasoning, not for the few thousand tokens the
review itself takes; a response the cap cuts short is posted with a
note, or reported when it cut before any text.

The first version posts one top-level review comment rather than inline
threads. That keeps the machinery deterministic and avoids trusting an
LLM to manufacture GitHub diff positions. File/line references remain in
the review text.
