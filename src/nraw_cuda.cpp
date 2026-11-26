// N-RAW auf der GPU, Host-Teil (Kernel: nraw_cuda.cu, nraw_cuda_entropy.cuh). Eigene Bibliothek, die die Hauptbibliothek
// per dlopen lädt (nraw_gpu.h). Nur die CUDA-Driver-API über libcuda.so.1 (dlopen), kein cudart:
// Resolve lädt selbst eines.
//
//   Hauptbibliothek ─► nrawGpuApi ─► allocHost / bayer / decode ─► libcuda (dlsym) ─► PTX (eingebettet)
//
// Stufe A (bayer): Ebenen hoch, Farbstufen, Bild runter. Stufe B (decode): Codestream und kleine
// Tabellen hoch, Entropie (codes, gcli, coefs) füllt die Ebenen auf der Karte, dann dieselben
// Farbstufen, Bild und Prüfwort runter. Ein Stream, eine Synchronisation.
//
// Eigener Kontext, nie der primäre: die Threads gehören Resolve. Jeder Aufruf schiebt ihn auf
// den Kontextstapel des Threads und nimmt ihn am Ende wieder herunter. Kontext und Puffer
// entstehen beim ersten Aufruf und bleiben. Jeder CUDA-Fehler macht die GPU-Variante für den
// Prozess tot; die Hauptbibliothek dekodiert dann auf der CPU.
#include <cuda.h>
#include <dlfcn.h>

#include <algorithm>
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
constexpr unsigned GCLI_BLOCK = 128;
constexpr unsigned CODES_BLOCK = 256; // wie BLOCK in nraw_cuda_entropy.cuh
constexpr unsigned SLICE_PRECS = 16;
constexpr unsigned CHAINS = 7;
constexpr unsigned SHARED_MAX = 48 * 1024; // ohne Opt-in je Block
constexpr size_t GROUP_COEFS = 4;
constexpr size_t STREAM_SLACK = 4;      // Puffer wächst um 1/4 über die Größe

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
    CUresult (*memset32)(CUdeviceptr, unsigned, size_t, CUstream);
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

enum Kernel { K_LIFT, K_STEP1, K_STEP2, K_STEP3, K_STEP4, K_LAST, K_CODES, K_GCLI, K_COEFS, KERNELS };
constexpr const char *KERNEL_NAME[KERNELS] = {"lift", "step1", "step2", "step3", "step4",
                                              "last", "codes", "gcli", "coefs"};
enum Stamp { T_START, T_UPLOADED, T_CODES, T_GCLI, T_COEFS, T_COMPUTED, T_DOWNLOADED, STAMPS };

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

    // Stufe B
    CUdeviceptr dStream = 0;      // Codestream
    CUdeviceptr dTables = 0;      // NrawGpuPrec[nprec], dann gtli[nprec * WGT]
    CUdeviceptr dCodes = 0;       // Zählcodes je Gruppe und Paket
    CUdeviceptr dGcli = 0;        // GCLI je Gruppe und Paket
    CUdeviceptr dFlag = 0;        // Prüfwort
    size_t streamCap = 0;
    size_t tablesBytes = 0;
    size_t codesBytes = 0;
    void *hostStream = nullptr;   // gepinnt, Quelle des Uploads
    void *hostTables = nullptr;
    int *hostFlag = nullptr;

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

// Gerät, Kontext und Speicher.
bool loadMemory(void *lib, Driver &d)
{
    return sym(lib, "cuInit", d.init) && sym(lib, "cuDeviceGetCount", d.deviceCount) &&
           sym(lib, "cuDeviceGet", d.deviceGet) && sym(lib, "cuCtxCreate_v2", d.ctxCreate) &&
           sym(lib, "cuCtxPushCurrent_v2", d.ctxPush) && sym(lib, "cuCtxPopCurrent_v2", d.ctxPop) &&
           sym(lib, "cuMemAlloc_v2", d.memAlloc) && sym(lib, "cuMemFree_v2", d.memFree) &&
           sym(lib, "cuMemAllocHost_v2", d.hostAlloc) && sym(lib, "cuMemFreeHost", d.hostFree) &&
           sym(lib, "cuMemcpyHtoDAsync_v2", d.toDevice) && sym(lib, "cuMemcpyDtoHAsync_v2", d.toHost) &&
           sym(lib, "cuMemsetD32Async", d.memset32);
}

