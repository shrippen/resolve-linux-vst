# Changelog

The release notes of resolve-linux-vst, newest first. Each section is the text of the release on
Gitea and GitHub and of its entry in `#changes` on the landing page (`docs/index.html`).

## [1.0.2] — 2026-10-27

### English

MiniMeters no longer takes Resolve down when its editor opens.

#### Fixed
- MiniMeters crashed Resolve when its editor opened in a project saved before the editor had ever been open (Audio-Server: every time a project was reopened). Plug-in requests for a window size of 0 are now dropped.

All changes: https://github.com/shrippen/resolve-linux-vst/compare/v1.0.1...v1.0.2

### Deutsch

MiniMeters reißt Resolve beim Öffnen seines Editors nicht mehr mit.

#### Behoben
- MiniMeters brachte Resolve zum Absturz, wenn sich sein Editor in einem Projekt öffnete, das gespeichert wurde, bevor der Editor je offen war (Audio-Server: bei jedem erneuten Öffnen eines Projekts). Fenstergrößen von 0 aus Plugins werden jetzt ausgelassen.

Alle Änderungen: https://github.com/shrippen/resolve-linux-vst/compare/v1.0.1...v1.0.2

## [1.0.1] — 2026-10-27

### English

License housekeeping for the bundled components and a more reliable source download.

#### Fixed
- The archive now contains the license texts of FFmpeg (LGPL-2.1) and Steinberg's VST3 interfaces (MIT); the FFmpeg sources of the AAC decoder are attached to the release
- Building from source falls back to a mirror when ffmpeg.org is unreachable

All changes: https://github.com/shrippen/resolve-linux-vst/compare/v1.0.0...v1.0.1

### Deutsch

Lizenzpflege für die mitgelieferten Komponenten und ein zuverlässigerer Quellen-Download.

#### Behoben
- Das Archiv enthält jetzt die Lizenztexte von FFmpeg (LGPL-2.1) und Steinbergs VST3-Schnittstellen (MIT); die FFmpeg-Quellen des AAC-Decoders hängen am Release
- Der Bau aus den Quellen weicht auf einen Spiegel aus, wenn ffmpeg.org nicht erreichbar ist

Alle Änderungen: https://github.com/shrippen/resolve-linux-vst/compare/v1.0.0...v1.0.1

## [1.0.0] — 2026-10-27

### English

The first public release: plug-ins, AAC and N-RAW for DaVinci Resolve Studio 21.1.1 on Linux, with an installer and a self-check for Resolve updates.

#### New
- VST2 loader and VST3 adapter, so plug-ins appear in the Fairlight effects library at all
- Plug-in categories, HiDPI editor sizes and Wine DPI per plug-in
- AAC import for MP4, MOV and M4A, and complete AAC renders that browsers play
- Nikon N-RAW through Resolve's RED decoder
- `resolve-linux-vst check`: after a Resolve update it shows which hooks still match

#### Note
- Tested with Resolve Studio 21.1.1. Always start Resolve through `resolve-linux-vst`; otherwise its scanner marks all plug-ins as failed (`resolve-linux-vst rescan` fixes that).

### Deutsch

Die erste öffentliche Version: Plugins, AAC und N-RAW für DaVinci Resolve Studio 21.1.1 unter Linux, mit Installer und Selbstprüfung für Resolve-Updates.

#### Neu
- VST2-Lader und VST3-Adapter, damit Plugins überhaupt in der Fairlight-Effektbibliothek erscheinen
- Plugin-Kategorien, Editorgrößen bei HiDPI und Wine-DPI je Plugin
- AAC-Import für MP4, MOV und M4A und vollständige AAC-Renders, die Browser abspielen
- Nikon N-RAW über Resolves RED-Decoder
- `resolve-linux-vst check`: zeigt nach einem Resolve-Update, welche Einhängepunkte noch passen

#### Hinweis
- Getestet mit Resolve Studio 21.1.1. Resolve immer über `resolve-linux-vst` starten; sonst markiert sein Scanner alle Plugins als fehlerhaft (`resolve-linux-vst rescan` behebt das).
