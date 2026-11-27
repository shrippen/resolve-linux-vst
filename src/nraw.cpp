// N-RAW dekodieren: Eingriff in die Dekodierfunktion von fastticolib (REDR3D-x64.so).
//
// Resolve liest .NEV als R3D (nev.cpp); den Frame dekodiert fastticolib auf der CPU:
//
//   RED-Leser ──► decode(ctx, handle, …, Codestream, dst) ──► Bayer-Frame (uint16) im Host-Speicher
//                   ▲
//                   └─ hier: Sprung auf nrawDecode; das Original bleibt über ein Trampolin erreichbar
//
// Standard: der eigene Decoder (nraw_decode.h) dekodiert statt fastticolib,
// die ersten Bilder und jedes 500. dekodiert zusätzlich RED zum Vergleich; bei einer Abweichung
// oder jedem Fehler dekodiert RED. RESOLVE_LINUX_VST_NRAW_DECODE=0: nur RED. =probe: Messmodus
// (nraw_probe.h), ruft immer das Original und protokolliert, womit es gerufen wird, was es
// verändert, wie oft und wie parallel es läuft.
// Fremden Speicher liest der Ersatz nur über process_vm_readv: Ein ungültiger Zeiger kostet
// eine Messung, keinen Absturz.

#include "nraw.h"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "log.h"
#include "nraw_curves.h"
#include "nraw_decision.h"
#include "nraw_decode.h"
#include "nraw_probe.h"
#include "nraw_watch.h"
#include "patch.h"
#include "process.h"
#include "sites.h"

namespace {

constexpr const char *kEnvMode = "RESOLVE_LINUX_VST_NRAW_DECODE";
constexpr const char *kModeProbe = "probe";
constexpr const char *kModeOff = "0";
constexpr const char *kLibSuffix = "REDR3D-x64.so";

DecodeFn gOriginal = nullptr;

enum class Attach { Retry, Done };

enum class Mode { Off, Probe, Own };

Mode mode()
{
    static const Mode m = [] {
        const char *env = getenv(kEnvMode);
        if (!isResolveMain() || (env && strcmp(env, kModeOff) == 0)) {
            return Mode::Off;
        }
        return env && strcmp(env, kModeProbe) == 0 ? Mode::Probe : Mode::Own;
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
    std::atomic<bool> otherModeLogged{false};
    std::atomic<uint64_t> calls{0};
    std::atomic<uint64_t> fallbacks{0};
    std::atomic<uint64_t> wallNs{0};
    std::atomic<uint64_t> cpuNs{0};
    ArgWatch args;
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

// Prüfbild: RED dekodiert in dst, wir in einen eigenen Puffer. Weicht ein Pixel ab, ist der
// eigene Decoder für den Rest des Prozesses aus.
int ownVerify(void *ctx, void *handle, void *a3, size_t size, const uint8_t *cs, size_t dstBytes, uint16_t *dst,
              long a8, long a9, const NrawCurve &curve)
{
    int result = gOriginal(ctx, handle, a3, size, cs, dstBytes, dst, a8, a9);
    if (result != 0) {
        return result;
    }

    const uint64_t n = gOwn.calls.load();
    compareWithRed(n, cs, size, curve, dst, dstBytes / sizeof(uint16_t), &gOwn.disabled);
    return result;
}

void ownStats(uint64_t n)
{
    logMessage("N-RAW: %llu Bilder, davon %llu an RED, eigener Decoder Wand %.1f ms, CPU %.1f ms je Bild",
               (unsigned long long)n, (unsigned long long)gOwn.fallbacks.load(),
               double(gOwn.wallNs.load()) / double(n) / 1e6, double(gOwn.cpuNs.load()) / double(n) / 1e6);
}

// Eigener Decoder statt fastticolib; bei jedem Zweifel dekodiert RED.
int ownDecode(void *ctx, void *handle, void *a3, size_t size, const uint8_t *cs, size_t dstBytes, uint16_t *dst,
              long a8, long a9)
{
    const uint64_t n = ++gOwn.calls;
    gOwn.args.observe(n, a3, a8, a9);
    if ((a8 != 0 || a9 != 0) && !gOwn.otherModeLogged.exchange(true)) {
        logMessage("N-RAW: a8=%ld a9=%ld (nicht 0), solche Bilder dekodiert RED", a8, a9);
    }
    NrawCurve curve;
    if (nrawToRed(gOwn.disabled, a8, a9, dstBytes) || !ownCurve(ctx, &curve)) {
        ++gOwn.fallbacks;
        return gOriginal(ctx, handle, a3, size, cs, dstBytes, dst, a8, a9);
    }
    if (nrawVerifyDue(n)) {
        return ownVerify(ctx, handle, a3, size, cs, dstBytes, dst, a8, a9, curve);
    }

    const Stopwatch watch;
    NrawResult r = decodeNraw(cs, size, curve, dst, dstBytes / sizeof(uint16_t));
    gOwn.cpuNs += watch.cpuNs();
    gOwn.wallNs += watch.wallNs();
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
    return nrawProbe(gOriginal, ctx, handle, a3, size, cs, dstBytes, dst, a8, a9);
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
