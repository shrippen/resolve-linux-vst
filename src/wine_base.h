// Wine-DPI: Entscheidung über die Basis eines Prefix, rein gerechnet ohne Wine, Dateisystem und
// Log (Gründe und Hintergrund in wine_dpi.cpp). Eigene Datei, damit tests/wine-base-test.cpp sie
// ohne den Rest der Bibliothek baut.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>

constexpr int kWineDefaultDpi = 96; // LogPixels nicht gesetzt (Registry-Wert -1)

// Registry-Wert als DPI: nicht gesetzt (< 0) heißt Standard.
inline int wineDpiOrDefault(int registryDpi)
{
    return registryDpi < 0 ? kWineDefaultDpi : registryDpi;
}

enum class WineSession { Stopped, Running };

enum class WineReset { No, Yes };

struct WineBase {
    int base;
    WineReset reset; // DPI vor der nächsten Sitzung auf die Basis zurücksetzen
};

// Basis ist der Wert vor Resolve (gemerkter Vorwert, sonst der aktuelle Registry-Wert).
// Zurückgesetzt wird nur ohne laufende Sitzung und wenn der aktuelle Wert von der Basis
// abweicht, sonst startete die nächste Sitzung mit dem zuletzt gesetzten Wert.
inline WineBase wineBaseFor(WineSession session, int currentDpi, std::optional<int> remembered)
{
    const int base = wineDpiOrDefault(remembered ? *remembered : currentDpi);
    const bool differs = wineDpiOrDefault(currentDpi) != base;
    return {base, session == WineSession::Stopped && differs ? WineReset::Yes : WineReset::No};
}

// Einstellung eines Plugins, einmal aus den Texten der Konfiguration bestimmt.
enum class WineMode {
    Leave,  // DPI nicht anfassen, keine Wine-Aufrufe
    Fixed,  // feste DPI
    Prefix, // Basis-DPI des Prefix
    Zoom,   // Basis / Faktor, Fenster vergrößert der Lader (Instance::zoom)
};

struct WineSetting {
    WineMode mode;
    int dpi;     // nur bei Fixed
    double zoom; // nur bei Zoom
};

inline WineSetting wineFixed(int dpi)
{
    return {dpi > 0 ? WineMode::Fixed : WineMode::Leave, dpi, 1.0};
}

// wine-zoom: "auto" = Resolves Faktor, Zahl = fest, 0 oder leer = aus (0).
inline double wineZoomValue(const std::string &zoom, double scale)
{
    if (zoom == "auto") {
        return scale > 0 ? scale : 0;
    }
    const double value = atof(zoom.c_str());
    return value > 0 ? value : 0;
}

// wine-dpi: "auto" = 96 × Resolves Faktor, "prefix" = Basis, Zahl = fest (0 = nicht anfassen).
// Leer oder unbekannt: auto.
inline WineSetting wineDpiValue(const std::string &dpi, double scale)
{
    const char *v = dpi.c_str();
    while (*v == ' ' || *v == '\t') {
        ++v;
    }
    if (strncmp(v, "prefix", 6) == 0) {
        return {WineMode::Prefix, 0, 1.0};
    }
    char *end;
    const long n = strtol(v, &end, 10);
    // Was nicht in int passt, gilt wie vorher als auto (int(n) < 0).
    if (strncmp(v, "auto", 4) != 0 && end != v && n >= 0 && int(n) >= 0) {
        return wineFixed(int(n));
    }
    return wineFixed(int(lround(kWineDefaultDpi * scale)));
}

// Vorrang: wine-zoom > 0, dann wine-dpi, dann Umgebungsvariable (nur ohne wine-dpi), dann auto.
inline WineSetting wineSettingFor(const std::string &zoom, const std::string &dpi, const std::string &env,
                                  double scale)
{
    if (const double factor = wineZoomValue(zoom, scale); factor > 0) {
        return {WineMode::Zoom, 0, factor};
    }
    return wineDpiValue(dpi.empty() ? env : dpi, scale);
}

// DPI für ein Plugin bei gegebener Basis des Prefix (bei Leave 0).
inline int wineDpiFor(const WineSetting &setting, int base)
{
    switch (setting.mode) {
    case WineMode::Zoom:
        return std::max(1, int(lround(base / setting.zoom)));
    case WineMode::Prefix:
        return base;
    case WineMode::Fixed:
        return setting.dpi;
    case WineMode::Leave:
        break;
    }
    return 0;
}

// Faktor für Instance::zoom: nur bei Zoom Basis / DPI, sonst 1.
inline double wineZoomFor(const WineSetting &setting, int base, int wantedDpi)
{
    return setting.mode == WineMode::Zoom ? double(base) / wantedDpi : 1.0;
}
