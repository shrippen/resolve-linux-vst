# Code-Audit resolve-linux-vst, 16.11.2026, Stand 53320c0

## Urteil
Das Repo ist so wartbar wie beim letzten Audit, an einer Stelle etwas weniger. Keiner der neun Funde vom 14.11. ist behoben. `ARCHITECTURE.md` ist seitdem dazugekommen und führt sie als bekannte Abweichungen. Die Datei des Laders ist um 93 Zeilen auf 1182 gewachsen, und der Grenzwert in der Bestandsliste wurde angehoben. Der PR hat das begründet. Gewachsen ist die Steuerung der Wine-DPI: Sie ist jetzt der größte und schwierigste Teil dieser Datei, und einige ihrer Regeln stehen mehrfach darin. Besser geworden ist eines: Der Starter hat seinen ersten automatischen Test bekommen, und `make test` führt ihn aus.

## Kennzahlen
| | letztes Audit | jetzt |
|---|---|---|
| Quelldateien | 17 | 19 |
| Einträge in der Bestandsliste | 11 | 11 |
| größte Datei (Zeilen) | 1750 | 1750 |
| längste Funktion (Zeilen) | 208 | 208 |

## Funde

### 1. Das Protokoll (Log) gehört zur VST2-Datei, und es hat zwei Namen für dieselbe Aufgabe
Dieser Fund ist unverändert offen. Alle Teile schreiben in dasselbe Protokoll. Die Schreibfunktion steht aber in der Datei des VST2-Laders, und ihre Ankündigung (Deklaration) steht in der Kopfdatei der VST2-Schnittstelle. Deshalb binden die NEV- und die X11-Datei die VST2-Schnittstelle nur dafür ein. Das Hilfsprogramm `vst3-classes` baut sich eine eigene Kopie der Funktion. In der Hauptdatei gibt es außerdem zwei gleichwertige Funktionen, `logf` und `logMessage`. Wer das Protokoll ändert, muss die VST2-Datei anfassen und dabei zwei Funktionen pflegen. Vorschlag: eine eigene kleine Protokolldatei mit einer einzigen Funktion. Aufwand klein. Risiko gering: Der Build zeigt jeden vergessenen Aufruf. Beleg: `src/resolve_linux_vst.cpp:61`, `src/resolve_linux_vst.cpp:234`, `src/vst2.h:38`, `src/nev.cpp:33`, `src/x11.cpp:17`, `tools/vst3-classes.cpp:11`.

### 2. Die Kennzahlen der VST2-Schnittstelle stehen in zwei Dateien
Dieser Fund ist unverändert offen. Lader und VST3-Adapter haben je eine eigene Liste der Befehlsnummern, etwa „Editor öffnen“ = 14, und der Kennung „PreS/AeCs“. Dabei ist `vst2.h` laut eigenem Kommentar für beide da. Wer eine Nummer ergänzt oder berichtigt, muss beide Listen finden. Vorschlag: die Listen nach `vst2.h` verschieben und die Kopien löschen. Aufwand klein. Risiko gering: Der Build prüft es. Beleg: `src/resolve_linux_vst.cpp:249`, `src/resolve_linux_vst.cpp:256`, `src/vst3.cpp:713`, `src/vst3.cpp:734`.

### 3. „Läuft hier Resolve?“ ist dreimal gebaut, und die Bibliothek kennt jetzt den Namen eines Testwerkzeugs
Drei Dateien lesen jede für sich den Namen des laufenden Programms aus: Lader, AAC und NEV. Der Lader liest ihn an einer vierten Stelle noch einmal. Ändert Blackmagic den Programmnamen, müssen alle Stellen gefunden werden. Eine vergessene schaltet still eine Funktion ab.

Neu seit dem letzten Audit: Der Lader zählt jetzt auch das Testwerkzeug `editor-test` als Resolve, damit es die Wine-DPI wie Resolve setzt. Damit kennt die Bibliothek ein Werkzeug aus `tools/`. `ARCHITECTURE.md` erlaubt aber nur die umgekehrte Richtung. Außerdem greift die Korrektur des Zahlenformats beim Start dadurch auch im Testwerkzeug. Wer das Werkzeug umbenennt, schaltet die Wine-DPI im Plugin-Test still ab.

