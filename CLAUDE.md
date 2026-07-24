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

Deps: Qt6 (Widgets, Network), FFmpeg dev libraries (libavformat/avcodec/avutil/swscale), C++23 and C23 compilers, mpv at runtime. All executables land in `build/` (not `build/src/`). `cmake/compiler.cmake` injects LTO flags per compiler via `CMAKE_USER_MAKE_RULES_OVERRIDE`.

## What this is

`srtview` is a Qt6 subtitle-transcript viewer that remote-controls mpv. The full user-facing spec — keybindings, env vars (`SRTVIEW_MPV_ARGS`, `SRTVIEW_DEBUG`), WSLg audio workarounds — lives in the header comment of `src/main.cpp`. `srtjump/srtjump` is a standalone bash sibling (less/Kate integration) that predates the GUI and shares the same player instances.

## Architecture

Every source file opens with a comment stating its role and constraints; read those first — they are the authoritative per-file docs.

**Layering (mixed-language, deliberate):**
- **Plain C core** (`fundo.c/h`, `list.c/h`, `cutil.h`, C23): the undo *tree* with persistent side branches. Opaque byte payloads, exact-content identity, byte-identical actions re-adopt existing branches instead of forking. `*_priv.h` headers hold internals shared with tests. No Qt, no C++.
- **Qt-free C++** (`srt.cpp/hpp`, `topics.cpp/hpp`, C++23): SRT cue model, encoding normalization (UTF-8/UTF-16/Windows-1252), and a CRTP push parser (`srt::parser<Derived>` emits `on_cue()` statically dispatched); the topic-file model — the hand-written corpus format of videos plus named, composable regexes (`\{name:}` references) whose grammar is documented in the `topics.hpp` header comment. The UI converts to Qt types at its own boundary.
- **Qt layer**: everything else.

Tests mirror the layering: `tests/parse_test.cpp` and `tests/topics_test.cpp` link their modules with no Qt at all; `tests/fundo_test.c` links the C core with no C++. Keep those modules dependency-free or the test targets break.

**Component/controller split in the Qt layer:**
- `concepts.hpp` defines host contracts (`playback_host`, `search_host`, `mpv_observer`). Components (`srtedit`, `searchbar`, `mpvlink`) are thin header-only templates constrained by these concepts and never name a concrete host; each has a non-template base (`srt_view_base`, `search_bar_base`, `mpv_link_base`) compiled once in the .cpp, which is what controllers hold.
- Controllers/mediators: `PlaybackCtl` (`playback.cpp`) executes transport verbs against mpv; `SearchCtl` (`search.cpp`) owns pattern semantics and match navigation.
- `Trail` (`trail.cpp/hpp`) is the C++ facade over the fundo C core: undo/redo breadcrumbs of search/jump/seek steps. Each node stores a *state*: a bitmask of the facets the step touched (search text, text cursor, video position) plus each touched facet's after-value — one step can combine facets ("jumped in text and video"). Undo resolves departed facets to their nearest recorded ancestor values; encoding is deterministic so identical steps trigger branch adoption. Search hits wrapping the document form fundo travel rings: a hop byte-identical to its ring neighbor travels instead of growing the tree, per-node pass counters linearize multi-lap travel, and on a ring backward search coincides with undo (forward with redo). Its "applying" latch suppresses recording while a step is replayed.
- `MainWin` (`mainwin.cpp`) is the composition root — owns components and controllers, wires everything; nothing depends on it except `main` and `selftest`. It also owns the corpus: a loaded topic file becomes the playlist (Videos menu) and the id→path registry cross-video steps resolve through. The trail's video facet carries the discovery identity (`discovery::id_for_video`, the socket-naming hash), so undo/redo switch videos when a step's id differs from the current one.
- `Prefs` (QSettings-backed) persists last directory, recent files, search history.

**Export:** `exporter.cpp/hpp` turns the corpus into per-*grouping* directories driven by `topics::export_plan()`: only top-level topics (referenced by no other) export, each with a Markdown digest (pattern, per-video sections, matched cue ± neighbors, three frame links per hit) plus copied picks. Components earn their own digest through acknowledgment capture parens in a top-level topic (grammar in `topics.hpp`); their hits are attributed via named groups unioned over every match in a cue, and their frames are **relative symlinks** into the parent grouping's copies — one PNG on disk however many digests cite it. Deterministic build artifact: File → "Export frames" writes what the cache has into `<corpus-stem>-export/` beside the topic file, enqueues missing hits on the grabber, and MainWin re-runs the writer on `grabsIdle()` until complete (or until a pass makes no progress).

**Frame grabs:** `Grabber` (`grabber.cpp/hpp`) lives on its own worker thread — decoding, PNG encoding, thumbnail diffs and picks bookkeeping never touch the UI thread; mutating calls marshal onto the worker, `picksFor`/`framePath` read under a lock, and listener notifications are queued into the listener's thread. It shadows the session: every video jump enqueues its timestamp, and three picks per hit — the hit frame plus one from the different-looking content on either side, found by bisecting for content-change boundaries — land in `$XDG_CACHE_HOME/srtview/frames/<video id>/`, with `picks.txt` as both manifest and cross-session dedupe record. Extraction is an in-process libav decode context: `decoder.cpp/hpp` (`media::decoder`, std C++23 + FFmpeg, **no Qt** — `decoderq.hpp` is the concrete inline Qt shim) keeps the demuxer open per video, decodes bisection probes straight into 64×36 grayscale compare thumbs that never touch a PNG encoder or the disk (~35 ms/probe vs ~200 ms per round-trip of the former mpv shadow player), and produces a full RGB frame only for a pick about to be encoded. A hit inside an already-bisected segment reuses that segment's boundaries (content-compared), so a cluster of hits on one slide costs one encoded frame each.

