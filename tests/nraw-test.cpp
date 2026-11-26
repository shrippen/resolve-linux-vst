// Prüft den N-RAW-Referenzdecoder bitgenau gegen Mitschnitte von fastticolib. Die Prüfdaten sind
// privat und nicht im Repo: $NRAW_FIXTURES (Standard ~/.cache/claude/tico-test/fixtures) mit
// lut.bin (int32 LE), iso800.in/.out, iso3200.in/.out, bright.in/.out (helles Motiv: Werte über
// int16 nach der waagerechten Hebung). Läuft nicht in `make test`.
// Je Threadzahl und Variante (CPU, GPU): 0 abweichende Pixel, Wandzeit je Bild (min/Median/max),
// CPU-Zeit (Median). Die GPU-Variante braucht build/libresolve-linux-vst-nraw-cuda.so neben dem
// Programm und eine Karte, sonst wird sie übersprungen. Zeitaufteilung der GPU-Variante:
// Entropie (CPU) = Wand - Upload - Kernel - Download - Kopie.
#include "../src/log.h"
#include "../src/nraw_decode.h"
#include "../src/nraw_pool.h"

#include <time.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
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
    double up = med(&NrawGpuTimes::upload);
    double kernel = med(&NrawGpuTimes::kernel);
    double down = med(&NrawGpuTimes::download);
    double copy = med(&NrawGpuTimes::copy);
    printf("         Aufteilung (Median): Entropie CPU %.1f  Upload %.1f  Kernel %.1f  Download %.1f  Kopie %.1f ms\n",
           wall - up - kernel - down - copy, up, kernel, down, copy);
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

} // namespace

int main()
{
    setLogFile(stderr);
    const char *env = getenv("NRAW_FIXTURES");
    std::string dir = env ? env : std::string(getenv("HOME")) + "/.cache/claude/tico-test/fixtures";

    std::vector<uint8_t> raw;
    if (!load(dir + "/lut.bin", raw)) {
        return 1;
    }
    std::vector<int32_t> lut(raw.size() / 4);
    for (size_t i = 0; i < lut.size(); i++) {
        lut[i] = int32_t(raw[4 * i] | (raw[4 * i + 1] << 8) | (raw[4 * i + 2] << 16) | (uint32_t(raw[4 * i + 3]) << 24));
    }

    size_t bad = 0;
    for (const char *name : {"iso800", "iso3200", "bright"}) {
        for (unsigned threads : THREAD_COUNTS) {
            for (NrawBackend backend : {NrawBackend::Cpu, NrawBackend::Gpu}) {
                size_t n = check(dir, name, lut, threads, backend);
                bad = n == SIZE_MAX || bad == SIZE_MAX ? SIZE_MAX : bad + n;
            }
        }
    }
    return bad == 0 ? 0 : 1;
}
