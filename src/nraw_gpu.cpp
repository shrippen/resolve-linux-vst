// N-RAW, GPU-Anbindung der Hauptbibliothek (Schnittstelle: nraw_gpu.h). Lädt
// libresolve-linux-vst-nraw-cuda.so beim ersten Gebrauch aus dem Ordner, in dem diese Bibliothek
// (oder das Programm) liegt. Fehlt sie oder die Karte, dekodiert die CPU; jeder Fehler der GPU
// schaltet sie für den Prozess ab, im Kindprozess nach fork() ist sie von Anfang an aus.
#include <dlfcn.h>
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "log.h"
#include "nraw_frame.h"
#include "nraw_gpu.h"
#include "nraw_pool.h"

namespace nraw {

namespace {

constexpr const char *CUDA_LIB = "libresolve-linux-vst-nraw-cuda.so";
constexpr size_t COPY_CHUNK = 1 << 20; // Werte je Kopierauftrag
// Paket je Paketart (tief, hoch, eins), dessen Layout die Art beschreibt (PACKET_DEF in nraw_frame.h).
constexpr int LAYOUT_OF_KIND[NRAW_GPU_KINDS] = {0, 4, 2};
static_assert(PACKET_DEF[LAYOUT_OF_KIND[NRAW_GPU_LOW]].kind == Kind::Low &&
                  PACKET_DEF[LAYOUT_OF_KIND[NRAW_GPU_HIGH]].kind == Kind::High &&
                  PACKET_DEF[LAYOUT_OF_KIND[NRAW_GPU_ONE]].kind == Kind::One,
              "LAYOUT_OF_KIND passt zur Pakettabelle");

std::once_flag gLoadOnce;
const NrawGpuApi *gApi = nullptr;
std::atomic<bool> gDead{false};

void childAfterFork()
{
    gDead.store(true);
}

// Ordner dieser Bibliothek, bei einem Programm (Tests) der des Programms.
std::string ownDir()
{
    Dl_info info;
    if (!dladdr(reinterpret_cast<void *>(&childAfterFork), &info) || !info.dli_fname) {
        return {};
    }
    char real[PATH_MAX];
    if (!realpath(info.dli_fname, real)) {
        return {};
    }
    std::string path = real;
    return path.substr(0, path.rfind('/'));
}

void load()
{
    pthread_atfork(nullptr, nullptr, childAfterFork);

    std::string dir = ownDir();
    if (dir.empty()) {
        return;
    }
    void *lib = dlopen((dir + "/" + CUDA_LIB).c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        logMessage("N-RAW GPU: %s", dlerror());
        return;
    }
    auto entry = reinterpret_cast<const NrawGpuApi *(*)(int)>(dlsym(lib, NRAW_GPU_ENTRY));
    gApi = entry ? entry(NRAW_GPU_VERSION) : nullptr;
    if (!gApi) {
        logMessage("N-RAW GPU: %s hat eine andere Version", CUDA_LIB);
    }
}

// Schaltet die GPU ab und loggt den Grund (nur das erste Mal).
void kill()
{
    if (gDead.exchange(true)) {
        return;
    }
    const char *why = gApi->error();
    logMessage("N-RAW GPU aus: %s, die CPU dekodiert", why ? why : "Fehler");
}

struct CopyJob {
    const uint16_t *src;
    uint16_t *dst;
    size_t count;
};

void copyTask(size_t i, void *ctx)
{
    const CopyJob &job = *static_cast<CopyJob *>(ctx);
    size_t from = i * COPY_CHUNK;
    memcpy(job.dst + from, job.src + from, std::min(COPY_CHUNK, job.count - from) * sizeof(uint16_t));
}

// Die Pinned-Kopie in den Zielpuffer, auf allen Threads des Pools.
void copyOut(const uint16_t *src, uint16_t *dst, size_t count)
{
    CopyJob job{src, dst, count};
    parallelFor((count + COPY_CHUNK - 1) / COPY_CHUNK, copyTask, &job);
}

} // namespace

bool gpuUsable()
{
    std::call_once(gLoadOnce, load);
    return gApi && !gDead.load();
}

void *gpuAlloc(size_t bytes)
{
    if (!gpuUsable()) {
        return nullptr;
    }
    void *p = gApi->allocHost(bytes);
    if (!p) {
        kill();
    }
    return p;
}

void gpuFree(void *p)
{
    gApi->freeHost(p);
}

GpuRun gpuBayer(const Frame &f, const LevelTable &levels, uint16_t *bayer)
{
    if (!gpuUsable()) {
        return GpuRun::Failed;
    }

    NrawGpuFrame g{uint32_t(f.h.wf), uint32_t(f.h.hf), uint32_t(f.compw), uint32_t(f.nprec),
                   f.low[K0],        levels.values,    uint32_t(levels.maxIdx + 1)};
    switch (gApi->bayer(&g, bayer, copyOut)) {
    case NRAW_GPU_OK:
        return GpuRun::Done;
    case NRAW_GPU_BUSY:
        return GpuRun::Busy;
    default:
        kill();
        return GpuRun::Failed;
    }
}

GpuRun gpuDecode(const Frame &f, const uint8_t *cs, const Precinct *precs, const LevelTable &levels, uint16_t *bayer,
                 float scanMs)
{
    if (!gpuUsable() || f.h.end > UINT32_MAX) {
        return GpuRun::Failed;
    }
    for (size_t p = 0; p < f.nprec; p++) {
        if (!precs[p].plain) {
            return GpuRun::Busy;
        }
    }

    std::vector<NrawGpuPrec> table(f.nprec);
    for (size_t p = 0; p < f.nprec; p++) {
        for (int k = 0; k < PACKETS; k++) {
            table[p].pkt[k] = uint32_t(precs[p].pkt[k]);
        }
        table[p].end = uint32_t(precs[p].end);
    }
    std::vector<uint8_t> gtli(f.nprec * WGT_COUNT);
    gtliTable(f, precs, f.nprec, gtli.data());

    NrawGpuStream s{};
    s.wf = uint32_t(f.h.wf);
    s.hf = uint32_t(f.h.hf);
    s.compw = uint32_t(f.compw);
    s.nprec = uint32_t(f.nprec);
    s.cs = cs;
    s.size = f.h.end;
    s.precs = table.data();
    s.gtli = gtli.data();
    s.levels = levels.values;
    s.levelCount = uint32_t(levels.maxIdx + 1);
    s.scanMs = scanMs;
    for (int kind = 0; kind < NRAW_GPU_KINDS; kind++) {
        const Layout &lay = f.lay[LAYOUT_OF_KIND[kind]];
        s.layout.bands[kind] = uint32_t(lay.bands);
        for (int b = 0; b < lay.bands; b++) {
            s.layout.groups[kind][b] = uint32_t(lay.groups[b]);
            s.layout.width[kind][b] = uint32_t(lay.width[b]);
        }
    }

    switch (gApi->decode(&s, bayer, copyOut)) {
    case NRAW_GPU_OK:
        return GpuRun::Done;
    case NRAW_GPU_FAILED:
        kill();
        return GpuRun::Failed;
    default:
        return GpuRun::Busy;
    }
}

} // namespace nraw

size_t nrawGpuPeek(int what, void *dst, size_t bytes)
{
    return nraw::gpuUsable() ? nraw::gApi->peek(what, dst, bytes) : 0;
}

bool nrawGpuTimes(NrawGpuTimes *times)
{
    if (!nraw::gpuUsable()) {
        return false;
    }
    nraw::gApi->times(times);
    return true;
}
