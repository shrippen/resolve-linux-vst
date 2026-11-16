# Code-Audit resolve-linux-vst, 16.11.2026, Stand dde5d41

## Urteil
Das Repo ist deutlich besser wartbar als beim letzten Audit. Alle zehn Funde vom 16.11. (Stand 53320c0) wurden in den PRs #13 bis #23 bearbeitet. Neun sind behoben, einer zum Teil: Bei den Tests fehlt noch viel. Das sieht man an den Zahlen. Die größte Datei hat statt 1750 jetzt 648 Zeilen, und die Bestandsliste hat statt 11 jetzt 6 Einträge. Jeder Teil hat seine eigene Datei, und `make test` läuft auf dde5d41 grün. Übrig sind kleinere Doppelungen, an denen `ARCHITECTURE.md` noch nicht eingehalten ist: die Namen der umgeleiteten Funktionen in Resolve und die Zahlen der VST2-Schnittstelle. Dazu kommen Lücken bei den Tests.

## Kennzahlen
| | letztes Audit | jetzt |
|---|---|---|
| Quelldateien | 19 | 43 |
| Einträge in der Bestandsliste | 11 | 6 |
| größte Datei (Zeilen) | 1750 | 648 |
| längste Funktion (Zeilen) | 208 | 153 |

Die Quelldateien sind wie beim letzten Mal gezählt: `src/`, `tools/`, `tests/`, Starter, `install.sh`, `start.sh` und `Makefile`, ohne die Strukturprüfung und ohne den FFmpeg-Patch. Die größte Datei ist jetzt `src/aac.cpp`. Die längste Funktion ist `Adapter::dispatch` in `src/vst3_dispatch.cpp`.

## Funde

### 1. Die Namen der umgeleiteten Funktionen in Resolve stehen dreimal, und die Umleitung ist zweimal gebaut
Im Programm VstScanner leitet die Bibliothek vier Funktionen auf eigene um. Sie findet sie über ihren Namen in Resolves Programmdatei (Symbol). Zwei davon gehören dem Lader (Plugin laden und schließen), zwei dem Scanner-Teil (Kategorien). Beide Dateien bauen dafür dieselben Schritte: Programmdatei öffnen, jeden Namen suchen, einen Sprung schreiben und eine Protokollzeile ausgeben. Das Prüfwerkzeug `resolve-check` führt dieselben Namen ein drittes Mal als eigene Liste.

Benennt Blackmagic eine dieser Funktionen um, müssen zwei bis drei Stellen gleich geändert werden. Vergisst man das Prüfwerkzeug, meldet `resolve-linux-vst check` „passt“, obwohl der Teil still aus ist, oder umgekehrt. Laut `ARCHITECTURE.md` gehört das Wissen über Resolves Programmdatei nach `sites`.

Vorschlag: Die Namen kommen als eine Liste nach `sites.h`. Eine einzige Funktion „Funktionen des Programms umleiten“ kommt nach `patch.cpp`, und Lader und Scanner-Teil rufen sie auf. `resolve-check` prüft dieselbe Liste. Aufwand klein. Risiko gering: Die Mustersuche und das Schreiben des Sprungs haben Tests. Ob die Umleitung im Scanner greift, zeigt aber nur ein Start von Resolve. Beleg: `src/resolve_linux_vst.cpp:205`, `src/resolve_linux_vst.cpp:216`, `src/resolve_linux_vst.cpp:223`, `src/scanner.cpp:48`, `src/scanner.cpp:59`, `src/scanner.cpp:67`, `src/scanner.cpp:69`, `tools/resolve-check.cpp:105`, `tools/resolve-check.cpp:111`.

### 2. Ein Teil der VST2-Schnittstelle steht nicht in `vst2.h`, sondern als nackte Zahlen im VST3-Adapter
`ARCHITECTURE.md` legt fest, dass die Zahlen der VST2-Schnittstelle einmal stehen, und zwar in `vst2.h`. Die Befehlsnummern stehen inzwischen dort (alter Fund 2). Andere Teile der Schnittstelle definiert der VST3-Adapter aber selbst:
- den Aufbau der Zeitangaben (Tempo, Takt, Position) samt ihren Merkmalen,
- den Aufbau der Angaben zu Ein- und Ausgängen und zu Parametern,
- die Nummern der Plugin-Kategorien (2, 3, 4, 5, 6, 8, 11) und einige Merkmale, als bloße Zahlen, deren Bedeutung nur im Kommentar steht.

