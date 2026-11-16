# Review guide

Every PR is reviewed before it is merged (rule in `RULES.md` and the skill `pr-review`): by PR-Agent
(owner, 22.10.2026); a new Claude agent checks the **Wichtig** of earlier rounds, and reviews
when PR-Agent fails. The reviewer knows nothing of the session that wrote the change: it gets
the PR's intent, its diff, the repository and this file. The owner does not read code.

## What failure looks like here

resolve-linux-vst is a preload library plus a launcher that add VST2/VST3 plug-ins, AAC and
Nikon N-RAW to DaVinci Resolve Studio on Linux. It patches Resolve in memory, so a bug that
matters crashes or silences Resolve, or damages the user's files:

- **Resolve crashes or hangs:** a plug-in or Resolve call with an unchecked pointer, struct
  offset or size (window size 0, the MiniMeters crash); a patch written to Resolve's code
  without its pattern verified; a lock or callback order that freezes the GUI (the Neoverb
  `restartComponent` freeze).
- **A hook misses silently:** a byte pattern in `src/sites.cpp` that matches the wrong place or
  twice; a pattern that no longer matches and switches off more than its own feature, or
  without a log line; `resolve-linux-vst check` and `make test` out of step with `sites.cpp`.
- **Audio wrong:** a VST3 adapter that mishandles buses, channel counts, latency, state or
  automation; the AAC decoder or the render fix giving silence, wrong samples, or a file
  without a correct AAC-LC description.
- **Harms user files:** Resolve's files changed on disk (never allowed); plug-in files
  touched beyond the link folder; `.nev` files written to; the Wine DPI not restored on exit.
- **Breaks the launcher:** `resolve-linux-vst`, `install.sh` or `start.sh` failing on a path
  with spaces, an unset variable or a missing optional tool.
- **Lies:** `README.md`, `docs/HOW-IT-WORKS.md`, `CHANGELOG.md` or comments the code
  contradicts; an environment variable or `plugins.conf` key missing in the README.

## What to check in particular

- Code that runs in Resolve's audio thread (`process`, `processReplacing`): no allocation,
  lock or logging.
- Every new byte pattern or offset: where it is checked, what happens when it fails, that
  only `src/sites.cpp` holds it.
- A new function that replaces a symbol by LD_PRELOAD is in `src/exports.map`.
- Layers: as in `ARCHITECTURE.md`.
- Bug fixes: a test in `tests/` that failed before the fix and runs in `make test`, and the
  cause removed, not one case hidden. What only Resolve can show (hooks, editors, AAC render)
  is named in the PR as checked by hand, with the Resolve version.
- Repeated knowledge (`ARCHITECTURE.md`, "Known deviations"): the C++ table layout in
  `patch.h`, `aac.cpp` and `scanner.cpp`; the three
  meanings of "is this Resolve?". A change that fixed one place and left the other.

## What the owner confirms here

- Changes in `.gitea/` and `.github/` (`ci.yml`: its `release` job builds the archive that
  `install.sh` downloads).
- A release or tag (`RELEASE.md`), and a new public page, site or package.

## Skip

- `third_party/` (submodule), `tools/structure-check.py` (synced copy), images, style.

## How to report

- Every finding names file and line and the concrete input or state that
  goes wrong. No guess without evidence; if unsure, say so.
- Each finding gets one label: **Wichtig** (a bug from the list above or a
  finding `RULES.md` names as Wichtig; must be fixed or answered before the
  merge), **Kleinigkeit** (optional, at most five) or **Bestand** (the bug
  was there before this PR; not blocking).
- The report starts with the count, e.g. "2 Wichtig, 1 Kleinigkeit" (the
  session puts the heading above it). The count of **Wichtig** holds every
  one still open, also from an earlier round, e.g. "1 Wichtig (offen aus
  Runde 1)". Only when none is open it says plainly: "Nichts zu beheben."
- In a later round: say for each open **Wichtig** of the last round whether
  it is fixed, or whether its answer is accepted, with the reason. PR-Agent
  sees no earlier round: it reports only its own findings with their labels,
  and a new Claude agent closes the open ones (skill `pr-review`).
- Findings in German, code names as they are.
