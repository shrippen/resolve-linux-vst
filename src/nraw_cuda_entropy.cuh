// N-RAW auf der GPU, Entropie (eingebunden von nraw_cuda.cu). Bitgleich zu nraw_entropy.cpp.
//
// Die unären Codes des Zählteils hängen nicht von der Vorhersage ab, nur die GCLI-Rekurrenz ist
// seriell, und zwar je Gruppenspalte über die Precincts einer Slice:
//
//   codes (je Paket)   Sig-Bits + Zählteil ─► x je Gruppe (255 = ohne Code)
//   gcli  (je Gruppe,  16 Precincts nacheinander, vertikale Vorhersage) ─► GCLI je Gruppe
//          Slice, Kette)
//   coefs (je Paket)   Datenebenen, Vorzeichen, Dequantisierung, waagerechte Hebung ─► Ebenen
//
// Jede Leseposition bleibt in den Grenzen des Pakets; ein beschädigter Strom liefert Müll und
// setzt ein Bit im Prüfwort (flag), liest aber nie außerhalb.
#include <stdint.h>

#include "nraw_gpu.h"

namespace {

constexpr int BLOCK = 256;
constexpr int WARP = 32;
constexpr int SLICE_PRECS = 16;
constexpr int PACKETS = NRAW_GPU_PACKETS;
constexpr int GSTRIDE = NRAW_GPU_GROUP_STRIDE;
constexpr int WGT = NRAW_GPU_WGT;
constexpr int PKT_HDR = 7;
constexpr int SIG_BYTES = 9;
constexpr int PKT_FIXED = PKT_HDR + SIG_BYTES;
constexpr int SIG_RUN = 8;
constexpr int MAX_RUNS = 72;      // Bits im Signifikanzteil
constexpr int GROUP = 4;
constexpr int MAX_GCLI = 18;
constexpr int GCLI_CLAMP = MAX_GCLI + 1;
constexpr int MAX_CODE = 254;     // größere Codes sind ohnehin Müll (GCLI wird begrenzt)
constexpr int FQ_SHIFT = 4;
constexpr int KINDS = NRAW_GPU_KINDS;
constexpr int CHAINS = 7;
constexpr unsigned RAW_FLAG = 0x80;

// Bits des Prüfworts.
enum Flag : int {
    F_EXTENT = 1,   // Paketkopf: Raw-Modus oder Länge außerhalb des Precincts
    F_LAYOUT = 2,   // Aufbau passt nicht zu den Tabellen
    F_CODES = 4,    // zu wenige Codes im Zählteil
    F_CNT_REST = 8, // Rest des Zählteils nicht 0..7 Bit
    F_GCLI = 16,    // GCLI über 18
    F_DAT_REST = 32,
    F_SGN_REST = 64,
};

// Paketart, Abschneide-Eintrag, Ebene und Zeilenfaktor je Paketnummer (nraw_frame.h PACKET_DEF).
__constant__ unsigned char KIND_OF[PACKETS] = {NRAW_GPU_LOW, NRAW_GPU_LOW, NRAW_GPU_ONE, NRAW_GPU_LOW,
                                               NRAW_GPU_HIGH, NRAW_GPU_HIGH, NRAW_GPU_ONE, NRAW_GPU_HIGH};
__constant__ unsigned char WGT_OF[PACKETS] = {0, 6, 12, 13, 19, 21, 12, 23};
// Ebenensatz im Puffer: K0 tief 0, K0 hoch 1, K1 tief 2, K1 hoch 3, K3 tief 4, K3 hoch 5, K2 6.
__constant__ unsigned char SET_OF[PACKETS] = {0, 2, 6, 4, 1, 3, 6, 5};
// Erste Pakete der sieben Ketten (2 = die Pakete 2 und 6 zusammen).
__constant__ unsigned char CHAIN_FIRST[CHAINS] = {0, 1, 2, 3, 4, 5, 7};

constexpr int PACKET_K2A = 2;
constexpr int PACKET_K2B = 6;

// Band und Platz in der Gruppenfolge des Pakets.
struct Spot {
    int band;
    int j;      // Gruppe im Band
    int first;  // erste Gruppe des Bandes im Paket
};

__device__ __forceinline__ Spot spotOf(const NrawGpuLayout &lay, int kind, int g)
{
    int first = 0;
    int b = 0;
    int bands = int(lay.bands[kind]);
    while (b + 1 < bands && g >= first + int(lay.groups[kind][b])) {
        first += int(lay.groups[kind][b]);
        b++;
    }
    return Spot{b, g - first, first};
}

__device__ __forceinline__ int groupsOf(const NrawGpuLayout &lay, int kind)
{
    int n = 0;
    for (unsigned b = 0; b < lay.bands[kind]; b++) {
        n += int(lay.groups[kind][b]);
    }
    return n;
}

__device__ __forceinline__ unsigned be32(const uint8_t *p, unsigned avail)
{
    unsigned w = 0;
    for (unsigned i = 0; i < 4; i++) {
        w = (w << 8) | (i < avail ? p[i] : 0xff); // hinter dem Ende Einsen: kein Nullbit
    }
    return w;
}

// Exklusive Präfixsumme über den Block; total = Summe aller. Braucht 32 Ints (warpSums).
__device__ int blockScan(int v, int *warpSums, int *total)
{
    int lane = threadIdx.x % WARP;
    int warp = threadIdx.x / WARP;
    int incl = v;
    for (int d = 1; d < WARP; d <<= 1) {
        int up = __shfl_up_sync(0xffffffffu, incl, d);
        incl += lane >= d ? up : 0;
    }
    if (lane == WARP - 1) {
        warpSums[warp] = incl;
    }
    __syncthreads();

    int before = 0;
    int sum = 0;
    for (int w = 0; w < BLOCK / WARP; w++) {
        before += w < warp ? warpSums[w] : 0;
        sum += warpSums[w];
    }
    *total = sum;
    __syncthreads();
    return before + incl - v;
}

// ---- codes -------------------------------------------------------------------------------------

struct PacketAt {
    const uint8_t *base;
    unsigned ld, lc, ls;
    bool ok;
};

// Paketkopf lesen und gegen das Ende des Pakets prüfen (kein Raw-Modus).
__device__ PacketAt packetAt(const uint8_t *cs, const NrawGpuPrec &pr, int k)
{
    PacketAt a{cs + pr.pkt[k], 0, 0, 0, false};
    unsigned start = pr.pkt[k];
    unsigned limit = k + 1 < PACKETS ? pr.pkt[k + 1] : pr.end;
    if (limit < start + PKT_FIXED) {
        return a;
    }

    const uint8_t *h = a.base;
    if (h[0] & RAW_FLAG) {
        return a;
    }
    unsigned ld = ((unsigned(h[0]) & 0x7f) << 13) | (unsigned(h[1]) << 5) | ((unsigned(h[2]) >> 3) & 0x1f);
    unsigned lc = ((unsigned(h[2]) & 7) << 17) | (unsigned(h[3]) << 9) | (unsigned(h[4]) << 1) | (unsigned(h[5]) >> 7);
    unsigned ls = ((unsigned(h[5]) & 0x7f) << 8) | h[6];
    unsigned long long used = (unsigned long long)PKT_FIXED + ld + lc + ls;
    if (start + used > limit) {
        return a;
    }
    a.ld = ld;
    a.lc = lc;
    a.ls = ls;
    a.ok = true;
    return a;
}

// Shared Memory des Kernels codes.
struct CodeMem {
    int runBase[MAX_RUNS + 8]; // Code-Nummer der ersten Gruppe des Laufs, -1 = ruhig
    int ende[GSTRIDE + 8];     // Bitposition des r-ten Nullbits im Zählteil
    int warpSums[BLOCK / WARP];
    int total;
    int codeCount;
    PacketAt pa;
};

// Thread 0: Paketkopf lesen, aus den Sig-Bits die Code-Nummer je Lauf von 8 Gruppen bestimmen.
__device__ void sigRuns(const uint8_t *cs, const NrawGpuPrec &pr, int k, const NrawGpuLayout &lay, CodeMem &m,
                        int *flag)
{
    int kind = KIND_OF[k];
    m.pa = packetAt(cs, pr, k);
    int runs = 0;
    int n = 0;
    const uint8_t *sig = m.pa.base + PKT_HDR;
    for (unsigned b = 0; b < lay.bands[kind] && m.pa.ok; b++) {
        for (int j = 0; j < int(lay.groups[kind][b]); j += SIG_RUN, runs++) {
            if (runs >= MAX_RUNS) {
                m.pa.ok = false;
                break;
            }
            bool quiet = (sig[runs >> 3] >> (7 - (runs & 7))) & 1;
            m.runBase[runs] = quiet ? -1 : n;
            n += quiet ? 0 : min(SIG_RUN, int(lay.groups[kind][b]) - j);
        }
    }
    m.codeCount = n;
    if (!m.pa.ok) {
        atomicOr(flag, F_EXTENT);
    }
}

// Nullbits des Zählteils Wort für Wort auf ihre Rangnummer streuen (Rang = Nullbits davor);
// gibt die Zahl aller Nullbits zurück. Ränge ab codeCount sind Füllbits.
__device__ int scatterZeros(CodeMem &m)
{
    const uint8_t *cnt = m.pa.base + PKT_FIXED;
    int words = int((m.pa.lc + 3) / 4);
    int carry = 0;
    for (int w0 = 0; w0 < words; w0 += BLOCK) {
        int w = w0 + threadIdx.x;
        unsigned word = 0xffffffffu;
        if (w < words) {
            word = be32(cnt + 4 * w, m.pa.lc - 4 * w);
        }
        int before = blockScan(__popc(~word), m.warpSums, &m.total);

        unsigned rest = ~word;
        for (int r = carry + before; rest; r++) {
            int lz = __clz(rest);
            rest &= ~(0x80000000u >> lz);
            if (r < m.codeCount) {
                m.ende[r] = 32 * w + lz;
            }
        }
        carry += m.total;
        __syncthreads();
    }
    return carry;
}

// Code x je Gruppe: Abstand der Nullbits; Gruppen in ruhigen Läufen ohne Code.
__device__ void writeCodes(const NrawGpuLayout &lay, int kind, const CodeMem &m, uint8_t *out)
{
    int groups = groupsOf(lay, kind);
    for (int g = threadIdx.x; g < groups; g += BLOCK) {
        Spot s = spotOf(lay, kind, g);
        int run = s.j / SIG_RUN;
        for (int b = 0; b < s.band; b++) {
            run += int((lay.groups[kind][b] + SIG_RUN - 1) / SIG_RUN);
        }

        int base = m.runBase[run];
        if (base < 0) {
            out[g] = NRAW_GPU_QUIET;
            continue;
        }
        int r = base + s.j % SIG_RUN;
        int x = r < GSTRIDE ? m.ende[r] - (r > 0 ? m.ende[r - 1] : -1) - 1 : 0;
        out[g] = uint8_t(min(max(x, 0), MAX_CODE));
    }
}

} // namespace