Das Testwerkzeug `editor-test` nutzt außerdem die Nummer 1 für „Host-Version“, die in `vst2.h` fehlt. Wer etwas an der VST2-Schnittstelle sucht oder ergänzt, findet in `vst2.h` nur die Hälfte. Braucht der Lader eines Tages die Zeitangaben, baut er sie ein zweites Mal.

Vorschlag: Aufbau und benannte Konstanten nach `vst2.h` verschieben und im Adapter nur noch die Namen verwenden. Aufwand klein. Risiko gering: Die Werte ändern sich nicht, und der Build prüft jede Stelle. Beleg: `src/vst3.cpp:26`, `src/vst3.cpp:30`, `src/vst3_dispatch.cpp:18`, `src/vst3_dispatch.cpp:24`, `src/vst3_dispatch.cpp:42`, `src/vst3_dispatch.cpp:44`, `src/vst3_dispatch.cpp:52`, `src/vst3_dispatch.cpp:316`, `tools/editor-test.cpp:51`, `ARCHITECTURE.md:79`.

### 3. „Läuft hier Resolve?“ hat drei Bedeutungen, und das Testwerkzeug zählt auch beim Zahlenformat als Resolve
Den Programmnamen liest die Bibliothek jetzt nur noch an einer Stelle (alter Fund 3). Die Frage danach heißt aber dreimal ähnlich und meint jedes Mal etwas anderes:
- `isResolveProcess` heißt „resolve, VstScanner oder das Testwerkzeug `editor-test`“.
- AAC und NEV fragen jeweils mit einer eigenen, gleichen kleinen Funktion, ob das Programm genau „resolve“ ist.
- Im AAC-Teil heißt ein Wert `isResolve`, meint aber „Resolves FFmpeg in Version 60 gefunden“.

`process.h` und `ARCHITECTURE.md` sagen, dass `editor-test` nur für die Wine-DPI als Resolve zählt. Der Scanner-Teil nutzt dieselbe Funktion aber auch für die Korrektur des Zahlenformats. Damit ändert auch das Testwerkzeug sein Zahlenformat. Wer die Ausnahme für das Testwerkzeug ändert oder einen weiteren Programmnamen braucht, kann am Namen der Funktion nicht erkennen, welche Teile mitbetroffen sind.

Vorschlag: In `process.h` kommen zwei Fragen, deren Namen sagen, was sie fragen, etwa „ist das Hauptprogramm resolve“ und „lädt Plugins wie Resolve“. AAC und NEV nutzen die erste, und der Wert im AAC-Teil bekommt einen eigenen Namen. Danach entscheidet der Eigentümer, ob das Zahlenformat im Testwerkzeug gelten soll, und Code oder Doku folgen. Aufwand klein. Risiko: Der Test prüft nur das Lesen des Namens, alles andere zeigt nur ein Start von Resolve. Beleg: `src/process.h:8`, `src/process.h:18`, `src/process.cpp:42`, `src/aac.cpp:64`, `src/aac.cpp:536`, `src/nev.cpp:154`, `src/scanner.cpp:90`, `ARCHITECTURE.md:23`.

### 4. Das Einfügen in Resolves Codec-Tabelle steht im AAC-Teil und hat keinen Test, obwohl es ohne Resolve prüfbar wäre
Der AAC-Teil trägt zwei Einträge in eine Tabelle in Resolves Speicher ein. Dafür baut er rund 110 Zeilen nach, die eine Tabelle der C++-Bibliothek ausliest, einen Eintrag einfügt und den Baum dabei im Gleichgewicht hält, genau wie die Bibliothek selbst. Den Aufbau dieser Tabelle kennt laut `ARCHITECTURE.md` `patch.h`. Auch der Scanner-Teil liest dieselbe Art Tabelle mit eigenem Code.

