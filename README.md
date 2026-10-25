# resolve-vst

VST2- und VST3-Plugins in DaVinci Resolve (Studio) unter Linux. Prototyp, getestet mit
Resolve Studio 21.1.1.

## Warum das geht

Resolve bringt unter Linux den vollständigen VST2-Host der Windows- und macOS-Version mit
(`VSTHost`, `VSTPlugin`, `VSTPluginManager` in `libFairlightPage.so`, dazu der Scanner
`/opt/resolve/bin/VstScanner`). Nur eine Funktion fehlt: `VSTPluginLoader::Load()`, die die
Plugin-Bibliothek öffnet und `VSTPluginMain` aufruft, ist ein leerer Rumpf und liefert immer
`nullptr`. Darum findet Resolve unter Linux kein einziges Plugin, obwohl die Einstellungen da sind.

`libresolve-vst.so` liefert diese Funktion nach und wird per `LD_PRELOAD` geladen:

- **resolve** ruft `Load` über die PLT von `libFairlightPage.so` auf; das gleichnamige
  Symbol aus der Preload-Bibliothek gewinnt.
- **VstScanner** ist ein Executable ohne PIE mit direktem Aufruf. Beim Laden sucht die
  Bibliothek die Funktion in der Symboltabelle von `/proc/self/exe` und überschreibt ihren
  Anfang im Speicher mit einem Sprung auf die eigene Funktion.

An den Dateien von Resolve wird nichts geändert.

Der Scanner nimmt nur Einträge mit der Endung `.vst` (wie Bundles unter macOS), keine `.so`.
`resolve-vst` legt darum für jede VST2-Bibliothek einen Symlink `Name.vst` an.

VST3 kann Resolve unter Linux gar nicht: `VST3Host` in `libFairlightPage.so` besteht nur aus
leeren Rümpfen, Code zum Laden und Betreiben eines VST3-Plugins fehlt. Darum bringt
`libresolve-vst.so` einen VST3-Adapter mit (siehe unten), der VST3-Plugins als VST2-Plugins
zeigt.

## Einrichten

Die VST3-Schnittstellen von Steinberg (MIT-Lizenz) sind ein Git-Submodul:

```sh
git submodule update --init
```

Direkt aus dem Ordner, ohne Installation:

```sh
./start.sh            # baut bei Bedarf, legt die Links an, startet Resolve
```

Oder installiert:

```sh
make
make install          # nach ~/.local (PREFIX=… für anderswo)
resolve-vst           # Links anlegen, Resolve starten
```

Einmalig in Resolve: **Einstellungen → Benutzer → Audio-Plugins → VST-Plugin-Pfade**,
`~/.local/share/resolve-vst/vst` hinzufügen, Scan anstoßen. Die Plugins erscheinen in der
Effektbibliothek unter Audio-FX.

Wichtig: Resolve immer über `resolve-vst` (oder den Starter „DaVinci Resolve (VST)“) starten.
Ohne Preload findet der Scanner nichts und markiert alle Plugins als nicht funktionsfähig;
dann `~/.local/share/DaVinciResolve/configs/Fairlight/Effects/FXConfiguration.xml` löschen und
neu scannen.

## Welche Plugins

- **Native Linux-VST2** aus `~/.vst`, `~/.lxvst`, `/usr/lib/vst`, `/usr/local/lib/vst`
  (oder `$VST_PATH`). Gezählt wird jede `.so`, die `VSTPluginMain` oder `main` exportiert.
- **Windows-VST2 über yabridge** (`yabridgectl`, VST2-Ziel `~/.vst/yabridge`).
- **VST3** aus `~/.vst3`, `/usr/local/lib/vst3`, `/usr/lib/vst3` (oder `$VST3_PATH`),
  nativ oder über yabridge (VST3-Ziel `~/.vst3/yabridge`), über den VST3-Adapter.
