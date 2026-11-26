// N-RAW-Referenzdecoder (siehe nraw_decode.h). Stufen:
//
//   Codestream ─► Kopf ─► Slices/Precincts/Pakete (Entropie, Dequantisierung)
//              ─► inverse 5/3-Wavelet je Komponente ─► inverse Star-Tetrix auf dem CFA-Gitter
//              ─► Kennlinie ─► Bayer RGGB
//
// 4 Komponenten (halbe Breite und Höhe): K0 Helligkeit, K1/K3 Farbdifferenzen, K2 ohne Wavelet.
// Je Precinct (2 Komponentenzeilen) 8 Pakete: 0/1/3 = K0/K1/K3 tief (LL5, HL5..HL1), 4/5/7 = K0/K1/K3
// hoch (LH1, HH1), 2/6 = K2 Zeile 0/1.
#include "nraw_decode.h"

#include <algorithm>
#include <cstring>
#include <vector>

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
constexpr size_t WGT_COUNT = 25;
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
constexpr int MAX_GCLI = 18;      // Bw
constexpr int MAX_TRUNC = 15;
constexpr int FQ_SHIFT = 4;
constexpr int LEVELS = 5;         // horizontale Wavelet-Stufen

constexpr size_t PREC_HDR = 12;   // Lprc (3), Q, R, 7 Byte Zusatz
constexpr size_t PKT_HDR = 7;     // Ldat 20, Lcnt 20, Lsgn 15 Bit
constexpr size_t SIG_BYTES = 9;   // Signifikanz-Bits des Pakets
constexpr size_t PKT_FIXED = PKT_HDR + SIG_BYTES;
constexpr int PACKETS = 8;
constexpr int MAX_BANDS = 6;
constexpr size_t GROUP = 4;       // Koeffizienten je Gruppe
constexpr size_t SIG_RUN = 8;     // Gruppen je Signifikanz-Bit

constexpr int CURVE_OFFSET = 32768; // Mitte des Wavelet-Wertebereichs
constexpr int CURVE_ROUND = 2;      // 4 Stufen der Tabelle je Ausgabestufe
constexpr int CURVE_SHIFT = 2;
constexpr int OUT_MAX = 65535;

enum class Kind { Low, High, One };

struct PacketDef {
    Kind kind;
    int wgt; // erster Eintrag in der WGT
    int comp;
};

enum Comp { K0, K1, K2, K3, COMPS };

constexpr PacketDef PACKET_DEF[PACKETS] = {
    {Kind::Low, 0, K0},  {Kind::Low, 6, K1},  {Kind::One, 12, K2},  {Kind::Low, 13, K3},
    {Kind::High, 19, K0}, {Kind::High, 21, K1}, {Kind::One, 12, K2}, {Kind::High, 23, K3},
};

// Lage der Komponente im 2x2-Block des CFA-Gitters (Zeile, Spalte).
struct Pos {
    int dy;
    int dx;
};
constexpr Pos COMP_POS[COMPS] = {{0, 1}, {0, 0}, {1, 0}, {1, 1}};

struct Header {
    size_t wf = 0;
    size_t hf = 0;
    size_t end = 0;     // Ende des Codestreams (Lcod)
    size_t dataPos = 0; // erster SLH
    uint8_t gain[WGT_COUNT] = {};
    uint8_t prio[WGT_COUNT] = {};
};

uint64_t be(const uint8_t *p, int bytes)
{
    uint64_t v = 0;
    for (int i = 0; i < bytes; i++) {
        v = (v << 8) | p[i];
    }
    return v;
}

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

// ---- Entropie ----------------------------------------------------------------------------

// Bitleser, MSB zuerst. Hinter dem Ende liefert er 0 und merkt es sich (bad).
class Bits {
public:
    Bits(const uint8_t *p, size_t bytes) : p_(p), nbits_(bytes * 8) {}

    int bit()
    {
        if (pos_ >= nbits_) {
            bad_ = true;
            return 0;
        }
        int v = (p_[pos_ >> 3] >> (7 - (pos_ & 7))) & 1;
        pos_++;
        return v;
    }

