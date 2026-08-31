# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build and test

```bash
cmake -B build && cmake --build build     # plain build
./build.sh                                # full rebuild: WIPES build/, native-tuned flags, LTO, strips the binary
ctest --test-dir build                    # all tests
ctest --test-dir build -R fundo_tests     # one test suite (or run ./build/fundo_tests directly)
./build/srtview --selftest VIDEO.mp4      # scripted offscreen exercise of the real key paths against a live mpv
```

Deps: Qt6 (Widgets, Network), FFmpeg dev libraries (libavformat/avcodec/avutil/swscale), libcurl, libtesseract + leptonica dev packages (`tesseract.pc` and `lept.pc`; tessdata language models at runtime), C++23 and C23 compilers, mpv at runtime. All executables land in `build/` (not `build/src/`). `cmake/compiler.cmake` injects LTO flags per compiler via `CMAKE_USER_MAKE_RULES_OVERRIDE`.

## What this is

`srtview` is a Qt6 subtitle-transcript viewer that remote-controls mpv. The full user-facing spec — keybindings, env vars (`SRTVIEW_MPV_ARGS`, `SRTVIEW_DEBUG`), WSLg audio workarounds — lives in the header comment of `src/main.cpp`. `srtjump/srtjump` is a standalone bash sibling (less/Kate integration) that predates the GUI and shares the same player instances.

## Architecture

Every source file opens with a comment stating its role and constraints; read those first — they are the authoritative per-file docs.

**Layering (mixed-language, deliberate):**
- **Plain C core** (`fundo.c/h`, `list.c/h`, `loop.c/h`, `cutil.h`, C23): the undo *tree* with persistent side branches, plus the epoll dispatcher the llm client's worker runs on. Opaque byte payloads, exact-content identity, byte-identical actions re-adopt existing branches instead of forking. `*_priv.h` headers hold internals shared with tests. No Qt, no C++.
- **Qt-free C++** (`srt.cpp/hpp`, `topics.cpp/hpp`, C++23): SRT cue model, encoding normalization (UTF-8/UTF-16/Windows-1252), and a CRTP push parser (`srt::parser<Derived>` emits `on_cue()` statically dispatched); the topic-file model — the hand-written corpus format of videos plus named, composable regexes (`\{name:}` references) whose grammar is documented in the `topics.hpp` header comment; `rx.cpp/hpp` — mechanical regex algebra over finite string sets (`knit`/`unknit` compress a word list into the minimal portable pattern and re-open it losslessly, `braid` merges near-identical OCR variants into consensus + tolerant pattern), which `topics::tidy` leans on so machine patterns converge instead of listing — the knit must pay for itself in length, and coverage keys see through composites word-by-word. The UI converts to Qt types at its own boundary.
- **Qt layer**: everything else.

Tests mirror the layering: `tests/parse_test.cpp` and `tests/topics_test.cpp` link their modules with no Qt at all; `tests/fundo_test.c` links the C core with no C++. Keep those modules dependency-free or the test targets break.

