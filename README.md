# resolve-vst

VST2-Plugins in DaVinci Resolve (Studio) unter Linux. Prototyp, getestet mit Resolve Studio 21.1.1.

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

## Einrichten

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
- **VST3, LV2, CLAP** nicht direkt (Resolve hat unter Linux keinen VST3-Host), aber über
  **Carla** (`CarlaRackFX`, `CarlaPatchbayFX`): Carla ist selbst ein VST2-Plugin und lädt
  darin alles andere, auch VST3-Plugins über yabridge.

## Plugin-Fenster und HiDPI

Resolve läuft skaliert (`QT_AUTO_SCREEN_SCALE_FACTOR=1`, setzt `resolve-vst`). Plugins melden
ihre Editorgröße in Bildschirmpixeln, Resolve nimmt sie als logische Pixel und multipliziert
mit dem Faktor; das Fenster würde zu groß. Der Lader hängt sich darum zwischen Resolve und
Plugin (eigener Dispatcher und Host-Callback) und teilt `effEditGetRect` und
`audioMasterSizeWindow` durch den Faktor des Editorfensters. Das Log zeigt beide Größen.

Wie groß das Plugin selbst zeichnet, entscheidet das Plugin:

- **Qt-/Linux-Plugins** wie Carla erben die Skalierung aus der Umgebung.
- **JUCE-Plugins** (z. B. MPC Beats, viele Windows-Plugins über yabridge) skalieren als VST2
  nur, wenn der Host es ihnen sagt: `effVendorSpecific` mit `'PreS'`/`'AeCs'`, Faktor in
  `opt` (wie Bitwig und Ableton). Resolve sendet das nicht; der Lader schickt Resolves
  Faktor vor dem Öffnen des Editors. Getestet mit MPC Beats: 1280×768 → 3200×1920 bei 2,5.
- **Andere Windows-Plugins über yabridge** zeichnen mit der DPI ihres Wine-Prefix
  (`HKCU\Control Panel\Desktop\LogPixels`; Bottles' `custom_dpi` schreibt dorthin). Mit
  `RESOLVE_VST_WINE_DPI=auto` stellt der Lader sie vor dem Laden auf `96 × Resolves Faktor`
  (über `$WINELOADER` wie yabridge). Standardmäßig aus: Wine kennt keine DPI pro Prozess,
  der Wert gilt für das ganze Prefix und damit auch in anderen Hosts.

## Umgebungsvariablen

| Variable | Bedeutung |
|---|---|
| `RESOLVE_VST_LOG` | Logdatei (Standard `~/.local/state/resolve-vst/resolve-vst.log`, `-` = stderr) |
| `RESOLVE_VST_LINKS` | Ordner für die `.vst`-Links (Standard `~/.local/share/resolve-vst/vst`) |
| `RESOLVE_VST_EDITOR_SCALE` | Skalierung der Editorgröße festlegen (Standard: Faktor von Resolves Fenster; `1` = aus) |
| `RESOLVE_VST_CONTENT_SCALE` | Faktor für `'PreS'`/`'AeCs'` festlegen (Standard: Resolves Faktor; `0` = nicht senden) |
| `RESOLVE_VST_WINE_DPI` | DPI für yabridge-Prefixe: `auto` = 96 × Faktor, Zahl = fest (Standard: nicht anfassen) |
| `RESOLVE_VST_DEEPBIND=1` | Plugins mit `RTLD_DEEPBIND` laden (bei Symbolkonflikten mit Resolve) |
| `RESOLVE_BIN` | Pfad zu Resolve (Standard `/opt/resolve/bin/resolve`) |
| `VST_PATH` | Suchpfade für VST2, durch `:` getrennt |

## Stand

- Scanner: getestet; Carla, yabridge-Plugins (MPC Beats) werden mit Name, Hersteller und
  Kanälen erkannt, defekte Plugins sauber als nicht funktionsfähig markiert.
- In Resolve: Plugins erscheinen, lassen sich auf Busse legen, Editoren öffnen sich in einem
  eigenen Resolve-Fenster (X11-Fenster über `QWidget::winId()` an `effEditOpen`).
- Editorgröße unter HiDPI: Umrechnung eingebaut, in Resolve noch nicht bestätigt.
- Bibliotheken werden nie entladen (`dlclose` fehlt bewusst, siehe Quelltext).
- Bricht, wenn Blackmagic `VSTPluginLoader` umbenennt oder die Signatur ändert; dann
  meldet das Log, dass nichts umgeleitet wurde.