// Modul, Kernel, Stream, Ereignisse.
bool loadRun(void *lib, Driver &d)
{
    return sym(lib, "cuModuleLoadData", d.modLoad) && sym(lib, "cuModuleGetFunction", d.modFunc) &&
           sym(lib, "cuLaunchKernel", d.launch) && sym(lib, "cuStreamCreate", d.streamNew) &&
           sym(lib, "cuStreamSynchronize", d.streamWait) && sym(lib, "cuEventCreate", d.eventNew) &&
           sym(lib, "cuEventRecord", d.eventRecord) && sym(lib, "cuEventElapsedTime", d.eventMs) &&
           sym(lib, "cuGetErrorName", d.errName);
}

bool loadDriver(Driver &d)
{
    void *lib = dlopen(LIBCUDA, RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        return failText("libcuda.so.1 fehlt");
    }
    return loadMemory(lib, d) && loadRun(lib, d);
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

// Kernel mit eindimensionalem Block (Entropie), shared = dynamischer Shared Memory in Bytes.
bool launch1d(State &s, Kernel k, unsigned gx, unsigned gy, unsigned gz, unsigned block, unsigned shared, void **args)
{
    CUresult r = s.d.launch(s.fn[k], gx, gy, gz, block, 1, 1, shared, s.stream, args, nullptr);
    return r == CUDA_SUCCESS || fail("cuLaunchKernel", r);
}

// Senkrechte Hebung der drei Komponenten, Star-Tetrix, Kennlinie; die Ebenen liegen auf der Karte.
bool runColor(State &s, const NrawGpuFrame &f)
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

// Bild (und bei Stufe B das Prüfwort) herunter, warten.
bool download(State &s, size_t flagBytes)
{
    CUresult r = s.d.toHost(s.hostBayer, s.dBayer, s.bayerBytes, s.stream);
    if (r == CUDA_SUCCESS && flagBytes) {
        r = s.d.toHost(s.hostFlag, s.dFlag, flagBytes, s.stream);
    }
    if (r != CUDA_SUCCESS) {
        return fail("cuMemcpyDtoHAsync", r);
    }
    if (!stamp(s, T_DOWNLOADED)) {
        return false;
    }
    r = s.d.streamWait(s.stream);
    return r == CUDA_SUCCESS || fail("cuStreamSynchronize", r);
}

// Stufe A: Upload, Farbstufen, Download in einem Stream, eine Synchronisation am Ende.
bool pipeline(State &s, const NrawGpuFrame &f)
{
    if (!stamp(s, T_START)) {
        return false;
    }
    CUresult r = s.d.toDevice(s.dPlanes, f.planes, s.planesBytes, s.stream);
    if (r != CUDA_SUCCESS) {
        return fail("cuMemcpyHtoDAsync", r);
    }
    if (!stamp(s, T_UPLOADED) || !runColor(s, f) || !stamp(s, T_COMPUTED) || !download(s, 0)) {
        return false;
    }

    s.times = NrawGpuTimes{};
    s.times.upload = between(s, T_START, T_UPLOADED);
    s.times.kernel = between(s, T_UPLOADED, T_COMPUTED);
    s.times.download = between(s, T_COMPUTED, T_DOWNLOADED);
    return true;
}

// ---- Stufe B -----------------------------------------------------------------------------------

struct Counts {
    unsigned maxGroups;  // Gruppen des größten Pakets
    unsigned valsInts;   // Koeffizienten des größten Pakets
};

Counts countsOf(const NrawGpuLayout &lay)
{
    Counts c{0, 0};
    for (unsigned k = 0; k < NRAW_GPU_KINDS; k++) {
        unsigned n = 0;
        for (unsigned b = 0; b < lay.bands[k]; b++) {
            n += lay.groups[k][b];
        }
        c.maxGroups = std::max(c.maxGroups, n);
    }
    c.valsInts = c.maxGroups * unsigned(GROUP_COEFS);
    return c;
}

// Dynamischer Shared Memory von coefs: Koeffizienten plus zwei Puffer der Hebung.
size_t coefShared(const NrawGpuStream &f)
{
    return (countsOf(f.layout).valsInts + 2 * size_t(f.compw)) * sizeof(int32_t);
}

// Passt das Bild in die festen Grenzen der Kernel? Sonst macht die CPU es.
bool fits(const NrawGpuStream &f)
{
    bool bandsOk = true;
    for (unsigned k = 0; k < NRAW_GPU_KINDS; k++) {
        bandsOk = bandsOk && f.layout.bands[k] >= 1 && f.layout.bands[k] <= NRAW_GPU_BANDS;
    }
    return bandsOk && countsOf(f.layout).maxGroups <= NRAW_GPU_GROUP_STRIDE && coefShared(f) <= SHARED_MAX &&
           f.nprec > 0 && f.size > 0;
}

bool hostAlloc(void *&p, size_t bytes, const char *what)
{
    if (p) {
        gS.d.hostFree(p);
        p = nullptr;
    }
    CUresult r = gS.d.hostAlloc(&p, bytes);
    return r == CUDA_SUCCESS || fail(what, r);
}

// Puffer der Stufe B: Strom wächst nur, Tabellen und Zwischenwerte folgen der Precinct-Zahl.
bool ensureEntropy(State &s, const NrawGpuStream &f)
{
    if (!s.dFlag && (!devAlloc(s.dFlag, sizeof(int), "cuMemAlloc") || !hostAlloc(reinterpret_cast<void *&>(s.hostFlag),
                                                                                 sizeof(int), "cuMemAllocHost"))) {
        return false;
    }

    if (f.size > s.streamCap) {
        size_t cap = f.size + f.size / STREAM_SLACK;
        if (!devAlloc(s.dStream, cap, "cuMemAlloc") || !hostAlloc(s.hostStream, cap, "cuMemAllocHost")) {
            return false;
        }
        s.streamCap = cap;
    }

    size_t tables = size_t(f.nprec) * (sizeof(NrawGpuPrec) + NRAW_GPU_WGT);
    size_t codes = size_t(f.nprec) * NRAW_GPU_PACKETS * NRAW_GPU_GROUP_STRIDE;
    if (tables == s.tablesBytes && codes == s.codesBytes) {
        return true;
    }
    s.tablesBytes = 0;
    s.codesBytes = 0;
    if (!devAlloc(s.dTables, tables, "cuMemAlloc") || !hostAlloc(s.hostTables, tables, "cuMemAllocHost") ||
        !devAlloc(s.dCodes, codes, "cuMemAlloc") || !devAlloc(s.dGcli, codes, "cuMemAlloc")) {
        return false;
    }
    s.tablesBytes = tables;
    s.codesBytes = codes;
    return true;
}

// Codestream und Tabellen in die gepinnten Puffer (Host, vor dem ersten Stempel).
void stageStream(State &s, const NrawGpuStream &f)
{
    size_t precBytes = size_t(f.nprec) * sizeof(NrawGpuPrec);
    memcpy(s.hostStream, f.cs, f.size);
    memcpy(s.hostTables, f.precs, precBytes);
    memcpy(static_cast<uint8_t *>(s.hostTables) + precBytes, f.gtli, size_t(f.nprec) * NRAW_GPU_WGT);
}

// Gepinnte Puffer asynchron hoch; Prüfwort auf 0.
bool uploadStream(State &s, const NrawGpuStream &f)
{
    CUresult r = s.d.toDevice(s.dStream, s.hostStream, f.size, s.stream);
    if (r == CUDA_SUCCESS) {
        r = s.d.toDevice(s.dTables, s.hostTables, s.tablesBytes, s.stream);
    }
    if (r == CUDA_SUCCESS) {
        r = s.d.memset32(s.dFlag, 0, 1, s.stream);
    }
    return r == CUDA_SUCCESS || fail("cuMemcpyHtoDAsync", r);
}

// codes, gcli, coefs: Entropie, Dequantisierung und waagerechte Hebung in die Ebenen.
bool runEntropy(State &s, const NrawGpuStream &f)
{
    NrawGpuLayout lay = f.layout;
    Counts c = countsOf(lay);
    int nprec = int(f.nprec);
    int compw = int(f.compw);
    int valsInts = int(c.valsInts);
    size_t planeInts = s.planeBytes / sizeof(int32_t);
    CUdeviceptr gtli = s.dTables + size_t(f.nprec) * sizeof(NrawGpuPrec);
    unsigned packets = f.nprec * NRAW_GPU_PACKETS;
    unsigned slices = (f.nprec + SLICE_PRECS - 1) / SLICE_PRECS;

    void *aC[] = {&s.dStream, &s.dTables, &lay, &s.dCodes, &s.dFlag};
    void *aG[] = {&s.dCodes, &gtli, &lay, &s.dGcli, &s.dFlag, &nprec};
    void *aF[] = {&s.dStream, &s.dTables, &gtli,    &s.dGcli, &s.dPlanes,
                  &s.dFlag,   &planeInts, &compw, &valsInts, &lay};
    unsigned gx = (c.maxGroups + GCLI_BLOCK - 1) / GCLI_BLOCK;
    return launch1d(s, K_CODES, packets, 1, 1, CODES_BLOCK, 0, aC) && stamp(s, T_CODES) &&
           launch1d(s, K_GCLI, gx, slices, CHAINS, GCLI_BLOCK, 0, aG) && stamp(s, T_GCLI) &&
           launch1d(s, K_COEFS, packets, 1, 1, CODES_BLOCK, unsigned(coefShared(f)), aF) && stamp(s, T_COEFS);
}

// Stufe B: Codestream hoch, Entropie, Farbstufen, Bild und Prüfwort runter.
bool pipelineB(State &s, const NrawGpuStream &f, const NrawGpuFrame &dims)
{
    auto t0 = std::chrono::steady_clock::now();
    stageStream(s, f);
    float hostMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();

    if (!stamp(s, T_START) || !uploadStream(s, f) || !stamp(s, T_UPLOADED) || !runEntropy(s, f) || !runColor(s, dims) || !stamp(s, T_COMPUTED) ||
        !download(s, sizeof(int))) {
        return false;
    }

    s.times = NrawGpuTimes{};
    s.times.scan = f.scanMs;
    s.times.upload = hostMs + between(s, T_START, T_UPLOADED);
    s.times.codes = between(s, T_UPLOADED, T_CODES);
    s.times.gcli = between(s, T_CODES, T_GCLI);
    s.times.coefs = between(s, T_GCLI, T_COEFS);
    s.times.kernel = between(s, T_COEFS, T_COMPUTED);
    s.times.download = between(s, T_COMPUTED, T_DOWNLOADED);
    s.times.flags = unsigned(*s.hostFlag);
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

// Pinned-Bild in den Zielpuffer des Aufrufers.
void copyResult(State &s, size_t count, uint16_t *dst, NrawGpuCopy copy)
{
    auto t0 = std::chrono::steady_clock::now();
    copy(static_cast<const uint16_t *>(s.hostBayer), dst, count);
    s.times.copy = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
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
    copyResult(s, size_t(f->wf) * f->hf, dst, copy);
    return NRAW_GPU_OK;
}

int decode(const NrawGpuStream *f, uint16_t *dst, NrawGpuCopy copy)
{
    if (!ready()) {
        return NRAW_GPU_FAILED;
    }
    std::unique_lock<std::mutex> run(gRun, std::try_to_lock);
    if (!run.owns_lock() || !fits(*f)) {
        return NRAW_GPU_BUSY;
    }

    Current cur;
    State &s = gS;
    NrawGpuFrame dims{f->wf, f->hf, f->compw, f->nprec, nullptr, f->levels, f->levelCount};
    if (!ensureBuffers(s, dims) || !ensureLevels(s, dims) || !ensureEntropy(s, *f) || !pipelineB(s, *f, dims)) {
        return NRAW_GPU_FAILED;
    }
    if (s.times.flags) {
        return NRAW_GPU_CHECK;
    }
    copyResult(s, size_t(f->wf) * f->hf, dst, copy);
    return NRAW_GPU_OK;
}

// Test: Zwischenergebnis des letzten Bildes.
size_t peek(int what, void *dst, size_t bytes)
{
    if (!gReady.load() || gDead.load()) {
        return 0;
    }
    std::lock_guard<std::mutex> run(gRun);
    Current cur;
    State &s = gS;
    CUdeviceptr from = what == NRAW_GPU_DUMP_CODES ? s.dCodes : (what == NRAW_GPU_DUMP_GCLI ? s.dGcli : s.dPlanes);
    size_t have = what == NRAW_GPU_DUMP_PLANES ? s.planesBytes : s.codesBytes;
    size_t n = std::min(bytes, have);
    if (!from || n == 0 || s.d.toHost(dst, from, n, s.stream) != CUDA_SUCCESS ||
        s.d.streamWait(s.stream) != CUDA_SUCCESS) {
        return 0;
    }
    return n;
}

const char *error()
{
    return gHasError.load() ? gError : nullptr;
}

void times(NrawGpuTimes *out)
{
    *out = gS.times;
}

const NrawGpuApi API = {NRAW_GPU_VERSION, allocHost, freeHost, bayer, error, times, decode, peek};

} // namespace

extern "C" __attribute__((visibility("default"))) const NrawGpuApi *nrawGpuApi(int version)
{
    return version == NRAW_GPU_VERSION ? &API : nullptr;
}