    bool bad() const { return bad_; }

private:
    const uint8_t *p_;
    size_t nbits_;
    size_t pos_ = 0;
    bool bad_ = false;
};

// Vorhersage aus dem Paket darüber: GCLI je Gruppe und Abschneidestufe je Band.
struct Pred {
    std::vector<uint8_t> g;
    uint8_t t[MAX_BANDS] = {};
    bool valid = false;
};

struct Layout {
    int bands = 0;
    size_t groups[MAX_BANDS] = {};
    size_t gOff[MAX_BANDS] = {};  // Start der Bandgruppen in Pred::g
    size_t cOff[MAX_BANDS] = {};  // Start der Bandkoeffizienten in vals
    size_t width[MAX_BANDS] = {}; // gültige Koeffizienten je Band
    size_t totalGroups = 0;
};

// Abschneidestufe (GTLI) eines Bandes: Verstärkung g, Priorität p, Quantisierung q, Verfeinerung r.
int truncation(int g, int p, int q, int r)
{
    int up = p < r ? 1 : 0;
    if (q < g + up) {
        return 0;
    }
    return std::min(q - g - up, MAX_TRUNC);
}

// GCLI-Werte eines Bandes: je 8 Gruppen ein Signifikanz-Bit (1 = alle wie vorhergesagt), sonst je
// Gruppe ein unärer Wert, der den Abstand zur Vorhersage (Maximum aus oberem Nachbarn und GTLI) codiert.
void gcliBand(Bits &sig, Bits &cnt, const uint8_t *top, int topT, int t, size_t groups, uint8_t *g)
{
    int tt = top ? std::max(topT, t) : t;

    for (size_t s0 = 0; s0 < groups; s0 += SIG_RUN) {
        bool quiet = sig.bit() == 1;
        size_t end = std::min(s0 + SIG_RUN, groups);

        for (size_t j = s0; j < end; j++) {
            int mt = std::max(top ? int(top[j]) : 0, tt);
            if (quiet) {
                g[j] = mt;
                continue;
            }

            int x = 0;
            while (cnt.bit() == 1) {
                x++;
            }
            int th = std::max(mt - t, 0);
            int dm = 0;
            if (x > 2 * th) {
                dm = x - th;
            } else if (x & 1) {
                dm = -((x + 1) / 2);
            } else {
                dm = x / 2;
            }
            // Außerhalb der Bittiefe kommt nur ein beschädigter Strom (der Leser hat dann schon bad).
            g[j] = std::clamp(mt + dm, 0, MAX_GCLI + 1);
        }
    }
}

// Gleichmäßige Dequantisierung: Betrag um die abgeschnittenen Bitebenen mit Rundungsanteil zurück.
int dequant(int m, int t, int gcli)
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

// Bitebenen eines Bandes (je Gruppe 4 Koeffizienten parallel), danach die Vorzeichen der Werte != 0.
void dataBand(Bits &dat, Bits &sgn, const uint8_t *g, size_t groups, int t, int32_t *out)
{
    for (size_t j = 0; j < groups; j++) {
        int planes = std::max(int(g[j]) - t, 0);
        int c[GROUP] = {};
        for (int k = 0; k < planes; k++) {
            for (size_t e = 0; e < GROUP; e++) {
                c[e] = (c[e] << 1) | dat.bit();
            }
        }

        for (size_t e = 0; e < GROUP; e++) {
            int32_t v = 0;
            if (c[e] != 0) {
                v = dequant(c[e], t, g[j]);
                v = sgn.bit() ? -v : v;
            }
            out[j * GROUP + e] = v * (1 << FQ_SHIFT);
        }
    }
}

struct Frame {
    Header h;
    size_t compw = 0;
    size_t nprec = 0;
    size_t lowW[MAX_BANDS] = {};
    Layout lay[PACKETS];
    Pred pred[PACKETS];
    std::vector<int32_t> low[COMPS];  // nprec Zeilen (K2: 2*nprec)
    std::vector<int32_t> high[COMPS]; // nprec Zeilen
    std::vector<int32_t> vals;        // Koeffizienten des aktuellen Pakets
};

