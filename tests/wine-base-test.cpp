// Test der Basis-DPI-Entscheidung und der Plugin-Einstellung (src/wine_base.h) mit erfundenen
// Werten, ohne Wine und plugins.conf. Deckt den
// Fehler aus PR #11 ab: Die Basis muss der Wert vor Resolve bleiben, nicht der zuletzt gesetzte,
// sonst sinkt die DPI mit jeder Sitzung. Läuft ohne Resolve (make test).
#include "../src/wine_base.h"

#include <cmath>
#include <cstdio>
#include <optional>
#include <string>

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

constexpr double kScale = 2.0; // Resolves Faktor (erfunden)

WineSetting setting(const char *zoom, const char *dpi, const char *env = "")
{
    return wineSettingFor(zoom, dpi, env, kScale);
}

bool dpiIs(const WineSetting &s, int base, int dpi)
{
    return wineDpiFor(s, base) == dpi;
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

    // Einstellung des Plugins: wine-zoom vor wine-dpi vor Umgebungsvariable vor auto.
    const WineSetting zoom = setting("4", "100");
    expect(zoom.mode == WineMode::Zoom && dpiIs(zoom, 180, 45), "wine-zoom 4, Basis 180: DPI 45");
    expect(wineZoomFor(zoom, 180, 45) == 4.0, "wine-zoom 4: Rückgabe-Zoom 4");
    expect(dpiIs(setting("1000", ""), 180, 1), "wine-zoom sehr groß: DPI mindestens 1");
    expect(dpiIs(setting("auto", ""), 180, 90), "wine-zoom auto, Faktor 2: Basis / 2");
    expect(setting("0", "0").mode == WineMode::Leave, "wine-zoom 0 fällt auf wine-dpi zurück");
    expect(dpiIs(setting("", "120"), 180, 120) && wineZoomFor(setting("", "120"), 180, 120) == 1.0,
           "wine-dpi 120: fest, Zoom 1");
    expect(setting("", "0").mode == WineMode::Leave, "wine-dpi 0: nicht anfassen");
    expect(dpiIs(setting("", "2147483648"), 180, 192), "wine-dpi über int hinaus: auto wie vorher");
    expect(setting("", "0", "144").mode == WineMode::Leave, "wine-dpi 0 geht der Umgebungsvariable vor");
    expect(dpiIs(setting("", "", "144"), 180, 144), "Umgebungsvariable nur ohne wine-dpi");
    expect(dpiIs(setting("", "120", "144"), 180, 120), "wine-dpi geht der Umgebungsvariable vor");
    expect(dpiIs(setting("", "prefix"), 180, 180) && setting("", "prefix").mode == WineMode::Prefix,
           "prefix: Basis des Prefix");
    expect(dpiIs(setting("", " auto"), 180, kWineDefaultDpi * 2), "auto: 96 × Faktor, Leerzeichen erlaubt");
    expect(dpiIs(setting("", ""), 180, kWineDefaultDpi * 2), "nichts gesetzt: auto");
    expect(dpiIs(setting("", "quatsch"), 180, kWineDefaultDpi * 2), "unbekannter Wert: auto");
    expect(dpiIs(setting("", "-5"), 180, kWineDefaultDpi * 2), "negativer Wert: auto");
    expect(wineSettingFor("", "auto", "", 0.0).mode == WineMode::Leave, "auto mit DPI 0: nicht anfassen");

    return failures ? 1 : 0;
}
