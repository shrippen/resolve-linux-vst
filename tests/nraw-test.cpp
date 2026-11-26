// Prüft den N-RAW-Referenzdecoder bitgenau gegen Mitschnitte von fastticolib. Die Prüfdaten sind
// privat und nicht im Repo: $NRAW_FIXTURES (Standard ~/.local/share/resolve-linux-vst-fixtures) mit
// lut.bin (int32 LE), iso800.in/.out, iso3200.in/.out, bright.in/.out (helles Motiv: Werte über
// int16 nach der waagerechten Hebung), clip.in/.out (ausgebrannte Lichter, Ausgabe von RED). Läuft nicht in `make test`.
// Je Threadzahl und Variante (CPU, GPU): 0 abweichende Pixel, Wandzeit je Bild (min/Median/max),
// CPU-Zeit (Median). Die GPU-Variante braucht build/libresolve-linux-vst-nraw-cuda.so neben dem
// Programm und eine Karte, sonst wird sie übersprungen. Zeitaufteilung der GPU-Variante: Vorabsuche
// (CPU), Upload, Kernel je Stufe, Download, Kopie; der Rest der Wand ist Wartezeit und Aufruf.
//
// Dazu je Bild ein Vergleich der Zwischenstufen der GPU (Zählcodes, GCLI je Gruppe) mit einer
// einfachen Referenz im Test (refDump), und ein Test mit beschädigten Strömen: kein Absturz, das
// Ergebnis der GPU-Variante gleicht der CPU-Variante, die Prüfung der Karte schlägt an.
#include "../src/log.h"
#include "../src/nraw_decode.h"
#include "../src/nraw_gpu.h"
#include "../src/nraw_pool.h"

#include <time.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr size_t OUT_WIDTH = 4140;
constexpr size_t OUT_HEIGHT = 2330;
constexpr unsigned THREAD_COUNTS[] = {1, 8, 12, 16, 24};
constexpr int RUNS = 10;

bool load(const std::string &path, std::vector<uint8_t> &data)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) {
        printf("FEHLER  kann %s nicht lesen\n", path.c_str());
        return false;
    }
    uint8_t buf[1 << 16];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
        data.insert(data.end(), buf, buf + n);
    }
    fclose(f);
    return true;
}

double cpuMs()
{
    timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return double(ts.tv_sec) * 1e3 + double(ts.tv_nsec) / 1e6;
}