Vorschlag: eine Funktion an einer Stelle, die den Programmnamen liefert; jede Datei vergleicht dann nur noch. `editor-test` steht dort als ausdrücklich genannte Ausnahme, und `ARCHITECTURE.md` nennt sie. Aufwand klein. Risiko: keine Tests, prüfbar nur durch einen Start von Resolve. Beleg: `src/resolve_linux_vst.cpp:607`, `src/resolve_linux_vst.cpp:616`, `src/resolve_linux_vst.cpp:1123`, `src/resolve_linux_vst.cpp:1166`, `src/aac.cpp:574`, `src/nev.cpp:153`, `ARCHITECTURE.md:57`.

### 4. Die Regeln der Wine-DPI stehen mehrfach, im Starter und jetzt auch innerhalb der Bibliothek
Die Wine-DPI ist die Bildschirmauflösung, die Wine den Windows-Plugins meldet. Wie beim letzten Audit halten Starter (ein Shell-Skript) und Bibliothek drei Dinge von Hand gleich: den Ort der Datei mit den Vorwerten, ihr Zeilenformat („-1 = war nicht gesetzt“) und den Wine-Befehl samt Registry-Schlüssel.

Seitdem ist die Bibliothek gewachsen, und dabei sind neue Doppelungen in ihr selbst entstanden:
- Der Befehl, der die DPI schreibt, steht zweimal vollständig da: einmal beim Zurücksetzen vor einer neuen Wine-Sitzung und einmal beim Setzen für ein Plugin.
- Ob für ein Plugin „wine-zoom“ gilt, wird zweimal entschieden. Einmal geschieht es in der Rechnung der gewünschten DPI, einmal danach beim Ausrechnen der Vergrößerung. Vorher läuft dieselbe Rechnung schon einmal mit einem Platzhalterwert, nur um „nicht anfassen“ zu erkennen. `plugins.conf` wird dadurch je Plugin bis zu viermal gelesen.
- Der Standardwert 96 steht einmal als benannte Konstante und einmal als nackte Zahl da.

Wer die Bedeutung von `wine-zoom` ändert oder den Befehl anpasst (etwa für einen anderen Wine-Starter), muss alle Stellen finden. Eine vergessene verstellt die DPI still. Vorschlag: je eine Funktion für „DPI lesen“, „DPI schreiben“ und „Einstellung des Plugins bestimmen“, deren Ergebnis beide Rechnungen nutzen. Der Starter bekommt den Pfad der Datei von der Bibliothek bzw. gibt ihn ihr mit, wie beim letzten Mal vorgeschlagen. Aufwand klein. Risiko: Kein automatischer Test deckt das ab, siehe Fund 8. Beleg: `src/resolve_linux_vst.cpp:71`, `src/resolve_linux_vst.cpp:713`, `src/resolve_linux_vst.cpp:730`, `src/resolve_linux_vst.cpp:755`, `src/resolve_linux_vst.cpp:771`, `src/resolve_linux_vst.cpp:814`, `src/resolve_linux_vst.cpp:835`, `src/resolve_linux_vst.cpp:907`, `src/resolve_linux_vst.cpp:920`, `src/resolve_linux_vst.cpp:932`, `resolve-linux-vst:37`, `resolve-linux-vst:254`.

### 5. `ARCHITECTURE.md` beschreibt den Starter und die Tests nicht mehr ganz richtig
Seit PR #10 beendet der Starter vor dem Start, nach dem Ende von Resolve und bei `restore` Prozesse, die eine yabridge-Plugingruppe zurückgelassen hat. In der Tabelle der Bausteine fehlt diese Aufgabe. Außerdem steht unter den bekannten Abweichungen noch „nur die Mustersuche hat Tests“. Inzwischen gibt es auch den Test des Starters. Wer nach `ARCHITECTURE.md` entscheidet, wohin eine Änderung an den Gruppenprozessen gehört, findet sie dort nicht. Vorschlag: die Zeile des Starters und die Liste der Abweichungen nachziehen, zusammen mit der Ausnahme aus Fund 3. Aufwand klein, nur Text, kein Risiko. Beleg: `ARCHITECTURE.md:16`, `ARCHITECTURE.md:103`, `resolve-linux-vst:280`, `resolve-linux-vst:321`, `resolve-linux-vst:358`, `resolve-linux-vst:369`.

