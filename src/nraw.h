// N-RAW dekodieren (nraw.cpp): hängt sich in die Dekodierfunktion von fastticolib in Resolves
// RED-Bibliothek (REDR3D-x64.so); dekodiert wird auf der GPU oder der CPU (nraw_decode.h).
#pragma once

// Versucht, die Dekodierfunktion zu übernehmen. Vom NEV-Teil (nev.cpp) bei jedem auf eine
// .NEV-Datei umgeleiteten Pfad gerufen: Ist REDR3D noch nicht geladen, klappt es beim nächsten.
// Idempotent und threadsicher; mit RESOLVE_LINUX_VST_NRAW_DECODE=0 geschieht nichts.
void nrawAttach();
