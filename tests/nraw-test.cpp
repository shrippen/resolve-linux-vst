// Prüft den N-RAW-Referenzdecoder bitgenau gegen Mitschnitte von fastticolib. Die Prüfdaten sind
// privat und nicht im Repo: $NRAW_FIXTURES (Standard ~/.cache/claude/tico-test/fixtures) mit
// lut.bin (int32 LE), iso800.in/.out, iso3200.in/.out. Läuft nicht in `make test`.
// Je Threadzahl: 0 abweichende Pixel, Wand- und CPU-Zeit je Bild (Median).
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

// Gibt die Zahl abweichender Pixel zurück, oder SIZE_MAX bei einem Fehler.
size_t check(const std::string &dir, const char *name, const std::vector<int32_t> &lut, unsigned threads)
{
    std::vector<uint8_t> in;
    std::vector<uint8_t> ref;
    if (!load(dir + "/" + name + ".in", in) || !load(dir + "/" + name + ".out", ref)) {
        return SIZE_MAX;
    }

    poolSetThreads(threads);
    std::vector<uint16_t> out(OUT_WIDTH * OUT_HEIGHT, 0);
    std::vector<double> wall;
    std::vector<double> cpu;
    NrawResult r = NrawResult::Ok;
    for (int i = 0; i < RUNS && r == NrawResult::Ok; i++) {
        auto t0 = std::chrono::steady_clock::now();
        double c0 = cpuMs();
        r = decodeNraw(in.data(), in.size(), NrawCurve{lut.data(), lut.size()}, out.data(), out.size());
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
    printf("%-8s %2u Threads  abweichende Pixel %zu von %zu  Wand %6.1f ms  CPU %6.1f ms\n", name, threads, bad,
           out.size(), median(wall), median(cpu));
    return bad;
}

} // namespace

int main()
{
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
    for (const char *name : {"iso800", "iso3200"}) {
        for (unsigned threads : THREAD_COUNTS) {
            size_t n = check(dir, name, lut, threads);
            bad = n == SIZE_MAX || bad == SIZE_MAX ? SIZE_MAX : bad + n;
        }
    }
    return bad == 0 ? 0 : 1;
}