// Je Paket ein Block: Sig-Bits -> Code-Nummer je Gruppe, Zählteil -> Code je Rang, Ablage je Gruppe.
extern "C" __global__ void codes(const uint8_t *cs, const NrawGpuPrec *precs, NrawGpuLayout lay, uint8_t *xs, int *flag)
{
    __shared__ CodeMem m;
    int pk = blockIdx.x;
    int k = pk % PACKETS;

    if (threadIdx.x == 0) {
        sigRuns(cs, precs[pk / PACKETS], k, lay, m, flag);
    }
    __syncthreads();
    if (!m.pa.ok) {
        return;
    }

    int zeros = scatterZeros(m);
    if (threadIdx.x == 0) {
        int last = m.codeCount > 0 ? m.ende[m.codeCount - 1] + 1 : 0;
        int rest = int(8 * m.pa.lc) - last;
        if (zeros < m.codeCount) {
            atomicOr(flag, F_CODES);
        } else if (rest < 0 || rest >= 8) {
            atomicOr(flag, F_CNT_REST);
        }
    }
    writeCodes(lay, KIND_OF[k], m, xs + size_t(pk) * GSTRIDE);
}

// ---- gcli --------------------------------------------------------------------------------------

namespace {

// GCLI einer Gruppe aus Code x, Vorhersage prev (GCLI darüber) und Stufen t, topT.
__device__ __forceinline__ int gcliStep(int x, int prev, int topT, int t)
{
    int mt = max(prev, max(topT, t));
    if (x == NRAW_GPU_QUIET) {
        return mt;
    }

    int th = max(mt - t, 0);
    int dm = x > 2 * th ? x - th : ((x & 1) ? -((x + 1) / 2) : x / 2);
    return min(max(mt + dm, 0), GCLI_CLAMP);
}

} // namespace

