---
name: review-bots
description: Fetch, triage, and act on AI-reviewer PR feedback
  (CodeRabbit, Copilot) — reply, resolve, commit, push
disable-model-invocation: true
allowed-tools: Bash, Read, Grep, Glob, Edit, Write
---
Plumbing: bash .claude/skills/review-bots/rabbit.sh   — CodeRabbit
          bash .claude/skills/review-bots/pilot.sh    — Copilot
(fetch | reply <tid> <body> | resolve <tid> | unresolve <tid> | comment <body>)
Both entry points speak the same verbs and emit the same shapes; only
the reviewer they filter differs. Invoked bare, cover both bots; an
argument naming one narrows to it.

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
  ~a minute); Copilot never answers replies — a reply there is audit
  trail, not conversation. Resolve only if dismissal was approved.
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
re-reviews only when a review is re-requested. Run fetch once more;
REPORT any new findings but do not act on them — they belong to the
next invocation. This is the loop guard.
