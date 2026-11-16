# Architecture

How this repository is meant to be built. Where the code differs today is listed under
"Known deviations". Change this file in the PR that changes the structure.

## What it does

Adds what DaVinci Resolve Studio lacks on Linux: VST2 and VST3 plug-ins, AAC sound and
Nikon N-RAW clips. A launcher script starts Resolve with a library loaded into its process
(`LD_PRELOAD`); the library changes Resolve in memory only, never its files on disk.

## Building blocks

| Path | Its one task |
|---|---|
| `resolve-linux-vst` | The launcher: links the plug-ins into the folder Resolve scans (a VST3 plug-in as a folder with `bundle` and `class`), writes their categories, starts Resolve with the library and hands it the Wine DPI file, resets the Wine DPI afterwards, stops processes yabridge plug-in groups left behind (before Resolve, after it and on `restore`). Calls the built tools `vst3-classes` and `resolve-check`. |
| `src/resolve_linux_vst.cpp` | The VST2 loader: loads a plug-in for Resolve. |
| `src/wine_base.h` | Wine DPI: the decision which base DPI a Wine session has and whether to reset first, pure, so `tests/` can run it. |
| `src/wine_dpi.cpp`, `src/wine_dpi.h` | Wine DPI: sets the DPI of a Windows plug-in's Wine prefix before it loads and tells the loader how far Wine scales it. |
| `src/loader.h` | What the loader file offers its parts: settings per plug-in (`plugins.conf`), scale factor. |
| `src/process.cpp`, `src/process.h` | Which program the library runs in (read once); the program names, and `editor-test` as the one exception that counts as Resolve for the Wine DPI. |
| `src/vst3*.cpp`, `src/vst3*.h` | The VST3 adapter: shows a VST3 plug-in to Resolve as a VST2 plug-in. `vst3.cpp` builds it and runs audio, `vst3_dispatch.cpp` answers Resolve's commands (editor, state, speakers), `vst3_host.cpp` the host objects and event loop a plug-in asks for, `vst3_module.cpp` opens a bundle and lists its classes, `vst3_iids.cpp` defines the interface IDs once. |
| `src/aac.cpp` | AAC: gives Resolve an AAC decoder from a second, small FFmpeg library. |
| `src/nev.cpp` | N-RAW: shows Resolve every `.NEV` file as `.R3D`. |
| `src/x11.cpp` | Drops window sizes of 0 that would make a plug-in abort Resolve. |
| `src/log.cpp`, `src/log.h` | The log: one function that writes a line with time and process; the caller sets the file. |
| `src/patch.cpp`, `src/patch.h` | Writes a jump into Resolve's code and knows the layout of the C++ tables loader and AAC read; no feature. |
| `src/sites.cpp`, `src/sites.h` | Finds the places in Resolve's code by byte patterns. Knows Resolve's binary, no feature. |
| `src/exports.map` | The list of functions the library replaces in Resolve. What is not listed here has no effect. |
| `src/vst2.h` | The VST2 interface, shared by loader and adapter: structures, command numbers, the adapter's entry points. |
| `tools/*.cpp` | Programs built from `src/`: `resolve-check` (do the patterns still match?), `vst3-classes` (list the plug-ins of a bundle), `editor-test` (open a plug-in window without Resolve). Also the patch the AAC decoder's FFmpeg is built with. |
| `tests/` | Tests that run without Resolve (`make test`: pattern search, program name, writing a jump, Wine DPI decisions, launcher cleanup) and the local plug-in test `wine-dpi.sh` (`make plugin-test`). |
| `install.sh`, `start.sh`, `Makefile` | Install, start from the source folder, build, release tarball (`make dist`). |
| `third_party/vst3_pluginterfaces` | The VST3 interface headers (submodule), unchanged. |
| `docs/`, `scripts/`, `tools/structure-check.py` | Landing page and internals; release notes; the structure check (synced copy). |