// Je (Gruppe, Slice, Kette) ein Thread: die Precincts der Slice nacheinander. Die Kette 2/6
// verschränkt: Paket 2 sieht Paket 6 des vorigen Precincts, Paket 6 sieht Paket 2 desselben.
extern "C" __global__ void gcli(const uint8_t *xs, const uint8_t *gtli, NrawGpuLayout lay, uint8_t *gs, int *flag,
                                int nprec)
{
    int g = blockIdx.x * blockDim.x + threadIdx.x;
    int slice = blockIdx.y;
    int first = CHAIN_FIRST[blockIdx.z];
    int kind = KIND_OF[first];
    if (g >= groupsOf(lay, kind)) {
        return;
    }

    int w = WGT_OF[first] + spotOf(lay, kind, g).band;
    int row0 = slice * SLICE_PRECS;
    int count = min(SLICE_PRECS, nprec - row0);
    int steps = first == PACKET_K2A ? 2 : 1;
    int prev = 0;
    int topT = 0;
    int bad = 0;

    for (int i = 0; i < count; i++) {
        int p = row0 + i;
        int t = gtli[p * WGT + w];
        for (int j = 0; j < steps; j++) {
            size_t at = size_t(p * PACKETS + (j ? PACKET_K2B : first)) * GSTRIDE + g;
            bool top = i > 0 || j > 0;
            int v = gcliStep(xs[at], top ? prev : 0, top ? topT : t, t);
            gs[at] = uint8_t(v);
            bad |= v > MAX_GCLI;
            prev = v;
            topT = t;
        }
    }
    if (bad) {
        atomicOr(flag, F_GCLI);
    }
}

