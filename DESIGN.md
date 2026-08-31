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
gating rule, one currency of urgency, immutable snapshots that
are selected rather than mutated, and workers that report facts
instead of coordinating.

- **Waiting is a dependency.**  If work must not run yet, that is
  expressed in `agenda::task::deps` and nowhere else.  There are
  no pause flags, no hold booleans, no mode enums beside the
  plan.  Anything that used to be "executor state" is either a
  dependency or it was a bug.
- **Rivalry is heat, beneath rank.**  A ready task's place in
  line is a lexicographic key `{kind rank, heat − tier + export
  edge}`; priority inheritance lifts the whole key.  Kind rank is
  dominant by construction -- unbounded heat reorders work only
  within its class -- and ordering lives in `plan::score()`
  alone.  Nothing peeks twice.
- **Nothing is undone.**  Evidence accumulates into immutable
  cuts; the system moves by *selecting* a newer cut, never by
  revising an older one.  Every completed fact stays true of the
  snapshot it was about.
- **Workers report.**  A worker owns its state behind its own
  lock, produces drainable events, and pokes.  No worker knows
  another worker exists.

## The four planes

State lives on four planes, distinct even where they are
physically colocated:

1. **Base corpus** -- videos, transcript revisions, user-authored
   topics.  Only the user writes here.
2. **Cut** -- one immutable, frame-enriched evidence snapshot:
   the windows, their text, their frame lines.  Identified by
   `cut_key = H("cut-v1", base revision, OCR recipe, ordered
   window content ids)`.  A byte-identical successor is the same
   cut.
3. **Artifact store** -- immutable model outputs, content-
   addressed with exact input provenance.  Written once, never
   edited; an empty reply writes nothing, so the cache can never
   mask a failure.
4. **Projection** -- the derived records, topics, terms and
   glosses *for one cut_key*, folded from artifacts in canonical
   corpus-derived order.  What the UI shows is the projection of
   the currently selected cut.

A machine-generated result never silently becomes base-corpus
input; where a derived result motivates a further ask, that is an
explicit edge in the derived graph, so callback order can never
change task identity.

## The scheduler

`agenda::plan` is the sole authority on dependency admission and
order.  Tasks are (id, kind, deps, keys, tier, cut_key).
`ready()` admits when every dep is done -- the only
dependency-admission rule in the program; lane fit is mechanical
resource compatibility, not policy.  `done()` remembers unknown
ids as done (tombstones), so marking and staging cannot race.
Failure has a taxonomy, not a single pit: terminal faults park,
transient faults schedule a retry, and work unreachable from any
live cut is marked obsolete -- three named states with three
different cache semantics.

**Witnesses** are ids that stand for facts about one cut.  The
ground witness of a cut means "this exact cut contains every
planned OCR result, drained and folded."  It is marked done by
the one place that observes publication, and -- like every fact
about an immutable snapshot -- is never unmarked.  Frame-
sensitive kinds (extract, judge, terms) and every *adoption*
action carry their cut's ground witness in `deps`; frame-
independent kinds (leaf, node, dive, probe, focus) do not, which
is why the model is never idle while a corpus reads itself.
With the reader off, the witness is marked at staging.

**Cuts succeed each other; epochs bound lifetimes.**  New OCR
evidence builds a *candidate* cut privately while the published
cut stands immutable; when a drain-then-probe proves the
candidate quiet and folded, `current_cut` advances in one atomic
selection.  In-flight work of the old cut remains semantically
valid -- it may finish and populate the artifact store, and its
adoptions write only their own cut's projection, so a late
completion cannot corrupt the current view.  Cancelling such
work is a cost decision (reachability, remaining spend), never a
correctness requirement.  Orthogonal to all of this, `epoch` is
the operational incarnation stamped on completion envelopes --
`{task id, cut_key, epoch}` -- used to reject callbacks across
reset and teardown; it never enters durable identity.

## The executor

