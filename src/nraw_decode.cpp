// N-RAW-Referenzdecoder (siehe nraw_decode.h). Stufen:
//
//   Codestream ─► Kopf ─► Precinct-/Paketlagen vorab lesen (seriell, billig)
//              ─► Ketten (Slice, Paketart) parallel: Entropie, Dequantisierung, waagerechte
//                 5/3-Wavelet (nraw_entropy.cpp)
//              ─► Kacheln parallel: senkrechte 5/3-Wavelet, inverse Star-Tetrix auf dem
//                 CFA-Gitter, Kennlinie ─► Bayer RGGB (nraw_color.cpp)
//
// 4 Komponenten (halbe Breite und Höhe): K0 Helligkeit, K1/K3 Farbdifferenzen, K2 ohne Wavelet.
// Je Precinct (2 Komponentenzeilen) 8 Pakete: 0/1/3 = K0/K1/K3 tief (LL5, HL5..HL1), 4/5/7 = K0/K1/K3
// hoch (LH1, HH1), 2/6 = K2 Zeile 0/1.
#include <stdlib.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <vector>

#include "nraw_decode.h"
#include "nraw_frame.h"
#include "nraw_pool.h"

using namespace nraw;

namespace {

constexpr unsigned MARKER_SOC = 0xFF10;
constexpr unsigned MARKER_PIH = 0xFF12;
constexpr unsigned MARKER_CDT = 0xFF13;
constexpr unsigned MARKER_WGT = 0xFF14;
constexpr unsigned MARKER_SLH = 0xFF20;
constexpr unsigned MARKER_CAP = 0xFF50;
constexpr unsigned MARKER_EOC = 0xFF11;

constexpr size_t MARKER_LEN = 2;
constexpr size_t SLH_SIZE = 6;     // Marker, Länge (4), Slice-Nummer
constexpr size_t SLH_LENGTH = 4;
constexpr size_t PIH_PAYLOAD = 37; // 24 Standardbytes + 13 Zusatzbytes
constexpr size_t CDT_PAYLOAD = 12;

// Feste Felder des PIH (Byte-Nummern in der Nutzlast); Erwartung aus dem geprüften Aufbau.
constexpr size_t PIH_LCOD = 0;
constexpr size_t PIH_WF = 8;
constexpr size_t PIH_HF = 10;
constexpr size_t PIH_CW = 12;
constexpr size_t PIH_HSL = 14;
constexpr size_t PIH_FIXED = 16; // ab hier Nc, Ng, Ss, Bw, Fq|Br, Fslc|Ppoc|Cpih, Nlx|Nly, Lh..Rm
constexpr uint8_t PIH_EXPECT[] = {4, 4, 8, 18, 0x44, 0x10, 0x51, 0x14};
constexpr uint8_t CDT_EXPECT[CDT_PAYLOAD] = {14, 0x11, 0x51, 14, 0x11, 0x51, 14, 0x11, 0x00, 14, 0x11, 0x51};
constexpr size_t HSL_EXPECT = 16; // Precincts je Slice
constexpr int CURVE_ROUND = 2; // 4 Stufen der Tabelle je Ausgabestufe
constexpr int CURVE_SHIFT = 2;
constexpr int OUT_MAX = 65535;
constexpr int LEVELS = 5;         // horizontale Wavelet-Stufen
constexpr size_t MIN_PRECINCTS = 2; // die Kachelung der Bildstufe braucht mindestens 4 Ebenenzeilen
constexpr size_t MIN_WIDTH = 16;
constexpr size_t PLANE_COUNT = 8;   // Ebenen im Arbeitsspeicher: 3 tief, 3 hoch, K2 doppelt hoch
constexpr size_t ALIGN = 64;      // Cachezeile

// ---- Kopf --------------------------------------------------------------------------------

NrawResult parsePih(const uint8_t *p, size_t n, Header &h)
{
    if (n != PIH_PAYLOAD || be(p + PIH_CW, 2) != 0 || be(p + PIH_HSL, 2) != HSL_EXPECT) {
        return NrawResult::Unsupported;
    }
    if (memcmp(p + PIH_FIXED, PIH_EXPECT, sizeof PIH_EXPECT) != 0) {
        return NrawResult::Unsupported;
    }
    h.end = be(p + PIH_LCOD, 4);
    h.wf = be(p + PIH_WF, 2);
    h.hf = be(p + PIH_HF, 2);
    // Wf durch 4 (halbe Breite gerade), Hf gerade: Bänder gleich breit, ganze Komponentenzeilen.
    if (h.wf == 0 || h.hf == 0 || h.wf % 4 != 0 || h.hf % 2 != 0) {
        return NrawResult::Unsupported;
    }
    return NrawResult::Ok;
}

NrawResult parseWgt(const uint8_t *p, size_t n, Header &h)
{
    if (n != 2 * WGT_COUNT) {
        return NrawResult::Unsupported;
    }
    for (size_t i = 0; i < WGT_COUNT; i++) {
        h.gain[i] = p[2 * i];
        h.prio[i] = p[2 * i + 1];
    }
    return NrawResult::Ok;
}

NrawResult parseSegment(unsigned marker, const uint8_t *p, size_t n, Header &h, unsigned &seen)
{
    switch (marker) {
    case MARKER_CAP:
        return NrawResult::Ok;
    case MARKER_PIH:
        seen |= 1;
        return parsePih(p, n, h);
    case MARKER_CDT:
        seen |= 2;
        return n == CDT_PAYLOAD && memcmp(p, CDT_EXPECT, n) == 0 ? NrawResult::Ok : NrawResult::Unsupported;
    case MARKER_WGT:
        seen |= 4;
        return parseWgt(p, n, h);
    default:
        return NrawResult::Unsupported;
    }
}

NrawResult parseHeader(const uint8_t *d, size_t n, Header &h)
{
    if (n < MARKER_LEN || be(d, 2) != MARKER_SOC) {
        return NrawResult::Unsupported;
    }

    size_t pos = MARKER_LEN;
    unsigned seen = 0;
    while (true) {
        if (pos + 4 > n) {
            return NrawResult::Corrupt;
        }
        unsigned marker = be(d + pos, 2);
        size_t len = be(d + pos + 2, 2);
        if (marker == MARKER_SLH) {
            break;
        }
        if (len < 2 || pos + MARKER_LEN + len > n) {
            return NrawResult::Corrupt;
        }
        NrawResult r = parseSegment(marker, d + pos + 4, len - 2, h, seen);
        if (r != NrawResult::Ok) {
            return r;
        }
        pos += MARKER_LEN + len;
    }

    const unsigned all = 1 | 2 | 4;
    if (seen != all) {
        return NrawResult::Unsupported;
    }
    if (h.end > n || h.end < pos + MARKER_LEN) {
        return NrawResult::Corrupt;
    }
    h.dataPos = pos;
    return NrawResult::Ok;
}

// ---- Lagen im Strom ----------------------------------------------------------------------

// Precinct ab pos: Länge, Q, R und Anfang jedes Pakets lesen, ohne zu dekodieren.
NrawResult scanPrecinct(const uint8_t *d, size_t pos, size_t end, Precinct &pr)
{
    size_t avail = end - pos;
    if (avail < PREC_HDR) {
        return NrawResult::Corrupt;
    }
    size_t lp = be(d + pos, 3);
    size_t used = PREC_HDR + lp;
    if (lp == 0 || used > avail) {
        return NrawResult::Corrupt;
    }

    pr.end = pos + used;
    pr.q = d[pos + 3];
    pr.r = d[pos + 4];
    size_t at = pos + PREC_HDR;
    for (int k = 0; k < PACKETS; k++) {
        size_t len = 0;
        pr.pkt[k] = at;
        NrawResult r = packetExtent(d + at, pr.end - at, len);
        if (r != NrawResult::Ok) {
            return r;
        }
        at += len;
    }
    return NrawResult::Ok;
}

NrawResult scanSlices(const Frame &f, const uint8_t *d, std::vector<Precinct> &precs)
{
    size_t pos = f.h.dataPos;
    size_t nslices = (f.nprec + HSL_EXPECT - 1) / HSL_EXPECT;

    for (size_t s = 0; s < nslices; s++) {
        if (pos + SLH_SIZE > f.h.end || be(d + pos, 2) != MARKER_SLH || be(d + pos + 2, 2) != SLH_LENGTH ||
            be(d + pos + 4, 2) != s) {
            return NrawResult::Corrupt;
        }
        pos += SLH_SIZE;

        size_t count = std::min(HSL_EXPECT, f.nprec - s * HSL_EXPECT);
        for (size_t i = 0; i < count; i++) {
            Precinct &pr = precs[s * HSL_EXPECT + i];
            NrawResult r = scanPrecinct(d, pos, f.h.end, pr);
            if (r != NrawResult::Ok) {
                return r;
            }
            pos = pr.end;
        }
    }
    bool eoc = pos + MARKER_LEN <= f.h.end && be(d + pos, 2) == MARKER_EOC;
    return eoc ? NrawResult::Ok : NrawResult::Corrupt;
}

// ---- Bild --------------------------------------------------------------------------------

// Layouts: Bandbreiten der Wavelet-Zerlegung (LL5, HL5..HL1), Gruppenzahlen und Offsets je Paket.
NrawResult setupFrame(Frame &f)
{
    f.compw = f.h.wf / 2;
    f.nprec = (f.h.hf / 2 + 1) / 2;
    if (f.h.wf < MIN_WIDTH || f.nprec < MIN_PRECINCTS) {
        return NrawResult::Unsupported;
    }

    // dims[l] = Breite nach l Stufen; HL(l+1) hat floor(dims[l]/2).
    size_t dims[LEVELS + 1];
    dims[0] = f.compw;
    for (int l = 0; l < LEVELS; l++) {
        dims[l + 1] = (dims[l] + 1) / 2;
    }
    f.lowW[0] = dims[LEVELS];
    for (int l = LEVELS - 1; l >= 0; l--) {
        f.lowW[LEVELS - l] = dims[l] / 2;
    }

    for (int k = 0; k < PACKETS; k++) {
        Layout &lay = f.lay[k];
        Kind kind = PACKET_DEF[k].kind;
        lay.bands = kind == Kind::Low ? MAX_BANDS : (kind == Kind::High ? 2 : 1);
        size_t sigBits = 0;
        for (int b = 0; b < lay.bands; b++) {
            lay.width[b] = kind == Kind::Low ? f.lowW[b] : (kind == Kind::High ? f.compw / 2 : f.compw);
            lay.groups[b] = (lay.width[b] + GROUP - 1) / GROUP;
            lay.gOff[b] = lay.totalGroups;
            lay.cOff[b] = lay.totalGroups * GROUP;
            lay.totalGroups += lay.groups[b];
            sigBits += (lay.groups[b] + SIG_RUN - 1) / SIG_RUN;
            if (lay.width[b] == 0) {
                return NrawResult::Unsupported;
            }
        }
        if (sigBits > SIG_BYTES * 8 || lay.groups[0] == 0) {
            return NrawResult::Unsupported;
        }
    }
    return NrawResult::Ok;
}

// Arbeitsspeicher der Wavelet-Ebenen. Die letzte Fassung bleibt für das nächste Bild liegen
// (sonst kostet das Anfassen frischer Seiten mehr als die ganze Entropie); gleichzeitige
// Aufrufe bekommen eigenen Speicher.
struct Workspace {
    Coef *mem;
    size_t count;
};

std::atomic<Workspace *> gSpare{nullptr};

void freeWorkspace(Workspace *w)
{
    if (w) {
        free(w->mem);
        delete w;
    }
}

Workspace *acquire(size_t count)
{
    Workspace *w = gSpare.exchange(nullptr);
    if (w && w->count == count) {
        return w;
    }
    freeWorkspace(w);

    size_t bytes = (count * sizeof(Coef) + ALIGN - 1) / ALIGN * ALIGN;
    void *mem = aligned_alloc(ALIGN, bytes);
    if (!mem) {
        return nullptr;
    }
    return new Workspace{static_cast<Coef *>(mem), count};
}

void release(Workspace *w)
{
    freeWorkspace(gSpare.exchange(w));
}

// Ebenen im Arbeitsspeicher verteilen: je Komponente K0, K1, K3 tief und hoch, K2 mit doppelter Höhe.
void assignPlanes(Frame &f, Coef *mem)
{
    size_t plane = f.nprec * f.compw;
    for (int c : {K0, K1, K3}) {
        f.low[c] = mem;
        f.high[c] = mem + plane;
        mem += 2 * plane;
    }
    f.low[K2] = mem;
}

struct Chain {
    size_t slice;
    int first;
    size_t cost; // Bytes der Pakete der Kette, grob die Rechenzeit
};

struct ChainJob {
    const Frame *f;
    const uint8_t *d;
    const Precinct *precs;
    const Chain *chains;
    std::atomic<int> failure{int(NrawResult::Ok)};
};

void chainTask(size_t i, void *ctx)
{
    ChainJob &job = *static_cast<ChainJob *>(ctx);
    if (job.failure.load(std::memory_order_relaxed) != int(NrawResult::Ok)) {
        return;
    }

    const Chain &c = job.chains[i];
    size_t row0 = c.slice * HSL_EXPECT;
    size_t count = std::min(HSL_EXPECT, job.f->nprec - row0);
    NrawResult r = decodeChain(*job.f, job.d, job.precs + row0, count, row0, c.first);
    if (r != NrawResult::Ok) {
        job.failure.store(int(r));
    }
}

// Unabhängige Ketten je Slice: Paket 0, 1, 3, 4, 5, 7 einzeln, 2 und 6 zusammen. Größte zuerst.
std::vector<Chain> planChains(const Frame &f, const std::vector<Precinct> &precs)
{
    constexpr int FIRSTS[] = {0, 1, 2, 3, 4, 5, 7};
    constexpr int PAIR_FIRST = 2;
    constexpr int PAIR_SECOND = 6;
    size_t nslices = (f.nprec + HSL_EXPECT - 1) / HSL_EXPECT;

    std::vector<Chain> chains;
    for (size_t s = 0; s < nslices; s++) {
        size_t row0 = s * HSL_EXPECT;
        size_t count = std::min(HSL_EXPECT, f.nprec - row0);
        for (int first : FIRSTS) {
            size_t cost = 0;
            for (size_t i = 0; i < count; i++) {
                const Precinct &pr = precs[row0 + i];
                auto size = [&](int k) { return (k + 1 < PACKETS ? pr.pkt[k + 1] : pr.end) - pr.pkt[k]; };
                cost += size(first) + (first == PAIR_FIRST ? size(PAIR_SECOND) : 0);
            }
            chains.push_back(Chain{s, first, cost});
        }
    }
    std::sort(chains.begin(), chains.end(), [](const Chain &a, const Chain &b) { return a.cost > b.cost; });
    return chains;
}

struct TileJob {
    const Frame *f;
    const LevelTable *levels;
    uint16_t *bayer;
};

void tileTask(size_t i, void *ctx)
{
    const TileJob &job = *static_cast<TileJob *>(ctx);
    decodeTile(*job.f, i, *job.levels, job.bayer);
}

// Ausgabestufe je Tabellenstufe: (Wert + 2) >> 2, auf 16 Bit begrenzt.
std::vector<uint16_t> levelsOf(const NrawCurve &curve)
{
    std::vector<uint16_t> levels(curve.count);
    for (size_t i = 0; i < curve.count; i++) {
        int v = (curve.values[i] + CURVE_ROUND) >> CURVE_SHIFT;
        levels[i] = uint16_t(std::clamp(v, 0, OUT_MAX));
    }
    return levels;
}

NrawResult decodeBody(Frame &f, const uint8_t *cs, const std::vector<uint16_t> &levels, uint16_t *bayer)
{
    std::vector<Precinct> precs(f.nprec);
    NrawResult r = scanSlices(f, cs, precs);
    if (r != NrawResult::Ok) {
        return r;
    }

    Workspace *ws = acquire(PLANE_COUNT * f.nprec * f.compw);
    if (!ws) {
        return NrawResult::Unsupported;
    }
    assignPlanes(f, ws->mem);

    std::vector<Chain> chains = planChains(f, precs);
    ChainJob chainJob{&f, cs, precs.data(), chains.data()};
    parallelFor(chains.size(), chainTask, &chainJob);
    r = NrawResult(chainJob.failure.load());
    if (r == NrawResult::Ok) {
        LevelTable table{levels.data(), int(levels.size()) - 1};
        TileJob tileJob{&f, &table, bayer};
        parallelFor(tileCount(f), tileTask, &tileJob);
    }

    release(ws);
    return r;
}

} // namespace

NrawResult decodeNraw(const uint8_t *cs, size_t size, const NrawCurve &curve, uint16_t *bayer, size_t bayerCount)
{
    Frame f;
    NrawResult r = parseHeader(cs, size, f.h);
    if (r != NrawResult::Ok) {
        return r;
    }
    if (bayerCount != f.h.wf * f.h.hf || curve.count == 0 || !curve.values) {
        return NrawResult::Unsupported;
    }
    r = setupFrame(f);
    if (r != NrawResult::Ok) {
        return r;
    }
    return decodeBody(f, cs, levelsOf(curve), bayer);
}
