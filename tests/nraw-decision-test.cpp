// Entscheidung im N-RAW-Hook: wann RED dekodiert, wann ein Prüfbild fällig ist.
#include <cstdio>

#include "nraw_decision.h"

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
    expect(!nrawToRed(false, 0, 0, 8), "Normalfall: eigener Decoder");
    expect(nrawToRed(true, 0, 0, 8), "abgeschaltet: RED");
    expect(nrawToRed(false, 1, 0, 8), "a8 != 0: RED");
    expect(nrawToRed(false, 0, -1, 8), "a9 != 0: RED");
    expect(nrawToRed(false, 0, 0, 0), "dstBytes 0: RED");
    expect(nrawToRed(false, 0, 0, 7), "dstBytes ungerade: RED");

    expect(nrawVerifyDue(1) && nrawVerifyDue(2) && nrawVerifyDue(3), "Aufrufe 1 bis 3 prüfen");
    expect(!nrawVerifyDue(4) && !nrawVerifyDue(499), "Aufruf 4 und 499 nicht");
    expect(nrawVerifyDue(500) && nrawVerifyDue(1000), "jeder 500. prüft");
    expect(!nrawVerifyDue(501), "Aufruf 501 nicht");
    return gFailures == 0 ? 0 : 1;
}
