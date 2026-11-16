# Code-Audit resolve-linux-vst, 16.11.2026, Stand f0e1140

## Urteil
Das Repo ist gut wartbar und besser als beim letzten Audit (Stand dde5d41). Alle zehn Funde von damals wurden in den PRs #28 bis #38 bearbeitet. Neun sind behoben, die Tests (Fund 8) zum Teil. Die Zahlen zeigen das: Die Bestandsliste hat statt 6 jetzt 4 Einträge, und die größte Datei hat statt 648 jetzt 530 Zeilen. `ARCHITECTURE.md` führt nur noch eine bekannte Abweichung, und `make test` läuft auf f0e1140 grün. Was jetzt noch auffällt, ist klein. Es liegt vor allem am neuen Werkzeug, das die Wine-DPI zurücksetzt, und an den Tests, die noch fehlen.

## Kennzahlen
| | letztes Audit | jetzt |
|---|---|---|
| Quelldateien | 43 | 53 |
| Einträge in der Bestandsliste | 6 | 4 |
| größte Datei (Zeilen) | 648 | 530 |
| längste Funktion (Zeilen) | 153 | 153 |

Gezählt ist wie beim letzten Mal: `src/`, `tools/`, `tests/`, der Starter, `install.sh`, `start.sh` und `Makefile`, ohne die Strukturprüfung und ohne den FFmpeg-Patch. Die größte Datei ist weiterhin `src/aac.cpp`. Die längste Funktion ist weiterhin `Adapter::dispatch` in `src/vst3_dispatch.cpp`. Die Strukturprüfung meldet 54 Dateien, keine Überschreitung und 4 Einträge in der Bestandsliste.

## Funde

### 1. Die Datei mit den alten DPI-Werten wird an zwei Stellen gelesen, und das auf verschiedene Weise
Bevor die Bibliothek die Wine-DPI eines Plugins ändert, schreibt sie den alten Wert in eine Datei. Jede Zeile hat die Form „Prefix, Tabulator, Wert“. Diese Datei wird an zwei Stellen gelesen. Die Bibliothek selbst liest sie, um den Wert vor Resolve zu finden. Das neue Werkzeug `wine-dpi-restore` liest sie, um die Werte nach Resolve zurückzusetzen. Beide Leser verhalten sich verschieden. Die Bibliothek macht aus einem unlesbaren Wert stillschweigend 0. Das Werkzeug lehnt denselben Wert ab und lässt die Zeile für immer stehen. Dazu kommen Sonderwerte, die meist als nackte Zahl im Code stehen. „-1“ heißt „war nicht gesetzt“ und hat nur an einer Stelle einen Namen. „-2“ heißt einmal „Wine nicht abfragbar“ und einmal „kein Eintrag in der Datei“.

Wer das Format ändert, zum Beispiel einen weiteren Wert je Zeile ergänzt, muss beide Leser gleich ändern. Wer einen der Sonderwerte ändert, muss alle nackten Zahlen finden. Vorschlag: Das Lesen einer Zeile kommt einmal in `wine_restore.h`, wo es schon einen Test gibt. Die Bibliothek nutzt es auch, und die Sonderwerte bekommen je einen Namen. Aufwand klein. Risiko gering, denn der Teil hat einen Test. Beleg: `src/wine_dpi.cpp:191`, `src/wine_dpi.cpp:202`, `src/wine_dpi.cpp:220`, `src/wine_restore.h:22`, `tools/wine-dpi-restore.cpp:33`, `src/wine_dpi.cpp:142`, `src/wine_dpi.cpp:148`, `src/wine_dpi.cpp:153`, `src/wine_dpi.cpp:180`, `src/wine_dpi.cpp:303`, `src/wine_dpi.h:60`.

