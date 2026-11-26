// N-RAW-Messmodus, siehe nraw_probe.h.

#include "nraw_probe.h"

#include <cstdio>
#include <cstring>

#include "log.h"

namespace {

constexpr size_t kSnapshotBytes = 0x400;
constexpr size_t kWordBytes = 8;
constexpr size_t kMaxDiffs = 16;
constexpr size_t kMaxCodeShow = 8;
constexpr size_t kMaxDstBytes = size_t(1) << 28; // Obergrenze für den Referenzpuffer

constexpr uint64_t kFirstCalls = 3; // so viele Aufrufe mit voller Ausgabe
constexpr uint64_t kRefEvery = 500; // Referenzvergleich, dazu bei Aufruf 1 und 2

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
        logMessage("N-RAW: Kennlinie nicht lesbar oder falsche Anzahl");
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

// Der eigene Decoder auf demselben Codestream: gleiches Bild wie fastticolib?
void checkReference(uint64_t n, void *ctx, const uint8_t *cs, size_t size, size_t dstBytes, const uint16_t *dst)
{
    if (dstBytes == 0 || dstBytes % sizeof(uint16_t) != 0 || dstBytes > kMaxDstBytes) {
        return;
    }

    std::vector<int32_t> curve;
    if (!readCurve(ctx, &curve)) {
        logMessage("N-RAW: Referenz übersprungen, Kennlinie nicht lesbar");
        return;
    }
    compareWithRed(n, cs, size, NrawCurve{curve.data(), curve.size()}, dst, dstBytes / sizeof(uint16_t));
}

// Statistik über alle Aufrufe; Zähler atomar, Decoder laufen in mehreren Threads.
struct Stats {
    std::atomic<uint64_t> calls{0};
    std::atomic<int> running{0};
    std::atomic<int> maxRunning{0};
    std::atomic<uint64_t> totalNs{0};
    std::atomic<uint64_t> totalCpuNs{0};
    std::atomic<uint64_t> maxNs{0};
    ArgWatch args;
};

Stats gStats;

template <typename T>
void raiseMax(std::atomic<T> &maximum, T value)
{
    T seen = maximum.load();
    while (value > seen && !maximum.compare_exchange_weak(seen, value)) {
    }
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
               (unsigned long long)gStats.args.varying());
}

} // namespace

int nrawProbe(DecodeFn original, void *ctx, void *handle, void *a3, size_t size, const uint8_t *cs, size_t dstBytes,
              uint16_t *dst, long a8, long a9)
{
    const uint64_t n = ++gStats.calls;
    const bool verbose = n <= kFirstCalls;

    if (verbose) {
        logArgs(n, ctx, handle, a3, size, cs, dstBytes, dst, a8, a9);
    }
    gStats.args.observe(n, a3, a8, a9);

    Snapshot snap;
    snap.ctxOk = snap.handleOk = false;
    if (verbose) {
        takeSnapshot(&snap, ctx, handle);
    }

    raiseMax(gStats.maxRunning, ++gStats.running);
    const Stopwatch watch;
    int result = original(ctx, handle, a3, size, cs, dstBytes, dst, a8, a9);
    const uint64_t cpuNs = watch.cpuNs();
    const uint64_t ns = watch.wallNs();
    --gStats.running;

    gStats.totalCpuNs += cpuNs;
    gStats.totalNs += ns;
    raiseMax(gStats.maxNs, ns);

    if (verbose) {
        logSideEffects(snap, ctx, handle);
    }
    if (n % kReportEvery == 0) {
        logStats(n);
    }
    if (result == 0 && (n <= 2 || n % kRefEvery == 0)) {
        checkReference(n, ctx, cs, size, dstBytes, dst);
    }
    return result;
}
