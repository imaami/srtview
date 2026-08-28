# srtview — design

This document describes the architecture as it is meant to be
read: from the end state.  Per-file header comments remain the
authoritative micro-docs; this page holds only the shape of the
whole and the invariants that keep it.  The build teaches the
layering (`tests/` links each module without the layers above
it); this page teaches the *time* — who may run, when, and why
that question has exactly one answer.

## Thesis

srtview is a corpus of videos being distilled into knowledge by
machines with wildly different clocks: a decoder that answers in
milliseconds, an OCR engine in tens of milliseconds, a language
model in minutes.  The design problem is not making them fast --
it is making them *simultaneous* without letting any of them lie
to the others about time.  The answer has one scheduler, one
gating rule, one currency of urgency, and workers that report
facts instead of coordinating with each other.

- **Waiting is a dependency.**  If work must not run yet, that is
  expressed in `agenda::task::deps` and nowhere else.  There are
  no pause flags, no hold booleans, no mode enums beside the
  plan.  Anything that used to be "executor state" is either a
  dependency or it was a bug.
- **Rivalry is heat.**  If work *may* run, its place in line is
  its score -- kind rank first, user attention (heat) within the
  rank.  Ordering lives in `plan::score()`; nothing peeks twice.
- **Workers report.**  A worker owns its own state behind its own
  lock, produces drainable events, and pokes.  No worker knows
  another worker exists.  The composition root wires them and
  owns their lifetimes; it keeps no coordination state of its
  own.

## The scheduler

`agenda::plan` is the sole authority on admission and order.

**Tasks** are (id, kind, deps, keys, tier).  `ready()` admits a
task when every dep is done -- the only admission rule in the
program.  `done()` remembers unknown ids as done (tombstones), so
marking and staging cannot race; `fail()` parks for the session.
Score is `kKindBase[kind]` dominant, heat within it: the semantic
chain (extract, judge) outranks summaries by *rank*, so no free
lane ever needs a second opinion, and a user's browsing reorders
work only within its class.

**Witnesses** are ids that stand for facts instead of artifacts.
A witness is staged nowhere, executed never, and marked `done()`
by the one place that observes the fact.  The ground-truth
witness -- derived from (corpus identity, OCR recipe) -- means
"the corpus has been read and cut with its frames."  Every
frame-sensitive kind (extract, judge, terms) carries it in
`deps`; frame-independent kinds (leaf, node, dive, probe, focus)
do not, which is the whole reason the model is never idle while
the corpus reads itself.  With the reader off, the witness is
marked at staging; a new cut is a new generation whose witness id
differs, so a witness, once done, is never *un*-done -- time in
the plan moves only forward.

**Generations.**  A cut of the corpus stages its tasks and
retires its predecessor's: the stale set is old-minus-new by id
(identical windows share ids and survive untouched), parked via
`fail()`.  Nothing is deleted; artifacts are content-addressed,
so a retired ask that already landed is simply a cache hit next
generation.

## The executor

`Facts` runs the plan against the model with two lanes -- urgent
(answers) and background (everything else) -- and zero policy of
its own: each free lane takes `peek()`'s best ready task for its
fit, submits, and returns to the pool on completion.  The pace
between background tasks is a thermal courtesy to the
accelerator, not scheduling.  The executor's only opinions are
mechanical: never advance past teardown, never publish a reply
from a dead generation (epochs), never let an empty reply
masquerade as an artifact.

## The workers

`scribe`, `grabber`, and the decoder they share follow one
pattern: mutating calls marshal in, results drain out behind a
poke, and quiescence is a question the worker answers about its
*whole* pipeline -- bench, in flight, and undrained -- so a
drain-then-probe caller can never release against undelivered
news.  The scribe's reading plan feeds the OCR archive; the
settle debounce is a sensor concern (when has the picture
stopped changing?) that ends in exactly one act: cut the corpus,
stage the generation, mark its witness.  That mark is the only
signal that ever crosses from the OCR world to the model world,
and it crosses inside the scheduler.

## The harvest

Artifacts return to the corpus through harvesters: pure functions
from a landed artifact to a corpus mutation (adopt a topic, grow
a fragment, file a gloss), invoked from one dispatch keyed on
completions rather than five state machines polling `landed()`.
Every harvester is re-derivable: cache plus corpus reproduces the
same adoptions in the same order on any machine, any session --
determinism is what makes the cache a proof rather than a risk.

## Invariants

- **I1** No admission decision is made outside `plan::ready()`.
- **I2** No ordering decision is made outside `plan::score()`.
- **I3** A witness, once done, stays done; new facts mean new
  witness ids, never reversals.
- **I4** No worker blocks, polls, or observes another worker; the
  only cross-domain signal is a witness mark.
- **I5** The model is never idle while any ready task exists.
- **I6** No frame-sensitive ask is ever submitted before its
  ground-truth witness is done.
- **I7** The UI thread owns lifetimes and drains events; it holds
  no coordination state.
- **I8** Every artifact and every adoption is a deterministic
  function of (corpus, caches, recipes); a wiped cache changes
  cost, never outcome.

Each invariant is testable without the app: the plan and the
mailbox prove I1-I6 in their unit batteries, and the
zero-interaction rigs prove I5-I8 against a live pipeline.

## From here to there

Three atomic steps, each a red diff, each revertible alone:

1. **The witness.**  Frame-sensitive tasks gain the ground-truth
   witness in `deps`; the re-cut marks it done.  `Facts::hold()`,
   its boolean, the `patient` fit and all four lifecycle call
   sites are deleted; `queueTerms()` loses its quiescence gate.
2. **Rank into score.**  The ground-first double peek dissolves
   into `kKindBase` so ordering has one home.
3. **The harvest dispatch.**  The terms/spell/merge/focus/dive
   pollers become completion-driven harvesters under one
   dispatch.  (Overlaps the frozen structural round; last, and
   only when its time comes.)
