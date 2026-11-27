# Changelog

The release notes of resolve-linux-vst, newest first. Each section is the text of the release on
Gitea and GitHub and of its entry in `#changes` on the landing page (`docs/index.html`).

## [2.0.0] — 2026-11-27

### English

Nikon N-RAW clips play and export faster: resolve-linux-vst now decodes the frames itself, on an NVIDIA graphics card, instead of leaving them to the RED decoder on the CPU. The picture is identical to RED's, bit for bit.

#### New
- Own N-RAW decoder, on by default. On an NVIDIA card (Turing or newer, driver 525 or newer) a frame takes about 5 ms and little CPU time; RED needs about 11 ms on all cores. Without a card it runs on the CPU. Export of 4.1K N-RAW to ProRes 422 UHD on a Ryzen 9 3900X with an RTX 3090: 45 to 55 instead of 35 to 40 frames per second.
- RED's colour processing and the Camera RAW settings stay as they are: only the step that unpacks a frame is replaced.
- Safety net: the first frames and every 500th are also decoded by RED and compared. On any difference, error or unknown format RED decodes again, and the log says why.

#### Note
- `RESOLVE_LINUX_VST_NRAW_DECODE=0` leaves N-RAW decoding to RED, as in 1.x.
- Tested with one camera format (4.1K, 25p). Other formats are checked against RED in the same way.

All changes: https://github.com/shrippen/resolve-linux-vst/compare/v1.1.1...v2.0.0

### Deutsch

Nikon-N-RAW-Clips laufen und exportieren schneller: resolve-linux-vst dekodiert die Bilder jetzt selbst, auf einer NVIDIA-Grafikkarte, statt sie dem RED-Decoder auf der CPU zu überlassen. Das Bild ist mit dem von RED identisch, Bit für Bit.

#### Neu
- Eigener N-RAW-Decoder, standardmäßig an. Auf einer NVIDIA-Karte (ab Turing, Treiber ab 525) braucht ein Bild etwa 5 ms und wenig CPU-Zeit; RED braucht etwa 11 ms auf allen Kernen. Ohne Karte läuft er auf der CPU. Export von 4.1K-N-RAW nach ProRes 422 UHD auf einem Ryzen 9 3900X mit RTX 3090: 45 bis 55 statt 35 bis 40 Bilder je Sekunde.
- REDs Farbverarbeitung und die Camera-RAW-Einstellungen bleiben, wie sie sind: ersetzt wird nur der Schritt, der ein Bild auspackt.
- Sicherheitsnetz: Die ersten Bilder und jedes 500. dekodiert zusätzlich RED zum Vergleich. Bei jeder Abweichung, jedem Fehler und jedem unbekannten Format dekodiert wieder RED, und das Log nennt den Grund.

#### Hinweis
- `RESOLVE_LINUX_VST_NRAW_DECODE=0` überlässt das Dekodieren RED, wie in 1.x.
- Getestet mit einem Kameraformat (4.1K, 25p). Andere Formate werden genauso gegen RED geprüft.

Alle Änderungen: https://github.com/shrippen/resolve-linux-vst/compare/v1.1.1...v2.0.0

## [1.1.1] — 2026-11-16

### English

VST3 plug-ins with stepped controls (mode selectors, switches with several positions) tell Resolve their steps correctly.

#### Improved
- If AAC cannot adjust Resolve, the log now says why.

#### Fixed
- A VST3 control with a fixed number of positions told Resolve to move in fractional steps of size 0, so Resolve did not know where its positions were. It now reports whole-number positions from 0 to the last one.
- `./install.sh --source` failed with "Permission denied" because the script was not executable.

All changes: https://github.com/shrippen/resolve-linux-vst/compare/v1.1.0...v1.1.1

### Deutsch

VST3-Plugins mit gestuften Reglern (Moduswahl, Schalter mit mehreren Stellungen) melden Resolve ihre Stufen richtig.

#### Verbessert
- Kann AAC Resolve nicht anpassen, nennt das Log jetzt den Grund.