Ändert sich die C++-Bibliothek, in der Resolve gebaut ist, muss das in `patch.h`, `aac.cpp` und `scanner.cpp` nachgezogen werden. Das Einfügen ist reine Rechnung ohne Resolve, hat aber keinen Test. Ein Fehler darin zeigt sich erst als Absturz oder stummer Ton in Resolve.

Vorschlag: Auslesen und Einfügen zu `MapNode` nach `patch.cpp` verschieben. `patch-test` fügt dann in eine selbst gebaute Tabelle ein und prüft Reihenfolge und Gleichgewicht. `aac.cpp` schrumpft dadurch auf etwa 540 Zeilen. Aufwand klein bis mittel. Risiko: Heute gibt es keinen Test, der Umbau bringt den ersten. Beleg: `src/aac.cpp:425`, `src/aac.cpp:437`, `src/aac.cpp:468`, `src/aac.cpp:518`, `src/aac.cpp:555`, `src/scanner.cpp:25`, `src/patch.h:37`.

### 5. `REVIEW.md` und `ARCHITECTURE.md` nennen Doppelungen, die es nicht mehr gibt, und die heutigen nicht
`REVIEW.md` lässt Prüfer nach Änderungen suchen, die nur eine von zwei Kopien treffen. Als Beispiele nennt es das Protokoll, „Läuft hier Resolve?“, das Schreiben eines Sprungs und die VST2-Nummern, mit Verweis auf `AUDIT.md`. Diese vier Kopien sind behoben. Die Abweichungen in den Funden 1, 2 und 9 dieses Berichts fehlen dagegen in der Liste der bekannten Abweichungen in `ARCHITECTURE.md`, obwohl die Datei alle führen soll.

Ein Prüfer sucht so nach Kopien, die nicht mehr da sind, und übersieht die, die da sind. Vorschlag: In `REVIEW.md` die Beispiele durch die Funde 1, 2 und 9 ersetzen und sie in `ARCHITECTURE.md` unter den bekannten Abweichungen ergänzen. Aufwand klein, nur Text. Risiko keines. Beleg: `REVIEW.md:42`, `ARCHITECTURE.md:102`, `ARCHITECTURE.md:104`.

### 6. Zwei Arten, einen Sprung in Resolve zu schreiben, melden Fehler verschieden
`patch.h` bietet drei Funktionen an. Zwei schreiben dieselbe Art Sprung, eine davon zusätzlich mit Protokollzeile samt Grund. Die dritte schreibt eine andere Art Sprung und meldet nichts. Lader und Scanner-Teil nutzen die erste Art und bekommen den Grund ins Protokoll. Der AAC-Teil nutzt die dritte und schreibt selbst nur „mprotect“ ohne Grund. Wer einen weiteren Eingriff baut, muss zwischen fast gleichen Funktionen wählen, und der Weg des Fehlers hängt an dieser Wahl.

Vorschlag: eine Funktion je Art Sprung, und beide melden den Grund auf dieselbe Weise. Aufwand klein. Risiko gering: `patch-test` deckt das Schreiben ab. Beleg: `src/patch.h:26`, `src/patch.h:29`, `src/patch.h:32`, `src/patch.cpp:49`, `src/patch.cpp:56`, `src/patch.cpp:66`, `src/aac.cpp:571`, `src/aac.cpp:578`.

### 7. Die Wine-DPI fragt die Editorgröße nach Resolves Skalierungsfaktor
Für „auto“ braucht die Wine-DPI den Skalierungsfaktor von Resolves Oberfläche. Diese Abfrage bei Qt steht in der Datei der Editorgröße, und die Wine-DPI bindet dafür deren Kopfdatei ein. So kennen sich zwei Teile des Laders, die nach `ARCHITECTURE.md` nebeneinander stehen. Die Datei führt das als bekannte Abweichung.