// ---- coefs -------------------------------------------------------------------------------------

namespace {

// Gleichmäßige Dequantisierung (nraw_entropy.cpp dequant).
__device__ __forceinline__ int dequant(int m, int t, int gcli)
{
    int v = m << t;
    if (t == 0 || gcli <= t) {
        return v;
    }

    int s = gcli - t + 1;
    int r = 0;
    while (v > 0) {
        r += v;
        v >>= s;
    }
    return r;
}

__device__ __forceinline__ int bitAt(const uint8_t *p, unsigned bytes, unsigned bit)
{
    return (bit >> 3) < bytes ? (p[bit >> 3] >> (7 - (bit & 7))) & 1 : 0;
}

// Eine Hebungsstufe (nraw_entropy.cpp inv53): l hat nl, h hat nh Werte, Ergebnis nl + nh Werte.
__device__ void lift53(const int *l, int nl, const int *h, int nh, int *out)
{
    for (int i = threadIdx.x; i < nl; i += BLOCK) {
        int left = h[min(max(i - 1, 0), nh - 1)];
        out[2 * i] = l[i] - ((left + h[min(i, nh - 1)] + 2) >> 2);
    }
    __syncthreads();

    for (int i = threadIdx.x; i < nh; i += BLOCK) {
        out[2 * i + 1] = h[i] + ((out[2 * i] + out[2 * min(i + 1, nl - 1)]) >> 1);
    }
    __syncthreads();
}


// Bitebenen einer Gruppe ab Bitposition off (4 Bit je Ebene, Koeffizient 0 zuerst) nach c.
__device__ __forceinline__ void planesOf(const uint8_t *dat, unsigned bytes, unsigned off, int planes, unsigned *c)
{
    c[0] = c[1] = c[2] = c[3] = 0;
    for (int k = 0; k < planes; k++) {
        unsigned bit = off + 4 * k;
        unsigned byte = (bit >> 3) < bytes ? dat[bit >> 3] : 0;
        unsigned nibble = (bit & 4) ? byte & 0xf : byte >> 4;
        for (int e = 0; e < GROUP; e++) {
            c[e] = (c[e] << 1) | ((nibble >> (3 - e)) & 1);
        }
    }
}

// Gruppen eines Threads: eine zusammenhängende Folge.
struct Chunk {
    int g0, g1;
};

__device__ __forceinline__ Chunk chunkOf(int groups)
{
    int chunk = (groups + BLOCK - 1) / BLOCK;
    int g0 = min(int(threadIdx.x) * chunk, groups);
    return Chunk{g0, min(g0 + chunk, groups)};
}

// Ein Paket im Block: GCLI-Spalte, Abschneidestufen, Teile des Pakets.
struct PacketCtx {
    PacketAt pa;
    const uint8_t *gc;   // GCLI je Gruppe
    const uint8_t *gtli; // Abschneidestufe je Band
    int kind;
    int *flag;
};

// Datenebenen und Vorzeichen: Koeffizienten (dequantisiert, mal 16) nach vals.
__device__ void readCoefs(const PacketCtx &c, const NrawGpuLayout &lay, int *vals, int *warpSums, int *total)
{
    const PacketAt &pa = c.pa;
    const uint8_t *dat = pa.base + PKT_FIXED + pa.lc;
    const uint8_t *sgn = dat + pa.ld;
    Chunk ch = chunkOf(groupsOf(lay, c.kind));

    // Datenbits vor der Gruppe: Summe von 4 * Ebenen.
    int bits = 0;
    for (int g = ch.g0; g < ch.g1; g++) {
        bits += GROUP * max(int(c.gc[g]) - int(c.gtli[spotOf(lay, c.kind, g).band]), 0);
    }
    int off = blockScan(bits, warpSums, total);
    if (threadIdx.x == 0 && (*total > int(8 * pa.ld) || int(8 * pa.ld) - *total >= 8)) {
        atomicOr(c.flag, F_DAT_REST);
    }

    int nz = 0;
    for (int g = ch.g0; g < ch.g1; g++) {
        int n = max(int(c.gc[g]) - int(c.gtli[spotOf(lay, c.kind, g).band]), 0);
        unsigned m[GROUP];
        planesOf(dat, pa.ld, unsigned(off), n, m);
        off += GROUP * n;
        for (int e = 0; e < GROUP; e++) {
            vals[g * GROUP + e] = int(m[e]);
            nz += m[e] != 0;
        }
    }

    // Vorzeichen: Nummer = Zahl der Koeffizienten != 0 davor, über alle Bänder des Pakets.
    int sIdx = blockScan(nz, warpSums, total);
    if (threadIdx.x == 0 && (*total > int(8 * pa.ls) || int(8 * pa.ls) - *total >= 8)) {
        atomicOr(c.flag, F_SGN_REST);
    }
    for (int g = ch.g0; g < ch.g1; g++) {
        int t = c.gtli[spotOf(lay, c.kind, g).band];
        for (int e = 0; e < GROUP; e++) {
            int m = vals[g * GROUP + e];
            int v = 0;
            if (m != 0) {
                v = dequant(m, t, c.gc[g]);
                v = bitAt(sgn, pa.ls, unsigned(sIdx++)) ? -v : v;
            }
            vals[g * GROUP + e] = v * (1 << FQ_SHIFT);
        }
    }
    __syncthreads();
}

// Waagerechte Rücktransformation einer tiefen oder hohen Zeile: vals -> out (global).
__device__ void liftRow(const NrawGpuLayout &lay, int kind, const int *vals, int *bufA, int *bufB, int *out)
{
    int bands = int(lay.bands[kind]);
    const int *in = vals;
    int w = int(lay.width[kind][0]);
    int cOff = GROUP * int(lay.groups[kind][0]);
    for (int b = 1; b < bands; b++) {
        int nh = int(lay.width[kind][b]);
        int *dst = b == bands - 1 ? out : (b % 2 ? bufA : bufB);
        lift53(in, w, vals + cOff, nh, dst);
        in = dst;
        w += nh;
        cOff += GROUP * int(lay.groups[kind][b]);
    }
}

} // namespace

