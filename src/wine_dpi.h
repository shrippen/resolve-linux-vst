// Wine-DPI: stellt die DPI des Wine-Prefix für ein yabridge-Plugin ein (Aufbau und Gründe
// siehe wine_dpi.cpp). Vom Lader aufgerufen, bevor er ein Windows-Plugin lädt.
#pragma once

#include <string>

#include "plugin_conf.h"

// Prefix eines yabridge-Plugins: VST2 (Name.so mit Name.dll daneben) bzw. VST3-Bundle;
// leer, wenn es kein Wine-Plugin ist.
std::string winePrefixFor(const std::string &so);
std::string winePrefixForBundle(const std::string &bundle);

// Stellt die Wine-DPI für ein Plugin ein, liefert den Faktor für Instance::zoom (Basis / DPI
// bei wine-zoom, sonst 1). `prefix` aus winePrefixFor bzw. winePrefixForBundle.
double syncWineDpi(const Names &names, const std::string &prefix);

constexpr int kWineDpiUnset = -1; // LogPixels war nicht gesetzt

// Setzt die DPI eines Prefix auf einen gemerkten Vorwert zurück; kWineDpiUnset löscht den Wert.
// Für tools/wine-dpi-restore.cpp, nicht an Resolves Prozess gebunden.
bool restoreWineDpi(const std::string &prefix, int dpi);
