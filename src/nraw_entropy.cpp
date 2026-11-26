// N-RAW-Decoder, Entropie: Pakete lesen, GCLI vorhersagen, dequantisieren und die Zeile gleich
// waagerecht zurücktransformieren. Eine Kette (Slice, Paketart) hängt über die vertikale
// Vorhersage von Precinct zu Precinct; verschiedene Ketten sind unabhängig.
#include <algorithm>
#include <climits>
#include <cstring>
#include <vector>

#include "nraw_frame.h"

namespace nraw {

namespace {

constexpr int MAX_GCLI = 18; // Bw
constexpr int MAX_TRUNC = 15;
constexpr int FQ_SHIFT = 4;
constexpr int PACKET_K2A = 2; // K2 Zeile 0
constexpr int PACKET_K2B = 6; // K2 Zeile 1

// Bitleser, MSB zuerst. Hinter dem Ende liefert er 0 und merkt es sich (bad): ein solcher Strom
// ist beschädigt, die danach gelesenen Werte zählen nicht.
class Bits {
public:
    Bits(const uint8_t *p, size_t bytes) : p_(p), bytes_(bytes), nbits_(bytes * 8) {}

    // n Bits (1..MAX_READ) als Zahl.
    uint64_t bits(unsigned n)
    {
        if (n == 0) {
            return 0;
        }
        if (pos_ + n > nbits_) {
            bad_ = true;
            pos_ = nbits_;
            return 0;
        }
        uint64_t w = window();
        pos_ += n;
        return w >> (BITS64 - n);
    }

    int bit()
    {
        if (pos_ >= nbits_) {
            bad_ = true;
            return 0;
        }
        int v = (p_[pos_ >> 3] >> (BYTE_MASK - (pos_ & BYTE_MASK))) & 1;
        pos_++;
        return v;
    }

    // Zählt 1-Bits bis zur nächsten 0 (wird mitgelesen); ohne 0 vor dem Ende ist bad gesetzt.
    int ones()
    {
        int x = 0;
        while (pos_ < nbits_) {
            size_t avail = std::min<size_t>(BITS64 - (pos_ & BYTE_MASK), nbits_ - pos_);
            uint64_t inverted = ~window();
            size_t run = inverted ? size_t(__builtin_clzll(inverted)) : BITS64;
            if (run < avail) {
                pos_ += run + 1;
                return x + int(run);
            }
            x += int(avail);
            pos_ += avail;
        }
        bad_ = true;
        return x;
    }

    bool bad() const { return bad_; }

private:
public:
    static constexpr unsigned MAX_READ = 56; // 64 Bit Fenster minus die Lage im Byte

private:
    static constexpr unsigned BITS64 = 64;
    static constexpr size_t BYTE_MASK = 7;
    static constexpr size_t WORD_BYTES = 8;

    // 64 Bit ab der Leseposition, linksbündig; hinter dem Ende Nullen.
    uint64_t window() const
    {
        size_t at = pos_ >> 3;
        uint64_t w = 0;
        if (at + WORD_BYTES <= bytes_) {
            memcpy(&w, p_ + at, WORD_BYTES);
            w = __builtin_bswap64(w);
        } else {
            for (size_t i = 0; i < WORD_BYTES; i++) {
                w = (w << 8) | (at + i < bytes_ ? p_[at + i] : 0);
            }
        }
        return w << (pos_ & BYTE_MASK);
    }

    const uint8_t *p_;
    size_t bytes_;
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

