// N-RAW dekodieren: Eingriff in die Dekodierfunktion von fastticolib (REDR3D-x64.so).
//
// Resolve liest .NEV als R3D (nev.cpp); den Frame dekodiert fastticolib auf der CPU:
//
//   RED-Leser ──► decode(ctx, handle, …, Codestream, dst) ──► Bayer-Frame (uint16) im Host-Speicher
//                   ▲
//                   └─ hier: Sprung auf nrawDecode; das Original bleibt über ein Trampolin erreichbar
//
// RESOLVE_LINUX_VST_NRAW_DECODE=1: der eigene Decoder (nraw_decode.h) dekodiert statt fastticolib,
// die ersten Bilder und jedes 500. dekodiert zusätzlich RED zum Vergleich; bei einer Abweichung
// oder jedem Fehler dekodiert RED. =probe: Messmodus, ruft immer das Original und protokolliert,
// womit es gerufen wird, was es verändert, wie oft und wie parallel es läuft.
// Fremden Speicher liest der Ersatz nur über process_vm_readv: Ein ungültiger Zeiger kostet
// eine Messung, keinen Absturz.

#include "nraw.h"

#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "log.h"
#include "nraw_curves.h"
#include "nraw_decision.h"
#include "nraw_decode.h"
#include "patch.h"
#include "process.h"
#include "sites.h"

