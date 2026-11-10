# Release steps

Release steps, in this order (`X.Y.Z` the new version, `A.B.C` the previous one). A release is done
only after step 10.

1. Version: on a branch from `main`. The build takes it from the tag (`Makefile`: `git describe`;
   the release job passes `VERSION`), so the code has nothing to bump; update the example
   `install.sh --version X.Y.Z` in `README.md`.
2. Write the release log before the tag in `CHANGELOG.md`, the single source: a section
   `## [X.Y.Z] — YYYY-MM-DD` at the top with `### English` and `### Deutsch`, each with a lead (what
   the release is about), then the changes grouped as New, Improved, Fixed and Note (breaking
   changes, steps the user must take) and the compare link
   (`https://github.com/shrippen/resolve-linux-vst/compare/vA.B.C...vX.Y.Z`). Each point says what
   changed for the user and why, not which code moved. Check the headings with
   `scripts/release-notes.sh --check` (CI runs it on every push) and read the notes with
   `scripts/release-notes.sh X.Y.Z`.
3. Landing page `docs/index.html` (Kante `.changelog`): the same text, in English and German, as a
   new entry at the top of `#changes`; from the fourth entry on, the oldest move into
   `details.changelog-more`. Bump the footer version (`vX.Y.Z`). There is no demo world here (no
   `demo/shots.json`): changes get a diagram (Kante `.flow` or inline SVG) where possible.
4. Open the PR on Gitea, review it as the skill `pr-review` says (a release is published: the owner
   confirms it in words), merge on Gitea.
5. Tag the merge commit and push the tag to Gitea only:
   `git tag vX.Y.Z <merge commit> && git push origin vX.Y.Z` (the mirror carries it to GitHub).
6. Gitea release, notes only, never files: `scripts/release-notes.sh X.Y.Z > notes.md`, then
   `gt release create vX.Y.Z --repo shrippen/resolve-linux-vst --title "resolve-linux-vst X.Y.Z" --notes-file notes.md --no-input`.
7. GitHub: the tag runs `.github/workflows/ci.yml`; after `test`, the `release` job takes the notes
   from the section (fails without one), runs `make dist` and creates the release
   "resolve-linux-vst X.Y.Z" with `resolve-linux-vst-x86_64.tar.gz`, its `.sha256`, the FFmpeg
   sources and `tools/ffmpeg-binutils.patch`. Check the run
   (`gh run list -R shrippen/resolve-linux-vst --workflow ci.yml`) and the release
   (`gh release view vX.Y.Z -R shrippen/resolve-linux-vst`: notes and files).
8. Update check: `python3 ../shrippen.github.io/overview/tools/build-versions.py`, then commit
   `docs/versions.json` in `shrippen.github.io` via a PR there.
9. No deploy: users install and update with `install.sh`, which downloads from the GitHub release.
   To check the published archive: `./install.sh --version X.Y.Z`, then `resolve-linux-vst check`.
10. Done when https://shrippen.github.io/resolve-linux-vst/ shows X.Y.Z at the top of `#changes`
    and in the footer, and https://shrippen.github.io/versions.json lists `resolve-linux-vst` with
    X.Y.Z.

`scripts/release-notes.sh` is a copy of Kante's `tools/release-notes.sh`: change it only there, then
sync with `../Kante/tools/sync-release-notes.sh "$PWD"`.