## Layers

```
 resolve-linux-vst (launcher) ─► Resolve, with libresolve-linux-vst.so preloaded
        │ calls the built tools               │
        ▼ vst3-classes, resolve-check         │
        ┌───────────────┬─────────────┬───────┴─────┬─────────────┐
        ▼               ▼             ▼             ▼             ▼
   VST2 loader ─► VST3 adapter       AAC          N-RAW          X11        features
        │        (via vst2.h)         │
        ▼                             │
   plugins.conf, editor size,         │                          the loader's own parts
   Wine DPI                           │
        └──────────────┬──────────────┘
                       ▼
       shared: sites (find places), writing into Resolve's code,
               log, "which program is this?"                              below all

 a sixth feature beside these five: the changes to Resolve's scanner and number format

 tools/, tests/ ─► src/        (never the other way round)
 launcher ◄─► library: two agreed formats (VST3 entry folder, Wine DPI file)
```

- Each feature has its own file and knows no other feature. The one exception: the VST2
  loader hands a VST3 entry to the adapter, through the entry points in `vst2.h`.
- Each feature starts itself, through the functions it replaces (`src/exports.map`) or
  its own start-up hook. No feature starts another; the VST3 adapter, which the loader
  calls, is the same exception.
- The loader file only loads plug-ins. What it needs besides lives in files of their own
  below it: the settings per plug-in (`plugins.conf`), the editor size, the Wine DPI. The
  changes to Resolve's scanner (categories) and to its number format at start are a
  feature of their own.
- What two features need lives below them, once, in a file that is no feature: the log,
  the question which program the library runs in, writing a jump into Resolve's code.
- Only `sites.cpp` holds byte patterns. A feature asks it for its places and never
  searches or uses a fixed address itself. A pattern that no longer matches switches off
  its own feature only and logs why.
- `sites.cpp` builds without the rest of the library, so `resolve-check` and the tests
  run without Resolve.
- The numbers of the VST2 interface stand once, in `vst2.h`.
- Launcher and library agree on two formats and nothing else: the folder of a VST3 entry
  (written by the launcher, read by the adapter, described in `vst2.h`) and the file
  with the Wine DPI to restore (its place and format defined on one side and handed to
  the other).
- `src/` includes nothing from `tools/` or `tests/`.
- `third_party/` is never edited.

## Where new things go

| Change | Touch |
|---|---|
| A new feature inside Resolve (another codec, another fix) | A new `src/<feature>.cpp`, the functions it replaces in `src/exports.map`, `SOURCES` in the `Makefile`, `docs/HOW-IT-WORKS.md`, its switch in the README |
| A new place to find in Resolve, or a pattern after a Resolve update | `src/sites.cpp`, `src/sites.h`, `tests/sites-test.cpp`, `tools/resolve-check.cpp` |
| A new VST2 command Resolve sends | `src/vst2.h` (the number), then loader or adapter |
| Something only VST3 plug-ins need | `src/vst3*.cpp` only |
| A new launcher command | `resolve-linux-vst`, README |
| A new setting per plug-in (`plugins.conf`) | The file that reads `plugins.conf` (today part of the loader), README |
| Logic that can run without Resolve | Its own function, with a test in `tests/` and a line in `make test` |
| A release | `RELEASE.md`, `CHANGELOG.md`, landing page |

## Known deviations

- The loader file has five tasks: besides loading also opening the log file, `plugins.conf`,
  the editor size and the changes to the scanner (`AUDIT.md`, finding 9). The Wine DPI has its own file
  but reaches back into the loader file for `plugins.conf` and scale factor
  (`src/loader.h`).
- No automatic tests for loading plug-ins, reading `plugins.conf`, the editor size, the VST3
  adapter and its saved state, AAC, N-RAW and the launcher's categories; only the plug-in test
  checks the Wine DPI with real plug-ins (`AUDIT.md`, finding 8).
