// Test der Editorgrößen-Rechnung (src/editor_rect.h) mit erfundenen Größen, ohne Resolve und X11.
// Die Erwartungen sind die Werte, die der Code vor dem Auslagern lieferte. Läuft ohne Resolve
// (make test).
#include "../src/editor_rect.h"

#include <cstdio>

namespace {

int failures = 0;

void expect(bool ok, const char *what)
{
    printf("%s  %s\n", ok ? "ok   " : "FEHLER", what);
    failures += !ok;
}

bool same(const ERect &r, int top, int left, int bottom, int right)
{
    return r.top == top && r.left == left && r.bottom == bottom && r.right == right;
}

} // namespace

int main()
{
    // Faktor 1: unverändert.
    expect(editorScaled(600, 1.0) == 600, "Faktor 1 ändert nichts");
    expect(editorScaled(0, 2.0) == 0, "Größe 0 bleibt 0");

    // Rundung auf die nächste ganze Zahl, .5 weg von 0.
    expect(editorScaled(600, 1.5) == 400, "600 / 1,5 = 400");
    expect(editorScaled(601, 2.0) == 301, "300,5 rundet auf 301");
    expect(editorScaled(1001, 3.75) == 267, "266,93 rundet auf 267");
    expect(editorScaled(-5, 2.0) == -3, "-2,5 rundet auf -3");

    expect(editorZoomed(0, 3.75) == 0, "Zoom: Größe 0 bleibt 0");
    expect(editorZoomed(500, 1.0) == 500, "Zoom 1 ändert nichts");
    expect(editorZoomed(400, 3.75) == 1500, "Zoom 3,75: 400 -> 1500");
    expect(editorZoomed(101, 1.5) == 152, "Zoom: 151,5 rundet auf 152");

    // audioMasterSizeWindow: Wine-Vergrößerung durch Skalierung.
    expect(editorToResolve(800, 1.0, 1.0) == 800, "an Resolve: Faktor 1 ändert nichts");
    expect(editorToResolve(0, 3.75, 2.0) == 0, "an Resolve: 0 bleibt 0");
    expect(editorToResolve(800, 1.0, 2.0) == 400, "an Resolve: 800 / 2 = 400");
    expect(editorToResolve(801, 1.0, 2.0) == 401, "an Resolve: 400,5 rundet auf 401");
    expect(editorToResolve(400, 3.75, 3.75) == 400, "Zoom gleich Skalierung hebt sich auf");
    expect(editorToResolve(100, 2.0, 1.5) == 133, "an Resolve: 133,3 rundet auf 133");

    // Gemeldetes Rechteck als Bildschirmgröße: Ursprung 0/0, nur die Größe.
    const ERect reported = {10, 20, 310, 520}; // 500 breit, 300 hoch
    expect(same(editorScreenRect(reported, 1.0), 0, 0, 300, 500), "Rechteck: Zoom 1");
    expect(same(editorScreenRect(reported, 1.5), 0, 0, 450, 750), "Rechteck: Zoom 1,5");
    expect(same(editorScreenRect({0, 0, 0, 0}, 3.75), 0, 0, 0, 0), "Rechteck: leer bleibt leer");

    // Zielgröße als Rechteck (Breite rechts, Höhe unten).
    expect(same(editorTargetRect(640, 480), 0, 0, 480, 640), "Zielrechteck");

    // Unter der Zielgröße gilt nur, wenn beide Seiten kleiner sind.
    expect(editorBelowTarget(400, 300, 640, 480), "beide Seiten kleiner");
    expect(!editorBelowTarget(640, 300, 640, 480), "gleiche Breite zählt nicht");
    expect(!editorBelowTarget(400, 480, 640, 480), "gleiche Höhe zählt nicht");
    expect(!editorBelowTarget(700, 300, 640, 480), "eine Seite größer zählt nicht");
    expect(!editorBelowTarget(0, 0, 0, 0), "ohne Ziel nie");

    // Rechteck an Resolve: jede Kante durch den Faktor geteilt.
    expect(same(editorToResolveRect({0, 0, 480, 640}, 1.0), 0, 0, 480, 640), "an Resolve: Faktor 1");
    expect(same(editorToResolveRect({0, 0, 481, 641}, 2.0), 0, 0, 241, 321), "an Resolve: Faktor 2 rundet auf");
    expect(same(editorToResolveRect({0, 0, 450, 750}, 1.5), 0, 0, 300, 500), "an Resolve: Faktor 1,5");

    return failures ? 1 : 0;
}
