# resolve-linux-vst: notes for agents

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

## Release rule

- Every release gets a release log, written before the tag, in `CHANGELOG.md` (the single source):
  a section `## [X.Y.Z] — YYYY-MM-DD` at the top with `### English` and `### Deutsch`, each with a
  lead (what the release is about), then the changes grouped as New, Improved, Fixed and Note
  (breaking changes, steps the user must take) and the compare link to the previous tag. Each point
  says what changed for the user and why, not which code moved.
- The same text goes into the release notes on Gitea and as a new entry at the top of `#changes` on
  the landing page (`docs/index.html`, Kante `.changelog`); from the fourth entry on, the oldest move
  into `details.changelog-more`. Bump the version in the footer.
- The release job in `.github/workflows/ci.yml` takes the GitHub release notes from the tag's
  section in `CHANGELOG.md` and fails without one. A release is done when the landing page shows it.
- The notes come from `scripts/release-notes.sh X.Y.Z` (Gitea too: `scripts/release-notes.sh X.Y.Z >
  notes.md`, then `tea release create --tag vX.Y.Z --title … --note-file notes.md`); CI checks the
  headings on every push (`scripts/release-notes.sh --check`). The script is a copy of Kante's
  `tools/release-notes.sh`: change it only there, then sync with
  `../Kante/tools/sync-release-notes.sh "$PWD"`.

## GUI rule

- Every GUI of this project is generated from Kante, not inspired by it: landing pages,
  web apps, Qt Quick / Kirigami apps, Plasma widgets, dialogs, e-mail and print layouts.
  Source: https://github.com/shrippen/Kante (checkout `../Kante`).
  Web: link `https://shrippen.github.io/v1/shrippen.css` and `shrippen.js`, or vendor them
  unchanged. Apps: copy `qml/Kante` (and `KantePlasma` for Plasma widgets) unchanged.
- Use Kante's tokens, roles, components, classes, QML components and motion as they are.
  No own colours, fonts, sizes, radii, cuts, shadows, animation timings, no own copy or
  variant of a component that Kante has. Raw values (`#hex`, `px` for controls) are a bug;
  use roles (`--primary`, `--focus`, `--warn`, `KanteStyle.*`).
- A missing element is added to Kante first (CSS or QML, docs, catalogue), then used here.
  Never solve it locally in this project and never wait with a "temporary" copy.
- Exception: Kimai plugins take their GUI from Knust (`kimai/knust/` in Kante) and
  the kit (`kimai/kit/`), the Kante spinoff that adapts Kante to Kimai's look. The same rule applies to Knust: use it
  as it is, and add missing elements to Knust.
- Exception: Kintsugi (`shrippen/kintsugi`) uses Kante Gold (`<html data-kante="gold">`),
  the noble variant defined in Kante itself. The same rule applies: use it as it is, and
  add a missing element or Gold detail to Kante (its Gold block) first. No other project
  uses Kante Gold without a decision recorded here.
- A project without a GUI (library, CLI, scripts) has nothing to do here.

resolve-linux-vst has no GUI of its own (it adds plug-ins to Resolve), so the only GUI is the landing
page `docs/index.html`; there is no demo mode and there are no screenshots.
