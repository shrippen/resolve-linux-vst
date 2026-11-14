# Code-Audit resolve-linux-vst, 14.11.2026, Stand 55f6947

## Urteil
Das Repo ist klein und gut kommentiert. Die nächste Änderung ist trotzdem oft teurer als nötig, aus zwei Gründen. Erstens erledigt die Hauptdatei des Laders sechs verschiedene Aufgaben. Zweitens steht einiges zwei- oder dreimal da: die Prüfung, ob der laufende Prozess Resolve ist, der Eingriff in Resolves Speicher, die Kennzahlen der VST2-Schnittstelle und die Regeln für die Wine-DPI. Automatische Tests gibt es nur für die Mustersuche. Jeder Umbau außerhalb davon lässt sich deshalb nur von Hand mit einem installierten Resolve prüfen. Dies ist das erste Audit, einen Vergleich gibt es noch nicht.

## Kennzahlen
| | letztes Audit | jetzt |
|---|---|---|
| Quelldateien | – | 17 |
| Einträge in der Bestandsliste | – | 11 |
| größte Datei (Zeilen) | – | 1750 |
| längste Funktion (Zeilen) | – | 208 |

## Funde

### 1. Das Protokoll (Log) gehört zur VST2-Datei, und es hat zwei Namen für dieselbe Aufgabe
Alle Teile schreiben in dasselbe Protokoll: AAC, NEV, X11 und VST3. Die Schreibfunktion steht aber in der Datei des VST2-Laders, und ihre Ankündigung (Deklaration, also die Zeile, die anderen Dateien sagt, dass es die Funktion gibt) steht in der Kopfdatei der VST2-Schnittstelle. Deshalb binden die NEV- und die X11-Datei die VST2-Schnittstelle ein, obwohl sie damit nichts zu tun haben. Das Hilfsprogramm `vst3-classes` muss sich eine eigene Kopie der Funktion bauen. In der Hauptdatei gibt es außerdem zwei gleichwertige Funktionen, `logf` und `logMessage`. Wer das Protokoll ändert, etwa ein neues Ziel oder eine Stufe für Fehler, muss die VST2-Datei anfassen und dabei zwei Funktionen pflegen. Vorschlag: eine eigene kleine Protokolldatei mit einer Funktion, die alle Teile einbinden. Aufwand klein, Risiko gering: Der Build zeigt jeden vergessenen Aufruf, Tests gibt es dafür keine. Beleg: `src/resolve_linux_vst.cpp:61`, `src/resolve_linux_vst.cpp:234`, `src/vst2.h:37`, `src/nev.cpp:33`, `src/x11.cpp:17`, `tools/vst3-classes.cpp:11`.

### 2. Die Kennzahlen der VST2-Schnittstelle stehen in zwei Dateien
Resolve und Plugin verständigen sich über Befehlsnummern, etwa „Editor öffnen“ = 14, und über die Kennung „PreS/AeCs“ für die Skalierung. Der VST2-Lader und der VST3-Adapter haben je eine eigene Liste dieser Zahlen. Dabei gibt es eine gemeinsame Kopfdatei `vst2.h`, die laut eigenem Kommentar genau für beide da ist. Wer eine Nummer ergänzt oder berichtigt, muss beide Listen finden. Vorschlag: die Listen nach `vst2.h` verschieben und die Kopien löschen. Aufwand klein, Risiko gering: Der Build prüft es. Beleg: `src/resolve_linux_vst.cpp:249`, `src/resolve_linux_vst.cpp:256`, `src/vst3.cpp:713`, `src/vst3.cpp:734`, `src/vst2.h:1`.

### 3. „Läuft hier Resolve?“ ist dreimal gebaut
Drei Dateien lesen jede für sich den Namen des laufenden Programms aus und vergleichen ihn mit „resolve“: Lader, AAC und NEV. Der Lader lässt zusätzlich „VstScanner“ gelten und liest den Namen an einer vierten Stelle noch einmal. Ändert Blackmagic den Programmnamen oder kommt ein weiteres Programm dazu, müssen alle Stellen gefunden werden. Eine vergessene schaltet still eine Funktion ab. Vorschlag: eine Funktion, die den Programmnamen liefert, an einer Stelle; jede Datei vergleicht dann nur noch. Aufwand klein. Risiko: keine Tests, prüfbar nur durch Start von Resolve. Beleg: `src/resolve_linux_vst.cpp:607`, `src/resolve_linux_vst.cpp:1030`, `src/aac.cpp:574`, `src/nev.cpp:153`.

