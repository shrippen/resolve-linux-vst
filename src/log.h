// Das Protokoll: eine Schreibfunktion, die Datei setzt der Aufrufer (Lader: openLog in
// resolve_linux_vst.cpp, vst3-classes: stderr). Ohne Datei bleibt logMessage stumm.
#pragma once

#include <cstdio>

void setLogFile(FILE *file);

// Eine Zeile mit Zeitstempel und PID, wie printf.
void logMessage(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
