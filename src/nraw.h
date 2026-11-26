// N-RAW auf der GPU (nraw.cpp): hängt sich in die Dekodierfunktion von fastticolib in Resolves
// RED-Bibliothek (REDR3D-x64.so).
#pragma once

// Versucht, die Dekodierfunktion zu übernehmen. Vom NEV-Teil (nev.cpp) bei jedem auf eine
// .NEV-Datei umgeleiteten Pfad gerufen: Ist REDR3D noch nicht geladen, klappt es beim nächsten.
// Idempotent und threadsicher; ohne RESOLVE_LINUX_VST_NRAW_GPU geschieht nichts.
void nrawAttach();