**Component/controller split in the Qt layer:**
- `concepts.hpp` defines host contracts (`playback_host`, `search_host`, `mpv_observer`). Components (`srtedit`, `searchbar`, `mpvlink`) are thin header-only templates constrained by these concepts and never name a concrete host; each has a non-template base (`srt_view_base`, `search_bar_base`, `mpv_link_base`) compiled once in the .cpp, which is what controllers hold.
- Controllers/mediators: `PlaybackCtl` (`playback.cpp`) executes transport verbs against mpv; `SearchCtl` (`search.cpp`) owns pattern semantics and match navigation; `Refinery` (`refinery.cpp/hpp`) owns every machine that turns landed model artifacts into corpus adoptions — term windows, spelling votes, the directory fold, topic dives, probe/focus chains — plus the completion-poked dispatch driving them: Facts pokes on landing, one queued wake per burst runs the chain (`landed()`-swept, so idle costs nothing), and the UI is reached only through `refinery_host` (one adoption-held question, one something-changed report).
- `Trail` (`trail.cpp/hpp`) is the C++ facade over the fundo C core: undo/redo breadcrumbs of search/jump/seek steps. Each node stores a *state*: a bitmask of the facets the step touched (search text, text cursor, video position) plus each touched facet's after-value — one step can combine facets ("jumped in text and video"). Undo resolves departed facets to their nearest recorded ancestor values; encoding is deterministic so identical steps trigger branch adoption. Search hits wrapping the document form fundo travel rings: a hop byte-identical to its ring neighbor travels instead of growing the tree, per-node pass counters linearize multi-lap travel, and on a ring backward search coincides with undo (forward with redo). Its "applying" latch suppresses recording while a step is replayed.
- `MainWin` (`mainwin.cpp`) is the composition root — owns components and controllers, wires everything; nothing depends on it except `main` and `selftest`. It also owns the corpus: a loaded topic file becomes the playlist (Videos menu) and the id→path registry cross-video steps resolve through. The trail's video facet carries the video's *content identity* — `Ident` (`ident.cpp/hpp`), a persistent std::jthread pool that streams BLAKE2b-256 over file bytes off the UI thread, memoized in `$XDG_CACHE_HOME/srtview/ids` — so undo/redo switch videos when a step's id differs from the current one. Content is identity everywhere data is cached or fingerprinted (a cache copied to another machine with renamed inputs matches, given the same playlist content order); discovery's path hash remains only the per-machine socket rendezvous shared with srtjump. The corpus load splits at the identity seam: paths and playback immediately, the identified tail on the hash batch's completion poke.
- `engine::SemanticEngine<Backend>` (`semantic_engine.hpp`, standard C++23, header-only template over a five-call backend concept — `Facts` in the app, a fake in `tests/engine_test.cpp`) owns cue-window extraction, evidence validation, bounded consolidation, the relation graph's readers and cited question answering outside the UI; the same cut of windows serves the terms pass. `semantic.cpp/hpp` is the Qt-free ontology/parser/catalog/vocabulary: a record is a subject–relation–object triple with its sentence and exact source cue spans, never a regex; retrieval weighs words by corpus frequency, never by a word list. Equivalence edges (same verdicts, or byte-identical triples) produce a consolidated concept view, and the catalog holds that view as a graph of entities — name → predicates → objects, an object that names an entity linking to it, two names judged one thing folding into one entity — which the Knowledge pane draws as a tree with cross-links.
- `Prefs` (QSettings-backed) persists last directory, recent files, search history.

**Export:** `exporter.cpp/hpp` turns the corpus into per-*grouping* directories driven by `topics::export_plan()`: only top-level topics (referenced by no other) export, each with a Markdown digest (pattern, per-video sections, matched cue ± neighbors, three frame links per hit) plus copied picks. Components earn their own digest through acknowledgment capture parens in a top-level topic (grammar in `topics.hpp`); their hits are attributed via named groups unioned over every match in a cue, and their frames are **relative symlinks** into the parent grouping's copies — one PNG on disk however many digests cite it. Deterministic build artifact: File → "Export frames" writes what the cache has into `<corpus-stem>-export/` beside the topic file, enqueues missing hits on the grabber, and MainWin re-runs the writer on `grabsIdle()` until complete (or until a pass makes no progress).

**Frame grabs:** `Grabber` (`grabber.cpp/hpp`) lives on its own worker thread — decoding, PNG encoding, thumbnail diffs and picks bookkeeping never touch the UI thread; mutating calls marshal onto the worker, `picksFor`/`framePath` read under a lock, and listener notifications are queued into the listener's thread. It shadows the session: every video jump enqueues its timestamp, and three picks per hit — the hit frame plus one from the different-looking content on either side, found by bisecting for content-change boundaries — land in `$XDG_CACHE_HOME/srtview/frames/<video id>/`, with `picks.txt` as both manifest and cross-session dedupe record. Extraction is an in-process libav decode context: `decoder.cpp/hpp` (`media::decoder`, std C++23 + FFmpeg, **no Qt** — `decoderq.hpp` is the concrete inline Qt shim) keeps the demuxer open per video, decodes bisection probes straight into 64×36 grayscale compare thumbs that never touch a PNG encoder or the disk (~35 ms/probe vs ~200 ms per round-trip of the former mpv shadow player), and produces a full RGB frame only for a pick about to be encoded. A hit inside an already-bisected segment reuses that segment's boundaries (content-compared), so a cluster of hits on one slide costs one encoded frame each.

