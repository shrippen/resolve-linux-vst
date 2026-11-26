// Gemeinsame Typen der N-RAW-Decoderdateien (nraw_decode.cpp, nraw_entropy.cpp, nraw_color.cpp).
// Nicht öffentlich: die Schnittstelle nach außen ist nraw_decode.h.
#pragma once

#include <cstddef>
#include <cstdint>

#include "nraw_decode.h"

namespace nraw {

constexpr size_t WGT_COUNT = 25;
constexpr size_t PREC_HDR = 12;   // Lprc (3), Q, R, 7 Byte Zusatz
constexpr size_t PKT_HDR = 7;     // Ldat 20, Lcnt 20, Lsgn 15 Bit
constexpr size_t SIG_BYTES = 9;   // Signifikanz-Bits des Pakets
constexpr size_t PKT_FIXED = PKT_HDR + SIG_BYTES;
constexpr int PACKETS = 8;
constexpr int MAX_BANDS = 6;
constexpr size_t GROUP = 4;       // Koeffizienten je Gruppe
constexpr size_t SIG_RUN = 8;     // Gruppen je Signifikanz-Bit

enum class Kind { Low, High, One };

struct PacketDef {
    Kind kind;
    int wgt; // erster Eintrag in der WGT
    int comp;
};

// K0 Helligkeit, K1/K3 Farbdifferenzen, K2 ohne Wavelet.
enum Comp { K0, K1, K2, K3, COMPS };

constexpr PacketDef PACKET_DEF[PACKETS] = {
    {Kind::Low, 0, K0},  {Kind::Low, 6, K1},  {Kind::One, 12, K2},  {Kind::Low, 13, K3},
    {Kind::High, 19, K0}, {Kind::High, 21, K1}, {Kind::One, 12, K2}, {Kind::High, 23, K3},
};

struct Header {
    size_t wf = 0;
    size_t hf = 0;
    size_t end = 0;     // Ende des Codestreams (Lcod)
    size_t dataPos = 0; // erster SLH
    uint8_t gain[WGT_COUNT] = {};
    uint8_t prio[WGT_COUNT] = {};
};

struct Layout {
    int bands = 0;
    size_t groups[MAX_BANDS] = {};
    size_t gOff[MAX_BANDS] = {};  // Start der Bandgruppen in Pred::g
    size_t cOff[MAX_BANDS] = {};  // Start der Bandkoeffizienten in vals
    size_t width[MAX_BANDS] = {}; // gültige Koeffizienten je Band
    size_t totalGroups = 0;
};

// Wavelet-Ebenen im Arbeitsspeicher, je nprec Zeilen zu compw Werten (K2: 2 * nprec Zeilen).
// Tiefe Zeile = LL5 + HL5..HL1 waagerecht zurücktransformiert, hohe Zeile = LH1 + HH1.
using Coef = int32_t; // Wert der Wavelet-Ebenen

struct Frame {
    Header h;
    size_t compw = 0; // Breite einer Komponente
    size_t nprec = 0; // Precincts: je 2 Komponentenzeilen
    size_t lowW[MAX_BANDS] = {};
    Layout lay[PACKETS];
    Coef *low[COMPS] = {};
    Coef *high[COMPS] = {}; // K2: keine
};

// Lage eines Precincts im Strom, vorab gelesen (nraw_decode.cpp), damit Ketten parallel laufen.
struct Precinct {
    size_t end = 0; // Ende im Strom
    int q = 0;
    int r = 0;
    size_t pkt[PACKETS] = {}; // Anfang der Pakete
    bool plain = true;        // Standardaufbau (D = 3 je Band, kein Raw-Modus): die GPU darf es dekodieren
};

inline uint64_t be(const uint8_t *p, int bytes)
{
    uint64_t v = 0;
    for (int i = 0; i < bytes; i++) {
        v = (v << 8) | p[i];
    }
    return v;
}

// Länge des Pakets ab q aus seinem Kopf; Corrupt, wenn es nicht in avail Bytes passt.
NrawResult packetExtent(const uint8_t *q, size_t avail, size_t &used);

// Kette der Pakete first (0, 1, 3, 4, 5, 7; 2 = die Pakete 2 und 6 zusammen) durch alle
// Precincts einer Slice: Entropie, Dequantisierung, waagerechte Rücktransformation in f.low/f.high.
// row0 = Zeile des ersten Precincts.
NrawResult decodeChain(const Frame &f, const uint8_t *d, const Precinct *precs, size_t count, size_t row0, int first);

// Abschneidestufen (GTLI) aller Precincts und WGT-Einträge: out[p * WGT_COUNT + w], p < count.
void gtliTable(const Frame &f, const Precinct *precs, size_t count, uint8_t *out);

// Kennlinie als fertige Ausgabestufen: values[i] = Ausgabewert zur Tabellenstufe i, i <= maxIdx.
struct LevelTable {
    const uint16_t *values;
    int maxIdx;
};

// Senkrechte Rücktransformation, Star-Tetrix und Kennlinie, in Kacheln.
size_t tileCount(const Frame &f);
void decodeTile(const Frame &f, size_t index, const LevelTable &levels, uint16_t *bayer);

// GPU-Anbindung (nraw_gpu.cpp): Ebenen im gepinnten Speicher der CUDA-Bibliothek, Stufen nach der
// Entropie auf der Karte. Die Ebenen liegen ab f.low[K0] in der Reihenfolge von nraw_gpu.h.
enum class GpuRun { Done, Busy, Failed }; // Busy: Karte besetzt, die CPU macht das Bild

bool gpuUsable();
void *gpuAlloc(size_t bytes); // nullptr, wenn die GPU nicht läuft
void gpuFree(void *p);
GpuRun gpuBayer(const Frame &f, const LevelTable &levels, uint16_t *bayer);

// Stufe B: das ganze Bild aus dem Codestream auf der Karte (Precincts aus der Vorabsuche, scanMs
// deren Dauer). Busy auch, wenn die Prüfung der Karte anschlägt (beschädigter Strom): die CPU macht es.
GpuRun gpuDecode(const Frame &f, const uint8_t *cs, const Precinct *precs, const LevelTable &levels, uint16_t *bayer,
                 float scanMs);

} // namespace nraw
