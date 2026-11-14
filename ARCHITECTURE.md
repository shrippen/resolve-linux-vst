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
| `resolve-linux-vst` | The launcher: links the plug-ins into the folder Resolve scans, writes their categories, starts Resolve with the library, resets the Wine DPI afterwards. |
| `src/resolve_linux_vst.cpp` | The VST2 loader: loads a plug-in for Resolve and starts the other features when the library is loaded. |
| `src/vst3.cpp` | The VST3 adapter: shows a VST3 plug-in to Resolve as a VST2 plug-in. |
| `src/aac.cpp` | AAC: gives Resolve an AAC decoder from a second, small FFmpeg library. |
| `src/nev.cpp` | N-RAW: shows Resolve every `.NEV` file as `.R3D`. |
| `src/x11.cpp` | Drops window sizes of 0 that would make a plug-in abort Resolve. |
| `src/sites.cpp`, `src/sites.h` | Finds the places in Resolve's code by byte patterns. Knows Resolve's binary, no feature. |
| `src/vst2.h` | The VST2 interface, shared by loader and adapter: structures, command numbers, the adapter's entry points. |
| `tools/*.cpp` | Programs built from `src/`: `resolve-check` (do the patterns still match?), `vst3-classes` (list the plug-ins of a bundle), `editor-test` (open a plug-in window without Resolve). |
| `tests/` | Tests that run without Resolve. |
| `install.sh`, `start.sh`, `Makefile` | Install, start from the source folder, build, release tarball (`make dist`). |
| `third_party/vst3_pluginterfaces` | The VST3 interface headers (submodule), unchanged. |
| `docs/`, `scripts/`, `tools/structure-check.py` | Landing page and internals; release notes; the structure check (synced copy). |

## Layers

```
 resolve-linux-vst (launcher) ─► Resolve, with libresolve-linux-vst.so preloaded
                                              │
        ┌───────────────┬─────────────┬───────┴─────┬─────────────┐
        ▼               ▼             ▼             ▼             ▼
   VST2 loader ─► VST3 adapter       AAC          N-RAW          X11        features
        │        (via vst2.h)         │
        └──────────────┬──────────────┘
                       ▼
       shared: sites (find places), writing into Resolve's code,
               log, "which program is this?"                              below all

 tools/, tests/ ─► src/        (never the other way round)
```

- Each feature has its own file and knows no other feature. The one exception: the VST2
  loader hands a VST3 entry to the adapter, through the entry points in `vst2.h`.
- What two features need lives below them, once, in a file that is no feature: the log,
  the question which program the library runs in, writing a jump into Resolve's code.
- Only `sites.cpp` holds byte patterns. A feature asks it for its places and never
  searches or uses a fixed address itself. A pattern that no longer matches switches off
  its own feature only and logs why.
- `sites.cpp` builds without the rest of the library, so `resolve-check` and the tests
  run without Resolve.
- The numbers of the VST2 interface stand once, in `vst2.h`.
- Launcher and library share one thing, the file with the Wine DPI to restore; its place
  and format are defined on one side and handed to the other.
- `src/` includes nothing from `tools/` or `tests/`.
- `third_party/` is never edited.

## Where new things go

| Change | Touch |
|---|---|
| A new feature inside Resolve (another codec, another fix) | A new `src/<feature>.cpp`, `SOURCES` in the `Makefile`, its switch in the README |
| A new place to find in Resolve, or a pattern after a Resolve update | `src/sites.cpp`, `src/sites.h`, `tests/sites-test.cpp`, `tools/resolve-check.cpp` |
| A new VST2 command Resolve sends | `src/vst2.h` (the number), then loader or adapter |
| Something only VST3 plug-ins need | `src/vst3.cpp` only |
| A new launcher command | `resolve-linux-vst`, README |
| A new setting per plug-in (`plugins.conf`) | Where the file is read (today in the loader), README |
| Logic that can run without Resolve | Its own function, with a test in `tests/` and a line in `make test` |
| A release | `RELEASE.md`, `CHANGELOG.md`, landing page |

## Known deviations

- The log lives in the VST2 loader and is declared in `vst2.h`; N-RAW and X11 include the
  VST2 interface for it (`AUDIT.md`, finding 1).
- Loader and adapter each keep their own list of the VST2 numbers (`AUDIT.md`, finding 2).
- "Is this Resolve?" is built three times (`AUDIT.md`, finding 3).
- Writing a jump into Resolve's code is built twice, in loader and AAC
  (`AUDIT.md`, finding 5).
- Launcher and library keep place and format of the Wine DPI file equal by hand
  (`AUDIT.md`, finding 6).
- The loader file has six tasks: besides loading also the log, `plugins.conf`, the editor
  size, the Wine DPI and the changes to the scanner (`AUDIT.md`, finding 8).
- `src/vst3.cpp` holds host objects, event loop, module loading and adapter in one file of
  1750 lines (`AUDIT.md`, finding 9).
- Only the pattern search has tests (`AUDIT.md`, finding 7).
