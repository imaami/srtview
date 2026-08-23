---
name: review-bots
description: Fetch, triage, and act on AI-reviewer PR feedback
  (CodeRabbit, Copilot, Codex, GPT) — reply, resolve, commit, push
disable-model-invocation: true
allowed-tools: Bash, Read, Grep, Glob, Edit, Write
---
Plumbing: bash .claude/skills/review-bots/rabbit.sh   — CodeRabbit
          bash .claude/skills/review-bots/pilot.sh    — Copilot
          bash .claude/skills/review-bots/codex.sh    — Codex
          bash .claude/skills/review-bots/gpt.sh      — GPT
(fetch | reply <tid> <body> | resolve <tid> | unresolve <tid> | comment <body>)
All entry points speak the same verbs and emit the same shapes; only
the reviewer they filter differs. Invoked bare, cover every bot; an
argument naming one narrows to it.

GPT is the comment-triggered reviewer under .github/gpt-review: it
reviews only when told to, and gpt.sh has the extra verb
`ask [focus]`, which posts the `/gpt review` command. A review spends
the owner's OpenAI credits: ask only when the user requests a GPT
review or has approved triggering one. GPT writes one top-level PR
comment per review (no threads; a failure is a comment too, flagged
`failed`), so its fetch rows carry the comment's node id as the
thread id: `reply` posts a PR comment linking the review, `resolve`
minimizes the review as resolved once every finding in it has its
fix or refutation on record, `unresolve` restores it, and `sweep`
deletes the workflow's skipped runs (each real run also sweeps on
its way out).

## Phase 1 — fetch & triage. No code edits, no API writes.
Run fetch per bot in scope. Triage EVERY unresolved finding, including
outdated ones (outdated ≠ wrong; code moved, the bug may not have).
Skip already-resolved threads but report their count. These are AI
reviewers: weigh, don't defer. For any bug claim you'd call VALID,
verify concretely — trace the path or write a failing test; if you
can't reproduce it, it's NEEDS-VERIFICATION.

Produce a table: bot | thread-id | file:line | summary | verdict
(VALID / WRONG / STYLE-ONLY / NEEDS-VERIFICATION) | reason |
proposed action. Then STOP and wait for explicit approval.

## Phase 2 — act. Only approved items, exactly as approved.
- VALID: implement; verify (build/tests); commit with a message
  referencing the finding; push ONCE after all fixes. Then per thread:
  reply with what changed + the commit SHA, then resolve.
- WRONG: reply with the concrete refutation. Mention @coderabbitai
  only if we want its counter-response (it answers asynchronously,
  ~a minute); Copilot never answers replies, and Codex only answers
  an explicit @codex mention — a plain reply there is audit trail,
  not conversation. Resolve only if dismissal was approved.
- STYLE-ONLY: as decided at the gate; if dismissed, one-line reply,
  then resolve.
- NEEDS-VERIFICATION: reply stating what's missing. Never resolve.

Rules: never post a blanket `@coderabbitai resolve`. Every resolved
thread must contain either a fix reference or a stated reason — audit
trail. Never force-push in this workflow; rewriting published history
orphans the review anchors. Never use git commands not on the allowed
list.

`comment` and `reply` prepend a disclaimer line above the comment
clarifying Claude's authorship of the text and the permission from
the account owner to post it.

## Phase 3 — after push.
CodeRabbit incrementally re-reviews new commits on its own; Copilot
re-reviews only when a review is re-requested; GPT only when asked
(Phase 2's rules for `ask` apply). Run fetch once more;
REPORT any new findings but do not act on them — they belong to the
next invocation. This is the loop guard.