### 2. Die Datei der Wine-DPI hat zwei Aufgaben, darum baut das Werkzeug fremde Teile mit und verliert seine Fehlergründe
`src/wine_dpi.cpp` startet Wine und liest oder schreibt den DPI-Wert. Dieselbe Datei entscheidet in Resolve auch, welchen Wert ein Plugin bekommt. Für die Entscheidung braucht sie `plugins.conf`, Resolves Skalierungsfaktor und den Programmnamen. Das Werkzeug `wine-dpi-restore` braucht nur den ersten Teil. Weil beides in einer Datei steht, baut es trotzdem alle diese Teile mit ein, sechs Quelldateien statt zwei. Das Werkzeug öffnet außerdem kein Protokoll. Startet Wine nicht, schreibt die Datei den Grund ins Protokoll. Im Werkzeug geht er deshalb verloren. Der Nutzer liest nur „Wine DPI … not reset“ und erfährt nicht, warum.

Jede Änderung an der Entscheidung in Resolve ändert so auch, wie das Werkzeug gebaut wird. Wer verstehen will, warum das Zurücksetzen scheitert, findet keinen Grund. Vorschlag: Der Aufruf von Wine samt Lesen, Schreiben und Löschen des Werts kommt in eine eigene kleine Datei unter der Wine-DPI. Das Werkzeug baut nur diese Datei ein und leitet das Protokoll auf die Fehlerausgabe, wie es `vst3-classes` schon tut. Aufwand klein. Risiko: Es gibt keinen automatischen Test für den Aufruf von Wine, nur den Plugin-Test mit echten Plugins. Der Build prüft aber jede verschobene Stelle. Beleg: `Makefile:32`, `src/wine_dpi.cpp:91`, `src/wine_dpi.cpp:128`, `src/wine_dpi.cpp:173`, `src/wine_dpi.cpp:333`, `tools/wine-dpi-restore.cpp:57`, `tools/vst3-classes.cpp:13`, `src/log.h:1`.

### 3. Die Editorgröße spricht selbst das Protokoll des Fenstersystems
Damit das Fenster eines Windows-Plugins nicht abgeschnitten wird, setzt die Editorgröße die Größe der inneren Fenster selbst. Dafür lädt sie die Bibliothek des Fenstersystems X11 (xcb) von Hand und beschreibt deren Datenstrukturen nach. Sie ruft ihre Funktionen direkt auf und nutzt deren Kennzahlen als nackte Zahlen („4 | 8“ für Breite und Höhe). Das sind rund 110 der 335 Zeilen der Datei. Laut `RULES.md` gehört solcher Zugriff auf ein System in eine eigene Schicht mit einer einfachen Schnittstelle, etwa „innere Fenster auf diese Größe bringen“. Die Editorgröße würde dann nur noch Größen rechnen.

Wer das Rechnen der Größen ändert, muss sich heute durch den Fensterzugriff lesen. Wer den Fensterzugriff ändert, etwa für eine andere Art von Fenster, ändert eine Datei, deren Aufgabe das Umrechnen ist. Vorschlag: Der Teil mit xcb kommt in eine eigene Datei unter der Editorgröße, mit einer einzigen Funktion nach außen. Aufwand klein. Risiko: Es gibt keinen Test. Nur `editor-test` oder Resolve mit einem yabridge-Plugin zeigen, dass es noch geht. Beleg: `src/editor_size.cpp:96`, `src/editor_size.cpp:118`, `src/editor_size.cpp:139`, `src/editor_size.cpp:169`, `src/editor_size.cpp:192`, `src/editor_size.cpp:259`.

### 4. Ein Teil der Logik, die ohne Resolve läuft, hat noch keinen Test
Neu getestet sind `plugins.conf`, die Eigenschaften der VST3-Parameter, die Tabellen in Resolves Speicher, die beiden Fragen nach dem Programm und die Datei mit den alten DPI-Werten. Ohne Test sind noch:
- die Umbenennung von .NEV in .R3D,
- das Lesen und Bilden der AAC-Angaben (AudioSpecificConfig, die Beschreibung, die ein AAC-Decoder braucht),
- das Speicherformat des VST3-Zustands,
- das Umrechnen der Editorgröße,
- die Kategorien im Starter.

