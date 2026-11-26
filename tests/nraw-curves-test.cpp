// Ablage der N-RAW-Kennlinien: ein früher gelieferter Puffer bleibt gültig und unverändert.
#include <cstdio>

#include "nraw_curves.h"

namespace {

int gFailures = 0;

void expect(bool ok, const char *what)
{
    printf("%s %s\n", ok ? "ok  " : "FEHLER", what);
    if (!ok) {
        ++gFailures;
    }
}

} // namespace

int main()
{
    NrawCurves curves;
    const NrawCurveValues *a = curves.keep({0, 1, 1, 2});
    const NrawCurveValues *b = curves.keep({5, 6, 7, 8});
    expect(*a == NrawCurveValues{0, 1, 1, 2}, "erste Kennlinie unverändert nach einer zweiten");
    expect(*b == NrawCurveValues{5, 6, 7, 8}, "zweite Kennlinie gespeichert");

    const NrawCurveValues *again = curves.keep({0, 1, 1, 2});
    expect(again == a, "gleiche Tabelle teilt den Puffer");
    return gFailures == 0 ? 0 : 1;
}
