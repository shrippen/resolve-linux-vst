// Wine-DPI: Entscheidung über die Basis eines Prefix, rein gerechnet ohne Wine, Dateisystem und
// Log (Gründe und Hintergrund in wine_dpi.cpp). Eigene Datei, damit tests/wine-base-test.cpp sie
// ohne den Rest der Bibliothek baut.
#pragma once

#include <optional>

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