`ARCHITECTURE.md` führt das richtig als einzige bekannte Abweichung. Die Rechnungen von NEV und AAC stehen in Dateien, die Systemfunktionen ersetzen. Ein Test kann sie deshalb nicht einbinden, und jeder Umbau dort lässt sich nur mit Resolve prüfen. Vorschlag: Die reinen Rechnungen von NEV und AAC kommen in kleine eigene Teile, wie `wine_base.h` und `wine_restore.h`, je mit Test und in einem eigenen PR. Danach folgt die Editorgröße, vor Fund 3. Aufwand mittel. Beleg: `src/nev.cpp:55`, `src/nev.cpp:95`, `src/aac.cpp:204`, `src/aac.cpp:407`, `src/vst3_dispatch.cpp:185`, `src/editor_size.cpp:212`, `ARCHITECTURE.md:107`.

### 5. Bestandsliste: alle vier Einträge können bleiben, wie sie sind
Die Liste hat statt 6 jetzt 4 Einträge, und keine Datei liegt über 800 Zeilen.
- `Adapter::dispatch` (153 Zeilen, Verzweigungsmaß 80) ist eine flache Fallunterscheidung über die Befehle von Resolve. Die langen Fälle sind schon ausgelagert. Sie ist in Ordnung, wie sie ist.
- `xcb` ist eine Kette gleichartiger Abfragen. Sie zieht mit Fund 3 um und wird dabei nicht kürzer.
- `pluginSetting` und `ElfFile::imports` sind kurze Leseschleifen knapp über der Grenze. Sie sind in Ordnung.

Es ist kein Umbau nötig. Beleg: `structure.baseline:4`, `structure.baseline:7`.

## Seit dem letzten Audit
Neun der zehn Funde vom 16.11.2026 (Stand dde5d41) sind behoben, einer zum Teil (8). Kein Fund ist als abgelehnt vermerkt.
- **Alter Fund 1, umgeleitete Funktionen:** behoben. Die Namen stehen einmal in `sites.h`. Es gibt eine Funktion zum Umleiten in `patch.cpp`, die Lader und Scanner-Teil beide nutzen, und `resolve-check` prüft dieselbe Liste.
- **Alter Fund 2, VST2-Schnittstelle:** behoben. Zeitangaben, Ein- und Ausgänge, Parameter, Kategorien und die Versionsnummer stehen in `vst2.h`, und Adapter und `editor-test` nutzen deren Namen.
- **Alter Fund 3, „Läuft hier Resolve?“:** behoben. Es gibt zwei klar benannte Fragen. Der Wert im AAC-Teil heißt jetzt nach dem, was er meint. `ARCHITECTURE.md` sagt, dass `editor-test` auch beim Zahlenformat als Resolve zählt.
- **Alter Fund 4, Codec-Tabelle:** behoben. Lesen und Einfügen stehen in `patch.cpp`, AAC und Scanner nutzen sie, und `patch-test` prüft Reihenfolge und Gleichgewicht.
- **Alter Fund 5, `REVIEW.md` und `ARCHITECTURE.md`:** behoben. Die veralteten Beispiele sind weg, und die Liste der Abweichungen stimmt mit dem Code.
- **Alter Fund 6, Sprung schreiben:** behoben. Beide Arten melden den Grund auf dieselbe Weise, auch im AAC-Teil.
- **Alter Fund 7, Skalierungsfaktor:** behoben. Er hat eine eigene Datei unter Editorgröße und Wine-DPI.
- **Alter Fund 8, Tests:** zum Teil behoben. Fünf neue Tests sind dazugekommen, der Rest ist offen, siehe neuer Fund 4.
- **Alter Fund 9, Wine-Befehl im Starter:** behoben. Der Starter ruft das gebaute Werkzeug `wine-dpi-restore` auf und kennt weder Wine-Befehl noch Registry-Schlüssel. Das Werkzeug bringt die neuen Funde 1 und 2 mit sich.
- **Alter Fund 10, Bestandsliste:** behoben. `Adapter::init` und `main` von `editor-test` sind aufgeteilt und stehen nicht mehr auf der Liste.

Neu sind die Funde 1, 2 und 3. Fund 4 ist der Rest eines alten Funds. `make test` läuft auf f0e1140 grün.
