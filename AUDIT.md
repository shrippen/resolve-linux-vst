# Code-Audit resolve-linux-vst, 26.11.2026, Stand 0f6bcd2

## Urteil
Das Repo ist weiterhin gut wartbar. Es ist aber um ein Drittel gewachsen, fast nur durch die N-RAW-Dekodierung (PR #42 und #43). Der neue Teil hält sich an den Aufbau in `ARCHITECTURE.md` und an alle Grenzen der Strukturprüfung. Die Bestandsliste ist nicht gewachsen, und `make test` läuft auf 0f6bcd2 grün. Schwächer ist das Repo an zwei Stellen. Die festen Zahlen des N-RAW-Formats stehen an bis zu vier Stellen, die niemand gegeneinander prüft. Die Datei, die sich in RED einhängt, enthält zwei Betriebsarten mit doppelter Logik. Die vier Funde des letzten Audits sind alle noch offen, weil seitdem nur an N-RAW gearbeitet wurde.

## Kennzahlen
| | letztes Audit | jetzt |
|---|---|---|
| Quelldateien | 53 | 71 |
| Einträge in der Bestandsliste | 4 | 4 |
| größte Datei (Zeilen) | 530 | 688 |
| längste Funktion (Zeilen) | 153 | 153 |

Gezählt habe ich wie beim letzten Mal: `src/`, `tools/`, `tests/`, der Starter, `install.sh`, `start.sh` und `Makefile`, ohne die Strukturprüfung und ohne den FFmpeg-Patch. Die größte Datei ist jetzt `src/nraw_cuda.cpp`, der Teil, der die Grafikkarte steuert. Die längste Funktion ist weiterhin `Adapter::dispatch` in `src/vst3_dispatch.cpp`. Die Strukturprüfung meldet 70 Dateien, keine Überschreitung und 4 Einträge in der Bestandsliste. Die Programme für die Grafikkarte (`src/nraw_cuda.cu`, `src/nraw_cuda_entropy.cuh`) prüft sie nicht. Ich habe sie von Hand mit demselben Werkzeug gemessen. Sie liegen innerhalb der Grenzen: 521 Zeilen in der größten Datei, höchstens 48 Zeilen und Verzweigungsmaß 14 je Funktion. Dass die Prüfung diese Dateien auslässt, liegt an der Strukturprüfung selbst, die aus dem Regel-Repo kommt. Beheben lässt es sich nur dort.

## Funde

### 1. Die festen Zahlen des N-RAW-Formats stehen an bis zu vier Stellen
Der Decoder kennt den Aufbau eines N-RAW-Bildes über feste Zahlen. Ein Bild hat 8 Pakete je Bildstreifen, 16 Bildstreifen je Abschnitt und 25 Einträge in der Tabelle, die festlegt, wie viel abgeschnitten wird. Je 4 Werte bilden eine Gruppe, 7 Ketten laufen parallel. Dazu kommt die Tabelle, welches Paket welche Art ist. Diese Zahlen und die Tabelle stehen einmal für die CPU (`nraw_frame.h`, `nraw_decode.cpp`, `nraw_entropy.cpp`). Sie stehen ein zweites Mal in der gemeinsamen Schnittstelle zur Grafikkarte (`nraw_gpu.h`) und ein drittes Mal in den Programmen für die Grafikkarte (`nraw_cuda_entropy.cuh`, `nraw_cuda.cu`). Einige stehen ein viertes Mal im Steuerteil der Grafikkarte (`nraw_cuda.cpp`). Keine Stelle prüft, dass sie übereinstimmen. Die gemeinsame Schnittstelle hat für 8, 25 und 6 eigene Namen, aber die CPU-Seite nutzt sie nicht.

Unterstützt eine spätere Nikon-Kamera einen etwas anderen Aufbau, etwa andere Bildstreifen je Abschnitt, muss man drei bis vier Dateien gleich ändern. Vergisst man eine, rechnet die Grafikkarte mit falschen Grenzen. Der Fehler fällt dann nur im lokalen Bit-Vergleich auf. Vorschlag: Die Formatzahlen und die Pakettabelle stehen einmal in `nraw_gpu.h`, das CPU und Grafikkarte schon beide einbinden, und alle anderen Stellen nutzen diese Namen. Aufwand klein. Risiko gering: Jede vergessene Stelle fällt beim Bauen oder im Bit-Vergleich `make nraw-test` auf. Der läuft aber nur auf dem Rechner des Eigentümers, mit privaten Prüfbildern. Beleg: `src/nraw_frame.h:12`, `src/nraw_frame.h:14`, `src/nraw_frame.h:17`, `src/nraw_frame.h:19`, `src/nraw_frame.h:33`, `src/nraw_gpu.h:49`, `src/nraw_decode.cpp:52`, `src/nraw_decode.cpp:367`, `src/nraw_entropy.cpp:15`, `src/nraw_cuda_entropy.cuh:21`, `src/nraw_cuda_entropy.cuh:25`, `src/nraw_cuda_entropy.cuh:51`, `src/nraw_cuda_entropy.cuh:57`, `src/nraw_cuda.cpp:47`, `src/nraw_cuda.cpp:50`, `src/nraw_gpu.cpp:29`, `src/nraw_color.cpp:36`, `src/nraw_cuda.cu:19`.

### 2. Die Datei, die sich in RED einhängt, hat zwei Betriebsarten, und beide bauen dasselbe
`src/nraw.cpp` übernimmt die Dekodierfunktion in Resolves RED-Bibliothek. Mit `RESOLVE_LINUX_VST_NRAW_DECODE=1` dekodiert der eigene Decoder. Mit `=probe` misst die Datei nur, für die Fehlersuche nach einem Resolve-Update. Der Messteil macht rund 215 der 578 Zeilen aus. Beide Betriebsarten tun zum Teil dasselbe, jede auf ihre eigene Weise:
- Beide beobachten, ob sich die drei unbekannten Aufrufwerte ändern, und schreiben das einmal ins Protokoll.
- Beide dekodieren ein Bild zusätzlich selbst, vergleichen es mit RED und zählen die abweichenden Bildpunkte.
- Beide messen Zeit und schreiben alle 200 Bilder eine Statistikzeile.

Wer ändern will, wie ein Unterschied zu RED gemeldet oder abgelegt wird, muss das an zwei Stellen tun. Wer nur den Messteil braucht, liest sich durch den Decoderteil und umgekehrt. Vorschlag: Der Vergleich mit RED und das Beobachten der Aufrufwerte stehen je einmal und werden von beiden Betriebsarten genutzt. Der Messteil kommt in eine eigene Datei neben `nraw.cpp`. Aufwand klein. Risiko: Für das Einhängen gibt es keinen automatischen Test (siehe Fund 6), nur einen Lauf in Resolve mit N-RAW-Material. Beleg: `src/nraw.cpp:204`, `src/nraw.cpp:265`, `src/nraw.cpp:291`, `src/nraw.cpp:305`, `src/nraw.cpp:348`, `src/nraw.cpp:409`, `src/nraw.cpp:440`, `src/nraw.cpp:465`, `src/nraw.cpp:474`.

### 3. Die Datei mit den alten DPI-Werten wird an zwei Stellen gelesen, und das auf verschiedene Weise (offen seit dem letzten Audit)
Bevor die Bibliothek die Wine-DPI eines Plugins ändert, schreibt sie den alten Wert in eine Datei. Die Wine-DPI ist die Bildschirmauflösung, die Wine einem Windows-Plugin meldet. Die Bibliothek liest diese Datei selbst, und das Werkzeug `wine-dpi-restore` liest sie noch einmal. Beide Leser verhalten sich verschieden. Die Bibliothek macht aus einem unlesbaren Wert stillschweigend 0. Das Werkzeug lässt dieselbe Zeile für immer stehen. Die Sonderwerte „-1“ und „-2“ stehen meist als nackte Zahl im Code, und „-2“ hat zwei Bedeutungen.

Wer das Format ändert, muss beide Leser gleich ändern. Vorschlag: Das Lesen einer Zeile steht einmal in `wine_restore.h`, wo es schon einen Test gibt, und jeder Sonderwert bekommt einen Namen. Aufwand klein. Risiko gering. Beleg: `src/wine_dpi.cpp:191`, `src/wine_dpi.cpp:202`, `src/wine_dpi.cpp:220`, `src/wine_restore.h:22`, `tools/wine-dpi-restore.cpp:33`, `src/wine_dpi.cpp:142`, `src/wine_dpi.cpp:148`, `src/wine_dpi.cpp:153`, `src/wine_dpi.cpp:180`, `src/wine_dpi.cpp:303`, `src/wine_dpi.h:60`.

### 4. Die Datei der Wine-DPI hat zwei Aufgaben, darum baut das Werkzeug fremde Teile mit und verliert seine Fehlergründe (offen seit dem letzten Audit)
`src/wine_dpi.cpp` hat zwei Aufgaben. Sie spricht mit Wine, und sie entscheidet in Resolve, welchen Wert ein Plugin bekommt. Das Werkzeug `wine-dpi-restore` braucht nur das Erste, baut aber sechs Quelldateien ein statt zwei. Es öffnet kein Protokoll. Scheitert Wine, sieht der Nutzer nur „not reset“ und keinen Grund.

Jede Änderung an der Entscheidung in Resolve ändert auch, wie das Werkzeug gebaut wird. Vorschlag: Der Aufruf von Wine kommt in eine eigene kleine Datei unter der Wine-DPI. Das Werkzeug baut nur sie ein und schreibt das Protokoll auf die Fehlerausgabe, wie `vst3-classes`. Aufwand klein. Risiko: Es gibt keinen automatischen Test für den Aufruf von Wine, aber der Build prüft jede verschobene Stelle. Beleg: `Makefile:54`, `src/wine_dpi.cpp:91`, `src/wine_dpi.cpp:128`, `src/wine_dpi.cpp:173`, `src/wine_dpi.cpp:333`, `tools/wine-dpi-restore.cpp:57`, `tools/vst3-classes.cpp:13`.

### 5. Die Editorgröße spricht selbst das Protokoll des Fenstersystems (offen seit dem letzten Audit)
Die Editorgröße lädt die Bibliothek des Fenstersystems X11 (xcb) von Hand, beschreibt deren Datenstrukturen nach und nutzt deren Kennzahlen als nackte Zahlen. Das sind rund 110 der 335 Zeilen. Laut `RULES.md` gehört so ein Zugriff in eine eigene Schicht mit einer einfachen Schnittstelle.

Wer das Rechnen der Größen ändert, muss sich durch den Fensterzugriff lesen und umgekehrt. Vorschlag: Der Teil mit xcb kommt in eine eigene Datei unter der Editorgröße, mit einer einzigen Funktion nach außen. Aufwand klein. Risiko: Es gibt keinen Test, nur `editor-test` oder Resolve mit einem yabridge-Plugin. Beleg: `src/editor_size.cpp:96`, `src/editor_size.cpp:118`, `src/editor_size.cpp:139`, `src/editor_size.cpp:169`, `src/editor_size.cpp:192`, `src/editor_size.cpp:259`.

### 6. Was ohne Resolve läuft, ist zum Teil ungetestet, und N-RAW lässt sich nur auf einem Rechner ganz prüfen
Seit dem letzten Audit sind zwei N-RAW-Tests in `make test` dazugekommen: einer für beschädigte Bilder und einer für das Aufbewahren der Kennlinien. Ohne Test bleiben:
- die Umbenennung von .NEV in .R3D,
- das Lesen und Bilden der AAC-Angaben,
- das Speicherformat des VST3-Zustands,
- das Umrechnen der Editorgröße,
- die Kategorien im Starter.

Neu ohne Test ist die Entscheidung im N-RAW-Einhängen, wann ein Bild an RED zurückgeht und wann ein Prüfbild fällig ist. Der Bit-Vergleich mit RED und alles, was auf der Grafikkarte läuft, lässt sich nur mit `make nraw-test` prüfen. Das geht nur auf dem Rechner des Eigentümers, mit privaten Prüfbildern und einer NVIDIA-Karte. Die CI prüft davon nichts. `ARCHITECTURE.md` führt die fehlenden Tests richtig als bekannte Abweichung.

Jeder Umbau an N-RAW (Funde 1 und 2) braucht deshalb einen Lauf auf diesem Rechner. Jeder Umbau an NEV, AAC und Editorgröße braucht Resolve. Vorschlag: Die reinen Rechnungen von NEV und AAC und die Entscheidung im N-RAW-Einhängen kommen in kleine eigene Teile, wie `wine_base.h` und `nraw_curves.h`, je mit Test und in einem eigenen PR. Danach folgt die Editorgröße, vor Fund 5. Aufwand mittel. Beleg: `src/nev.cpp:55`, `src/nev.cpp:95`, `src/aac.cpp:204`, `src/aac.cpp:407`, `src/vst3_dispatch.cpp:185`, `src/editor_size.cpp:212`, `src/nraw.cpp:487`, `Makefile:109`, `Makefile:116`, `ARCHITECTURE.md:117`.

### 7. Drei Beschreibungen der N-RAW-Dateien sagen etwas Falsches
- Der Kopf von `nraw.h` nennt die Datei „N-RAW auf der GPU“. Sie hängt sich aber nur in RED ein und dekodiert auf CPU oder Grafikkarte.
- Der Kopf von `nraw_decode.h` sagt, der Decoder kenne „das Log nicht“. Sein Teil für die Grafikkarte (`nraw_gpu.cpp`) schreibt aber ins Protokoll. Deshalb muss jeder Test, der den Decoder einbaut, auch `log.cpp` mitbauen.
- `ARCHITECTURE.md` nennt in der Liste der Bausteine `nraw_cuda_entropy.cuh` nicht, den Teil der Grafikkarte, der die Daten auspackt.

Wer der Beschreibung glaubt, sucht an der falschen Stelle oder baut den Decoder ohne Protokoll ein und bekommt einen Fehler beim Bauen. Vorschlag: die drei Sätze richtigstellen. Aufwand klein. Risiko keines. Beleg: `src/nraw.h:1`, `src/nraw_decode.h:1`, `src/nraw_gpu.cpp:17`, `src/nraw_gpu.cpp:65`, `Makefile:106`, `ARCHITECTURE.md:32`.

### 8. Bestandsliste: alle vier Einträge können bleiben, wie sie sind
Die Liste hat wie beim letzten Mal 4 Einträge, und der neue N-RAW-Code hat keinen dazugebracht.
- `Adapter::dispatch` (153 Zeilen, Verzweigungsmaß 80) ist eine flache Fallunterscheidung über die Befehle von Resolve. Sie ist in Ordnung, wie sie ist.
- `xcb` ist eine Kette gleichartiger Abfragen. Sie zieht mit Fund 5 um.
- `pluginSetting` und `ElfFile::imports` sind kurze Leseschleifen knapp über der Grenze. Sie sind in Ordnung.

Es ist kein Umbau nötig. Beleg: `structure.baseline:4`, `structure.baseline:7`.

## Seit dem letzten Audit
Seit dem Audit vom 16.11.2026 (Stand f0e1140) kamen die N-RAW-Dekodierung auf CPU und Grafikkarte, ihre Tests und eine Regeländerung dazu. Keiner der alten Funde wurde bearbeitet, und keiner ist als abgelehnt vermerkt.
- **Alter Fund 1, Datei der alten DPI-Werte:** offen, jetzt Fund 3. Die Dateien sind unverändert.
- **Alter Fund 2, zwei Aufgaben der Wine-DPI:** offen, jetzt Fund 4. Der Verweis ins `Makefile` steht jetzt in Zeile 54.
- **Alter Fund 3, Fensterzugriff in der Editorgröße:** offen, jetzt Fund 5.
- **Alter Fund 4, Tests:** offen, jetzt Fund 6, ergänzt um N-RAW.
- **Alter Fund 5, Bestandsliste:** unverändert in Ordnung, jetzt Fund 8.

Neu sind die Funde 1, 2 und 7, alle aus dem N-RAW-Teil. `make` und `make test` laufen auf 0f6bcd2 grün.