**OCR:** a parallel evidence channel that reads on-screen text out of frames. `ocr.cpp/hpp` (`ocr::tess`, standard C++23) is the only tesseract-including code in the tree: raw gray8 views in, trimmed UTF-8 line spans with boxes and confidences out, LSTM-only so a result is a pure function of (pixels, options, lang, tessdata). Leptonica is never called for work — one call at engine construction tells it to keep quiet — since `media::decoder::gray_at` (packed gray8 at an integer upscale, one sws pass) does the scaling and tesseract's own Otsu binarizes. `scribe.hpp` (`ocr::scribe<B>`, std threads only) is the mailbox worker over a one-call backend concept: any-thread `post` with rush/rest lanes, identical requests coalesce (a rush post promotes a queued twin), `cancel` unhooks tickets anywhere including undrained notes, finished notes `drain` behind a cheap poke callback. `lector.cpp/hpp` (`ocr::lector`) is the real backend — one reopened-on-switch decode context feeding tess, ROIs and span boxes in source frame coordinates either side of the upscale — and `archive.hpp` (`ocr::archive<B>`) decorates any backend with a self-healing result cache keyed (discovery id, ms, layout, scale), so `scribe<archive<lector>>` reads a frame once, ever. In the app, `OcrQ` (`ocrq.hpp`, DecoderQ-style concrete shim) owns that stack and implements the grabber's `pick_sink`: the grabber's worker announces every pick frame — the followed video's hit is encoded and announced ahead of bisection (rush), boundaries and the once-per-session picks.txt manifest replay follow as rest — worker to worker, and the corpus **reads itself**: rebuildCorpus hands the scribe a reading plan (every cue start of every entry; the shown video preferred), performed one moment at a time whenever demand runs dry, pixels held in memory for exactly the duration of each read — only *touched* frames (jumps, exports) earn PNGs through the grabber, so the frame cache stays demand-sized while the OCR archive holds the corpus's whole text in kilobyte slots. The UI thread holds no OCR coordination state — it owns the stack's lifetime and nothing else: constructs, wires once, drains finished notes off the event loop (`SRTVIEW_DEBUG` journal) and stops at close. The default language walks a ladder — fin first (it reads this corpus best, its English included), then eng — resolved in one place (`kDefaultLangs`, ocr.cpp) and recorded per slot: the archive stamp carries the language that actually loaded plus its traineddata hash. Results land in `$XDG_CACHE_HOME/srtview/ocr/<video id>/`; `SRTVIEW_OCR=0` builds no model and starts no worker. Drained notes fold into MainWin's frame-text index as per-moment span lists (confidence-floored, a sensor gate not a semantic one), and on the debounced settle each video's moments pass through `loom.cpp/hpp` (`ocr::weave`, Qt-free like rx): near-identical spans sighted at the same box across consecutive moments link into *regions* — three-dimensional bounding boxes whose third axis is time, tolerances for pixel tremble, garbage characters and one-frame dropouts — and each region's `rx::braid` consensus joins the overlapping windows' bodies **and identities** as one `@ [H:MM:SS] text` line at its first sighting — the model is told what they are and does all grouping — so a slide is met once with its majority text, identities stop flapping on jitter, cached windows replay warm and only frame-touched identities re-ask. The region's variant-spanning pattern and box stay on `ocr::region`, the bridge a later search phase crosses; the Time cubes dock (`cubepane.cpp/hpp`) browses them live -- lazy children over MainWin's cached weave, double-click seeks the first sighting. Ground truth leads the model, by dependency alone: each cut's catalog generation hash doubles as its *ground witness*, listed in the deps of every frame-sensitive ask (extract, judge, terms) and marked done -- `Facts::mark()` -- only when the cut is complete (nothing left to read; a mid-read cut's asks never come ready and its successor retires them).  Frame-independent work (leaves, nodes, dives, probes, focus) runs throughout, and the released lane peeks extract/judge ahead of the rest.  No hold flags, no executor state: admission is `plan::ready()`, per DESIGN.md. `build/ocrview` stays the standalone uncached knob harness. Later phases: mmproj frame triage.