// Je Paket ein Block: Datenebenen und Vorzeichen -> Koeffizienten im Shared Memory, danach die
// waagerechte Hebung und die Zeile in den Ebenensatz.
extern "C" __global__ void coefs(const uint8_t *cs, const NrawGpuPrec *precs, const uint8_t *gtli, const uint8_t *gs,
                                 int *planes, int *flag, size_t planeInts, int compw, int valsInts, NrawGpuLayout lay)
{
    extern __shared__ int sm[];
    __shared__ int warpSums[BLOCK / WARP];
    __shared__ int total;
    __shared__ PacketCtx ctx;

    int pk = blockIdx.x;
    int p = pk / PACKETS;
    int k = pk % PACKETS;
    int kind = KIND_OF[k];
    int *vals = sm;

    if (threadIdx.x == 0) {
        ctx = PacketCtx{packetAt(cs, precs[p], k), gs + size_t(pk) * GSTRIDE, gtli + p * WGT + WGT_OF[k], kind, flag};
        if (!ctx.pa.ok) {
            atomicOr(flag, F_EXTENT);
        }
    }
    __syncthreads();
    if (!ctx.pa.ok) {
        return;
    }
    readCoefs(ctx, lay, vals, warpSums, &total);

    // Zeile des Pakets im Ebenensatz; K2 hat zwei Zeilen je Precinct (Pakete 2 und 6).
    int *set = planes + size_t(SET_OF[k]) * planeInts;
    if (kind == NRAW_GPU_ONE) {
        int *out = set + size_t(2 * p + (k == PACKET_K2B)) * compw;
        for (int i = threadIdx.x; i < compw; i += BLOCK) {
            out[i] = vals[i];
        }
        return;
    }
    int *bufA = sm + valsInts;
    liftRow(lay, kind, vals, bufA, bufA + compw, set + size_t(p) * compw);
}