- **LV2, CLAP** nicht direkt, aber über **Carla** (`CarlaRackFX`, `CarlaPatchbayFX`): Carla
  ist selbst ein VST2-Plugin und lädt darin alles andere.

### Plugins ausblenden

`~/.config/resolve-vst/ignore`: Shell-Muster auf die Pfade im Link-Ordner, eins je Zeile.
Resolve scannt jedes Plugin einzeln; bei yabridge startet dafür jedes Mal Wine (einige
Sekunden). Große Sammlungen, die man nicht braucht, lieber ausblenden:

```
# Waves-Shell mit über 600 Plugins
VST3/yabridge/WaveShell1-VST3 14.12_x64/*
carla/CarlaPatchbay[0-9]*
```

`resolve-vst link` legt den Ordner nur neu an, wenn sich etwas geändert hat (Liste in
`.entries` dort).

## VST3-Adapter

`src/vst3.cpp` lädt ein VST3-Plugin und übersetzt zwischen Resolves VST2-Host und den
VST3-Schnittstellen (`IComponent`, `IAudioProcessor`, `IEditController`, `IPlugView`):

- **Einträge:** Ein Bundle kann mehrere Plugins (Klassen) enthalten. `resolve-vst` liest sie mit
  `vst3-classes` aus (bei yabridge startet das Wine, darum zwischengespeichert in
  `~/.cache/resolve-vst/vst3`, neu gelesen, wenn sich die Bibliothek ändert) und legt je Klasse
  einen Ordner `VST3/…/Name.vst` an, mit `bundle` (Symlink aufs Bundle) und `class`
  (Klassen-ID). Bundles mit mehreren Klassen (z. B. Waves-Shells) bekommen einen eigenen Ordner.
- **Audio:** `processReplacing` → `IAudioProcessor::process`, 32 Bit. Nur der Hauptbus (nach
  Möglichkeit Stereo; `effSetSpeakerArrangement` für Mono und Surround), Nebenbusse wie
  Sidechain bekommen Stille. Zeitinformation von Resolve geht als `ProcessContext` mit.
- **Parameter:** VST2-Index = Reihenfolge im Controller, Werte normalisiert wie in VST3.
  Änderungen aus dem Editor gehen als Automation an Resolve, Werte vom Host an Prozessor und
  Controller. Ein Bypass-Parameter dient für `effSetBypass`.
- **Zustand:** ein Chunk aus Komponenten- und Controller-Zustand (Kennung `R3VS`).
- **Editor:** `IPlugView` im X11-Fenster von Resolve. Größen, Inhaltsskalierung
  (`IPlugViewContentScaleSupport` statt `'PreS'`), Rahmen-Fix und Wine-DPI wie bei VST2; der
  Name für `plugins.conf` ist der Name der Klasse.
- **IRunLoop:** Linux-Editoren brauchen Timer und Dateideskriptoren vom Host. Resolves Qt läuft
  auf der glib-Ereignisschleife, der Adapter hängt sie dort ein (`g_timeout_add_full`,
  `g_unix_fd_add_full`), Rückrufe kommen im GUI-Thread an.
- **Nicht unterstützt:** MIDI und Instrumente mit Noteneingang, Programmlisten, 64 Bit,
  Sidechain, ARA.

Die VST2-ID (`uniqueID`) ist aus der Klassen-ID gefaltet; Resolve speichert Plugins als
`Name:ID`. Projekte mit VST3-Plugins hängen damit am Adapter.

## Plugin-Fenster und HiDPI

Resolve läuft skaliert (`QT_AUTO_SCREEN_SCALE_FACTOR=1`, setzt `resolve-vst`). Plugins melden
ihre Editorgröße in Bildschirmpixeln, Resolve nimmt sie als logische Pixel und multipliziert
mit dem Faktor; das Fenster würde zu groß. Der Lader hängt sich darum zwischen Resolve und
Plugin (eigener Dispatcher und Host-Callback) und teilt `effEditGetRect` und
`audioMasterSizeWindow` durch den Faktor des Editorfensters. Das Log zeigt beide Größen.

