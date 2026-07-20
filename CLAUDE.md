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
- `MainWin` (`mainwin.cpp`) is the composition root — owns components and controllers, wires everything; nothing depends on it except `main` and `selftest`. It also owns the corpus: a loaded topic file becomes the playlist (Videos menu) and the id→path registry cross-video steps resolve through. The trail's video facet carries the discovery identity (`idForVideo`, the socket-naming hash), so undo/redo switch videos when a step's id differs from the current one.
- `Prefs` (QSettings-backed) persists last directory, recent files, search history.

**Export:** `exporter.cpp/hpp` turns the corpus into per-*grouping* directories driven by `topics::export_plan()`: only top-level topics (referenced by no other) export, each with a Markdown digest (pattern, per-video sections, matched cue ± neighbors, three frame links per hit) plus copied picks. Components earn their own digest through acknowledgment capture parens in a top-level topic (grammar in `topics.hpp`); their hits are attributed via named groups unioned over every match in a cue, and their frames are **relative symlinks** into the parent grouping's copies — one PNG on disk however many digests cite it. Deterministic build artifact: File → "Export frames" writes what the cache has into `<corpus-stem>-export/` beside the topic file, enqueues missing hits on the grabber, and MainWin re-runs the writer on `grabsIdle()` until complete (or until a pass makes no progress).

**Frame grabs:** `Grabber` (`grabber.cpp/hpp`) lives on its own worker thread — decoding, PNG encoding, thumbnail diffs and picks bookkeeping never touch the UI thread; mutating calls marshal onto the worker, `picksFor`/`framePath` read under a lock, and listener notifications are queued into the listener's thread. It shadows the session: every video jump enqueues its timestamp, and three picks per hit — the hit frame plus one from the different-looking content on either side, found by bisecting for content-change boundaries — land in `$XDG_CACHE_HOME/srtview/frames/<video id>/`, with `picks.txt` as both manifest and cross-session dedupe record. Extraction is an in-process libav decode context: `decoder.cpp/hpp` (`media::decoder`, std C++23 + FFmpeg, **no Qt** — `decoderq.hpp` is the concrete inline Qt shim) keeps the demuxer open per video, decodes bisection probes straight into 64×36 grayscale compare thumbs that never touch a PNG encoder or the disk (~35 ms/probe vs ~200 ms per round-trip of the former mpv shadow player), and produces a full RGB frame only for a pick about to be encoded. A hit inside an already-bisected segment reuses that segment's boundaries (content-compared), so a cluster of hits on one slide costs one encoded frame each.

**mpv integration:** `mpvclient.hpp/cpp` is the shared IPC client both `mpvlink` and `grabber` build on (CRTP event dispatch over a compiled-once base): commands go out as single raw input.conf lines — one per action, explicit flush per send — while mpv's JSON event lines come back for observation and sequencing. The viewing player is **one persistent instance whose internal playlist mirrors the app's in set and order**: switching videos is `playlist-play-index` inside the same window, never a respawn; the observed `path` property routes mpv-side playlist navigation (its own `<`/`>` keys) back into the app (`onMpvIndex` → `video_sync`), and `file-loaded` attaches the entry's subtitles via `sub-add` (per-entry `loadfile` options changed signature across mpv versions) and fires deferred seeks. Sockets from `discovery.cpp/hpp`, `$XDG_RUNTIME_DIR/srtjump/<sha256(realpath)[:16]>.sock`: a corpus playlist claims the topic file's own hash; a directly opened single video keeps the per-video scheme byte-for-byte shared with the `srtjump` script. Adopting an already-running instance preserves its playback position, resyncing the playlist around the playing entry (`playlist-clear` + appends, no reload). Bring-up never blocks the UI thread: connect/spawn runs off a retry timer and commands queue until the on-connect setup (observation, resync) has gone out. `mpvlink` also watches player health and respawns a wedged mpv at the last observed position/pause state.

## Conventions

- Tabs indent, spaces align (C, C++, CMake): continuation lines of a declaration or argument list are tab-indented to the statement's level, then space-padded into column alignment. Do not "fix" space-aligned continuations into tabs.
- Angle bracketed (system) include statements come first, then the double-quoted (local) include statements. Don't worry about this in particular, though, just fix these opportunistically and make sure to do it this way in new files.
- C core uses Doxygen `/** */` for API documentation (file headers, structs, public functions); internal implementation notes and test commentary are plain `/* */`. C++ uses `//` header-comment style.
- Exposed C APIs (headers consumed across the C/C++ boundary, e.g. `fundo.h`) use `int` for booleans, documented as 1/0 in the Doxygen comment — no `bool`/`_Bool` in signatures. C and C++ `bool` match only by platform-ABI convention, and the C core must stay ABI-safe under pre-C23 builds with polyfill macros. `bool` is fine in internal headers and header-inline helpers (`cutil.h`), which are never linked across the boundary.
- Don't use `int` everywhere by default, or without a good reason each time. You'll know that this is a problem if you see integrals being cast from `int` to `std:size_t` (or similar) and back at every turn. If you don't need negative values, don't pick a signed type, and follow the conventions of modern `std` (and C23) interfaces.
- Prefer standard, modern, non-Qt C++ as much as possible. Make full use of C++23 and `constexpr` STL types. The only situation where Qt is preferred is one where not doing so would incur a heavy performance penalty.
- Prefer standard, modern C23 and ignore misinformed ideas about "portable" C code. The latest standard is *the* actual C language.
- When writing C code, treat Undefined Behavior like a jealous spirit you don't want to piss off. In C jargon "it works, can't be UB" means "I added Heisenbugs because the old ones need company".
- Given a substantial amount of code, high-quality `std`-only C++ often performs better than an equivalent Qt implementation. The apparent cost of converting a small block of Qt to standard C++ is sometimes a local minimum; there may well be a deeper valley of decreased energy expenditure behind an immediate upwards slope.
- When a conversion between an `std` and a Qt type is necessary, try to do it as close to the Qt side as possible. For example, if the final function call in a chain of calls writes data to a socket, don't carry a Qt type all the way through but shed it early on.
- On average it's always better to delete code than add more. Of course that's not an absolute truth, but keep it in mind. As long as performance doesn't suffer and output binary size doesn't explode, a red diff is better than a green one.
- Avoid dynamic dispatch in C++ like a moderately inconvenient plague. Ten extra milliseconds at runtime is worse than ten extra minutes during build.
- `.gitignore` must stay sorted in the C locale (`LC_ALL=C sort`) — see AGENTS.md for the pre-commit check to run after `git add`.
- Untracked files (check `git status`) are arbitrary local temporaries, not part of the repo — only work with repository content.