### 6. Der VST3-Adapter liest die Klassen eines Plugins auf zwei Wegen
Dieser Fund ist unverändert offen. Zweimal läuft dieselbe Schleife über die Plugins (Klassen) eines VST3-Pakets, jeweils mit demselben Ausweichweg für alte Plugins: einmal beim Laden und einmal für die Liste, aus der der Starter die Einträge anlegt. Wer eine weitere Angabe braucht oder den Ausweichweg ändert, ändert ihn zweimal. Vorschlag: eine Funktion, die alle Klassen mit ihren Angaben liefert, und beide Stellen nutzen sie. Aufwand klein. Risiko: keine Tests, prüfbar mit `vst3-classes` an einem Plugin. Beleg: `src/vst3.cpp:684`, `src/vst3.cpp:1716`.

### 7. Eingriffe in Resolves Speicher sind zweimal gebaut
Dieser Fund ist unverändert offen. Zweimal schreibt die Bibliothek einen Sprung in Resolves Programmcode: einmal für die Plugin-Kategorien im Scanner (Lader) und einmal für AAC. Beide Male gehören dieselben Schritte dazu: Speicher beschreibbar machen, schreiben, wieder schützen, den Zwischenspeicher des Prozessors leeren. Beide Male braucht es auch dasselbe Wissen über eine Tabelle in Resolves C++-Bibliothek, samt derselben Prüfung der Abstände. Ändert Resolve diese Bibliothek, muss das an zwei Stellen gleich nachgezogen werden. Vorschlag: eine gemeinsame Stelle für „Sprung schreiben“ und für den Aufbau der Tabelle. Aufwand klein bis mittel. Risiko hoch: Kein Test deckt das Schreiben ab, `resolve-linux-vst check` prüft nur das Finden. Beleg: `src/resolve_linux_vst.cpp:1050`, `src/aac.cpp:425`, `src/resolve_linux_vst.cpp:1085`, `src/aac.cpp:449`.

### 8. Die schwierigste neue Logik, die Basis der Wine-DPI, prüft nur ein Test, der echte Plugins braucht
`make test` prüft jetzt die Mustersuche und den Starter (verwaiste Gruppenprozesse). Seit PR #11 rechnet die Bibliothek aber mit einer „Basis-DPI“ je Wine-Sitzung. Davon hängt ab, wann sie die DPI zurücksetzt und ob ein Plugin das ganze Fenster füllt. Der Fehler, den PR #11 behoben hat, ließ die DPI mit jeder neuen Sitzung sinken (180 → 48 → 13 → 3). Das prüft nur `tests/wine-dpi.sh`. Dieser Test läuft nur von Hand (`make plugin-test`) und braucht installierte iZotope- und Waves-Plugins. In der CI und auf jedem Rechner ohne diese Plugins überspringt er sich. Dazu bleiben ohne Test wie beim letzten Audit:
- das Lesen von `plugins.conf`,
- das Umrechnen der Editorgröße,
- das Speicherformat des VST3-Zustands,
- die Umbenennung von .NEV in .R3D,
- die Kategorien im Starter.

Wer die Funde 3, 4 oder 9 umbaut, kann einen Rückfall in das Sinken der DPI nur mit echten Plugins bemerken. Vorschlag: die Entscheidung „welche Basis, muss zurückgesetzt werden?“ von den Wine-Aufrufen trennen. So kann ein Test in `make test` sie mit erfundenen Werten durchspielen, auch den Fall aus PR #11. Danach folgen die übrigen Tests, je in einem eigenen Commit, vor den Funden 9 und 10. Aufwand mittel. Beleg: `Makefile:38`, `Makefile:43`, `tests/wine-dpi.sh:28`, `src/resolve_linux_vst.cpp:867`, `src/resolve_linux_vst.cpp:196`, `src/resolve_linux_vst.cpp:510`, `src/vst3.cpp:1365`, `src/vst3.cpp:1387`, `src/nev.cpp:94`, `resolve-linux-vst:195`.

### 9. Die Hauptdatei des Laders hat sechs Aufgaben, und sie wächst
Die Datei heißt nach dem VST2-Lader, enthält aber sechs Dinge:
- das Protokoll,
- das Lesen von `plugins.conf`,
- die Editorgröße samt eigenem Zugriff auf das X11-Fenstersystem,
- die Steuerung der Wine-DPI samt Start von Wine-Prozessen,
- das Laden der Plugins,
- die Eingriffe in den Scanner samt Zahlenformat.