            int x = cnt.ones();
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

// Bitebenen einer Gruppe (4 Koeffizienten parallel): je Ebene 4 Bits, Koeffizient 0 zuerst.
void planesOf(Bits &dat, int planes, uint32_t (&c)[GROUP])
{
    c[0] = c[1] = c[2] = c[3] = 0;
    while (planes > 0) {
        int n = std::min(planes, int(Bits::MAX_READ / GROUP));
        uint64_t chunk = dat.bits(unsigned(n) * GROUP);
        for (int k = n - 1; k >= 0; k--) {
            uint32_t nibble = uint32_t(chunk >> (k * GROUP)) & 0xf;
            c[0] = (c[0] << 1) | (nibble >> 3);
            c[1] = (c[1] << 1) | ((nibble >> 2) & 1);
            c[2] = (c[2] << 1) | ((nibble >> 1) & 1);
            c[3] = (c[3] << 1) | (nibble & 1);
        }
        planes -= n;
    }
}

// Bitebenen eines Bandes, danach die Vorzeichen der Werte != 0.
// Schreibt jede der groups * GROUP Positionen.
void dataBand(Bits &dat, Bits &sgn, const uint8_t *g, size_t groups, int t, int32_t *out)
{
    for (size_t j = 0; j < groups; j++) {
        int planes = std::max(int(g[j]) - t, 0);
        if (planes == 0) {
            memset(out + j * GROUP, 0, GROUP * sizeof *out);
            continue;
        }

        uint32_t c[GROUP];
        planesOf(dat, planes, c);

        for (size_t e = 0; e < GROUP; e++) {
            int32_t v = 0;
            if (c[e] != 0) {
                v = dequant(int(c[e]), t, g[j]);
                v = sgn.bit() ? -v : v;
            }
            out[j * GROUP + e] = v * (1 << FQ_SHIFT);
        }
    }
}

// Paket k eines Precincts: GCLI je Band (Vorhersage src, Ergebnis in next), dann die Werte in vals.
NrawResult decodePacket(const Layout &lay, const uint8_t *q, size_t avail, const int *gt, const Pred &src,
                        Pred &next, int32_t *vals)
{
    size_t used = 0;
    NrawResult res = packetExtent(q, avail, used);
    if (res != NrawResult::Ok) {
        return res;
    }

    uint64_t hdr = be(q, PKT_HDR);
    size_t ld = (hdr >> 35) & 0xfffff;
    size_t lc = (hdr >> 15) & 0xfffff;
    size_t ls = hdr & 0x7fff;

    Bits sig(q + PKT_HDR, SIG_BYTES);
    Bits cnt(q + PKT_FIXED, lc);
    Bits dat(q + PKT_FIXED + lc, ld);
    Bits sgn(q + PKT_FIXED + lc + ld, ls);

    next.g.assign(lay.totalGroups, 0);
    for (int b = 0; b < lay.bands; b++) {
        uint8_t *g = next.g.data() + lay.gOff[b];
        const uint8_t *top = src.valid ? src.g.data() + lay.gOff[b] : nullptr;
        gcliBand(sig, cnt, top, src.t[b], gt[b], lay.groups[b], g);
        dataBand(dat, sgn, g, lay.groups[b], gt[b], vals + lay.cOff[b]);
        next.t[b] = gt[b];
    }

    for (uint8_t g : next.g) {
        if (g > MAX_GCLI) {
            return NrawResult::Corrupt;
        }
    }
    if (sig.bad() || cnt.bad() || dat.bad() || sgn.bad()) {
        return NrawResult::Corrupt;
    }
    next.valid = true;
    return NrawResult::Ok;
}

// ---- Waagerechte Rücktransformation ------------------------------------------------------

// Inverse 5/3-Hebung: l hat nl (aufgerundet), h hat nh Werte (nh <= nl, nh >= 1), Ergebnis nl+nh
// Werte. Am Rand wird gespiegelt; das Innere ist eine einfache Schleife.
void inv53(const int32_t *l, size_t nl, const int32_t *h, size_t nh, int32_t *out)
{
    out[0] = l[0] - ((h[0] + h[0] + 2) >> 2);
    size_t inner = std::min(nl, nh);
    for (size_t i = 1; i < inner; i++) {
        out[2 * i] = l[i] - ((h[i - 1] + h[i] + 2) >> 2);
    }
    for (size_t i = inner; i < nl; i++) {
        out[2 * i] = l[i] - ((h[i - 1] + h[nh - 1] + 2) >> 2);
    }

    size_t last = nh == nl ? nh - 1 : nh; // letzter Index, dessen rechter Nachbar gespiegelt wird
    for (size_t i = 0; i < last; i++) {
        out[2 * i + 1] = h[i] + ((out[2 * i] + out[2 * i + 2]) >> 1);
    }
    for (size_t i = last; i < nh; i++) {
        out[2 * i + 1] = h[i] + ((out[2 * i] + out[2 * std::min(i + 1, nl - 1)]) >> 1);
    }
}

// Tiefe Zeile einer Komponente: LL5 plus HL5..HL1 waagerecht zusammensetzen. s0/s1 sind
// Zwischenpuffer; die letzte Stufe schreibt gleich nach out.
void rowLow(const int32_t *vals, const Layout &lay, int32_t *out, int32_t *s0, int32_t *s1)
{
    const int32_t *in = vals;
    size_t w = lay.width[0];

    for (int i = 1; i < lay.bands; i++) {
        int32_t *dst = i == lay.bands - 1 ? out : (i % 2 ? s0 : s1);
        inv53(in, w, vals + lay.cOff[i], lay.width[i], dst);
        in = dst;
        w += lay.width[i];
    }
}

struct Scratch {
    std::vector<int32_t> vals;
    std::vector<int32_t> a;
    std::vector<int32_t> b;
};

// Legt die Zeile des Pakets k in den Ebenen ab (waagerecht zurücktransformiert).
void storePacket(const Frame &f, int k, size_t row, const int32_t *v, Scratch &sc)
{
    const PacketDef &def = PACKET_DEF[k];
    const Layout &lay = f.lay[k];
    size_t w = f.compw;

    if (def.kind == Kind::Low) {
        rowLow(v, lay, f.low[def.comp] + row * w, sc.a.data(), sc.b.data());
        return;
    }
    if (def.kind == Kind::High) {
        inv53(v + lay.cOff[0], lay.width[0], v + lay.cOff[1], lay.width[1], f.high[def.comp] + row * w);
        return;
    }
    size_t line = 2 * row + (k == PACKET_K2B ? 1 : 0);
    memcpy(f.low[K2] + line * w, v, w * sizeof(Coef));
}

} // namespace

NrawResult packetExtent(const uint8_t *q, size_t avail, size_t &used)
{
    if (avail < PKT_FIXED) {
        return NrawResult::Corrupt;
    }

    uint64_t hdr = be(q, PKT_HDR);
    size_t ld = (hdr >> 35) & 0xfffff;
    size_t lc = (hdr >> 15) & 0xfffff;
    size_t ls = hdr & 0x7fff;
    used = PKT_FIXED + ld + lc + ls;
    return used > avail ? NrawResult::Corrupt : NrawResult::Ok;
}

void gtliTable(const Frame &f, const Precinct *precs, size_t count, uint8_t *out)
{
    for (size_t p = 0; p < count; p++) {
        for (size_t w = 0; w < WGT_COUNT; w++) {
            out[p * WGT_COUNT + w] = uint8_t(truncation(f.h.gain[w], f.h.prio[w], precs[p].q, precs[p].r));
        }
    }
}

NrawResult decodeChain(const Frame &f, const uint8_t *d, const Precinct *precs, size_t count, size_t row0, int first)
{
    // Kette 2/6: Paket 2 sieht Paket 6 des vorigen Precincts, Paket 6 sieht Paket 2 des selben.
    const bool pair = first == PACKET_K2A;
    const int steps = pair ? 2 : 1;
    const int ks[2] = {first, PACKET_K2B};

    size_t maxVals = 0;
    for (int j = 0; j < steps; j++) {
        maxVals = std::max(maxVals, f.lay[ks[j]].totalGroups * GROUP);
    }
    Scratch sc;
    sc.vals.resize(maxVals);
    sc.a.resize(f.compw);
    sc.b.resize(f.compw);

    Pred state[2];
    Pred next;
    for (size_t i = 0; i < count; i++) {
        const Precinct &pr = precs[i];
        for (int j = 0; j < steps; j++) {
            const int k = ks[j];
            const PacketDef &def = PACKET_DEF[k];
            int gt[MAX_BANDS];
            for (int b = 0; b < f.lay[k].bands; b++) {
                gt[b] = truncation(f.h.gain[def.wgt + b], f.h.prio[def.wgt + b], pr.q, pr.r);
            }

            const Pred &src = state[pair ? 1 - j : 0];
            NrawResult res = decodePacket(f.lay[k], d + pr.pkt[k], pr.end - pr.pkt[k], gt, src, next, sc.vals.data());
            if (res != NrawResult::Ok) {
                return res;
            }
            std::swap(state[j], next);
            storePacket(f, k, row0 + i, sc.vals.data(), sc);
        }
    }
    return NrawResult::Ok;
}

} // namespace nraw