**mpv integration:** `mpvclient.hpp/cpp` is the shared IPC client both `mpvlink` and `grabber` build on (CRTP event dispatch over a compiled-once base): commands go out as single raw input.conf lines — one per action, explicit flush per send — while mpv's JSON event lines come back for observation and sequencing. The viewing player is **one persistent instance whose internal playlist mirrors the app's in set and order**: switching videos is `playlist-play-index` inside the same window, never a respawn; the observed `path` property routes mpv-side playlist navigation (its own `<`/`>` keys) back into the app (`onMpvIndex` → `video_sync`), and `file-loaded` attaches the entry's subtitles via `sub-add` (per-entry `loadfile` options changed signature across mpv versions) and fires deferred seeks. Sockets from the `discovery` class (`discovery.cpp/hpp` — pure C++23/POSIX with cached identities and prefixes; one instance owned by MainWin), `$XDG_RUNTIME_DIR/srtjump/<blake2b-256(realpath)[:16]>.sock`: a corpus playlist claims the topic file's own hash; a directly opened single video keeps the per-video scheme byte-for-byte shared with the `srtjump` script (`cksum -a blake2b -l 256 --untagged`). Adopting an already-running instance preserves its playback position, resyncing the playlist around the playing entry (`playlist-clear` + appends, no reload). Bring-up never blocks the UI thread: connect/spawn runs off a retry timer and commands queue until the on-connect setup (observation, resync) has gone out. `mpvlink` also watches player health and respawns a wedged mpv at the last observed position/pause state.

## Bests and pests

`bests/` and `pests/` complement `tests/`; neither is built, and nothing in them is meant to compile. Each file is one very short snippet with a project-style name and the correct suffix, ingested whole at a glance. A best condenses one desirable habit — a style, solution, or trait to reproduce. A pest condenses the opposite: code that could pass tests yet must never be written here (first pair: `header_guards.h`, traditional guard vs. `#pragma once`). Both directories are comment-free zones — the snippets are ideas without natural language, and one that needs an explanatory comment fails at its only job. The sole exception is a file whose demonstrated subject is itself a comment-writing approach.

## Conventions

### Approach and attitude

- Use some time during every task to optimize, simplify, and harden.
- It's better to delete than to add LoC. Given equal performance and binary size, a red diff beats a green one.
- Elegance and performance often correlate. Trust your sense of beauty. Verify your sense of trust.
- All code is bad, everywhere, always. Never trust code you read _or_ write.
- All code is bad, everywhere, always; coders are not. Finding a bug is a happy and inclusive event.
- Undefined Behavior is a wrathful cosmic force. "It's not UB if it works" is what your bugs want you to believe.
- Use compile-time features to your advantage; template metaprogramming in C++, and `_Generic`, `typeof`, `sizeof`, etc. in C.

### C