double median(std::vector<double> v)
{
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

double minOf(const std::vector<double> &v)
{
    return *std::min_element(v.begin(), v.end());
}

double maxOf(const std::vector<double> &v)
{
    return *std::max_element(v.begin(), v.end());
}

void printSplit(const std::vector<NrawGpuTimes> &t, double wall)
{
    auto med = [&](float NrawGpuTimes::*field) {
        std::vector<double> v;
        for (const NrawGpuTimes &x : t) {
            v.push_back(x.*field);
        }
        return median(v);
    };
    double scan = med(&NrawGpuTimes::scan);
    double up = med(&NrawGpuTimes::upload);
    double codes = med(&NrawGpuTimes::codes);
    double gcli = med(&NrawGpuTimes::gcli);
    double coefs = med(&NrawGpuTimes::coefs);
    double kernel = med(&NrawGpuTimes::kernel);
    double down = med(&NrawGpuTimes::download);
    double copy = med(&NrawGpuTimes::copy);
    double rest = wall - scan - up - codes - gcli - coefs - kernel - down - copy;
    printf("         Aufteilung (Median): Vorabsuche %.2f  Upload %.2f  codes %.2f  gcli %.2f  coefs %.2f  "
           "Farbstufen %.2f  Download %.2f  Kopie %.2f  Rest %.2f ms\n",
           scan, up, codes, gcli, coefs, kernel, down, copy, rest);
}

// Die Prüfung der Karte darf bei unbeschädigten Bildern nie anschlagen.
bool flagsClear(const char *name, const std::vector<NrawGpuTimes> &gpu)
{
    for (const NrawGpuTimes &t : gpu) {
        if (t.flags != 0) {
            printf("FEHLER  %s: Prüfung der Karte schlug an (0x%x), das Bild lief nicht auf der GPU\n", name, t.flags);
            return false;
        }
    }
    return true;
}

// Gibt die Zahl abweichender Pixel zurück, oder SIZE_MAX bei einem Fehler. Gpu ohne laufende GPU:
// 0 Pixel geprüft, Meldung (kein Fehler).
size_t check(const std::string &dir, const char *name, const std::vector<int32_t> &lut, unsigned threads,
             NrawBackend backend)
{
    const char *label = backend == NrawBackend::Gpu ? "GPU" : "CPU";
    std::vector<uint8_t> in;
    std::vector<uint8_t> ref;
    if (!load(dir + "/" + name + ".in", in) || !load(dir + "/" + name + ".out", ref)) {
        return SIZE_MAX;
    }

    poolSetThreads(threads);
    std::vector<uint16_t> out(OUT_WIDTH * OUT_HEIGHT, 0);
    std::vector<double> wall;
    std::vector<double> cpu;
    std::vector<NrawGpuTimes> gpu;
    NrawResult r = NrawResult::Ok;
    for (int i = 0; i < RUNS + 1 && r == NrawResult::Ok; i++) {
        auto t0 = std::chrono::steady_clock::now();
        double c0 = cpuMs();
        r = decodeNraw(in.data(), in.size(), NrawCurve{lut.data(), lut.size()}, out.data(), out.size(), backend);
        NrawGpuTimes t{};
        if (backend == NrawBackend::Gpu && !nrawGpuTimes(&t)) {
            printf("%-8s %2u Threads  GPU: uebersprungen (keine Bibliothek oder Karte)\n", name, threads);
            return 0;
        }
        if (i == 0) {
            continue; // Aufwärmen: Speicher, Kontext, Kennlinie
        }
        gpu.push_back(t);
        cpu.push_back(cpuMs() - c0);
        wall.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }
    if (r != NrawResult::Ok || ref.size() != out.size() * 2) {
        printf("FEHLER  %s: Ergebnis %d\n", name, int(r));
        return SIZE_MAX;
    }

    if (!flagsClear(name, gpu)) {
        return SIZE_MAX;
    }

    size_t bad = 0;
    for (size_t i = 0; i < out.size(); i++) {
        uint16_t want = ref[2 * i] | (ref[2 * i + 1] << 8);
        bad += out[i] != want;
    }
    printf("%-8s %2u Threads %s  abweichende Pixel %zu von %zu  Wand min/Median/max %6.1f/%6.1f/%6.1f ms  CPU %6.1f ms\n",
           name, threads, label, bad, out.size(), minOf(wall), median(wall), maxOf(wall), median(cpu));
    if (backend == NrawBackend::Gpu) {
        printSplit(gpu, median(wall));
    }
    return bad;
}


// ---- Referenz der Zwischenstufen ---------------------------------------------------------------
// Liest den Strom Bit für Bit und seriell (bewusst einfach, unabhängig von src/): Zählcodes und GCLI
// je Gruppe und Paket in der Ordnung der GPU (je Paket NRAW_GPU_GROUP_STRIDE Bytes).

constexpr size_t PACKETS = 8;
constexpr size_t SLICE_PRECS = 16;
constexpr size_t PREC_HDR = 12;
constexpr size_t PKT_FIXED = 16;
constexpr size_t SIG_RUN = 8;
constexpr size_t BANDS = 6;
constexpr size_t COMP_WIDTH = OUT_WIDTH / 2;
constexpr size_t LEVELS = 5;
constexpr size_t MAX_TRUNC = 15;
constexpr int KIND_OF[PACKETS] = {0, 0, 2, 0, 1, 1, 2, 1};
constexpr int WGT_OF[PACKETS] = {0, 6, 12, 13, 19, 21, 12, 23};
constexpr size_t K2A = 2;
constexpr size_t K2B = 6;

struct PacketPos {
    size_t at, ld, lc, ls;
    size_t prec; // Anfang des Precincts
};

struct Ref {
    std::vector<uint8_t> codes;
    std::vector<uint8_t> gcli;
    std::vector<PacketPos> packets; // je Precinct 8
    size_t nprec = 0;
};

uint64_t be(const uint8_t *p, int n)
{
    uint64_t v = 0;
    for (int i = 0; i < n; i++) {
        v = (v << 8) | p[i];
    }
    return v;
}

// Gruppen je Band der drei Paketarten (tief, hoch, eins) bei der Breite COMP_WIDTH.
std::vector<size_t> groupsOfKind(int kind)
{
    if (kind == 1) {
        return {(COMP_WIDTH / 2 + 3) / 4, (COMP_WIDTH / 2 + 3) / 4};
    }
    if (kind == 2) {
        return {(COMP_WIDTH + 3) / 4};
    }
    size_t dims[LEVELS + 1] = {COMP_WIDTH};
    for (size_t l = 0; l < LEVELS; l++) {
        dims[l + 1] = (dims[l] + 1) / 2;
    }
    std::vector<size_t> widths = {dims[LEVELS]};
    for (size_t l = LEVELS; l-- > 0;) {
        widths.push_back(dims[l] / 2);
    }
    std::vector<size_t> groups;
    for (size_t w : widths) {
        groups.push_back((w + 3) / 4);
    }
    return groups;
}

class BitReader {
public:
    BitReader(const uint8_t *p) : p_(p) {}
    int bit() { int v = (p_[pos_ >> 3] >> (7 - (pos_ & 7))) & 1; pos_++; return v; }
    int ones()
    {
        int x = 0;
        while (bit()) {
            x++;
        }
        return x;
    }

private:
    const uint8_t *p_;
    size_t pos_ = 0;
};

int truncOf(int gain, int prio, int q, int r)
{
    int up = prio < r ? 1 : 0;
    return q < gain + up ? 0 : std::min<int>(q - gain - up, MAX_TRUNC);
}

struct RefState {
    std::vector<uint8_t> g[PACKETS];
    int t[PACKETS][BANDS] = {};
    bool valid[PACKETS] = {};
};

// Ein Paket: Codes und GCLI nach ref, Vorhersage aus st (Quelle ksrc), danach st[k] setzen.
void refPacket(const uint8_t *q, int k, int ksrc, const int *gt, RefState &st, uint8_t *codes, uint8_t *gcli)
{
    std::vector<size_t> groups = groupsOfKind(KIND_OF[k]);
    BitReader sig(q + 7);
    BitReader cnt(q + PKT_FIXED);
    std::vector<uint8_t> now;

    for (size_t b = 0; b < groups.size(); b++) {
        const bool top = st.valid[ksrc];
        const size_t first = now.size();
        int t = gt[b];
        int tt = top ? std::max(st.t[ksrc][b], t) : t;
        for (size_t s0 = 0; s0 < groups[b]; s0 += SIG_RUN) {
            bool quiet = sig.bit() == 1;
            for (size_t j = s0; j < std::min(s0 + SIG_RUN, groups[b]); j++) {
                int mt = std::max(top ? int(st.g[ksrc][first + j]) : 0, tt);
                int g = mt;
                uint8_t code = 255;
                if (!quiet) {
                    int x = cnt.ones();
                    int th = std::max(mt - t, 0);
                    int dm = x > 2 * th ? x - th : ((x & 1) ? -((x + 1) / 2) : x / 2);
                    g = std::clamp(mt + dm, 0, 19);
                    code = uint8_t(std::min(x, 254));
                }
                codes[first + j] = code;
                gcli[first + j] = uint8_t(g);
                now.push_back(uint8_t(g));
            }
        }
    }
    st.g[k] = now;
    for (size_t b = 0; b < groups.size(); b++) {
        st.t[k][b] = gt[b];
    }
    st.valid[k] = true;
}

bool refDump(const std::vector<uint8_t> &cs, Ref &ref)
{
    const uint8_t *d = cs.data();
    size_t pos = 2;
    uint8_t gain[25] = {}, prio[25] = {};
    while (be(d + pos, 2) != 0xff20) {
        size_t len = be(d + pos + 2, 2);
        if (be(d + pos, 2) == 0xff14) {
            for (int i = 0; i < 25; i++) {
                gain[i] = d[pos + 4 + 2 * i];
                prio[i] = d[pos + 5 + 2 * i];
            }
        }
        pos += 2 + len;
    }

    ref.nprec = (OUT_HEIGHT / 2 + 1) / 2;
    ref.codes.assign(ref.nprec * PACKETS * NRAW_GPU_GROUP_STRIDE, 0);
    ref.gcli.assign(ref.codes.size(), 0);
    for (size_t sl = 0; sl * SLICE_PRECS < ref.nprec; sl++) {
        pos += 6;
        RefState st;
        for (size_t i = 0; i < SLICE_PRECS && sl * SLICE_PRECS + i < ref.nprec; i++) {
            size_t p = sl * SLICE_PRECS + i;
            size_t start = pos;
            int q = d[pos + 3], r = d[pos + 4];
            size_t at = pos + PREC_HDR;
            for (size_t k = 0; k < PACKETS; k++) {
                size_t ld = (be(d + at, 7) >> 35) & 0xfffff, lc = (be(d + at, 7) >> 15) & 0xfffff, ls = be(d + at, 7) & 0x7fff;
                ref.packets.push_back(PacketPos{at, ld, lc, ls, start});
                int gt[BANDS];
                for (size_t b = 0; b < BANDS; b++) {
                    gt[b] = truncOf(gain[WGT_OF[k] + b], prio[WGT_OF[k] + b], q, r);
                }
                size_t ksrc = k == K2A ? K2B : (k == K2B ? K2A : k);
                size_t off = (p * PACKETS + k) * NRAW_GPU_GROUP_STRIDE;
                refPacket(d + at, int(k), int(ksrc), gt, st, ref.codes.data() + off, ref.gcli.data() + off);
                at += PKT_FIXED + ld + lc + ls;
            }
            pos += PREC_HDR + be(d + pos, 3);
        }
    }
    return true;
}

// Vergleicht die Zwischenstufen der GPU (letztes Bild) mit der Referenz; Meldung bei der ersten Abweichung.
bool checkStages(const char *name, const std::vector<uint8_t> &in)
{
    Ref ref;
    refDump(in, ref);
    for (int what : {NRAW_GPU_DUMP_CODES, NRAW_GPU_DUMP_GCLI}) {
        const std::vector<uint8_t> &want = what == NRAW_GPU_DUMP_CODES ? ref.codes : ref.gcli;
        std::vector<uint8_t> got(want.size());
        if (nrawGpuPeek(what, got.data(), got.size()) != got.size()) {
            printf("FEHLER  %s: Zwischenstufe %d nicht lesbar\n", name, what);
            return false;
        }
        for (size_t pk = 0; pk < ref.nprec * PACKETS; pk++) {
            size_t groups = 0;
            for (size_t g : groupsOfKind(KIND_OF[pk % PACKETS])) {
                groups += g;
            }
            size_t off = pk * NRAW_GPU_GROUP_STRIDE;
            if (memcmp(&got[off], &want[off], groups) != 0) {
                size_t g = 0;
                while (got[off + g] == want[off + g]) {
                    g++;
                }
                printf("FEHLER  %s: %s weicht ab, Precinct %zu Paket %zu Gruppe %zu (GPU %d, Referenz %d)\n", name,
                       what == NRAW_GPU_DUMP_CODES ? "x" : "gcli", pk / PACKETS, pk % PACKETS, g, got[off + g],
                       want[off + g]);
                return false;
            }
        }
    }
    printf("%-8s Zwischenstufen codes und gcli stimmen mit der Referenz überein (%zu Pakete)\n", name,
           ref.nprec * PACKETS);
    return true;
}

// ---- Beschädigte Ströme ------------------------------------------------------------------------

struct Decoded {
    NrawResult result;
    std::vector<uint16_t> pixels;
    unsigned flags;
};

Decoded run(const std::vector<uint8_t> &in, const std::vector<int32_t> &lut, NrawBackend backend)
{
    Decoded d{NrawResult::Ok, std::vector<uint16_t>(OUT_WIDTH * OUT_HEIGHT, 0), 0};
    d.result = decodeNraw(in.data(), in.size(), NrawCurve{lut.data(), lut.size()}, d.pixels.data(), d.pixels.size(), backend);
    NrawGpuTimes t{};
    if (backend == NrawBackend::Gpu && nrawGpuTimes(&t)) {
        d.flags = t.flags;
    }
    return d;
}

enum class Part { Count, Data, Sign };

// Zufallsbytes (oder 0xff) in einen Teil von count Paketen.
void damage(std::vector<uint8_t> &in, const Ref &ref, Part part, size_t count, bool ones, unsigned seed)
{
    srand(seed);
    for (size_t n = 0; n < count; n++) {
        const PacketPos &pp = ref.packets[size_t(rand()) % ref.packets.size()];
        size_t start = pp.at + PKT_FIXED + (part == Part::Count ? 0 : pp.lc + (part == Part::Sign ? pp.ld : 0));
        size_t len = part == Part::Count ? pp.lc : (part == Part::Data ? pp.ld : pp.ls);
        for (size_t i = 0; i < len; i++) {
            in[start + i] = ones ? 0xff : uint8_t(rand());
        }
    }
}

struct Case {
    const char *what;
    bool expectFlag;
};

// Ergebnis der GPU-Variante muss dem der CPU-Variante gleichen; expectFlag: die Prüfung schlägt an.
bool compareCase(const Case &c, const std::vector<uint8_t> &in, const std::vector<int32_t> &lut)
{
    Decoded cpu = run(in, lut, NrawBackend::Cpu);
    Decoded gpu = run(in, lut, NrawBackend::Gpu);
    bool same = cpu.result == gpu.result && (cpu.result != NrawResult::Ok || cpu.pixels == gpu.pixels);
    bool flagOk = !c.expectFlag || gpu.flags != 0;
    printf("%s  beschädigt: %-34s CPU %d GPU %d Prüfwort 0x%02x%s\n", same && flagOk ? "ok   " : "FEHLER", c.what,
           int(cpu.result), int(gpu.result), gpu.flags, same ? "" : "  Ergebnis weicht ab");
    return same && flagOk;
}

bool corruptTests(const std::string &dir, const std::vector<int32_t> &lut)
{
    std::vector<uint8_t> good;
    if (!load(dir + "/iso800.in", good)) {
        return false;
    }
    poolSetThreads(12);
    Ref ref;
    refDump(good, ref);
    Decoded base = run(good, lut, NrawBackend::Gpu);
    if (base.result != NrawResult::Ok || base.flags != 0) {
        printf("FEHLER  unbeschädigter Strom: Ergebnis %d Prüfwort 0x%x\n", int(base.result), base.flags);
        return false;
    }

    bool ok = true;
    std::vector<uint8_t> in = good;
    in.resize(good.size() / 2);
    ok &= compareCase({"abgeschnitten", false}, in, lut);

    in = good;
    in[ref.packets[100 * PACKETS].prec + 2] ^= 0x7f; // Lprc des Precincts 100
    ok &= compareCase({"Lprc verfälscht", false}, in, lut);

    in = good;
    damage(in, ref, Part::Count, 1, true, 1);
    ok &= compareCase({"Zählteil eines Pakets nur Einsen", true}, in, lut);

    for (unsigned seed = 1; seed <= 3; seed++) {
        in = good;
        damage(in, ref, Part::Count, 20, false, seed);
        ok &= compareCase({"Zufallsbytes im Zählteil (20 Pakete)", true}, in, lut);
    }
    for (Part part : {Part::Data, Part::Sign}) {
        in = good;
        damage(in, ref, part, 20, false, 7);
        ok &= compareCase({part == Part::Data ? "Zufallsbytes im Datenteil" : "Zufallsbytes im Vorzeichenteil", false}, in, lut);
    }

    // Nach den Fehlern muss das Bild wieder auf der GPU laufen.
    Decoded again = run(good, lut, NrawBackend::Gpu);
    bool back = again.result == NrawResult::Ok && again.flags == 0 && again.pixels == base.pixels;
    printf("%s  unbeschädigter Strom danach wieder auf der GPU\n", back ? "ok   " : "FEHLER");
    return ok && back;
}

} // namespace

