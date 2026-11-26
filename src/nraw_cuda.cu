// N-RAW auf der GPU, Kernel (Host-Teil: nraw_cuda.cpp). Dieselben Rechenschritte wie
// nraw_color.cpp, aber über das ganze Bild statt je Kachel, darum bitgleich:
//
//   Ebenen ─► lift (senkrecht, K0/K1/K3) ─► P, R, S ┐
//   K2 ──────────────────────────────────────────► Q ┴─► step1..4 ─► last ─► Kennlinie ─► Bayer
//
// Jeder Schritt ist ein eigener Aufruf (Synchronisation über das ganze Bild). Lesezugriffe über
// den Rand werden gespiegelt wie die Fensterränder der CPU (ganzzahlige Symmetrie bleibt durch
// die Hebung erhalten, darum genügt das Spiegeln beim Lesen).
//
// Stufe B setzt davor die Entropie auf der Karte: codes, gcli, coefs (nraw_cuda_entropy.cuh) füllen
// die Ebenen, die Stufe A sonst hochlädt.
#include <stdint.h>

#include "nraw_cuda_entropy.cuh"

namespace {

constexpr int CURVE_OFFSET = NRAW_GPU_CURVE_OFFSET;

// Ebene der Star-Tetrix: Klasse im 2x2-Block (Zeile, Spalte): R(0,0) P(0,1) Q(1,0) S(1,1).
// ROW/COL true = ungerade Gitterlage, bestimmt die Spiegelung am Rand.
__device__ __forceinline__ int mir(int i, int len, bool odd)
{
    if (i < 0) {
        return odd ? -i - 1 : -i;
    }
    if (i >= len) {
        return odd ? 2 * len - 2 - i : 2 * len - 1 - i;
    }
    return i;
}

template <bool ROW_ODD, bool COL_ODD>
__device__ __forceinline__ int ld(const int *p, int r, int c, int m, int n)
{
    return p[size_t(mir(r, m, ROW_ODD)) * n + mir(c, n, COL_ODD)];
}

} // namespace

// Senkrechte 5/3-Hebung: Zeile 2i = e_i, Zeile 2i+1 aus e_i, e_(i+1) und h_i. Am Anfang h_(-1) = h_0,
// am Ende e_nl = e_(nl-1). low/high: nprec Zeilen zu n Werten; out: 2 * nprec Zeilen.
extern "C" __global__ void lift(const int *low, const int *high, int *out, int nprec, int n)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int i = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= n || i >= nprec) {
        return;
    }

    int hc = high[size_t(i) * n + x];
    int hp = high[size_t(i > 0 ? i - 1 : 0) * n + x];
    int e = low[size_t(i) * n + x] - ((hp + hc + 2) >> 2);
    int next = e;
    if (i + 1 < nprec) {
        int hn = high[size_t(i + 1) * n + x];
        next = low[size_t(i + 1) * n + x] - ((hc + hn + 2) >> 2);
    }
    out[size_t(2 * i) * n + x] = e;
    out[size_t(2 * i + 1) * n + x] = hc + ((e + next) >> 1);
}

#define NRAW_POS                                                      \
    int c = blockIdx.x * blockDim.x + threadIdx.x;                    \
    int r = blockIdx.y * blockDim.y + threadIdx.y;                    \
    if (c >= n || r >= m) {                                           \
        return;                                                       \
    }                                                                 \
    size_t at = size_t(r) * n + c;

// P -= diag(Q) >> 3
extern "C" __global__ void step1(int *P, const int *Q, int m, int n)
{
    NRAW_POS
    P[at] -= (ld<true, false>(Q, r - 1, c, m, n) + ld<true, false>(Q, r - 1, c + 1, m, n) +
              ld<true, false>(Q, r, c, m, n) + ld<true, false>(Q, r, c + 1, m, n)) >> 3;
}

// Q += diag(P) >> 2
extern "C" __global__ void step2(int *Q, const int *P, int m, int n)
{
    NRAW_POS
    Q[at] += (ld<false, true>(P, r, c - 1, m, n) + ld<false, true>(P, r, c, m, n) +
              ld<false, true>(P, r + 1, c - 1, m, n) + ld<false, true>(P, r + 1, c, m, n)) >> 2;
}

// P -= cross(R, S) >> 3
extern "C" __global__ void step3(int *P, const int *R, const int *S, int m, int n)
{
    NRAW_POS
    P[at] -= (ld<false, false>(R, r, c, m, n) + ld<false, false>(R, r, c + 1, m, n) +
              ld<true, true>(S, r - 1, c, m, n) + ld<true, true>(S, r, c, m, n)) >> 3;
}

// Q -= cross(R, S) >> 3
extern "C" __global__ void step4(int *Q, const int *R, const int *S, int m, int n)
{
    NRAW_POS
    Q[at] -= (ld<true, true>(S, r, c - 1, m, n) + ld<true, true>(S, r, c, m, n) +
              ld<false, false>(R, r, c, m, n) + ld<false, false>(R, r + 1, c, m, n)) >> 3;
}

__device__ __forceinline__ unsigned short level(const unsigned short *lut, int maxIdx, int v)
{
    int idx = v + CURVE_OFFSET;
    idx = idx < 0 ? 0 : (idx > maxIdx ? maxIdx : idx);
    return lut[idx];
}

// R, S += cross(P, Q) >> 2, dann Kennlinie und Bayer RGGB: Zeile 2r = R, P; Zeile 2r+1 = Q, S.
// Nur die Zeilen rows (= Hf / 2) kommen ins Bild, die Ebenen haben eine Füllzeile mehr.
extern "C" __global__ void last(const int *R, const int *P, const int *Q, const int *S, const unsigned short *lut,
                                int maxIdx, unsigned short *bayer, int m, int n, int rows)
{
    NRAW_POS
    if (r >= rows) {
        return;
    }

    int rv = R[at] + ((ld<false, true>(P, r, c - 1, m, n) + P[at] + ld<true, false>(Q, r - 1, c, m, n) + Q[at]) >> 2);
    int sv = S[at] + ((Q[at] + ld<true, false>(Q, r, c + 1, m, n) + P[at] + ld<false, true>(P, r + 1, c, m, n)) >> 2);

    size_t wf = size_t(2) * n;
    ushort2 *top = reinterpret_cast<ushort2 *>(bayer + size_t(2 * r) * wf + size_t(2) * c);
    ushort2 *bottom = reinterpret_cast<ushort2 *>(bayer + size_t(2 * r + 1) * wf + size_t(2) * c);
    *top = make_ushort2(level(lut, maxIdx, rv), level(lut, maxIdx, P[at]));
    *bottom = make_ushort2(level(lut, maxIdx, Q[at]), level(lut, maxIdx, sv));
}