Seit dem letzten Audit ist sie von 1089 auf 1182 Zeilen gewachsen. Die Grenze liegt bei 800. Der Zuwachs kommt ganz aus der Wine-DPI; dieser Block umfasst jetzt rund 345 Zeilen (598 bis 941). Der Eintrag in der Bestandsliste wurde dafür angehoben, mit Grund im PR: Auslagern sei ein Umbau aus diesem Fund. Jede weitere Arbeit an der Wine-DPI hebt den Eintrag wieder an, solange der Block hier steht. Wer an ihm arbeitet, muss die Editorlogik mitlesen und umgekehrt. Die Nähte liegen bei den Kommentarblöcken: Editorgröße (244 bis 596), Wine-DPI (598 bis 941), Scanner und Zahlenformat (1047 bis 1173). Vorschlag: die Wine-DPI zuerst in eine eigene Datei, dann die beiden anderen Blöcke. Im Lader bleiben Laden und Start. Die Wine-DPI-Datei bleibt dabei mit rund 345 Zeilen unter der Grenze. Aufwand mittel, zwei bis drei PRs. Risiko hoch ohne die Tests aus Fund 8. Beleg: `src/resolve_linux_vst.cpp:1`, `src/resolve_linux_vst.cpp:244`, `src/resolve_linux_vst.cpp:598`, `src/resolve_linux_vst.cpp:1047`, `structure.baseline:4`.

### 10. Bestandsliste: was sich trennen lässt und was bleiben kann
Die Liste ist seit dem letzten Audit gleich geblieben, bis auf die Dateigröße des Laders (Fund 9).
- `src/vst3.cpp` (1750 Zeilen) ist durch Abschnittsmarken gegliedert. Die Host-Objekte und die Ereignisschleife (Zeilen 193 bis 608) lassen sich ohne Änderung am Verhalten in eine eigene Datei legen. Die Datei bleibt danach mit etwa 1330 Zeilen über der Grenze. Darunter kommt sie erst, wenn auch der Adapter (ab Zeile 711) eine eigene Datei wird. Mit rund 950 Zeilen läge diese Datei selbst über der Grenze. Der Adapter bräuchte dann eine weitere Naht, oder der Eintrag bleibt.
- `Adapter::dispatch` (208 Zeilen, Verzweigungsmaß 101) ist eine flache Fallunterscheidung über die Befehlsnummern und im Kern in Ordnung. Nur die drei langen Fälle gehören in eigene Funktionen: Parameterangaben, Kategorie und Lautsprecheranordnung.
- `Adapter::process` trennt sich am Übersetzen der Zeitangaben (Tempo, Takt, Position).
- `Adapter::init` trennt sich am Ausfüllen der VST2-Beschreibung am Ende.
- `dispatch` im Lader trennt sich am Fall „Editorgröße abfragen“.
- `runWine` trennt sich am Aufbau der Umgebungsvariablen.
- In Ordnung, wie sie sind: `pluginSetting`, `xcb` (eine Kette gleichartiger Abfragen), `ElfFile::imports` und `main` im Testwerkzeug `editor-test`.

Aufwand mittel, je ein kleiner PR. Risiko: keine Tests für den Adapter, siehe Fund 8. Beleg: `src/vst3.cpp:193`, `src/vst3.cpp:470`, `src/vst3.cpp:711`, `src/vst3.cpp:1416`, `src/vst3.cpp:1472`, `src/vst3.cpp:1531`, `src/vst3.cpp:1550`, `src/vst3.cpp:1191`, `src/vst3.cpp:979`, `src/resolve_linux_vst.cpp:510`, `src/resolve_linux_vst.cpp:655`.

## Seit dem letzten Audit
Behoben ist keiner der neun Funde vom 14.11.2026. Abgelehnt hat der Eigentümer keinen. Alle sind offen und stehen oben wieder, zum Teil erweitert:
- alter Fund 1 → jetzt 1
- alter Fund 2 → jetzt 2
- alter Fund 3 → jetzt 3, erweitert um `editor-test`
- alter Fund 4 → jetzt 6
- alter Fund 5 → jetzt 7
- alter Fund 6 → jetzt 4, erweitert um die neuen Doppelungen in der Bibliothek
- alter Fund 7 → jetzt 8, mit der Basis der Wine-DPI als wichtigster Lücke
- alter Fund 8 → jetzt 9; die Datei ist gewachsen
- alter Fund 9 → jetzt 10

Neu ist Fund 5. Seit dem Audit sind dazugekommen: `ARCHITECTURE.md` mit den Funden als bekannten Abweichungen, `tests/launcher-test.sh` in `make test` und der Plugin-Test `tests/wine-dpi.sh`, der nur von Hand läuft. `make test` läuft auf Stand 53320c0 grün.