Wer an der Editorgröße arbeitet, muss die Wine-DPI mitbedenken. Wer den Faktor anders bestimmen will, etwa für einen anderen Bildschirm, ändert eine Datei, deren Aufgabe eine andere ist. Vorschlag: Die Abfrage des Faktors kommt in eine eigene kleine Datei unter beiden, und die Abweichung in `ARCHITECTURE.md` wird gestrichen. Aufwand klein. Risiko gering: Es ist ein reines Verschieben, das der Build prüft. Einen Test gibt es nicht. Beleg: `src/wine_dpi.cpp:28`, `src/wine_dpi.cpp:171`, `src/editor_size.cpp:56`, `src/editor_size.cpp:65`, `ARCHITECTURE.md:102`.

### 8. Viel Logik, die ohne Resolve läuft, hat noch keinen Test
`make test` prüft jetzt diese fünf Dinge:
- die Mustersuche,
- den Programmnamen,
- das Schreiben eines Sprungs,
- die Entscheidung über die Basis der Wine-DPI,
- das Aufräumen des Starters.

`ARCHITECTURE.md` verlangt für jede Logik, die ohne Resolve läuft, eine eigene Funktion mit Test. Ohne Test sind noch:
- das Lesen von `plugins.conf`, das sich so, wie es ist, testen ließe,
- die Umbenennung von .NEV in .R3D,
- das Lesen und Bilden der AAC-Angaben (AudioSpecificConfig),
- das Speicherformat des VST3-Zustands,
- das Umrechnen der Editorgröße,
- die Kategorien im Starter.

Bis auf `plugins.conf` stecken diese Rechnungen in Dateien, die Systemfunktionen ersetzen oder Resolve brauchen. Deshalb kann sie heute kein Test erreichen. Jeder Umbau dort, auch die Funde 3 und 4, lässt sich nur durch einen Start von Resolve prüfen. Vorschlag: zuerst ein Test für `plugins.conf`. Danach werden die reinen Rechnungen von NEV und AAC so herausgelöst wie bei der Wine-DPI (`wine_base.h`), je in einem eigenen PR mit Test. Aufwand mittel. Beleg: `src/plugin_conf.cpp:52`, `src/nev.cpp:55`, `src/nev.cpp:95`, `src/aac.cpp:191`, `src/aac.cpp:409`, `src/vst3_dispatch.cpp:157`, `src/editor_size.cpp:256`, `resolve-linux-vst:196`, `ARCHITECTURE.md:97`, `ARCHITECTURE.md:104`.

### 9. Starter und Bibliothek kennen beide den Wine-Befehl und den Registry-Schlüssel der DPI
Den Ort der Datei mit den Vorwerten gibt jetzt der Starter der Bibliothek mit (alter Fund 4). Zum Zurücksetzen nach dem Ende von Resolve ruft der Starter aber selbst Wine auf. Dazu gehören derselbe Registry-Schlüssel, derselbe Wertname, `WINELOADER` und `WINEDEBUG=-all`, also dieselben Regeln wie in der Bibliothek, nur als Shell-Skript. `ARCHITECTURE.md` erlaubt zwischen Starter und Bibliothek nur zwei vereinbarte Formate, nicht einen gemeinsamen Befehl.

Ändert sich, wie Wine gestartet wird, etwa mit einer weiteren Umgebungsvariable wie bei yabridge, muss es an beiden Stellen gleich geändert werden. Sonst setzt eine Seite die DPI, und die andere setzt sie nicht zurück. Vorschlag: Das Zurücksetzen bekommt ein kleines Werkzeug, das wie `resolve-check` aus `src/wine_dpi.cpp` gebaut wird, und der Starter ruft es auf. Andernfalls nennt `ARCHITECTURE.md` die Ausnahme ausdrücklich. Aufwand mittel. Risiko: Nur der Plugin-Test mit echten Plugins deckt das ab. Beleg: `resolve-linux-vst:253`, `resolve-linux-vst:255`, `resolve-linux-vst:264`, `src/wine_dpi.cpp:76`, `src/wine_dpi.cpp:93`, `src/wine_dpi.cpp:137`, `ARCHITECTURE.md:80`.