Wie groß das Plugin selbst zeichnet, entscheidet das Plugin:

- **Qt-/Linux-Plugins** wie Carla skalieren sich selbst aus der Umgebung.
- **JUCE-Plugins** (z. B. MPC Beats) skalieren als VST2 nur auf Anweisung des Hosts:
  `effVendorSpecific` mit `'PreS'`/`'AeCs'`, Faktor in `opt` (wie Bitwig und Ableton). Resolve
  sendet das nicht; der Lader schickt Resolves Faktor vor dem Öffnen des Editors.
- **Windows-Plugins über yabridge** zeichnen zusätzlich mit der DPI ihres Wine-Prefix
  (`HKCU\Control Panel\Desktop\LogPixels`; Bottles' `custom_dpi` schreibt dorthin).

### Rahmen bei yabridge

yabridge bettet das Wine-Fenster in ein eigenes Zwischenfenster ein, das die Größe aus
`effEditGetRect` beim Öffnen bekommt und danach nicht mitwächst. JUCE-Plugins unter Wine
melden nach dem Öffnen außerdem wieder ihre Grundgröße, zeichnen aber skaliert: Der Editor
wird abgeschnitten. Der Lader bringt darum die Unterfenster von Resolves Editorfenster auf die
Zielgröße (Größe vor dem Öffnen bzw. letzte `audioMasterSizeWindow`), direkt nach dem Öffnen
und in den ersten Sekunden bei `effEditIdle`, und ersetzt die kleinere Größe nach dem Öffnen.
Über xcb, nicht Xlib (Xlib beendet bei X-Fehlern den Prozess). `RESOLVE_VST_FIX_FRAME=0`
schaltet das ab.

### Einstellungen je Plugin

`~/.config/resolve-vst/plugins.conf` (bzw. `$XDG_CONFIG_HOME/resolve-vst/plugins.conf`),
Abschnitt je Plugin, Name = Dateiname der Plugin-`.so` ohne Endung:

```
[MPC Beats64]
wine-dpi = 96       # Zahl, auto (96 × Resolves Faktor), 0 = nicht anfassen
content-scale = 0   # Zahl, auto (Resolves Faktor), 0 = nicht senden
```

Ohne Eintrag gelten `RESOLVE_VST_WINE_DPI` und `RESOLVE_VST_CONTENT_SCALE`, sonst `auto`.

**Wine-DPI:** Vor dem Laden jedes yabridge-Plugins stellt der Lader die DPI des Prefix ein
(über `$WINELOADER` wie yabridge, nur wenn sie abweicht). Jedes yabridge-Plugin läuft in einem
eigenen Wine-Prozess und liest die DPI bei dessen Start, darum kann sie je Plugin verschieden
sein (nicht bei yabridge-Plugingruppen).

**MPC Beats** (JUCE unter Wine, getestet mit Wine 9.21 und 11.11) braucht beides wie oben:
Es zeichnet mit dem Faktor des Bildschirms (`Xft.dpi`), nicht mit dem vom Host. Mit höherer
Wine-DPI wird es kleiner oder verschwindet; mit Faktor vom Host rechnet es nur die
Fenstergröße damit (4800×2880 statt 3200×1920 bei 3,75), der Rest bleibt schwarz.

Wine kennt keine DPI pro Prozess, der Wert gilt für das ganze Prefix. Der Lader merkt sich
darum den Wert vor Resolve in `~/.local/state/resolve-vst/wine-dpi-restore`, und
`resolve-vst` setzt ihn zurück, sobald Resolve endet, auch nach einem Absturz. Wurde das
Skript selbst abgebrochen, holt es das beim nächsten Start nach (oder `resolve-vst restore`).

### Testen ohne Resolve

`make editor-test` baut `build/editor-test`: Es lädt ein Plugin wie Resolve (über
`libresolve-vst.so`), schickt einige Blöcke Audio durch, liest einen Parameter, schreibt den
Chunk zurück, öffnet den Editor und nimmt ihn auf. VST3-Plugins über ihren Eintrag im
Link-Ordner.

```sh
RESOLVE_VST_EDITOR_SCALE=3.75 build/editor-test ~/.vst/yabridge/'MPC Beats64.so' mpc.png
build/editor-test ~/.local/share/resolve-vst/vst/VST3/TB/TB_Compressor_v4.vst tb.png
```

Die Wine-DPI stellt es nicht ein; die vorher von Hand setzen. Unter KWin (Wayland) wird das
Testfenster nicht immer gezeigt; dann in einem eigenen X-Server aufnehmen
(`Xvfb :97 -screen 0 3840x2400x24 &`, `DISPLAY=:97 build/editor-test …`). Ohne
Fenstermanager scheitern manche Plugins an fehlenden X-Atomen (MiniMeters bricht ab).

## Umgebungsvariablen

| Variable | Bedeutung |
|---|---|
| `RESOLVE_VST_LOG` | Logdatei (Standard `~/.local/state/resolve-vst/resolve-vst.log`, `-` = stderr) |
| `RESOLVE_VST_LINKS` | Ordner für die `.vst`-Links (Standard `~/.local/share/resolve-vst/vst`) |
| `RESOLVE_VST_EDITOR_SCALE` | Skalierung der Editorgröße festlegen (Standard: Faktor von Resolves Fenster; `1` = aus) |
| `RESOLVE_VST_CONTENT_SCALE` | Faktor für `'PreS'`/`'AeCs'`: `auto` (Standard) = Resolves Faktor, Zahl = fest, `0` = nicht senden; je Plugin in `plugins.conf` |
| `RESOLVE_VST_WINE_DPI` | DPI für yabridge-Prefixe: `auto` (Standard) = 96 × Faktor, Zahl = fest, `0` = nicht anfassen; je Plugin in `plugins.conf` |
| `RESOLVE_VST_FIX_FRAME=0` | yabridge-Rahmen nicht anpassen |
| `RESOLVE_VST_DEEPBIND=1` | Plugins mit `RTLD_DEEPBIND` laden (bei Symbolkonflikten mit Resolve) |
| `RESOLVE_BIN` | Pfad zu Resolve (Standard `/opt/resolve/bin/resolve`) |
| `VST_PATH` | Suchpfade für VST2, durch `:` getrennt |
| `VST3_PATH` | Suchpfade für VST3, durch `:` getrennt |

## Stand

- Scanner: getestet; Carla, yabridge-Plugins (MPC Beats) werden mit Name, Hersteller und
  Kanälen erkannt, defekte Plugins sauber als nicht funktionsfähig markiert.
- In Resolve: Plugins erscheinen, lassen sich auf Busse legen, Editoren öffnen sich in einem
  eigenen Resolve-Fenster (X11-Fenster über `QWidget::winId()` an `effEditOpen`).
- Editorgröße unter HiDPI: in Resolve bestätigt, dass MPC Beats groß erscheint; mit
  `editor-test` geprüft, dass es mit den Einstellungen oben sein Fenster ohne Rand füllt
  (3200×1920 bei Faktor 3,75). Carla unverändert.
- VST3: Scanner erkennt native (ToneBoosters, MiniMeters) und yabridge-Plugins (iZotope);
  mit `editor-test` geprüft: Audio läuft durch (Kompressor regelt), Parameter, Chunk,
  Editor mit Inhaltsskalierung (ToneBoosters). In Resolve noch nicht ausprobiert.
- Bibliotheken werden nie entladen (`dlclose` fehlt bewusst, siehe Quelltext).
- Bricht, wenn Blackmagic `VSTPluginLoader` umbenennt oder die Signatur ändert; dann
  meldet das Log, dass nichts umgeleitet wurde.