namespace {

constexpr const char *kEnvMode = "RESOLVE_LINUX_VST_NRAW_DECODE";
constexpr const char *kModeProbe = "probe";
constexpr const char *kModeOwn = "1";
constexpr const char *kLibSuffix = "REDR3D-x64.so";

// Aufbau von ctx (fastticolib, Resolve 21.1.1): ctx+0x28 → Objekt, dort bei +0xd0 die Kennlinie.
constexpr size_t kCtxCurveObj = 0x28;
constexpr size_t kObjCurveInfo = 0xd0;
constexpr uint32_t kCurveCount = 81792;

constexpr size_t kSnapshotBytes = 0x400;
constexpr size_t kWordBytes = 8;
constexpr size_t kMaxDiffs = 16;
constexpr size_t kMaxCodeShow = 8;
constexpr size_t kMaxDstBytes = size_t(1) << 28; // Obergrenze für den Referenzpuffer

constexpr uint64_t kFirstCalls = 3;    // so viele Aufrufe mit voller Ausgabe
constexpr uint64_t kReportEvery = 200; // Statistikzeile
constexpr uint64_t kRefEvery = 500;    // Referenzvergleich, dazu bei Aufruf 1 und 2

constexpr uint64_t kNsPerSec = 1000000000ull;

constexpr uint64_t kFnvBasis = 0xcbf29ce484222325ull;
constexpr uint64_t kFnvPrime = 0x100000001b3ull;

using DecodeFn = int (*)(void *ctx, void *handle, void *a3, size_t size, const uint8_t *codestream, size_t dstBytes,
                         uint16_t *dst, long a8, long a9);

DecodeFn gOriginal = nullptr;

enum class Attach { Retry, Done };

// CPU-Zeit des ganzen Prozesses in ns (alle Threads, auch die von Resolve).
uint64_t processCpuNs()
{
    timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return uint64_t(ts.tv_sec) * kNsPerSec + uint64_t(ts.tv_nsec);
}

// Liest fremden Speicher; false bei ungültiger Adresse.
bool readMem(const void *from, void *to, size_t len)
{
    if (!from) {
        return false;
    }
    iovec local{to, len};
    iovec remote{const_cast<void *>(from), len};
    return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == ssize_t(len);
}

// Kennlinie aus ctx in `values` kopieren; false, wenn Zeiger oder Anzahl nicht stimmen.
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

// Argumente, Anfang des Codestreams und Kennlinie der ersten Aufrufe.
void logArgs(uint64_t n, void *ctx, void *handle, void *a3, size_t size, const uint8_t *cs, size_t dstBytes,
             uint16_t *dst, long a8, long a9)
{
    logMessage("N-RAW: Aufruf %llu: ctx=%p handle=%p a3=%p (%ld) size=%zu cs=%p dstBytes=%zu dst=%p a8=%#lx (%ld) "
               "a9=%#lx (%ld)",
               (unsigned long long)n, ctx, handle, a3, long(reinterpret_cast<intptr_t>(a3)), size, (const void *)cs,
               dstBytes, (void *)dst, a8, a8, a9, a9);

    uint8_t head[kMaxCodeShow];
    if (readMem(cs, head, sizeof head)) {
        logMessage("N-RAW: Codestream beginnt mit %02x %02x %02x %02x %02x %02x %02x %02x", head[0], head[1], head[2],
                   head[3], head[4], head[5], head[6], head[7]);
    }

    std::vector<int32_t> curve;
    if (!readCurve(ctx, &curve)) {
        logMessage("N-RAW: Kennlinie nicht lesbar oder count != %u", kCurveCount);
        return;
    }
    logMessage("N-RAW: Kennlinie count=%zu erste Werte %d %d %d %d FNV-1a=%016llx", curve.size(), curve[0], curve[1],
               curve[2], curve[3], (unsigned long long)fnv1a(curve));
}

// Zustand von ctx und handle vor dem Original, zum Vergleich danach.
struct Snapshot {
    uint8_t ctx[kSnapshotBytes];
    uint8_t handle[kSnapshotBytes];
    bool ctxOk;
    bool handleOk;
};

void takeSnapshot(Snapshot *s, const void *ctx, const void *handle)
{
    s->ctxOk = readMem(ctx, s->ctx, sizeof s->ctx);
    s->handleOk = readMem(handle, s->handle, sizeof s->handle);
}

void logDiffs(const char *what, const uint8_t *before, const void *obj)
{
    uint8_t after[kSnapshotBytes];
    if (!readMem(obj, after, sizeof after)) {
        return;
    }

    size_t shown = 0;
    size_t total = 0;
    for (size_t off = 0; off < kSnapshotBytes; off += kWordBytes) {
        if (memcmp(before + off, after + off, kWordBytes) == 0) {
            continue;
        }
        ++total;
        if (shown == kMaxDiffs) {
            continue;
        }
        uint64_t b, a;
        memcpy(&b, before + off, kWordBytes);
        memcpy(&a, after + off, kWordBytes);
        logMessage("N-RAW: %s+%#zx verändert: %#llx -> %#llx", what, off, (unsigned long long)b, (unsigned long long)a);
        ++shown;
    }
    logMessage("N-RAW: %s: %zu von %zu Wörtern verändert", what, total, kSnapshotBytes / kWordBytes);
}

void logSideEffects(const Snapshot &s, const void *ctx, const void *handle)
{
    if (s.ctxOk) {
        logDiffs("ctx", s.ctx, ctx);
    }
    if (s.handleOk) {
        logDiffs("handle", s.handle, handle);
    }
}

// Der Referenzdecoder auf demselben Codestream: gleiches Bild wie fastticolib?
void checkReference(void *ctx, const uint8_t *cs, size_t size, size_t dstBytes, const uint16_t *dst)
{
    if (dstBytes == 0 || dstBytes % sizeof(uint16_t) != 0 || dstBytes > kMaxDstBytes) {
        return;
    }

    std::vector<int32_t> curve;
    if (!readCurve(ctx, &curve)) {
        logMessage("N-RAW: Referenz übersprungen, Kennlinie nicht lesbar");
        return;
    }

    const size_t count = dstBytes / sizeof(uint16_t);
    std::vector<uint16_t> ref(count);
    auto t0 = std::chrono::steady_clock::now();
    const uint64_t cpu0 = processCpuNs();
    NrawResult r = decodeNraw(cs, size, NrawCurve{curve.data(), curve.size()}, ref.data(), count);
    const double cpuMs = double(processCpuNs() - cpu0) / 1e6;
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (r != NrawResult::Ok) {
        logMessage("N-RAW: Referenz liefert Ergebnis %d (0 Ok, 1 Unsupported, 2 Corrupt)", int(r));
        return;
    }

    size_t different = 0;
    for (size_t i = 0; i < count; ++i) {
        different += ref[i] != dst[i];
    }
    if (different == 0) {
        logMessage("N-RAW: Referenz gleich (%zu Pixel, Wand %.1f ms, CPU %.1f ms)", count, ms, cpuMs);
        return;
    }
    logMessage("N-RAW: Referenz: %zu von %zu Pixeln verschieden (Wand %.1f ms, CPU %.1f ms)", different, count, ms,
               cpuMs);
}

// Statistik über alle Aufrufe; Zähler atomar, Decoder laufen in mehreren Threads.
struct Stats {
    std::atomic<uint64_t> calls{0};
    std::atomic<int> running{0};
    std::atomic<int> maxRunning{0};
    std::atomic<uint64_t> totalNs{0};
    std::atomic<uint64_t> totalCpuNs{0};
    std::atomic<uint64_t> maxNs{0};
    std::atomic<uint64_t> varying{0};
    std::atomic<bool> haveFirst{false};
    std::atomic<bool> logged[3]{};
    long first[3] = {0, 0, 0}; // a3, a8, a9 des ersten Aufrufs
};

Stats gStats;

template <typename T>
void raiseMax(std::atomic<T> &maximum, T value)
{
    T seen = maximum.load();
    while (value > seen && !maximum.compare_exchange_weak(seen, value)) {
    }
}

// Ändern sich a3, a8 oder a9 gegenüber dem ersten Aufruf? Zählt und loggt den neuen Wert einmal.
void trackArgs(uint64_t n, void *a3, long a8, long a9)
{
    const long now[3] = {long(reinterpret_cast<intptr_t>(a3)), a8, a9};
    const char *names[3] = {"a3", "a8", "a9"};
    if (n == 1) {
        memcpy(gStats.first, now, sizeof now);
        gStats.haveFirst.store(true, std::memory_order_release);
        return;
    }
    if (!gStats.haveFirst.load(std::memory_order_acquire)) {
        return;
    }

    bool differs = false;
    for (int i = 0; i < 3; ++i) {
        if (now[i] == gStats.first[i]) {
            continue;
        }
        differs = true;
        if (!gStats.logged[i].exchange(true)) {
            logMessage("N-RAW: %s weicht ab: erster Aufruf %#lx, jetzt %#lx", names[i], gStats.first[i], now[i]);
        }
    }
    gStats.varying += differs;
}

void logStats(uint64_t n)
{
    double mean = double(gStats.totalNs.load()) / double(n) / 1e6;
    double max = double(gStats.maxNs.load()) / 1e6;
    // CPU-Zeit des Prozesses während des Aufrufs: enthält Resolves übrige Threads, nur bei
    // Einzelbildschritten (ein Aufruf, sonst ruht Resolve) die Kosten der Dekodierung selbst.
    double cpu = double(gStats.totalCpuNs.load()) / double(n) / 1e6;
    logMessage("N-RAW: %llu Aufrufe, höchstens %d gleichzeitig, Dauer mittel %.1f ms, max %.1f ms, CPU mittel "
               "%.1f ms, a3/a8/a9 abweichend in %llu Aufrufen",
               (unsigned long long)n, gStats.maxRunning.load(), mean, max, cpu,
               (unsigned long long)gStats.varying.load());
}

// Messmodus: ruft immer das Original und misst dabei.
int probeDecode(void *ctx, void *handle, void *a3, size_t size, const uint8_t *cs, size_t dstBytes, uint16_t *dst,
                long a8, long a9)
{
    const uint64_t n = ++gStats.calls;
    const bool verbose = n <= kFirstCalls;

    if (verbose) {
        logArgs(n, ctx, handle, a3, size, cs, dstBytes, dst, a8, a9);
    }
    trackArgs(n, a3, a8, a9);

    Snapshot snap;
    snap.ctxOk = snap.handleOk = false;
    if (verbose) {
        takeSnapshot(&snap, ctx, handle);
    }

    raiseMax(gStats.maxRunning, ++gStats.running);
    auto t0 = std::chrono::steady_clock::now();
    const uint64_t cpu0 = processCpuNs();
    int result = gOriginal(ctx, handle, a3, size, cs, dstBytes, dst, a8, a9);
    const uint64_t cpuNs = processCpuNs() - cpu0;
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
    --gStats.running;

    gStats.totalCpuNs += cpuNs;
    gStats.totalNs += uint64_t(ns);
    raiseMax(gStats.maxNs, uint64_t(ns));

    if (verbose) {
        logSideEffects(snap, ctx, handle);
    }
    if (n % kReportEvery == 0) {
        logStats(n);
    }
    if (result == 0 && (n <= 2 || n % kRefEvery == 0)) {
        checkReference(ctx, cs, size, dstBytes, dst);
    }
    return result;
}

enum class Mode { Off, Probe, Own };

Mode mode()
{
    static const Mode m = [] {
        const char *env = getenv(kEnvMode);
        if (!isResolveMain() || !env) {
            return Mode::Off;
        }
        if (strcmp(env, kModeProbe) == 0) {
            return Mode::Probe;
        }
        return strcmp(env, kModeOwn) == 0 ? Mode::Own : Mode::Off;
    }();
    return m;
}

// Eigener Decoder: Kennlinie je ctx einmal übernehmen (Aufrufe kommen seriell, die Sperre
// schützt trotzdem), Prüfbilder gegen RED, Statistik.
struct Own {
    std::mutex lock;
    const void *ctx = nullptr;
    NrawCurves curves;
    const NrawCurveValues *curve = nullptr;
    std::atomic<bool> disabled{false};
    std::atomic<uint64_t> calls{0};
    std::atomic<uint64_t> fallbacks{0};
    std::atomic<uint64_t> wallNs{0};
    std::atomic<uint64_t> cpuNs{0};
};

Own gOwn;

// Kennlinie für ctx bereitstellen; false: nicht lesbar, RED soll dekodieren.
bool ownCurve(const void *ctx, NrawCurve *curve)
{
    std::lock_guard<std::mutex> guard(gOwn.lock);
    if (gOwn.ctx != ctx) {
        // Erst lesen, dann ablegen: ein halb gelesener Puffer ersetzt nie die alte Kennlinie.
        NrawCurveValues values;
        if (!readCurve(ctx, &values)) {
            return false;
        }
        logMessage("N-RAW: Kennlinie übernommen, FNV-1a=%016llx", (unsigned long long)fnv1a(values));
        gOwn.curve = gOwn.curves.keep(std::move(values));
        gOwn.ctx = ctx;
    }
    *curve = NrawCurve{gOwn.curve->data(), gOwn.curve->size()};
    return true;
}

// Abweichendes Prüfbild für die Fehlersuche ablegen: Codestream, Bild von RED und unser Bild in
// stateDir()/nraw-mismatch.{in,red,own}, dazu Zahl und Lage der Pixel ins Log.
bool writeFile(const std::string &path, const void *data, size_t len)
{
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) {
        return false;
    }
    const bool ok = fwrite(data, 1, len, f) == len;
    return fclose(f) == 0 && ok;
}

