// Welches Programm läuft die Bibliothek? Gelesen wird es einmal, hier; die Dateien vergleichen nur.
#pragma once

// Programmnamen (Basename von /proc/self/exe).
constexpr const char *kProgResolve = "resolve";
constexpr const char *kProgScanner = "VstScanner";
// Ausnahme: Das Testwerkzeug lädt wie Resolve, auch mit Wine-DPI (tests/wine-dpi.sh).
// Es zählt nur in isResolveProcess, nicht bei AAC und NEV.
constexpr const char *kProgEditorTest = "editor-test";

// Pfad des laufenden Programms, leer wenn nicht lesbar.
const char *exePath();

// Basename des laufenden Programms, leer wenn nicht lesbar.
const char *programName();

// Resolve, VstScanner oder editor-test.
bool isResolveProcess();
