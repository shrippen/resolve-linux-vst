// N-RAW-Decoder, Bild: senkrechte 5/3-Rücktransformation, inverse Star-Tetrix und Kennlinie,
// kachelweise. Die vier Komponenten sind die vier Klassen des 2x2-Blocks im CFA-Gitter
// (P = K0, R = K1, Q = K2, S = K3), darum rechnet der Code auf vier getrennten Ebenen statt auf
// dem verschränkten Gitter:
//
//   f.low/f.high ─► senkrechte Hebung ─► P,R,S ┐
//   f.low[K2]    ─────────────────────► Q      ┴─► Star-Tetrix (5 Schritte) ─► Kennlinie ─► Bayer
//
// Eine Kachel liest einen Rand von HALO Werten mehr (am Bildrand gespiegelt) und rechnet die
// Schritte nur dort, wo ihr Ergebnis später gebraucht wird. So braucht es kein Zwischenbild.
//
// Die Kernfunktion ist eine Vorlage und wird zweimal übersetzt: allgemein und mit AVX2
// (Auswahl zur Laufzeit; die Bibliothek muss auf jeder x86-64-CPU laden). Dieselben Rechenschritte,
// darum dasselbe Ergebnis.
#include <algorithm>
#include <cstring>
#include <vector>

#include "nraw_frame.h"

#pragma GCC optimize("O3")

#define NRAW_INLINE inline __attribute__((always_inline))

