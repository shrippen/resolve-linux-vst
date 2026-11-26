// Wine-Aufruf der Wine-DPI: liest, schreibt und löscht LogPixels in der Registry eines Prefix
// über `wine reg`. Kennt weder Resolve noch die Entscheidung, welchen Wert ein Plugin bekommt
// (wine_dpi.cpp); darum baut wine-dpi-restore nur diese Datei ein.
#pragma once

#include <string>

constexpr int kWineQueryFailed = -2; // `wine reg query` war nicht möglich

// Aktueller Wert; kWineDpiUnset (wine_restore.h) = nicht gesetzt, kWineQueryFailed = Abfrage
// nicht möglich.
int queryWineDpi(const std::string &prefix);

// Schreibt die DPI des Prefix.
bool writeWineDpi(const std::string &prefix, int dpi);

// Setzt die Wine-DPI auf einen gemerkten Vorwert zurück (Aufrufer: wine-dpi-restore);
// kWineDpiUnset löscht den Wert.
bool restoreWineDpi(const std::string &prefix, int dpi);
