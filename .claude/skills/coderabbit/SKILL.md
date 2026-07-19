---
name: coderabbit
description: Fetch, triage, and act on CodeRabbit PR feedback — reply,
  resolve, commit, push
disable-model-invocation: true
allowed-tools: Bash, Read, Grep, Glob, Edit, Write
---
Plumbing: bash .claude/skills/coderabbit/cr.sh
(fetch | reply <tid> <body> | resolve <tid> | unresolve <tid> | comment <body>)

## Phase 1 — fetch & triage. No code edits, no API writes.
Run cr.sh fetch. Triage EVERY unresolved finding, including outdated
ones (outdated ≠ wrong; code moved, the bug may not have). Skip
already-resolved threads but report their count. CodeRabbit is an AI
reviewer: weigh, don't defer. For any bug claim you'd call VALID,
verify concretely — trace the path or write a failing test; if you
can't reproduce it, it's NEEDS-VERIFICATION.

Produce a table: thread-id | file:line | summary | verdict
(VALID / WRONG / STYLE-ONLY / NEEDS-VERIFICATION) | reason |
proposed action. Then STOP and wait for explicit approval.

## Phase 2 — act. Only approved items, exactly as approved.
- VALID: implement; verify (build/tests); commit with a message
  referencing the finding; push ONCE after all fixes. Then per thread:
  cr.sh reply with what changed + the commit SHA, then cr.sh resolve.
- WRONG: cr.sh reply with the concrete refutation. Mention
  @coderabbitai only if we want its counter-response (it answers
  asynchronously, ~a minute). Resolve only if dismissal was approved.
- STYLE-ONLY: as decided at the gate; if dismissed, one-line reply,
  then resolve.
- NEEDS-VERIFICATION: reply stating what's missing. Never resolve.

Rules: never post a blanket `@coderabbitai resolve`. Every resolved
thread must contain either a fix reference or a stated reason — audit
trail. Never force-push in this workflow; rewriting published history
orphans the review anchors. Never use git commands not on the allowed
list.

## Phase 3 — after push.
CodeRabbit incrementally re-reviews new commits. Run cr.sh fetch once
more; REPORT any new findings but do not act on them — they belong to
the next invocation. This is the loop guard.
