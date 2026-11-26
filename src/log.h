// Das Protokoll: eine Schreibfunktion, die Datei setzt der Aufrufer (Lader: openLog,
// vst3-classes: stderr). Ohne Datei bleibt logMessage stumm.
#pragma once

#include <cstdio>
#include <string>

void setLogFile(FILE *file);

// $XDG_STATE_HOME/resolve-linux-vst (Standard ~/.local/state/resolve-linux-vst), wird angelegt;
// leer ohne XDG_STATE_HOME und HOME.
std::string stateDir();

// Öffnet die Datei nach RESOLVE_LINUX_VST_LOG ("-" = stderr), sonst ~/.local/state/resolve-linux-vst/resolve-linux-vst.log.
void openLog();

// Eine Zeile mit Zeitstempel und PID, wie printf.
void logMessage(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
