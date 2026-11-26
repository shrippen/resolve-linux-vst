// N-RAW auf der GPU, Host-Teil (Kernel: nraw_cuda.cu). Eigene Bibliothek, die die Hauptbibliothek
// per dlopen lädt (nraw_gpu.h). Nur die CUDA-Driver-API über libcuda.so.1 (dlopen), kein cudart:
// Resolve lädt selbst eines.
//
//   Hauptbibliothek ─► nrawGpuApi ─► allocHost / bayer ─► libcuda (dlsym) ─► PTX (eingebettet)
//
// Eigener Kontext, nie der primäre: die Threads gehören Resolve. Jeder Aufruf schiebt ihn auf
// den Kontextstapel des Threads und nimmt ihn am Ende wieder herunter. Kontext und Puffer
// entstehen beim ersten Aufruf und bleiben. Jeder CUDA-Fehler macht die GPU-Variante für den
// Prozess tot; die Hauptbibliothek dekodiert dann auf der CPU.
#include <cuda.h>
#include <dlfcn.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include "nraw_gpu.h"

// PTX als nullterminierte Zeichenkette in der .so (Datei von nvcc -ptx).
asm(".section .rodata\n"
    ".global nrawPtx\n"
    "nrawPtx:\n"
    ".incbin \"" NRAW_PTX_PATH "\"\n"
    ".byte 0\n"
    ".previous\n");
extern "C" const char nrawPtx[];

namespace {

constexpr const char *LIBCUDA = "libcuda.so.1";
constexpr unsigned BLOCK_X = 32;
constexpr unsigned BLOCK_Y = 8;
constexpr size_t ERROR_BYTES = 192;
constexpr int PLANE_SETS = 8;   // Ebenen im Upload (nraw_gpu.h)
constexpr int MIRROR_ROWS = 2;  // Ebenenzeilen je Precinct

// Treiberfunktionen (versionierte Namen, die cuda.h als Makros einsetzt).
struct Driver {
    CUresult (*init)(unsigned);
    CUresult (*deviceCount)(int *);
    CUresult (*deviceGet)(CUdevice *, int);
    CUresult (*ctxCreate)(CUcontext *, unsigned, CUdevice);
    CUresult (*ctxPush)(CUcontext);
    CUresult (*ctxPop)(CUcontext *);
    CUresult (*memAlloc)(CUdeviceptr *, size_t);
    CUresult (*memFree)(CUdeviceptr);
    CUresult (*hostAlloc)(void **, size_t);
    CUresult (*hostFree)(void *);
    CUresult (*toDevice)(CUdeviceptr, const void *, size_t, CUstream);
    CUresult (*toHost)(void *, CUdeviceptr, size_t, CUstream);
    CUresult (*modLoad)(CUmodule *, const void *);
    CUresult (*modFunc)(CUfunction *, CUmodule, const char *);
    CUresult (*launch)(CUfunction, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, CUstream,
                       void **, void **);
    CUresult (*streamNew)(CUstream *, unsigned);
    CUresult (*streamWait)(CUstream);
    CUresult (*eventNew)(CUevent *, unsigned);
    CUresult (*eventRecord)(CUevent, CUstream);
    CUresult (*eventMs)(float *, CUevent, CUevent);
    CUresult (*errName)(CUresult, const char **);
};

enum Kernel { K_LIFT, K_STEP1, K_STEP2, K_STEP3, K_STEP4, K_LAST, KERNELS };
constexpr const char *KERNEL_NAME[KERNELS] = {"lift", "step1", "step2", "step3", "step4", "last"};
enum Stamp { T_START, T_UPLOADED, T_COMPUTED, T_DOWNLOADED, STAMPS };

struct State {
    Driver d{};
    CUcontext ctx = nullptr;
    CUstream stream = nullptr;
    CUfunction fn[KERNELS] = {};
    CUevent ev[STAMPS] = {};

    CUdeviceptr dPlanes = 0; // Upload, K2 darin wird verändert
    CUdeviceptr dP = 0;
    CUdeviceptr dR = 0;
    CUdeviceptr dS = 0;
    CUdeviceptr dBayer = 0;
    CUdeviceptr dLevels = 0;
    size_t planesBytes = 0;
    size_t planeBytes = 0;
    size_t bayerBytes = 0;
    std::vector<uint16_t> levels; // Stand auf der Karte
    void *hostBayer = nullptr;    // gepinnt