- Prefer standard, modern C23 and later. Claims about a "true C" of years past are the "golden age" delusion of C programming. The C language is by definition what the current standard says.
- Use `int` only if needed. "`int` by default" coding often necessitates more integer conversions which translate to costly sign extension instructions.
- Trace your call chains to see if you're e.g. calling `strlen()` effectively multiple times over the same input. If you need a C string's length more than once, measure it once and pass it down.
- Stay aware of the program flow. Are you repeating some task more than once when you could just use a variable? Fix it.
- Use helper macros when it's justified and reasonable, but undefine macros that don't need to be exposed ASAP. Typically this means defining something above a function and undefining it below.
- If you call `strlen()` inside a loop condition or on a string literal, you must spend a full day downtown pushing a baby stroller full of boiled cabbage.
- Exposed C APIs (headers consumed across the C/C++ boundary, e.g. `fundo.h`) use `int` for booleans, documented as 1/0 in the Doxygen comment — no `bool`/`_Bool` in signatures. C and C++ `bool` match only by platform-ABI convention, and the C core must stay ABI-safe under pre-C23 builds with polyfill macros. `bool` is fine in internal headers and header-inline helpers (`cutil.h`), which are never linked across the boundary.
- Arrange struct members so that implicit padding is minimal. Wider types first, narrower towards the end, grouped by width.
- Be mindful of width guarantees. Given 3 struct members, an `int64_t`, a `long`, and an `int32_t`, `long` goes between the fixed-width types as it could be either. Remember the corner cases; e.g. `int` is only _required_ to be 16 bits.
- If a struct has trailing padding, and the last member is an integer type or `bool`, change the the type such that it occupies the padding, unless it introduces more complexity (such as additional casts downstream).
- Prefer RAII-like variable use. Initialize variables at declaration time whenever possible, but avoid initializing with a useless value.
- Don't declare variables at the top of a function out of habit. It's no longer idiomatic but vestigial. Declare where it's possible to initialize usefully.
- Scope variables as narrowly as possible.
- Use anonymous structs and unions to combat nesting hell as needed.
- Practically nothing should be a `typedef`. The few exceptions when `typedef` is genuinely defensible are:
  - implementing opaque handle types in APIs (for example when library instantiation returns an instance pointer);
  - API callback function types (note: _not_ callback _pointer_ types - more about this later);
  - uniform interface semantics for C and C++ users of an API (`typedef struct Foo Foo` lets C code pretend `struct` is implicit);
  - `unsigned _BitInt()` because aligning it vertically with much shorter type names is awful, and/or you need to type it often:
    ```c
    // this is kind of ok. regrettably.
    typedef unsigned _BitInt(48) u48_type;
    ```
- Structs and unions with a tag but no type alias are great despite being somewhat more verbose to type:
  - they make it possible to have RAII initializer functions that return by value and are named like the tag;
  - an alias can be a `struct`, `union`, or a number of other things, but a tag tells the type semantics immediately.
- Function type aliases should not be pointer type aliases. While the former has its use cases, the latter is
  - unnecessary because appending an asterisk makes it a function pointer just like with any other type alias;
  - harmful because all pointer type aliases obscure pointer-ness, which is fundamentally unacceptable;
  - limited because a function type can forward declare a function but a function pointer type can not.

### C++

- Prefer C++23 and later. Follow the type conventions of modern `std` interfaces. Make full use of `constexpr` STL types.
- If there are upfront costs in converting _some_ Qt to idiomatic C++, see if it's really a "local minimum" obscuring a deeper valley of decreased energy expenditure, reachable by going the whole way.
- Avoid dynamic dispatch in C++. Ten extra milliseconds at runtime is worse than ten minutes lost during build.

### Qt

- Qt is mainly for the UI, and even there only when necessary. Prefer standard, modern C++23 (and later) unless it incurs a heavy penalty.
- Using Qt for something does _not_ mean you must _only_ use Qt for that thing. Use standard C++ even in UI code.
- Minimize the need to type-convert between `std` and Qt types; err on the side of `std`. Convert to C++ as early as possible. For example, if the last of a chain of function calls writes to a socket, don't carry a Qt type all the way through.

### Readability

- Tabs indent, spaces align (C, C++, CMake): continuation lines of a declaration or argument list are tab-indented to the statement's level, then space-padded into column alignment. Do not "fix" space-aligned continuations into tabs.
- Place angle-bracketed include statements above double-quoted ones. Separate these into groups by placing an empty line in between the two.
- A separating empty line is only mandatory between `<>` and `""` groups, but it is also allowed within these two groups at your discretion.
- Sort grouped includes - those not separated by empty lines - by header name in the C locale. Ignore whitespace so that e.g. `#include` and `# include` sort equal.
- C core uses Doxygen `/** */` for API documentation (file headers, structs, public functions); internal implementation notes and test commentary are plain `/* */`. C++ uses `//` header-comment style.

### Repository structure

- Untracked files (check `git status`) are arbitrary local temporaries, not part of the repo — only work with repository content.
- `.gitignore` must stay sorted in the C locale (`LC_ALL=C sort`) — see AGENTS.md for the pre-commit check to run after `git add`.