### 4. Der VST3-Adapter liest die Klassen eines Plugins auf zwei Wegen
Ein VST3-Paket (Bundle) kann mehrere Plugins enthalten, hier „Klassen“ genannt. Zweimal läuft dieselbe Schleife über diese Klassen, jeweils mit demselben Ausweichweg für alte Plugins: einmal beim Laden und einmal für die Liste, aus der der Starter die Einträge anlegt. Wer eine weitere Angabe braucht, etwa die Version in der Liste, oder den Ausweichweg ändert, ändert ihn zweimal. Vorschlag: eine Funktion, die alle Klassen mit ihren Angaben liefert, und beide Stellen nutzen sie. Aufwand klein. Risiko: keine Tests, prüfbar mit `vst3-classes` an einem Plugin. Beleg: `src/vst3.cpp:684`, `src/vst3.cpp:1716`.

### 5. Eingriffe in Resolves Speicher sind zweimal gebaut
Zweimal schreibt der Lader einen Sprung in Resolves Programmcode, damit Resolve eigenen Code aufruft: einmal für die Plugin-Kategorien im Scanner und einmal für AAC. Beide Male gehören dazu dieselben Schritte (Speicher beschreibbar machen, schreiben, wieder schützen, Prozessor-Zwischenspeicher leeren) und dasselbe Wissen über den inneren Aufbau einer Tabelle in Resolves C++-Bibliothek, samt derselben Prüfung der Abstände. Ändert Resolve die Bibliothek oder braucht ein Eingriff einen anderen Sprung, muss das an zwei Stellen gleich nachgezogen werden. Vorschlag: eine gemeinsame Stelle für „Sprung schreiben“ und für den Aufbau der Tabelle. Aufwand klein bis mittel. Risiko hoch: Kein Test deckt das Schreiben ab, `resolve-linux-vst check` prüft nur das Finden. Beleg: `src/resolve_linux_vst.cpp:957`, `src/aac.cpp:425`, `src/resolve_linux_vst.cpp:992`, `src/aac.cpp:449`.

### 6. Starter und Bibliothek halten die Regeln der Wine-DPI von Hand gleich
Die Bibliothek merkt sich vor einer Änderung der Wine-DPI (Bildschirmauflösung, die Wine Windows-Plugins meldet) den alten Wert in einer Datei. Der Starter, ein Shell-Skript, liest diese Datei nach dem Ende von Resolve und setzt den Wert zurück. Drei Dinge stehen deshalb in zwei Sprachen: der Ort der Datei (die Regel für den Statusordner), ihr Zeilenformat mit „-1 = war nicht gesetzt“ und der Wine-Befehl samt Registry-Schlüssel. Ändert jemand eines davon nur auf einer Seite, bleibt die DPI nach Resolve still verstellt. Ein Kommentar sagt das zwar, eine Prüfung gibt es nicht. Vorschlag: Der Starter gibt der Bibliothek den Pfad der Datei über eine Umgebungsvariable mit, wie schon beim Protokoll (`RESOLVE_LINUX_VST_LOG`). Format und Befehl stehen dann je an einer kommentierten Stelle, die auf die andere verweist. Aufwand klein. Risiko: keine Tests. Beleg: `src/resolve_linux_vst.cpp:71`, `src/resolve_linux_vst.cpp:770`, `src/resolve_linux_vst.cpp:841`, `resolve-linux-vst:36`, `resolve-linux-vst:251`.

### 7. Getestet ist nur die Mustersuche
`make test` baut und startet genau ein Testprogramm. Es prüft, ob die Stellen in Resolve gefunden werden. Ohne Test ist alles, was ohne Resolve prüfbar wäre:
- das Lesen von `plugins.conf`,
- das Umrechnen der Editorgröße,
- das Speicherformat des VST3-Zustands, also wie ein Projekt die Plugin-Einstellungen sichert,
- die Umbenennung von .NEV- in .R3D-Dateien,
- die Berechnung der Kategorien im Starter.