// Quelle der Vorhersage: K2 Zeile 0 sieht Zeile 1 des vorigen Precincts, Zeile 1 sieht Zeile 0.
int predSource(int k)
{
    if (k == 2) {
        return 6;
    }
    return k == 6 ? 2 : k;
}

// Pakete eines Precincts: Layout, GTLI je Band, Vorhersage, dann Entropie.
NrawResult decodePacket(Frame &f, int k, const uint8_t *q, size_t avail, const int *gt, size_t &used, Pred &next)
{
    const Layout &lay = f.lay[k];
    if (avail < PKT_FIXED) {
        return NrawResult::Corrupt;
    }

    uint64_t hdr = be(q, PKT_HDR);
    size_t ld = (hdr >> 35) & 0xfffff;
    size_t lc = (hdr >> 15) & 0xfffff;
    size_t ls = hdr & 0x7fff;
    used = PKT_FIXED + ld + lc + ls;
    if (used > avail) {
        return NrawResult::Corrupt;
    }

    Bits sig(q + PKT_HDR, SIG_BYTES);
    Bits cnt(q + PKT_FIXED, lc);
    Bits dat(q + PKT_FIXED + lc, ld);
    Bits sgn(q + PKT_FIXED + lc + ld, ls);

    const Pred &src = f.pred[predSource(k)];
    next.g.assign(lay.totalGroups, 0);
    f.vals.assign(lay.cOff[lay.bands - 1] + lay.groups[lay.bands - 1] * GROUP, 0);

    for (int b = 0; b < lay.bands; b++) {
        uint8_t *g = next.g.data() + lay.gOff[b];
        const uint8_t *top = src.valid ? src.g.data() + lay.gOff[b] : nullptr;
        gcliBand(sig, cnt, top, src.t[b], gt[b], lay.groups[b], g);
        dataBand(dat, sgn, g, lay.groups[b], gt[b], f.vals.data() + lay.cOff[b]);
        next.t[b] = gt[b];
    }

    for (int b = 0; b < lay.bands; b++) {
        for (size_t j = 0; j < lay.groups[b]; j++) {
            if (next.g[lay.gOff[b] + j] > MAX_GCLI) {
                return NrawResult::Corrupt;
            }
        }
    }
    if (sig.bad() || cnt.bad() || dat.bad() || sgn.bad()) {
        return NrawResult::Corrupt;
    }
    next.valid = true;
    return NrawResult::Ok;
}

// ---- Wavelet -----------------------------------------------------------------------------

// Inverse 5/3-Hebung entlang einer Achse. l hat nl (aufgerundet), h hat nh Werte (nh <= nl),
// Ergebnis nl+nh Werte. Am Rand wird gespiegelt.
void inv53(const int32_t *l, ptrdiff_t ls, size_t nl, const int32_t *h, ptrdiff_t hs, size_t nh, int32_t *out,
           ptrdiff_t os)
{
    for (size_t i = 0; i < nl; i++) {
        int32_t hl = h[ptrdiff_t(i > 0 ? i - 1 : 0) * hs];
        int32_t hr = h[ptrdiff_t(std::min(i, nh - 1)) * hs];
        out[ptrdiff_t(2 * i) * os] = l[ptrdiff_t(i) * ls] - ((hl + hr + 2) >> 2);
    }

    for (size_t i = 0; i < nh; i++) {
        int32_t e0 = out[ptrdiff_t(2 * i) * os];
        int32_t e1 = out[ptrdiff_t(2 * std::min(i + 1, nl - 1)) * os];
        out[ptrdiff_t(2 * i + 1) * os] = h[ptrdiff_t(i) * hs] + ((e0 + e1) >> 1);
    }
}

// Tiefe Zeile einer Komponente: LL5 plus HL5..HL1 waagerecht zusammensetzen.
void rowLow(const int32_t *vals, const Layout &lay, int32_t *out)
{
    std::vector<int32_t> a(vals, vals + lay.width[0]);
    std::vector<int32_t> b;
    size_t w = lay.width[0];

    for (int i = 1; i < lay.bands; i++) {
        size_t wh = lay.width[i];
        b.assign(w + wh, 0);
        inv53(a.data(), 1, w, vals + lay.cOff[i], 1, wh, b.data(), 1);
        a.swap(b);
        w += wh;
    }
    std::copy(a.begin(), a.end(), out);
}

