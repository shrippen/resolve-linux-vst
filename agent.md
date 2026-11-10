# resolve-linux-vst: notes for agents

The rules for all shrippen repositories are in `RULES.md` (writing, code, commits, bug
fixes, repository, GUI). It is a synced copy from the rules repository; do not edit it
here. This file holds only what is particular to this repository. A Claude Code session on
the owner's machine has the rules through the global `CLAUDE.md` already and reads
`RULES.md` only for its GUI rule, before work on a user interface.

A preload library (`libresolve-linux-vst.so`) plus the launcher script `resolve-linux-vst` that add VST2/VST3
plug-ins, AAC import and render, and Nikon N-RAW to DaVinci Resolve Studio on Linux. Internals:
[`docs/HOW-IT-WORKS.md`](docs/HOW-IT-WORKS.md); user docs: [`README.md`](README.md).

- Build `make`, AAC decoder `make aac`, tests `make test`, hook check against the installed
  Resolve `build/resolve-check` (or `resolve-linux-vst check`). Release tarball `make dist`.
- Hooks without symbols are found by byte patterns (`src/sites.cpp`), never by fixed addresses.
  A pattern that no longer matches must switch off only its own feature and log why.
- Resolve's files are never changed on disk; everything happens in memory at run time.
- Code comments and the library log are German; everything a user sees (CLI, installer,
  README, landing page) is English, the landing page also German.
- Tested against Resolve Studio 21.1.1. After a Resolve update run `resolve-linux-vst check` first.

## GUI (this project)

resolve-linux-vst has no GUI of its own (it adds plug-ins to Resolve), so the only GUI is the landing
page `docs/index.html`; there is no demo mode and there are no screenshots.

## Release

Release steps: `RELEASE.md`. Before a release, load the skill `release-changelog`. A release
is published, so the owner confirms it in words.

## Repository (this project)

- Everything else: `RULES.md` (Repository) and the skill `pr-review`.