### 10. Bestandsliste: was sich trennen lässt und was bleiben kann
Die Liste hat statt 11 jetzt 6 Einträge. Keine Datei liegt mehr über 800 Zeilen.
- `Adapter::dispatch` (153 Zeilen, Verzweigungsmaß 80) ist eine flache Fallunterscheidung über die Befehle von Resolve. Die langen Fälle sind schon ausgelagert. In Ordnung, wie sie ist.
- `Adapter::init` (Verzweigungsmaß 17) trennt sich am Block „Controller suchen oder anlegen“.
- `main` im Testwerkzeug `editor-test` (149 Zeilen) trennt sich an seinen drei Abschnitten: Audio durchschicken, Parameter und Zustand prüfen, Fenster aufnehmen. Es ist ein Werkzeug, das selten geändert wird, also ist der Nutzen gering.
- In Ordnung, wie sie sind: `xcb` (eine Kette gleichartiger Abfragen), `pluginSetting` und `ElfFile::imports`.

Aufwand klein, je ein kleiner PR. Risiko: Für den Adapter gibt es keine Tests, siehe Fund 8. Beleg: `structure.baseline:4`, `structure.baseline:9`, `src/vst3_dispatch.cpp:217`, `src/vst3.cpp:107`, `tools/editor-test.cpp:101`.

## Seit dem letzten Audit
Neun der zehn Funde vom 16.11.2026 sind behoben, einer zum Teil. Kein Fund ist als abgelehnt vermerkt.
- **Alter Fund 1, Protokoll:** behoben. Es gibt eine eigene Protokolldatei mit einer einzigen Schreibfunktion, und NEV, X11 und `vst3-classes` binden nur noch sie ein.
- **Alter Fund 2, VST2-Befehlsnummern:** behoben, sie stehen nur noch in `vst2.h`. Andere Teile der Schnittstelle stehen noch im Adapter, siehe neuer Fund 2.
- **Alter Fund 3, Programmname:** im Kern behoben. Er wird nur noch in `process.cpp` gelesen, und `editor-test` ist dort als Ausnahme benannt. Übrig sind zwei gleiche kleine Vergleiche und die unklaren Namen, siehe neuer Fund 3.
- **Alter Fund 4, Wine-DPI:** in der Bibliothek behoben. Es gibt eine Funktion zum Schreiben der DPI, eine zum Bestimmen der Einstellung und eine Konstante für 96. Der Starter gibt der Bibliothek den Ort der Datei mit. Übrig ist der Wine-Aufruf im Starter, siehe neuer Fund 9.
- **Alter Fund 5, `ARCHITECTURE.md`:** behoben. Die Zeile des Starters und die Tests sind nachgezogen.
- **Alter Fund 6, VST3-Klassen:** behoben. Es gibt eine Funktion, die Lader und Liste beide nutzen.
- **Alter Fund 7, Sprung in Resolves Code:** behoben. Er steht jetzt in `patch.cpp` und hat einen Test. Übrig sind die zwei Arten der Fehlermeldung (neuer Fund 6) und das Einfügen in die Tabelle im AAC-Teil (neuer Fund 4).
- **Alter Fund 8, Tests:** zum Teil behoben. Neu sind die Tests für die Basis der Wine-DPI, den Programmnamen und das Schreiben des Sprungs. Der Rest ist offen, siehe neuer Fund 8.
- **Alter Fund 9, Datei des Laders:** behoben. Sie hat 241 statt 1182 Zeilen und lädt nur noch Plugins. `plugins.conf`, Editorgröße, Wine-DPI und Scanner haben eigene Dateien. Übrig ist die Abhängigkeit aus neuem Fund 7, die `ARCHITECTURE.md` als Abweichung führt.
- **Alter Fund 10, Bestandsliste:** behoben. Der VST3-Adapter ist auf fünf Dateien verteilt. `Adapter::process`, `runWine` und der Dispatcher des Laders stehen nicht mehr auf der Liste.

Neu sind die Funde 1, 4, 5, 6 und 7. Die Funde 2, 3, 8 und 9 sind Reste alter Funde. `make test` läuft auf dde5d41 grün. Die Strukturprüfung meldet 44 Dateien, keine Überschreitung und 6 Einträge in der Bestandsliste.