void storePacket(Frame &f, int k, size_t row)
{
    const PacketDef &def = PACKET_DEF[k];
    const Layout &lay = f.lay[k];
    const int32_t *v = f.vals.data();
    size_t w = f.compw;

    if (def.kind == Kind::Low) {
        rowLow(v, lay, f.low[def.comp].data() + row * w);
        return;
    }
    if (def.kind == Kind::High) {
        inv53(v + lay.cOff[0], 1, lay.width[0], v + lay.cOff[1], 1, lay.width[1], f.high[def.comp].data() + row * w, 1);
        return;
    }
    size_t line = 2 * row + (k == 6 ? 1 : 0);
    std::copy(v, v + w, f.low[K2].data() + line * w);
}

// ---- Slices ------------------------------------------------------------------------------

NrawResult decodePrecinct(Frame &f, const uint8_t *p, size_t avail, size_t row, size_t &used)
{
    if (avail < PREC_HDR) {
        return NrawResult::Corrupt;
    }
    size_t lp = be(p, 3);
    int q = p[3];
    int r = p[4];
    used = PREC_HDR + lp;
    if (lp == 0 || used > avail) {
        return NrawResult::Corrupt;
    }

    size_t pos = PREC_HDR;
    for (int k = 0; k < PACKETS; k++) {
        const PacketDef &def = PACKET_DEF[k];
        int gt[MAX_BANDS];
        for (int b = 0; b < f.lay[k].bands; b++) {
            gt[b] = truncation(f.h.gain[def.wgt + b], f.h.prio[def.wgt + b], q, r);
        }

        Pred next;
        size_t pktUsed = 0;
        NrawResult res = decodePacket(f, k, p + pos, used - pos, gt, pktUsed, next);
        if (res != NrawResult::Ok) {
            return res;
        }
        pos += pktUsed;
        f.pred[k] = std::move(next);
        storePacket(f, k, row);
    }
    return NrawResult::Ok;
}

NrawResult decodeSlices(Frame &f, const uint8_t *d)
{
    size_t pos = f.h.dataPos;
    size_t nslices = (f.nprec + HSL_EXPECT - 1) / HSL_EXPECT;

    for (size_t s = 0; s < nslices; s++) {
        if (pos + SLH_SIZE > f.h.end || be(d + pos, 2) != MARKER_SLH || be(d + pos + 2, 2) != SLH_LENGTH ||
            be(d + pos + 4, 2) != s) {
            return NrawResult::Corrupt;
        }
        pos += SLH_SIZE;

        for (Pred &pr : f.pred) {
            pr.valid = false;
        }
        size_t count = std::min(HSL_EXPECT, f.nprec - s * HSL_EXPECT);
        for (size_t i = 0; i < count; i++) {
            size_t used = 0;
            NrawResult r = decodePrecinct(f, d + pos, f.h.end - pos, s * HSL_EXPECT + i, used);
            if (r != NrawResult::Ok) {
                return r;
            }
            pos += used;
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
        }
        if (sigBits > SIG_BYTES * 8 || lay.groups[0] == 0) {
            return NrawResult::Unsupported;
        }
    }

    for (int c = 0; c < COMPS; c++) {
        f.low[c].assign(f.nprec * f.compw * (c == K2 ? 2 : 1), 0);
        f.high[c].assign(c == K2 ? 0 : f.nprec * f.compw, 0);
    }
    return NrawResult::Ok;
}

// Vertikale Rücktransformation jeder Komponente, die Ergebniszeilen gleich an ihre Lage im Gitter.
void toLattice(const Frame &f, std::vector<int32_t> &lat)
{
    size_t latW = 2 * f.compw;
    size_t rows = 2 * f.nprec;

    for (int c = 0; c < COMPS; c++) {
        int32_t *base = lat.data() + COMP_POS[c].dy * latW + COMP_POS[c].dx;
        if (c == K2) {
            for (size_t y = 0; y < rows; y++) {
                for (size_t x = 0; x < f.compw; x++) {
                    base[y * 2 * latW + x * 2] = f.low[c][y * f.compw + x];
                }
            }
            continue;
        }
        for (size_t x = 0; x < f.compw; x++) {
            inv53(f.low[c].data() + x, ptrdiff_t(f.compw), f.nprec, f.high[c].data() + x, ptrdiff_t(f.compw), f.nprec,
                  base + 2 * x, ptrdiff_t(2 * latW));
        }
    }
}

