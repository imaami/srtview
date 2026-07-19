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

Deps: Qt6 (Widgets, Network), C++20 and C23 compilers, mpv at runtime. All executables land in `build/` (not `build/src/`). `cmake/compiler.cmake` injects LTO flags per compiler via `CMAKE_USER_MAKE_RULES_OVERRIDE`.

## What this is

`srtview` is a Qt6 subtitle-transcript viewer that remote-controls mpv. The full user-facing spec — keybindings, env vars (`SRTVIEW_MPV_ARGS`, `SRTVIEW_DEBUG`), WSLg audio workarounds — lives in the header comment of `src/main.cpp`. `srtjump/srtjump` is a standalone bash sibling (less/Kate integration) that predates the GUI and shares the same player instances.

## Architecture

Every source file opens with a comment stating its role and constraints; read those first — they are the authoritative per-file docs.

**Layering (mixed-language, deliberate):**
- **Plain C core** (`fundo.c/h`, `list.c/h`, `cutil.h`, C23): the undo *tree* with persistent side branches. Opaque byte payloads, exact-content identity, byte-identical actions re-adopt existing branches instead of forking. `*_priv.h` headers hold internals shared with tests. No Qt, no C++.
- **Qt-free C++** (`srt.cpp/hpp`, C++20): SRT cue model, encoding normalization (UTF-8/UTF-16/Windows-1252), and a CRTP push parser (`srt::parser<Derived>` emits `on_cue()` statically dispatched). The UI converts to Qt types at its own boundary.
- **Qt layer**: everything else.

Tests mirror the layering: `tests/parse_test.cpp` links `srt.cpp` with no Qt at all; `tests/fundo_test.c` links the C core with no C++. Keep those modules dependency-free or the test targets break.

**Component/controller split in the Qt layer:**
- `concepts.hpp` defines host contracts (`playback_host`, `search_host`, `mpv_observer`). Components (`srtedit`, `searchbar`, `mpvlink`) are thin header-only templates constrained by these concepts and never name a concrete host; each has a non-template base (`srt_view_base`, `search_bar_base`, `mpv_link_base`) compiled once in the .cpp, which is what controllers hold.
- Controllers/mediators: `PlaybackCtl` (`playback.cpp`) executes transport verbs against mpv; `SearchCtl` (`search.cpp`) owns pattern semantics and match navigation.
- `Trail` (`trail.cpp/hpp`) is the C++ facade over the fundo C core: undo/redo breadcrumbs of search/jump/seek steps, each carrying before- and after-state, encoded deterministically so identical transitions trigger branch adoption. Its "applying" latch suppresses recording while a step is replayed.
- `MainWin` (`mainwin.cpp`) is the composition root — owns components and controllers, wires everything; nothing depends on it except `main` and `selftest`.
- `Prefs` (QSettings-backed) persists last directory, recent files, search history.

**mpv integration:** `discovery.cpp/hpp` derives a deterministic per-video socket, `$XDG_RUNTIME_DIR/srtjump/<sha256(realpath)[:16]>.sock` — byte-for-byte the same scheme as the `srtjump` script, so srtview, srtjump, Kate and ad-hoc scripts all drive one player; live instances are reused and left running on exit. `mpvlink` owns the process and IPC: commands go out as single raw input.conf lines (not JSON), one line per action, with an explicit flush per send. It also watches player health and respawns a wedged mpv at the last observed position/pause state.

## Conventions

- Tabs indent, spaces align (C, C++, CMake): continuation lines of a declaration or argument list are tab-indented to the statement's level, then space-padded into column alignment. Do not "fix" space-aligned continuations into tabs.
- C core uses Doxygen `/** */` for API documentation (file headers, structs, public functions); internal implementation notes and test commentary are plain `/* */`. C++ uses `//` header-comment style.
- Exposed C APIs (headers consumed across the C/C++ boundary, e.g. `fundo.h`) use `int` for booleans, documented as 1/0 in the Doxygen comment — no `bool`/`_Bool` in signatures. C and C++ `bool` match only by platform-ABI convention, and the C core must stay ABI-safe under pre-C23 builds with polyfill macros. `bool` is fine in internal headers and header-inline helpers (`cutil.h`), which are never linked across the boundary.
- `.gitignore` must stay sorted in the C locale (`LC_ALL=C sort`) — see AGENTS.md for the pre-commit check to run after `git add`.
- Untracked files (check `git status`) are arbitrary local temporaries, not part of the repo — only work with repository content.