`Facts` runs the plan against the model with two lanes -- urgent
(answers) and background -- and zero policy of its own: a free
lane claims `peek()`'s best ready compatible task, submits, and
returns to the pool.  The pace between background tasks is a
thermal courtesy to the accelerator.  The executor's opinions
are mechanical: never advance past teardown, never publish a
reply from a dead epoch, never let an empty reply masquerade as
an artifact.

## The workers

`scribe`, `grabber`, and the decoder they share follow one
pattern: mutating calls marshal in, results drain out behind a
poke, and quiescence is a question the worker answers about its
*whole* pipeline -- bench, in flight, and undrained -- so a
drain-then-probe caller can never conclude against undelivered
news.  Locking follows ownership: one lock guards one coherent
state transition, and no field is atomic unless it is a whole
protocol by itself; workers never expose internals for
cross-thread inspection.  The settle debounce is a sensor
concern (when has the picture stopped changing?) that ends in
exactly one act: assemble the candidate cut and publish it.
Publication is the only signal that crosses from the OCR world
to the model world, and it crosses inside the scheduler.

## The harvest

Artifacts return to view through adoption actions: local agenda
work `H("adopt", artifact id, cut_key)` whose deps are the
artifact and its cut's witness, taken from the same plan as
everything else -- a cached artifact completes instantly but its
adoption still waits its turn, so availability and permission
never conflate.  Each harvester converts exact artifact bytes
into immutable facts keyed by artifact id; completion is only a
wakeup, and the projection folds facts in canonical
corpus-derived order, never callback order.  Sequential display
identities are assigned after the fold; incremental folding is
admissible only where the merge is associative, commutative and
idempotent.

## Invariants

- **I1** No dependency-admission decision is made outside
  `plan::ready()`; lane fit is mechanical resource compatibility.
- **I2** No ordering decision is made outside `plan::score()`.
- **I3** A witness, once done, stays done: it states a fact about
  an immutable cut.  New evidence means a new cut, never a
  revision.
- **I4** No worker waits on, polls, or observes another worker;
  the only cross-domain signal is cut publication, inside the
  scheduler.
- **I5** While online and outside thermal pacing, a free
  compatible lane claims the highest-scoring ready task before it
  sleeps.
- **I6** No frame-sensitive ask and no adoption starts before its
  cut's ground witness is done; an adoption writes only the
  projection named by its own cut_key.
- **I7** The composition root owns lifetimes; one owner
  serializes scheduler and projection state; the UI consumes
  published snapshots.  These may share a thread; that is an
  implementation choice, not a law.
- **I8** For a fixed base corpus, cut, recipes, and accepted
  artifact bytes, the projection is byte-identical and
  independent of completion order.  A cache wipe may change what
  the model says, never its identity, provenance, or the
  mechanical acceptance rules.
- **I9** Every task, artifact, and completion names its cut_key;
  every asynchronous completion also names its epoch.
- **I10** Publishing a cut is one atomic selection of an
  internally complete candidate; evidence accumulation never
  mutates a published cut.

The plan and the mailbox prove the scheduler- and worker-local
halves (I1-I3, I5, the quiescence of I4) in their unit
batteries; the cross-component invariants (I4, I6-I10) are
proven by the zero-interaction rigs against a live pipeline --
a unit test cannot vouch for a boundary it does not own.

## From here to there

Steps 0-3 are landed for the term directory: the catalog
generation hash is the cut's ground witness, frame-sensitive asks
gate on it in `deps`, the hold machinery is deleted, the score is
the lexicographic key, the harvest machines live in `Refinery`
driven by completion pokes, content identity replaced every path
hash (a cache travels; the portability rig proves it), and the
term directory AND the focus set are projections folded from
facts in canonical order — one unified erase, terms then focuses,
dives derived from the final directory, and no separate adoption
gate outside the plan: admission is `plan::ready()` alone, every
frame-sensitive ask carrying its cut's ground witness in `deps`.
adhocN topics stay event-ordered by design: a committed search is
a user action, and user actions are base-plane input in the order
they happened.  The migration is complete; what this page
describes is what the program now is.
