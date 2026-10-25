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

## Umgebungsvariablen

| Variable | Bedeutung |
|---|---|
| `RESOLVE_VST_LOG` | Logdatei (Standard `~/.local/state/resolve-vst/resolve-vst.log`, `-` = stderr) |
| `RESOLVE_VST_LINKS` | Ordner für die `.vst`-Links (Standard `~/.local/share/resolve-vst/vst`) |
| `RESOLVE_VST_DEEPBIND=1` | Plugins mit `RTLD_DEEPBIND` laden (bei Symbolkonflikten mit Resolve) |
| `RESOLVE_BIN` | Pfad zu Resolve (Standard `/opt/resolve/bin/resolve`) |
| `VST_PATH` | Suchpfade für VST2, durch `:` getrennt |

## Stand

- Scanner: getestet; Carla, yabridge-Plugins (MPC Beats) werden mit Name, Hersteller und
  Kanälen erkannt, defekte Plugins sauber als nicht funktionsfähig markiert.
- In Resolve selbst (Laden, Verarbeitung, Editor-Fenster): noch nicht getestet. Der Editor
  übergibt dem Plugin `QWidget::winId()` (X11-Fenster) an `effEditOpen`, das sollte reichen.
- Bibliotheken werden nie entladen (`dlclose` fehlt bewusst, siehe Quelltext).
- Bricht, wenn Blackmagic `VSTPluginLoader` umbenennt oder die Signatur ändert; dann
  meldet das Log, dass nichts umgeleitet wurde.
