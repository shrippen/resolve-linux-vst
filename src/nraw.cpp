// N-RAW auf der GPU: Eingriff in die Dekodierfunktion von fastticolib (REDR3D-x64.so).
//
// Resolve liest .NEV als R3D (nev.cpp); den Frame dekodiert fastticolib auf der CPU:
//
//   RED-Leser ──► decode(ctx, handle, …, Codestream, dst) ──► Bayer-Frame (uint16) im Host-Speicher
//                   ▲
//                   └─ hier: Sprung auf nrawDecode; das Original bleibt über ein Trampolin erreichbar
//
// Bisher nur der Messmodus (RESOLVE_LINUX_VST_NRAW_GPU=probe): nrawDecode ruft immer das
// Original und protokolliert, womit es gerufen wird, welche Daten es verändert, wie oft und wie
// parallel es läuft und ob der Referenzdecoder (nraw_decode.h) dasselbe Bild liefert.
// Fremden Speicher liest der Ersatz nur über process_vm_readv: Ein ungültiger Zeiger kostet
// eine Messung, keinen Absturz.

#include "nraw.h"

#include <sys/uio.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "log.h"
#include "nraw_decode.h"
#include "patch.h"
#include "process.h"
#include "sites.h"

namespace {

constexpr const char *kEnvMode = "RESOLVE_LINUX_VST_NRAW_GPU";
constexpr const char *kModeProbe = "probe";
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

constexpr uint64_t kFnvBasis = 0xcbf29ce484222325ull;
constexpr uint64_t kFnvPrime = 0x100000001b3ull;

using DecodeFn = int (*)(void *ctx, void *handle, void *a3, size_t size, const uint8_t *codestream, size_t dstBytes,
                         uint16_t *dst, long a8, long a9);

DecodeFn gOriginal = nullptr;

enum class Attach { Retry, Done };

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
    NrawResult r = decodeNraw(cs, size, NrawCurve{curve.data(), curve.size()}, ref.data(), count);
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
        logMessage("N-RAW: Referenz gleich (%zu Pixel, %.1f ms)", count, ms);
        return;
    }
    logMessage("N-RAW: Referenz: %zu von %zu Pixeln verschieden", different, count);
}

// Statistik über alle Aufrufe; Zähler atomar, Decoder laufen in mehreren Threads.
struct Stats {
    std::atomic<uint64_t> calls{0};
    std::atomic<int> running{0};
    std::atomic<int> maxRunning{0};
    std::atomic<uint64_t> totalNs{0};
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
    logMessage("N-RAW: %llu Aufrufe, höchstens %d gleichzeitig, Dauer mittel %.1f ms, max %.1f ms, a3/a8/a9 "
               "abweichend in %llu Aufrufen",
               (unsigned long long)n, gStats.maxRunning.load(), mean, max,
               (unsigned long long)gStats.varying.load());
}

// Ersatz: ruft immer das Original und misst dabei.
extern "C" int nrawDecode(void *ctx, void *handle, void *a3, size_t size, const uint8_t *cs, size_t dstBytes,
                          uint16_t *dst, long a8, long a9)
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
    int result = gOriginal(ctx, handle, a3, size, cs, dstBytes, dst, a8, a9);
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
    --gStats.running;

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

bool probeWanted()
{
    static const bool wanted = [] {
        const char *env = getenv(kEnvMode);
        return isResolveMain() && env && strcmp(env, kModeProbe) == 0;
    }();
    return wanted;
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
        logMessage("N-RAW: Muster %s passt nicht, GPU-Dekodierung aus", error.c_str());
        return Attach::Done;
    }

    gOriginal = reinterpret_cast<DecodeFn>(buildTrampoline(sites.decode, kNrawPrologLen));
    if (!gOriginal) {
        logMessage("N-RAW: Trampolin fehlgeschlagen, GPU-Dekodierung aus");
        return Attach::Done;
    }
    if (!patchJumpIndirect(sites.decode, kNrawPrologLen, reinterpret_cast<void *>(&nrawDecode))) {
        logMessage("N-RAW: Sprung bei %#lx fehlgeschlagen, GPU-Dekodierung aus", (unsigned long)sites.decode);
        return Attach::Done;
    }

    logMessage("N-RAW: Dekodierfunktion bei %#lx übernommen (Messmodus)", (unsigned long)sites.decode);
    return Attach::Done;
}

} // namespace

void nrawAttach()
{
    static std::atomic<bool> done{false};
    static std::mutex lock;

    if (done.load(std::memory_order_acquire) || !probeWanted()) {
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