void dumpMismatch(const uint8_t *cs, size_t size, const uint16_t *red, const uint16_t *own, size_t count)
{
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
    logMessage("N-RAW: %zu von %zu Pixeln verschieden, erster Index %zu, letzter %zu", different, count, first, last);

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

// Prüfbild: RED dekodiert in dst, wir in einen eigenen Puffer. Weicht ein Pixel ab, ist der
// eigene Decoder für den Rest des Prozesses aus.
int ownVerify(void *ctx, void *handle, void *a3, size_t size, const uint8_t *cs, size_t dstBytes, uint16_t *dst,
              long a8, long a9, const NrawCurve &curve)
{
    int result = gOriginal(ctx, handle, a3, size, cs, dstBytes, dst, a8, a9);
    if (result != 0) {
        return result;
    }

    const size_t count = dstBytes / sizeof(uint16_t);
    std::vector<uint16_t> own(count);
    NrawResult r = decodeNraw(cs, size, curve, own.data(), count);
    if (r != NrawResult::Ok) {
        logMessage("N-RAW: Prüfbild: eigener Decoder liefert Ergebnis %d, RED dekodiert dieses Bild", int(r));
        return result;
    }
    if (memcmp(own.data(), dst, dstBytes) != 0) {
        gOwn.disabled = true;
        logMessage("N-RAW: Prüfbild weicht von RED ab, eigener Decoder für diesen Lauf aus");
        dumpMismatch(cs, size, dst, own.data(), count);
        return result;
    }
    logMessage("N-RAW: Prüfbild gleich RED (Aufruf %llu)", (unsigned long long)gOwn.calls.load());
    return result;
}

void ownStats(uint64_t n)
{
    logMessage("N-RAW: %llu Bilder, davon %llu an RED, eigener Decoder Wand %.1f ms, CPU %.1f ms je Bild",
               (unsigned long long)n, (unsigned long long)gOwn.fallbacks.load(),
               double(gOwn.wallNs.load()) / double(n) / 1e6, double(gOwn.cpuNs.load()) / double(n) / 1e6);
}

// a8 und a9 waren in jedem gemessenen Aufruf 0; andere Werte (ein anderer Dekodiermodus?)
// gehen an RED. Neue Werte von a3, a8, a9 kommen je einmal ins Log.
void noteArgs(void *a3, long a8, long a9)
{
    static std::atomic<void *> lastA3{nullptr};
    static std::atomic<bool> loggedA89{false};
    if (lastA3.exchange(a3) != a3) {
        logMessage("N-RAW: a3=%p", a3);
    }
    if ((a8 != 0 || a9 != 0) && !loggedA89.exchange(true)) {
        logMessage("N-RAW: a8=%ld a9=%ld (nicht 0), solche Bilder dekodiert RED", a8, a9);
    }
}

// Eigener Decoder statt fastticolib; bei jedem Zweifel dekodiert RED.
int ownDecode(void *ctx, void *handle, void *a3, size_t size, const uint8_t *cs, size_t dstBytes, uint16_t *dst,
              long a8, long a9)
{
    const uint64_t n = ++gOwn.calls;
    noteArgs(a3, a8, a9);
    NrawCurve curve;
    if (nrawToRed(gOwn.disabled, a8, a9, dstBytes) || !ownCurve(ctx, &curve)) {
        ++gOwn.fallbacks;
        return gOriginal(ctx, handle, a3, size, cs, dstBytes, dst, a8, a9);
    }
    if (nrawVerifyDue(n)) {
        return ownVerify(ctx, handle, a3, size, cs, dstBytes, dst, a8, a9, curve);
    }

    auto t0 = std::chrono::steady_clock::now();
    const uint64_t cpu0 = processCpuNs();
    NrawResult r = decodeNraw(cs, size, curve, dst, dstBytes / sizeof(uint16_t));
    gOwn.cpuNs += processCpuNs() - cpu0;
    gOwn.wallNs += uint64_t(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
    if (n % kReportEvery == 0) {
        ownStats(n);
    }
    if (r == NrawResult::Ok) {
        return 0;
    }

    // dst ist bei einem Fehlschlag unberührt (decodeNraw schreibt erst am Ende).
    ++gOwn.fallbacks;
    return gOriginal(ctx, handle, a3, size, cs, dstBytes, dst, a8, a9);
}

// Ersatz für die Dekodierfunktion: Messmodus oder eigener Decoder.
extern "C" int nrawDecode(void *ctx, void *handle, void *a3, size_t size, const uint8_t *cs, size_t dstBytes,
                          uint16_t *dst, long a8, long a9)
{
    if (mode() == Mode::Own) {
        return ownDecode(ctx, handle, a3, size, cs, dstBytes, dst, a8, a9);
    }
    return probeDecode(ctx, handle, a3, size, cs, dstBytes, dst, a8, a9);
}

// Muster suchen, Trampolin bauen, Sprung schreiben. Retry nur, wenn REDR3D noch nicht geladen ist.
Attach attachNow()
{
    CodeImage code;
    if (!loadedImage(kLibSuffix, &code)) {
        return Attach::Retry;
    }

    NrawSites sites;
    std::string error;
    if (!findNrawSites(code, &sites, &error)) {
        logMessage("N-RAW: Muster %s passt nicht, eigene Dekodierung aus", error.c_str());
        return Attach::Done;
    }

    gOriginal = reinterpret_cast<DecodeFn>(buildTrampoline(sites.decode, kNrawPrologLen));
    if (!gOriginal) {
        logMessage("N-RAW: Trampolin fehlgeschlagen, eigene Dekodierung aus");
        return Attach::Done;
    }
    if (!patchJumpIndirect(sites.decode, kNrawPrologLen, reinterpret_cast<void *>(&nrawDecode))) {
        logMessage("N-RAW: Sprung bei %#lx fehlgeschlagen, eigene Dekodierung aus", (unsigned long)sites.decode);
        return Attach::Done;
    }

    logMessage("N-RAW: Dekodierfunktion bei %#lx übernommen (%s)", (unsigned long)sites.decode,
               mode() == Mode::Own ? "eigener Decoder" : "Messmodus");
    return Attach::Done;
}

} // namespace

void nrawAttach()
{
    static std::atomic<bool> done{false};
    static std::mutex lock;

    if (done.load(std::memory_order_acquire) || mode() == Mode::Off) {
        return;
    }

    std::lock_guard<std::mutex> guard(lock);
    if (done.load(std::memory_order_relaxed)) {
        return;
    }
    if (attachNow() == Attach::Done) {
        done.store(true, std::memory_order_release);
    }
}