struct Lattice {
    int32_t *d;
    int h;
    int w;

    // Spiegelung am Rand (gerade Größe: die Parität bleibt erhalten).
    int32_t at(int y, int x) const
    {
        y = y < 0 ? -y : (y >= h ? 2 * h - 2 - y : y);
        x = x < 0 ? -x : (x >= w ? 2 * w - 2 - x : x);
        return d[size_t(y) * w + x];
    }
    int32_t &ref(int y, int x) const { return d[size_t(y) * w + x]; }
    int32_t diag(int y, int x) const { return at(y - 1, x - 1) + at(y - 1, x + 1) + at(y + 1, x - 1) + at(y + 1, x + 1); }
    int32_t cross(int y, int x) const { return at(y, x - 1) + at(y, x + 1) + at(y - 1, x) + at(y + 1, x); }
};

enum class Step { Diag0, Diag3, Cross0, Cross3, Last };

// Ein Schritt an einer Klasse, deren Lage im 2x2-Block (y0, x0) ist. Die Nachbarn gehören zu
// anderen Klassen, die Reihenfolge innerhalb des Schritts ist darum gleichgültig.
void tetrixClass(const Lattice &L, Step step, int y0, int x0)
{
    for (int y = y0; y < L.h; y += 2) {
        for (int x = x0; x < L.w; x += 2) {
            switch (step) {
            case Step::Diag0:
                L.ref(y, x) -= L.diag(y, x) >> 3;
                break;
            case Step::Diag3:
                L.ref(y, x) += L.diag(y, x) >> 2;
                break;
            case Step::Cross0:
            case Step::Cross3:
                L.ref(y, x) -= L.cross(y, x) >> 3;
                break;
            case Step::Last:
                L.ref(y, x) += L.cross(y, x) >> 2;
                break;
            }
        }
    }
}

void tetrixStep(const Lattice &L, Step step)
{
    // Klassen im 2x2-Block: 0 = (0,1), 3 = (1,0), 1 = (1,1), 2 = (0,0); Schritt 5 trifft 1 und 2.
    switch (step) {
    case Step::Diag0:
    case Step::Cross0:
        tetrixClass(L, step, 0, 1);
        return;
    case Step::Diag3:
    case Step::Cross3:
        tetrixClass(L, step, 1, 0);
        return;
    case Step::Last:
        tetrixClass(L, step, 0, 0);
        tetrixClass(L, step, 1, 1);
        return;
    }
}

void emit(const Frame &f, const std::vector<int32_t> &lat, const NrawCurve &curve, uint16_t *bayer)
{
    size_t latW = 2 * f.compw;
    int maxIdx = int(curve.count) - 1;

    for (size_t y = 0; y < f.h.hf; y++) {
        for (size_t x = 0; x < f.h.wf; x++) {
            int idx = std::clamp(int(lat[y * latW + x]) + CURVE_OFFSET, 0, maxIdx);
            int v = (curve.values[idx] + CURVE_ROUND) >> CURVE_SHIFT;
            bayer[y * f.h.wf + x] = uint16_t(std::clamp(v, 0, OUT_MAX));
        }
    }
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
    r = decodeSlices(f, cs);
    if (r != NrawResult::Ok) {
        return r;
    }

    // Gitter mit 2 * (2 * nprec) Zeilen: ein Precinct deckt 2 Komponentenzeilen = 4 CFA-Zeilen.
    size_t latW = 2 * f.compw;
    std::vector<int32_t> lat(latW * 4 * f.nprec, 0);
    toLattice(f, lat);

    Lattice L{lat.data(), int(4 * f.nprec), int(latW)};
    for (Step s : {Step::Diag0, Step::Diag3, Step::Cross0, Step::Cross3, Step::Last}) {
        tetrixStep(L, s);
    }
    emit(f, lat, curve, bayer);
    return NrawResult::Ok;
}
