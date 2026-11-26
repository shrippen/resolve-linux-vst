// N-RAW-Hook, gemeinsame Teile, siehe nraw_watch.h.

#include "nraw_watch.h"

#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include "log.h"

namespace {

// Aufbau von ctx (fastticolib, Resolve 21.1.1): ctx+0x28 → Objekt, dort bei +0xd0 die Kennlinie.
constexpr size_t kCtxCurveObj = 0x28;
constexpr size_t kObjCurveInfo = 0xd0;
constexpr uint32_t kCurveCount = 81792;

constexpr uint64_t kNsPerSec = 1000000000ull;

constexpr uint64_t kFnvBasis = 0xcbf29ce484222325ull;
constexpr uint64_t kFnvPrime = 0x100000001b3ull;

// CPU-Zeit des ganzen Prozesses in ns (alle Threads, auch die von Resolve).
uint64_t processCpuNs()
{
    timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return uint64_t(ts.tv_sec) * kNsPerSec + uint64_t(ts.tv_nsec);
}

bool writeFile(const std::string &path, const void *data, size_t len)
{
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) {
        return false;
    }
    const bool ok = fwrite(data, 1, len, f) == len;
    return fclose(f) == 0 && ok;
}

// Abweichendes Bild für die Fehlersuche ablegen: Codestream, Bild von RED und unser Bild in
// stateDir()/nraw-mismatch.{in,red,own}.
void dumpMismatch(const uint8_t *cs, size_t size, const uint16_t *red, const uint16_t *own, size_t count)
{
    const std::string dir = stateDir();
    if (dir.empty()) {
        return;
    }
    const std::string base = dir + "/nraw-mismatch";
    const bool ok = writeFile(base + ".in", cs, size) && writeFile(base + ".red", red, count * sizeof(uint16_t)) &&
                    writeFile(base + ".own", own, count * sizeof(uint16_t));
    if (!ok) {
        logMessage("N-RAW: Prüfbild nicht ablegbar unter %s.*", base.c_str());
        return;
    }
    logMessage("N-RAW: Prüfbild abgelegt unter %s.{in,red,own}", base.c_str());
}

} // namespace

bool readMem(const void *from, void *to, size_t len)
{
    if (!from) {
        return false;
    }
    iovec local{to, len};
    iovec remote{const_cast<void *>(from), len};
    return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == ssize_t(len);
}

bool readCurve(const void *ctx, std::vector<int32_t> *values)
{
    struct Info {
        uint32_t count;
        uint32_t zero;
        const int32_t *values;
    } info;

    const uint8_t *base = static_cast<const uint8_t *>(ctx);
    const uint8_t *object = nullptr;
    if (!readMem(base + kCtxCurveObj, &object, sizeof object)) {
        return false;
    }
    if (!readMem(object + kObjCurveInfo, &info, sizeof info) || info.count != kCurveCount) {
        return false;
    }

    values->resize(info.count);
    return readMem(info.values, values->data(), info.count * sizeof(int32_t));
}

uint64_t fnv1a(const std::vector<int32_t> &values)
{
    uint64_t hash = kFnvBasis;
    for (int32_t v : values) {
        const uint8_t *p = reinterpret_cast<const uint8_t *>(&v);
        for (size_t i = 0; i < sizeof v; ++i) {
            hash = (hash ^ p[i]) * kFnvPrime;
        }
    }
    return hash;
}

Stopwatch::Stopwatch() : wall_(std::chrono::steady_clock::now()), cpu_(processCpuNs()) {}

uint64_t Stopwatch::wallNs() const
{
    return uint64_t(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - wall_).count());
}

uint64_t Stopwatch::cpuNs() const
{
    return processCpuNs() - cpu_;
}

void ArgWatch::observe(uint64_t n, void *a3, long a8, long a9)
{
    const long now[3] = {long(reinterpret_cast<intptr_t>(a3)), a8, a9};
    const char *names[3] = {"a3", "a8", "a9"};
    if (n == 1) {
        memcpy(first_, now, sizeof now);
        haveFirst_.store(true, std::memory_order_release);
        logMessage("N-RAW: erster Aufruf a3=%#lx a8=%ld a9=%ld", now[0], a8, a9);
        return;
    }
    if (!haveFirst_.load(std::memory_order_acquire)) {
        return;
    }

    bool differs = false;
    for (int i = 0; i < 3; ++i) {
        if (now[i] == first_[i]) {
            continue;
        }
        differs = true;
        if (!logged_[i].exchange(true)) {
            logMessage("N-RAW: %s weicht ab: erster Aufruf %#lx, jetzt %#lx", names[i], first_[i], now[i]);
        }
    }
    varying_ += differs;
}

void compareWithRed(uint64_t n, const uint8_t *cs, size_t size, const NrawCurve &curve, const uint16_t *red,
                    size_t count, std::atomic<bool> *switchOff)
{
    std::vector<uint16_t> own(count);
    const Stopwatch watch;
    NrawResult r = decodeNraw(cs, size, curve, own.data(), count);
    const double ms = double(watch.wallNs()) / 1e6;
    const double cpuMs = double(watch.cpuNs()) / 1e6;
    if (r != NrawResult::Ok) {
        logMessage("N-RAW: Vergleich %llu: eigener Decoder liefert Ergebnis %d (0 Ok, 1 Unsupported, 2 Corrupt), RED "
                   "dekodiert dieses Bild",
                   (unsigned long long)n, int(r));
        return;
    }

    if (memcmp(own.data(), red, count * sizeof(uint16_t)) == 0) {
        logMessage("N-RAW: Vergleich %llu: gleich RED (%zu Pixel, Wand %.1f ms, CPU %.1f ms)", (unsigned long long)n,
                   count, ms, cpuMs);
        return;
    }

    size_t different = 0;
    size_t first = count;
    size_t last = 0;
    for (size_t i = 0; i < count; ++i) {
        if (red[i] == own[i]) {
            continue;
        }
        ++different;
        first = std::min(first, i);
        last = i;
    }
    logMessage("N-RAW: Vergleich %llu: %zu von %zu Pixeln verschieden, erster Index %zu, letzter %zu (Wand %.1f ms, "
               "CPU %.1f ms)",
               (unsigned long long)n, different, count, first, last, ms, cpuMs);
    if (!switchOff) {
        return;
    }
    switchOff->store(true);
    logMessage("N-RAW: Prüfbild weicht von RED ab, eigener Decoder für diesen Lauf aus");
    dumpMismatch(cs, size, red, own.data(), count);
}
