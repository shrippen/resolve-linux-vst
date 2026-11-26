// Test der N-RAW-Namen (src/nev_names.h): Endung erkennen, X.R3D <-> X.NEV mit erhaltener
// Schreibung, andere Schreibung. Erwartungen sind die Werte, die nev.cpp vor dem Auslagern
// lieferte. Läuft ohne Resolve (make test).
#include "../src/nev_names.h"

#include <cstdio>
#include <cstring>

namespace {

int failures = 0;

void expect(bool ok, const char *what)
{
    printf("%s  %s\n", ok ? "ok   " : "FEHLER", what);
    failures += !ok;
}

bool has(const char *name, const char *ext)
{
    return hasExtension(name, strlen(name), ext);
}

// Wendet f auf die Endung von name an und vergleicht mit want.
template <typename F>
bool turns(const char *name, F f, const char *want)
{
    char buf[64];
    strcpy(buf, name);
    f(buf + strlen(buf) - 3);
    return strcmp(buf, want) == 0;
}

} // namespace

int main()
{
    expect(has("DSC_0001.R3D", "r3d"), "Endung R3D groß");
    expect(has("a.r3d", "r3d"), "Endung r3d klein");
    expect(has("a.R3d", "r3d"), "Endung gemischt");
    expect(has("a.NEV", "nev"), "Endung NEV");
    expect(!has("a.NEV", "r3d"), "NEV ist kein r3d");
    expect(!has(".r3d", "r3d"), "ohne Namen kein Treffer");
    expect(!has("r3d", "r3d"), "ohne Punkt kein Treffer");
    expect(!has("ar3d", "r3d"), "Punkt an falscher Stelle");
    expect(!has("a.r3dx", "r3d"), "längere Endung");

    expect(turns("DSC_0001.R3D", toNev, "DSC_0001.NEV"), "R3D -> NEV");
    expect(turns("a.r3d", toNev, "a.nev"), "r3d -> nev");
    expect(turns("a.R3d", toNev, "a.NEv"), "R3d -> NEv");
    expect(turns("DSC_0001.NEV", toR3d, "DSC_0001.R3D"), "NEV -> R3D");
    expect(turns("a.nev", toR3d, "a.r3d"), "nev -> r3d");
    expect(turns("a.NEv", toR3d, "a.R3d"), "NEv -> R3d");

    expect(turns("a.NEV", otherSpelling, "a.nev"), "groß -> klein");
    expect(turns("a.nev", otherSpelling, "a.NEV"), "klein -> groß");
    expect(turns("a.nEV", otherSpelling, "a.NEV"), "gemischt (n klein) -> groß");

    printf("%s\n", failures ? "FEHLGESCHLAGEN" : "alles ok");
    return failures ? 1 : 0;
}