namespace nraw {

namespace {

constexpr size_t TILE_H = 32;  // Ebenenzeilen je Kachel (höchstens)
constexpr size_t TILE_W = 192; // Ebenenspalten je Kachel (höchstens)
constexpr size_t HALO = 2;
constexpr size_t WIN_H = TILE_H + 2 * HALO;
constexpr size_t WIN_W = TILE_W + 2 * HALO;
constexpr size_t PLANES = 4;

constexpr int CURVE_OFFSET = 32768; // Mitte des Wavelet-Wertebereichs

// Ebenen im Fenster; die Namen sind die Klassen im 2x2-Block (Zeile, Spalte): R(0,0) P(0,1) Q(1,0) S(1,1).
enum Plane { PR, PP, PQ, PS };

// Gerade Ebenen (R, P) liegen auf geraden Gitterzeilen, ungerade (Q, S) auf ungeraden; ebenso die
// Spalten. Das bestimmt die Spiegelung am Rand (Gitterzeile -1 ist Zeile 1, Zeile -1 der
// ungeraden Ebenen ist ihre Zeile 0).
constexpr bool ROW_ODD[PLANES] = {false, false, true, true};
constexpr bool COL_ODD[PLANES] = {false, true, false, true};
constexpr Comp PLANE_COMP[PLANES] = {K1, K0, K2, K3};

struct Tile {
    long a, rows; // erste Ebenenzeile, Zeilenzahl
    long b, cols;
};

struct Geometry {
    size_t nx, ny;
    long m, n; // Ebenenzeilen, Ebenenspalten
};

Geometry geometry(const Frame &f)
{
    Geometry g;
    g.m = long(2 * f.nprec);
    g.n = long(f.compw);
    g.ny = (size_t(g.m) + TILE_H - 1) / TILE_H;
    g.nx = (size_t(g.n) + TILE_W - 1) / TILE_W;
    return g;
}

// Gerade Grenzen, gleichmäßig verteilt: jede Kachel hat gerade Größe, höchstens TILE_*, mindestens 4
// (damit die Spiegelung am Rand im Fenster bleibt).
Tile tileAt(const Geometry &g, size_t index)
{
    size_t ty = index / g.nx;
    size_t tx = index % g.nx;
    auto edge = [](size_t k, size_t parts, long len) { return long(size_t(len) * k / parts) & ~1L; };

    Tile t;
    t.a = edge(ty, g.ny, g.m);
    t.rows = edge(ty + 1, g.ny, g.m) - t.a;
    t.b = edge(tx, g.nx, g.n);
    t.cols = edge(tx + 1, g.nx, g.n) - t.b;
    return t;
}

// Ein Fenster je Ebene; (0, 0) der Kachel liegt bei rowp(plane, 0).
NRAW_INLINE int32_t *rowp(int32_t *plane, long r)
{
    return plane + size_t(r + long(HALO)) * WIN_W + HALO;
}

// Spiegelung eines Ebenenindex bei Länge len (Gitterzeile -1 = Zeile 1, Zeile len = len - 2).
NRAW_INLINE long mirror(long i, long len, bool odd)
{
    if (i < 0) {
        return odd ? -i - 1 : -i;
    }
    if (i >= len) {
        return odd ? 2 * len - 2 - i : 2 * len - 1 - i;
    }
    return i;
}

// ---- Senkrechte Hebung -------------------------------------------------------------------

NRAW_INLINE void evenRow(const Coef *l, const Coef *hPrev, const Coef *h, int32_t *out, size_t n)
{
    for (size_t x = 0; x < n; x++) {
        out[x] = int32_t(l[x]) - ((int32_t(hPrev[x]) + int32_t(h[x]) + 2) >> 2);
    }
}

NRAW_INLINE void oddRow(const Coef *h, const int32_t *e0, const int32_t *e1, int32_t *out, size_t n)
{
    for (size_t x = 0; x < n; x++) {
        out[x] = int32_t(h[x]) + ((e0[x] + e1[x]) >> 1);
    }
}

struct Span {
    long yy0, yy1; // Ebenenzeilen des Fensters, die im Bild liegen
    long cs;       // erste Ebenenspalte im Bild
    size_t n;      // Spalten im Bild
};

NRAW_INLINE Span spanOf(const Geometry &g, const Tile &t)
{
    long cs = std::max(t.b - long(HALO), 0L);
    long ce = std::min(t.b + t.cols + long(HALO), g.n);
    return Span{std::max(t.a - long(HALO), 0L), std::min(t.a + t.rows + long(HALO), g.m), cs, size_t(ce - cs)};
}

// Senkrechte Hebung einer Komponente für die Fensterzeilen im Bild, paarweise: Zeile 2i = e_i,
// Zeile 2i+1 = e_i, e_(i+1) und h_i. e_(i+1) steht schon in der nächsten Fensterzeile, außer am
// Fensterende (dort in tmp) und am Bildende (dort gespiegelt: e_i).
NRAW_INLINE void lift(const Frame &f, Comp c, const Tile &t, const Span &sp, int32_t *win)
{
    const size_t w = f.compw;
    const long nl = long(f.nprec);
    const Coef *lo = f.low[c] + sp.cs;
    const Coef *hi = f.high[c] + sp.cs;
    int32_t tmp[WIN_W];

    auto out = [&](long yy) { return rowp(win, yy - t.a) + (sp.cs - t.b); };
    auto lrow = [&](long i) { return lo + size_t(i) * w; };
    auto hrow = [&](long i) { return hi + size_t(i) * w; };

    long i0 = sp.yy0 / 2;
    evenRow(lrow(i0), hrow(std::max(i0 - 1, 0L)), hrow(i0), out(2 * i0), sp.n);
    for (long i = i0; i < sp.yy1 / 2; i++) {
        const int32_t *cur = out(2 * i);
        const int32_t *next = cur;
        if (i + 1 < nl) {
            int32_t *dst = 2 * i + 2 < sp.yy1 ? out(2 * i + 2) : tmp;
            evenRow(lrow(i + 1), hrow(i), hrow(i + 1), dst, sp.n);
            next = dst;
        }
        oddRow(hrow(i), cur, next, out(2 * i + 1), sp.n);
    }
}

NRAW_INLINE void loadPlane(const Frame &f, const Tile &t, const Span &sp, int32_t *win)
{
    const Coef *src = f.low[K2] + size_t(sp.cs);
    for (long yy = sp.yy0; yy < sp.yy1; yy++) {
        int32_t *dst = rowp(win, yy - t.a) + (sp.cs - t.b);
        const Coef *row = src + size_t(yy) * f.compw;
        for (size_t x = 0; x < sp.n; x++) {
            dst[x] = row[x];
        }
    }
}

// Ränder des Fensters aus dem Inneren spiegeln: erst Spalten (nur Zeilen im Bild), dann ganze Zeilen.
NRAW_INLINE void mirrorEdges(const Geometry &g, const Tile &t, const Span &sp, Plane pl, int32_t *win)
{
    const long ce = sp.cs + long(sp.n);
    for (long yy = sp.yy0; yy < sp.yy1; yy++) {
        int32_t *row = rowp(win, yy - t.a);
        for (long c = -long(HALO); c < sp.cs - t.b; c++) {
            row[c] = row[mirror(t.b + c, g.n, COL_ODD[pl]) - t.b];
        }
        for (long c = ce - t.b; c < t.cols + long(HALO); c++) {
            row[c] = row[mirror(t.b + c, g.n, COL_ODD[pl]) - t.b];
        }
    }

    const size_t bytes = size_t(t.cols + 2 * long(HALO)) * sizeof(int32_t);
    for (long r = -long(HALO); r < t.rows + long(HALO); r++) {
        long yy = t.a + r;
        if (yy >= 0 && yy < g.m) {
            continue;
        }
        long from = mirror(yy, g.m, ROW_ODD[pl]) - t.a;
        memcpy(rowp(win, r) - HALO, rowp(win, from) - HALO, bytes);
    }
}

// ---- Star-Tetrix -------------------------------------------------------------------------
// Rechenschritte wie im Gitter (Diag0 an P, Diag3 an Q, Cross0 an P, Cross3 an Q, Last an R und S);
// jeder nur auf dem Bereich, den ein späterer Schritt oder die Ausgabe braucht (Kachelkoordinaten).

NRAW_INLINE void tetrix(const Tile &t, int32_t *const *p)
{
    const long H = t.rows;
    const long W = t.cols;

    for (long r = -1; r <= H; r++) { // P -= diag(Q) >> 3
        int32_t *a = rowp(p[PP], r);
        const int32_t *q0 = rowp(p[PQ], r - 1);
        const int32_t *q1 = rowp(p[PQ], r);
        for (long c = -1; c <= W; c++) {
            a[c] -= (q0[c] + q0[c + 1] + q1[c] + q1[c + 1]) >> 3;
        }
    }
    for (long r = -1; r < H; r++) { // Q += diag(P) >> 2
        int32_t *a = rowp(p[PQ], r);
        const int32_t *p0 = rowp(p[PP], r);
        const int32_t *p1 = rowp(p[PP], r + 1);
        for (long c = 0; c <= W; c++) {
            a[c] += (p0[c - 1] + p0[c] + p1[c - 1] + p1[c]) >> 2;
        }
    }
    for (long r = 0; r <= H; r++) { // P -= cross(R, S) >> 3
        int32_t *a = rowp(p[PP], r);
        const int32_t *r0 = rowp(p[PR], r);
        const int32_t *s0 = rowp(p[PS], r - 1);
        const int32_t *s1 = rowp(p[PS], r);
        for (long c = -1; c < W; c++) {
            a[c] -= (r0[c] + r0[c + 1] + s0[c] + s1[c]) >> 3;
        }
    }
    for (long r = -1; r < H; r++) { // Q -= cross(R, S) >> 3
        int32_t *a = rowp(p[PQ], r);
        const int32_t *s0 = rowp(p[PS], r);
        const int32_t *r0 = rowp(p[PR], r);
        const int32_t *r1 = rowp(p[PR], r + 1);
        for (long c = 0; c <= W; c++) {
            a[c] -= (s0[c - 1] + s0[c] + r0[c] + r1[c]) >> 3;
        }
    }
    for (long r = 0; r < H; r++) { // R, S += cross(P, Q) >> 2
        int32_t *ra = rowp(p[PR], r);
        int32_t *sa = rowp(p[PS], r);
        const int32_t *p0 = rowp(p[PP], r);
        const int32_t *p1 = rowp(p[PP], r + 1);
        const int32_t *q0 = rowp(p[PQ], r - 1);
        const int32_t *q1 = rowp(p[PQ], r);
        for (long c = 0; c < W; c++) {
            ra[c] += (p0[c - 1] + p0[c] + q0[c] + q1[c]) >> 2;
            sa[c] += (q1[c] + q1[c + 1] + p0[c] + p1[c]) >> 2;
        }
    }
}

// ---- Ausgabe -----------------------------------------------------------------------------

// Tabellenstufe zu einem Wavelet-Wert (Mitte der Tabelle = 0).
NRAW_INLINE void indices(const int32_t *v, long n, int maxIdx, int32_t *idx)
{
    for (long c = 0; c < n; c++) {
        idx[c] = std::clamp(v[c] + CURVE_OFFSET, 0, maxIdx);
    }
}

// Bayer RGGB: Zeile 2y = R, P abwechselnd, Zeile 2y+1 = Q, S. Erst die Stufen aller Werte der
// Zeile (vektorisiert), dann die Nachschläge.
NRAW_INLINE void emitTile(const Frame &f, const Tile &t, int32_t *const *p, const LevelTable &levels, uint16_t *bayer)
{
    const long rows = std::min(t.rows, long(f.h.hf / 2) - t.a);
    const uint16_t *lut = levels.values;
    int32_t idx[PLANES][TILE_W];

    for (long r = 0; r < rows; r++) {
        uint16_t *top = bayer + size_t(2 * (t.a + r)) * f.h.wf + size_t(2 * t.b);
        uint16_t *bottom = top + f.h.wf;
        for (int i = 0; i < int(PLANES); i++) {
            indices(rowp(p[i], r), t.cols, levels.maxIdx, idx[i]);
        }
        for (long c = 0; c < t.cols; c++) {
            top[2 * c] = lut[idx[PR][c]];
            top[2 * c + 1] = lut[idx[PP][c]];
            bottom[2 * c] = lut[idx[PQ][c]];
            bottom[2 * c + 1] = lut[idx[PS][c]];
        }
    }
}

NRAW_INLINE void tileKernel(const Frame &f, const Tile &t, const LevelTable &levels, uint16_t *bayer)
{
    const Geometry g = geometry(f);
    const Span sp = spanOf(g, t);

    std::vector<int32_t> store(PLANES * WIN_H * WIN_W);
    int32_t *mem = store.data();
    int32_t *planes[PLANES];
    for (int i = 0; i < int(PLANES); i++) {
        planes[i] = mem + size_t(i) * WIN_H * WIN_W;
    }

    for (int i = 0; i < int(PLANES); i++) {
        Comp c = PLANE_COMP[i];
        if (c == K2) {
            loadPlane(f, t, sp, planes[i]);
        } else {
            lift(f, c, t, sp, planes[i]);
        }
        mirrorEdges(g, t, sp, Plane(i), planes[i]);
    }
    tetrix(t, planes);
    emitTile(f, t, planes, levels, bayer);
}

void tileGeneric(const Frame &f, const Tile &t, const LevelTable &levels, uint16_t *bayer)
{
    tileKernel(f, t, levels, bayer);
}

__attribute__((target("avx2"))) void tileAvx2(const Frame &f, const Tile &t, const LevelTable &levels, uint16_t *bayer)
{
    tileKernel(f, t, levels, bayer);
}

} // namespace

size_t tileCount(const Frame &f)
{
    Geometry g = geometry(f);
    return g.nx * g.ny;
}

void decodeTile(const Frame &f, size_t index, const LevelTable &levels, uint16_t *bayer)
{
    static const bool avx2 = __builtin_cpu_supports("avx2");
    Tile t = tileAt(geometry(f), index);
    if (avx2) {
        tileAvx2(f, t, levels, bayer);
        return;
    }
    tileGeneric(f, t, levels, bayer);
}

} // namespace nraw