#### Behoben
- Ein VST3-Regler mit fester Zahl an Stellungen sagte Resolve, er bewege sich in Bruchteil-Schritten der Größe 0; Resolve wusste so nicht, wo seine Stellungen liegen. Er meldet jetzt ganzzahlige Stellungen von 0 bis zur letzten.
- `./install.sh --source` scheiterte mit „Permission denied“, weil das Skript nicht ausführbar war.

Alle Änderungen: https://github.com/shrippen/resolve-linux-vst/compare/v1.1.0...v1.1.1

## [1.1.0] — 2026-11-16

### English

Windows plug-ins through yabridge fill their editor windows again and can be used. Tested with DaVinci Resolve Studio 21.2.

#### New
- `wine-dpi = prefix` in `plugins.conf` keeps the Wine prefix's own DPI; built in for Waves
- `resolve-linux-vst restore` also stops processes left on yabridge plug-in group sockets

#### Fixed
- Waves and iZotope editors drew only about three quarters of their window, offset to the top left, and clicks missed, whenever the Wine prefix's own DPI was not 240. Wine scales a plug-in window by the DPI its Wine session started with divided by the DPI of the plug-in; the scale is now computed from the prefix's own DPI instead of an assumed 240, and stays right when a Wine session ends and a new one starts.
- After Resolve had been closed, Waves plug-ins could fail to load ("Another process is already listening on this group's socket"): `WavesLocalServer.exe` kept the Waves group's socket. The launcher now stops such leftovers before Resolve starts and after it exits.

#### Note
- Remove a `wine-dpi` line you added under `[WaveShell*]` in `plugins.conf` as a workaround; the built-in setting now fits every prefix.
- Resolve's automation button in a plug-in window has no effect on VST plug-ins: Resolve implements it only for its own Fairlight FX.
- The library changes the Wine DPI only when Resolve is started through `resolve-linux-vst`, which sets it back afterwards. `build/editor-test` needs `RESOLVE_LINUX_VST_WINE_RESTORE` for it (see README).

All changes: https://github.com/shrippen/resolve-linux-vst/compare/v1.0.2...v1.1.0

### Deutsch

Windows-Plugins über yabridge füllen ihre Editorfenster wieder und lassen sich bedienen. Getestet mit DaVinci Resolve Studio 21.2.

#### Neu
- `wine-dpi = prefix` in `plugins.conf` behält die eigene DPI des Wine-Prefix; eingebaut für Waves
- `resolve-linux-vst restore` beendet auch Prozesse, die auf Sockets von yabridge-Plugingruppen zurückgeblieben sind

#### Behoben
- Editoren von Waves und iZotope zeichneten nur etwa drei Viertel ihres Fensters, nach oben links versetzt, und Klicks gingen daneben, sobald die eigene DPI des Wine-Prefix nicht 240 war. Wine vergrößert ein Plugin-Fenster mit der DPI, mit der seine Wine-Sitzung startete, geteilt durch die DPI des Plugins; gerechnet wird jetzt mit der eigenen DPI des Prefix statt mit angenommenen 240, und das bleibt richtig, wenn eine Wine-Sitzung endet und eine neue startet.
- Nach dem Beenden von Resolve ließen sich Waves-Plugins teils nicht laden („Another process is already listening on this group's socket“): `WavesLocalServer.exe` hielt den Socket der Waves-Gruppe. Der Starter beendet solche Reste jetzt vor dem Start von Resolve und nach dessen Ende.

#### Hinweis
- Eine `wine-dpi`-Zeile, die du als Notlösung unter `[WaveShell*]` in `plugins.conf` eingetragen hast, wieder entfernen; die eingebaute Einstellung passt jetzt zu jedem Prefix.
- Der Automationsknopf in Resolves Plugin-Fenster wirkt bei VST-Plugins nicht: Resolve setzt ihn nur für seine eigenen Fairlight-FX um.
- Die Bibliothek ändert die Wine-DPI nur, wenn Resolve über `resolve-linux-vst` startet, der sie danach zurücksetzt. `build/editor-test` braucht dafür `RESOLVE_LINUX_VST_WINE_RESTORE` (siehe README).

Alle Änderungen: https://github.com/shrippen/resolve-linux-vst/compare/v1.0.2...v1.1.0

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
