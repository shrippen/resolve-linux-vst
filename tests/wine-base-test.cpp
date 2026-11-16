// Test der Basis-DPI-Entscheidung (src/wine_base.h) mit erfundenen Werten, ohne Wine. Deckt den
// Fehler aus PR #11 ab: Die Basis muss der Wert vor Resolve bleiben, nicht der zuletzt gesetzte,
// sonst sinkt die DPI mit jeder Sitzung. Läuft ohne Resolve (make test).
#include "../src/wine_base.h"

#include <cmath>
#include <cstdio>
#include <optional>

namespace {

int failures = 0;

void expect(bool ok, const char *what)
{
    printf("%s  %s\n", ok ? "ok   " : "FEHLER", what);
    failures += !ok;
}

constexpr int kRest = 180;  // Ruhewert vor Resolve
constexpr int kZoomed = 48; // von einem Plugin gesetzt (wine-zoom)
constexpr double kZoomFactor = 3.75; // Resolves Faktor bei wine-zoom
constexpr int kSessions = 4;

bool is(const WineBase &b, int base, WineReset reset)
{
    return b.base == base && b.reset == reset;
}

} // namespace

int main()
{
    // PR #11: iZotope hat 48 gesetzt, Sitzung beendet, Vorwert 180 gemerkt.
    expect(is(wineBaseFor(WineSession::Stopped, kZoomed, kRest), kRest, WineReset::Yes),
           "Sitzung beendet, Registry 48, Vorwert 180: Basis 180, Zurücksetzen");

    // Mehrere Sitzungen nacheinander: Jede beginnt mit dem, was die vorige hinterließ (Basis
    // nach dem Zurücksetzen, dann setzt das Plugin Basis / Faktor). Die Basis bleibt der Vorwert.
    int registry = kRest;
    bool stable = true;
    for (int session = 0; session < kSessions; ++session) {
        const WineBase b = wineBaseFor(WineSession::Stopped, registry, kRest);
        stable = stable && b.base == kRest;
        registry = int(lround(b.base / kZoomFactor));
    }
    expect(stable, "Basis sinkt über mehrere Sitzungen nicht");

    expect(is(wineBaseFor(WineSession::Stopped, kRest, kRest), kRest, WineReset::No),
           "Registry gleich Vorwert: kein Zurücksetzen");

    // Laufende fremde Sitzung (dieser Prozess kennt den Prefix nicht), Vorwert gemerkt.
    expect(is(wineBaseFor(WineSession::Running, kZoomed, kRest), kRest, WineReset::No),
           "laufende Sitzung, Vorwert 180, Registry 48: Basis 180, kein Zurücksetzen");

    // Kein Vorwert: Basis ist der aktuelle Wert.
    expect(is(wineBaseFor(WineSession::Stopped, kRest, std::nullopt), kRest, WineReset::No),
           "ohne Vorwert: Basis = Registry, kein Zurücksetzen");
    expect(is(wineBaseFor(WineSession::Running, kRest, std::nullopt), kRest, WineReset::No),
           "ohne Vorwert, laufende Sitzung: Basis = Registry");

    // LogPixels nicht gesetzt (-1) zählt als 96.
    expect(is(wineBaseFor(WineSession::Stopped, -1, std::nullopt), kWineDefaultDpi, WineReset::No),
           "nicht gesetzt (-1) ohne Vorwert: Basis 96, kein Zurücksetzen");
    expect(is(wineBaseFor(WineSession::Stopped, -1, kWineDefaultDpi), kWineDefaultDpi, WineReset::No),
           "nicht gesetzt (-1), Vorwert 96: kein unnötiges Zurücksetzen");
    expect(is(wineBaseFor(WineSession::Stopped, kZoomed, -1), kWineDefaultDpi, WineReset::Yes),
           "Vorwert -1 (war nicht gesetzt), Registry 48: Basis 96, Zurücksetzen");
    expect(is(wineBaseFor(WineSession::Stopped, -1, kRest), kRest, WineReset::Yes),
           "Registry nicht gesetzt, Vorwert 180: Zurücksetzen auf 180");

    expect(wineDpiOrDefault(-1) == kWineDefaultDpi && wineDpiOrDefault(144) == 144, "nicht gesetzt: 96");

    return failures ? 1 : 0;
}