Jeder Umbau aus den Funden 1 bis 9 lässt sich deshalb nur von Hand mit echtem Resolve und echten Plugins prüfen. Vorschlag: Tests für diese Stellen zuerst, je in einem eigenen Commit, vor den Funden 8 und 9. Aufwand mittel. Beleg: `Makefile:38`, `src/resolve_linux_vst.cpp:196`, `src/resolve_linux_vst.cpp:510`, `src/vst3.cpp:1365`, `src/vst3.cpp:1387`, `src/nev.cpp:94`, `resolve-linux-vst:194`.

### 8. Die Hauptdatei des Laders hat sechs Aufgaben
Die Datei heißt nach dem VST2-Lader und beschreibt sich auch so. Sie enthält aber sechs Dinge:
- das Protokoll,
- das Lesen von `plugins.conf`,
- die Anpassung der Editorgröße, samt eigenem Zugriff auf das X11-Fenstersystem,
- die Steuerung der Wine-DPI, samt Start von Wine-Prozessen,
- das Laden von VST2-Plugins,
- die Eingriffe in den Scanner (Kategorien) und die Zahlenformat-Korrektur beim Start.

Sie ist mit 1089 Zeilen über der Grenze von 800 und hat vier Einträge in der Bestandsliste (`structure.baseline`, die Liste der Stellen, die eine Größengrenze heute schon überschreiten). Wer an der Wine-DPI arbeitet, muss die Editorlogik mitlesen und umgekehrt. Die Nähte liegen da, wo die Kommentarblöcke beginnen: Editorgröße mit Rahmen (Zeilen 244 bis 596), Wine-DPI (598 bis 848), Scanner-Eingriffe (985 bis 1080). Vorschlag: diese drei Blöcke in eigene Dateien; im Lader bleiben Laden und Start. Aufwand mittel, zwei bis drei PRs. Risiko hoch ohne die Tests aus Fund 7. Beleg: `src/resolve_linux_vst.cpp:1`, `src/resolve_linux_vst.cpp:244`, `src/resolve_linux_vst.cpp:598`, `src/resolve_linux_vst.cpp:985`, `structure.baseline:4`.

### 9. Bestandsliste: was sich trennen lässt und was bleiben kann
- `src/vst3.cpp` (1750 Zeilen) ist schon durch Abschnittsmarken gegliedert. Dazu gehören die Host-Objekte für das Plugin, die Ereignisschleife über glib (eine Bibliothek für Zeitgeber und Ereignisse, die Resolve mitbringt), das Laden der Module und der Adapter. Die Host-Objekte und die Ereignisschleife (Zeilen 193 bis 608) lassen sich ohne Änderung am Verhalten in eine eigene Datei legen. Dann liegt die Datei unter der Grenze.
- `Adapter::dispatch` (208 Zeilen, Verzweigungsmaß 101) ist eine flache Fallunterscheidung über die Befehlsnummern und im Kern in Ordnung. Nur die drei langen Fälle gehören in eigene Funktionen, wie es für den Editor schon geschehen ist: Lautsprecheranordnung, Kategorie und Parameterangaben.
- `Adapter::process` hat eine Naht: das Übersetzen der Zeitangaben von Resolve (Tempo, Takt, Position) in das VST3-Format.
- `Adapter::init` hat eine Naht: das Ausfüllen der VST2-Beschreibung am Ende.
- `dispatch` im Lader trennt sich am Fall „Editorgröße abfragen“.
- `runWine` trennt sich am Aufbau der Umgebungsvariablen.
- In Ordnung, wie sie sind: `pluginSetting` (kleiner Leser für die Konfiguration), `xcb` (eine Kette gleichartiger Abfragen), `ElfFile::imports` und `main` im Testwerkzeug `editor-test`.

Aufwand mittel, je ein kleiner PR. Risiko: keine Tests für den Adapter, siehe Fund 7. Beleg: `src/vst3.cpp:193`, `src/vst3.cpp:470`, `src/vst3.cpp:1416`, `src/vst3.cpp:1472`, `src/vst3.cpp:1531`, `src/vst3.cpp:1550`, `src/vst3.cpp:1199`, `src/vst3.cpp:1042`, `src/resolve_linux_vst.cpp:510`, `src/resolve_linux_vst.cpp:664`.

## Seit dem letzten Audit
Erstes Audit.