**mpv integration:** `mpvclient.hpp/cpp` is the shared IPC client both `mpvlink` and `grabber` build on (CRTP event dispatch over a compiled-once base): commands go out as single raw input.conf lines — one per action, explicit flush per send — while mpv's JSON event lines come back for observation and sequencing. The viewing player is **one persistent instance whose internal playlist mirrors the app's in set and order**: switching videos is `playlist-play-index` inside the same window, never a respawn; the observed `path` property routes mpv-side playlist navigation (its own `<`/`>` keys) back into the app (`onMpvIndex` → `video_sync`), and `file-loaded` attaches the entry's subtitles via `sub-add` (per-entry `loadfile` options changed signature across mpv versions) and fires deferred seeks. Sockets from the `discovery` class (`discovery.cpp/hpp` — pure C++23/POSIX with cached identities and prefixes; one instance owned by MainWin), `$XDG_RUNTIME_DIR/srtjump/<blake2b-256(realpath)[:16]>.sock`: a corpus playlist claims the topic file's own hash; a directly opened single video keeps the per-video scheme byte-for-byte shared with the `srtjump` script (`cksum -a blake2b -l 256 --untagged`). Adopting an already-running instance preserves its playback position, resyncing the playlist around the playing entry (`playlist-clear` + appends, no reload). Bring-up never blocks the UI thread: connect/spawn runs off a retry timer and commands queue until the on-connect setup (observation, resync) has gone out. `mpvlink` also watches player health and respawns a wedged mpv at the last observed position/pause state.

## Bests and pests

`bests/` and `pests/` complement `tests/`; neither is built, and nothing in them is meant to compile. Each file is one very short snippet with a project-style name and the correct suffix, ingested whole at a glance. A best condenses one desirable habit — a style, solution, or trait to reproduce. A pest condenses the opposite: code that could pass tests yet must never be written here (first pair: `header_guards.h`, traditional guard vs. `#pragma once`). Both directories are comment-free zones — the snippets are ideas without natural language, and one that needs an explanatory comment fails at its only job. The sole exception is a file whose demonstrated subject is itself a comment-writing approach.

## Conventions

### Approach and attitude

- It's better to delete than to add LoC, within reason. If it doesn't hurt performance or grow binary size, a red diff is better than a green one.
- Elegance and performance often correlate. Trust your sense of beauty but verify your sense of trust.
- All code is bad, everywhere, always. Never trust a single line you read or write.
- All code is bad, everywhere, always; coders are not. Finding a bug is a happy and inclusive event.
- Undefined Behavior is a wrathful cosmic force. "It's not UB if it works" is exactly what your bugs want you to believe.
- Use compile-time features when reasonable; template metaprogramming in C++, and `_Generic`, `typeof`, `sizeof`, etc. in C.
- Use some time during every programming task to think of optimizations and code deletions that can be done naturally on the side.

### C

- Prefer standard, modern C23 and later. Claims about a "true C" of years past are the "golden age" delusion of C programming. The C language is by definition what the current standard says.
- Use `int` only if needed. "`int` by default" coding often necessitates more integer conversions which translate to costly sign extension instructions.
- Trace your call chains to see if you're e.g. calling `strlen()` effectively multiple times over the same input. If you need a C string's length more than once, measure it once and pass it down.
- Stay aware of the program flow. Are you repeating some task more than once when you could just use a variable? Fix it.
- Use helper macros when it's justified and reasonable, but undefine macros that don't need to be exposed ASAP. Typically this means defining something above a function and undefining it below.
- If you call `strlen()` inside a loop condition or on a string literal, you must spend a full day downtown pushing a baby stroller full of boiled cabbage.
- Exposed C APIs (headers consumed across the C/C++ boundary, e.g. `fundo.h`) use `int` for booleans, documented as 1/0 in the Doxygen comment — no `bool`/`_Bool` in signatures. C and C++ `bool` match only by platform-ABI convention, and the C core must stay ABI-safe under pre-C23 builds with polyfill macros. `bool` is fine in internal headers and header-inline helpers (`cutil.h`), which are never linked across the boundary.

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
- Place angle bracketed include statements above double-quoted ones. Separate these into groups by placing an empty line in between the two.
- A separating empty line is only mandatory between `<>` and `""` groups, but it is also allowed within these two groups at your discretion.
- Sort grouped includes - those not separated by empty lines - by header name in the C locale. Ignore whitespace so that e.g. `#include` and `# include` sort equal.
- C core uses Doxygen `/** */` for API documentation (file headers, structs, public functions); internal implementation notes and test commentary are plain `/* */`. C++ uses `//` header-comment style.

### Repository structure

- Untracked files (check `git status`) are arbitrary local temporaries, not part of the repo — only work with repository content.
- `.gitignore` must stay sorted in the C locale (`LC_ALL=C sort`) — see AGENTS.md for the pre-commit check to run after `git add`.