    NrawGpuTimes times{};
};

State gS;
std::mutex gInit;               // Kontext anlegen
std::mutex gRun;                // ein Bild zur Zeit (Puffer sind einmal da)
std::atomic<bool> gReady{false};
std::atomic<bool> gDead{false};
char gError[ERROR_BYTES];
std::atomic<bool> gHasError{false};

// Setzt den ersten Fehler und macht die GPU tot. Gibt immer false zurück.
bool fail(const char *what, CUresult r)
{
    const char *name = "?";
    if (gS.d.errName) {
        gS.d.errName(r, &name);
    }
    if (!gDead.exchange(true)) {
        snprintf(gError, sizeof gError, "%s: %s", what, name ? name : "?");
        gHasError.store(true);
    }
    return false;
}

bool failText(const char *text)
{
    if (!gDead.exchange(true)) {
        snprintf(gError, sizeof gError, "%s", text);
        gHasError.store(true);
    }
    return false;
}

template <typename T>
bool sym(void *lib, const char *name, T &out)
{
    out = reinterpret_cast<T>(dlsym(lib, name));
    return out != nullptr || failText(name);
}

bool loadDriver(Driver &d)
{
    void *lib = dlopen(LIBCUDA, RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        return failText("libcuda.so.1 fehlt");
    }
    return sym(lib, "cuInit", d.init) && sym(lib, "cuDeviceGetCount", d.deviceCount) &&
           sym(lib, "cuDeviceGet", d.deviceGet) && sym(lib, "cuCtxCreate_v2", d.ctxCreate) &&
           sym(lib, "cuCtxPushCurrent_v2", d.ctxPush) && sym(lib, "cuCtxPopCurrent_v2", d.ctxPop) &&
           sym(lib, "cuMemAlloc_v2", d.memAlloc) && sym(lib, "cuMemFree_v2", d.memFree) &&
           sym(lib, "cuMemAllocHost_v2", d.hostAlloc) && sym(lib, "cuMemFreeHost", d.hostFree) &&
           sym(lib, "cuMemcpyHtoDAsync_v2", d.toDevice) && sym(lib, "cuMemcpyDtoHAsync_v2", d.toHost) &&
           sym(lib, "cuModuleLoadData", d.modLoad) && sym(lib, "cuModuleGetFunction", d.modFunc) &&
           sym(lib, "cuLaunchKernel", d.launch) && sym(lib, "cuStreamCreate", d.streamNew) &&
           sym(lib, "cuStreamSynchronize", d.streamWait) && sym(lib, "cuEventCreate", d.eventNew) &&
           sym(lib, "cuEventRecord", d.eventRecord) && sym(lib, "cuEventElapsedTime", d.eventMs) &&
           sym(lib, "cuGetErrorName", d.errName);
}

// Kontext, Modul, Stream und Ereignisse; der Kontext ist danach nicht mehr aktuell.
bool createContext(State &s)
{
    CUresult r = s.d.init(0);
    if (r != CUDA_SUCCESS) {
        return fail("cuInit", r);
    }
    int count = 0;
    r = s.d.deviceCount(&count);
    if (r != CUDA_SUCCESS || count < 1) {
        return failText("keine Karte");
    }
    CUdevice dev;
    if ((r = s.d.deviceGet(&dev, 0)) != CUDA_SUCCESS) {
        return fail("cuDeviceGet", r);
    }
    // Blockierendes Warten: der Host belegt beim Synchronisieren keinen Kern.
    if ((r = s.d.ctxCreate(&s.ctx, CU_CTX_SCHED_BLOCKING_SYNC, dev)) != CUDA_SUCCESS) {
        return fail("cuCtxCreate", r);
    }

    // cuCtxCreate macht den Kontext im Thread aktuell: wieder herunternehmen.
    CUcontext popped;
    s.d.ctxPop(&popped);

    s.d.ctxPush(s.ctx);
    bool ok = true;
    CUmodule mod;
    if ((r = s.d.modLoad(&mod, nrawPtx)) != CUDA_SUCCESS) {
        ok = fail("cuModuleLoadData", r);
    }
    for (int k = 0; ok && k < KERNELS; k++) {
        if ((r = s.d.modFunc(&s.fn[k], mod, KERNEL_NAME[k])) != CUDA_SUCCESS) {
            ok = fail("cuModuleGetFunction", r);
        }
    }
    if (ok && (r = s.d.streamNew(&s.stream, CU_STREAM_NON_BLOCKING)) != CUDA_SUCCESS) {
        ok = fail("cuStreamCreate", r);
    }
    for (int e = 0; ok && e < STAMPS; e++) {
        if ((r = s.d.eventNew(&s.ev[e], CU_EVENT_DEFAULT)) != CUDA_SUCCESS) {
            ok = fail("cuEventCreate", r);
        }
    }
    s.d.ctxPop(&popped);
    return ok;
}

// Legt beim ersten Aufruf alles an. Danach: false, wenn die GPU tot ist.
bool ready()
{
    if (gDead.load()) {
        return false;
    }
    if (gReady.load()) {
        return true;
    }

    std::lock_guard<std::mutex> guard(gInit);
    if (gDead.load()) {
        return false;
    }
    if (gReady.load()) {
        return true;
    }
    if (!loadDriver(gS.d) || !createContext(gS)) {
        return false;
    }
    gReady.store(true);
    return true;
}

// Kontext auf dem Stapel des Aufrufers, solange das Objekt lebt.
struct Current {
    Current() { gS.d.ctxPush(gS.ctx); }
    ~Current()
    {
        CUcontext popped;
        gS.d.ctxPop(&popped);
    }
};

bool devAlloc(CUdeviceptr &p, size_t bytes, const char *what)
{
    if (p) {
        gS.d.memFree(p);
        p = 0;
    }
    CUresult r = gS.d.memAlloc(&p, bytes);
    return r == CUDA_SUCCESS || fail(what, r);
}

// Puffer für die Bildgröße; bleiben stehen, solange die Größe gleich bleibt.
bool ensureBuffers(State &s, const NrawGpuFrame &f)
{
    size_t plane = size_t(f.nprec) * f.compw * sizeof(int32_t);
    size_t planes = PLANE_SETS * plane;
    size_t bayer = size_t(f.wf) * f.hf * sizeof(uint16_t);
    if (planes == s.planesBytes && bayer == s.bayerBytes && plane == s.planeBytes) {
        return true;
    }

    s.planesBytes = 0;
    s.planeBytes = 0;
    s.bayerBytes = 0;
    if (!devAlloc(s.dPlanes, planes, "cuMemAlloc") || !devAlloc(s.dP, 2 * plane, "cuMemAlloc") ||
        !devAlloc(s.dR, 2 * plane, "cuMemAlloc") || !devAlloc(s.dS, 2 * plane, "cuMemAlloc") ||
        !devAlloc(s.dBayer, bayer, "cuMemAlloc")) {
        return false;
    }
    if (s.hostBayer) {
        s.d.hostFree(s.hostBayer);
        s.hostBayer = nullptr;
    }
    CUresult r = s.d.hostAlloc(&s.hostBayer, bayer);
    if (r != CUDA_SUCCESS) {
        return fail("cuMemAllocHost", r);
    }
    s.planesBytes = planes;
    s.planeBytes = plane;
    s.bayerBytes = bayer;
    return true;
}

// Kennlinie hochladen, wenn sie sich geändert hat (Vergleich mit dem Stand auf der Karte).
bool ensureLevels(State &s, const NrawGpuFrame &f)
{
    size_t bytes = size_t(f.levelCount) * sizeof(uint16_t);
    if (s.dLevels && s.levels.size() == f.levelCount && memcmp(s.levels.data(), f.levels, bytes) == 0) {
        return true;
    }
    if (!devAlloc(s.dLevels, bytes, "cuMemAlloc")) {
        return false;
    }
    s.levels.assign(f.levels, f.levels + f.levelCount);
    CUresult r = s.d.toDevice(s.dLevels, s.levels.data(), bytes, s.stream);
    return r == CUDA_SUCCESS || fail("cuMemcpyHtoDAsync", r);
}

struct Grid {
    unsigned x, y;
};

Grid gridOf(int n, int rows)
{
    return Grid{(unsigned(n) + BLOCK_X - 1) / BLOCK_X, (unsigned(rows) + BLOCK_Y - 1) / BLOCK_Y};
}

bool launch(State &s, Kernel k, Grid g, void **args)
{
    CUresult r = s.d.launch(s.fn[k], g.x, g.y, 1, BLOCK_X, BLOCK_Y, 1, 0, s.stream, args, nullptr);
    return r == CUDA_SUCCESS || fail("cuLaunchKernel", r);
}

// Senkrechte Hebung der drei Komponenten, Star-Tetrix, Kennlinie.
bool runKernels(State &s, const NrawGpuFrame &f)
{
    int n = int(f.compw);
    int nprec = int(f.nprec);
    int m = MIRROR_ROWS * nprec;
    int rows = int(f.hf / 2);
    int maxIdx = int(f.levelCount) - 1;
    CUdeviceptr plane = s.dPlanes;
    CUdeviceptr step = s.planeBytes;
    CUdeviceptr k0Lo = plane, k0Hi = plane + step, k1Lo = plane + 2 * step, k1Hi = plane + 3 * step;
    CUdeviceptr k3Lo = plane + 4 * step, k3Hi = plane + 5 * step, q = plane + 6 * step;

    Grid gl = gridOf(n, nprec);
    Grid gf = gridOf(n, m);
    void *aP[] = {&k0Lo, &k0Hi, &s.dP, &nprec, &n};
    void *aR[] = {&k1Lo, &k1Hi, &s.dR, &nprec, &n};
    void *aS[] = {&k3Lo, &k3Hi, &s.dS, &nprec, &n};
    void *a1[] = {&s.dP, &q, &m, &n};
    void *a2[] = {&q, &s.dP, &m, &n};
    void *a3[] = {&s.dP, &s.dR, &s.dS, &m, &n};
    void *a4[] = {&q, &s.dR, &s.dS, &m, &n};
    void *aL[] = {&s.dR, &s.dP, &q, &s.dS, &s.dLevels, &maxIdx, &s.dBayer, &m, &n, &rows};
    return launch(s, K_LIFT, gl, aP) && launch(s, K_LIFT, gl, aR) && launch(s, K_LIFT, gl, aS) &&
           launch(s, K_STEP1, gf, a1) && launch(s, K_STEP2, gf, a2) && launch(s, K_STEP3, gf, a3) &&
           launch(s, K_STEP4, gf, a4) && launch(s, K_LAST, gf, aL);
}

bool stamp(State &s, Stamp t)
{
    CUresult r = s.d.eventRecord(s.ev[t], s.stream);
    return r == CUDA_SUCCESS || fail("cuEventRecord", r);
}

float between(State &s, Stamp a, Stamp b)
{
    float ms = 0;
    s.d.eventMs(&ms, s.ev[a], s.ev[b]);
    return ms;
}

// Upload, Kernel, Download in einem Stream, eine Synchronisation am Ende.
bool pipeline(State &s, const NrawGpuFrame &f)
{
    CUresult r;
    if (!stamp(s, T_START)) {
        return false;
    }
    r = s.d.toDevice(s.dPlanes, f.planes, s.planesBytes, s.stream);
    if (r != CUDA_SUCCESS) {
        return fail("cuMemcpyHtoDAsync", r);
    }
    if (!stamp(s, T_UPLOADED) || !runKernels(s, f) || !stamp(s, T_COMPUTED)) {
        return false;
    }
    r = s.d.toHost(s.hostBayer, s.dBayer, s.bayerBytes, s.stream);
    if (r != CUDA_SUCCESS) {
        return fail("cuMemcpyDtoHAsync", r);
    }
    if (!stamp(s, T_DOWNLOADED)) {
        return false;
    }
    r = s.d.streamWait(s.stream);
    if (r != CUDA_SUCCESS) {
        return fail("cuStreamSynchronize", r);
    }

    s.times.upload = between(s, T_START, T_UPLOADED);
    s.times.kernel = between(s, T_UPLOADED, T_COMPUTED);
    s.times.download = between(s, T_COMPUTED, T_DOWNLOADED);
    return true;
}

void *allocHost(size_t bytes)
{
    if (!ready()) {
        return nullptr;
    }
    Current cur;
    void *p = nullptr;
    CUresult r = gS.d.hostAlloc(&p, bytes);
    if (r != CUDA_SUCCESS) {
        fail("cuMemAllocHost", r);
        return nullptr;
    }
    return p;
}

void freeHost(void *p)
{
    if (!p || !gReady.load() || gDead.load()) {
        return; // bei totem Kontext lieber liegen lassen
    }
    Current cur;
    gS.d.hostFree(p);
}

int bayer(const NrawGpuFrame *f, uint16_t *dst, NrawGpuCopy copy)
{
    if (!ready()) {
        return NRAW_GPU_FAILED;
    }
    std::unique_lock<std::mutex> run(gRun, std::try_to_lock);
    if (!run.owns_lock()) {
        return NRAW_GPU_BUSY;
    }

    Current cur;
    State &s = gS;
    if (!ensureBuffers(s, *f) || !ensureLevels(s, *f) || !pipeline(s, *f)) {
        return NRAW_GPU_FAILED;
    }

    auto t0 = std::chrono::steady_clock::now();
    copy(static_cast<const uint16_t *>(s.hostBayer), dst, size_t(f->wf) * f->hf);
    s.times.copy = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return NRAW_GPU_OK;
}

const char *error()
{
    return gHasError.load() ? gError : nullptr;
}

void times(NrawGpuTimes *out)
{
    *out = gS.times;
}

const NrawGpuApi API = {NRAW_GPU_VERSION, allocHost, freeHost, bayer, error, times};

} // namespace

extern "C" __attribute__((visibility("default"))) const NrawGpuApi *nrawGpuApi(int version)
{
    return version == NRAW_GPU_VERSION ? &API : nullptr;
}