int main()
{
    setLogFile(stderr);
    const char *env = getenv("NRAW_FIXTURES");
    std::string dir = env ? env : std::string(getenv("HOME")) + "/.local/share/resolve-linux-vst-fixtures";

    std::vector<uint8_t> raw;
    if (!load(dir + "/lut.bin", raw)) {
        return 1;
    }
    std::vector<int32_t> lut(raw.size() / 4);
    for (size_t i = 0; i < lut.size(); i++) {
        lut[i] = int32_t(raw[4 * i] | (raw[4 * i + 1] << 8) | (raw[4 * i + 2] << 16) | (uint32_t(raw[4 * i + 3]) << 24));
    }

    size_t bad = 0;
    for (const char *name : {"iso800", "iso3200", "bright", "clip"}) {
        for (unsigned threads : THREAD_COUNTS) {
            for (NrawBackend backend : {NrawBackend::Cpu, NrawBackend::Gpu}) {
                size_t n = check(dir, name, lut, threads, backend);
                bad = n == SIZE_MAX || bad == SIZE_MAX ? SIZE_MAX : bad + n;
            }
        }

        // Zwischenstufen des letzten GPU-Bildes (nur, wenn die GPU lief).
        std::vector<uint8_t> in;
        NrawGpuTimes t{};
        if (nrawGpuTimes(&t) && load(dir + "/" + name + ".in", in) && !checkStages(name, in)) {
            bad = SIZE_MAX;
        }
    }

    NrawGpuTimes t{};
    if (nrawGpuTimes(&t) && !corruptTests(dir, lut)) {
        bad = SIZE_MAX;
    }
    return bad == 0 ? 0 : 1;
}
