// resolve-vst: VST2-Lader für DaVinci Resolve unter Linux.
//
// Resolve bringt unter Linux einen vollständigen VST2-Host mit (VSTHost, VSTPlugin,
// VSTPluginManager), aber VSTPluginLoader::Load() ist ein leerer Rumpf, der immer nullptr
// liefert. Diese Bibliothek wird per LD_PRELOAD geladen und liefert die fehlende Funktion:
//
//  - In resolve (libFairlightPage.so) läuft der Aufruf über die PLT, das gleichnamige
//    Symbol hier gewinnt automatisch.
//  - Im VstScanner (Executable ohne PIE, direkter Aufruf) wird der Anfang der leeren
//    Funktion beim Laden im Speicher auf unsere Funktion umgebogen. Die Adresse steht in
//    der Symboltabelle von /proc/self/exe.

#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <vector>

#define EXPORT __attribute__((visibility("default"), used, noinline))

namespace {

constexpr int32_t kEffectMagic = 0x56737450; // 'VstP'

FILE *gLog = nullptr;

void logf(const char *fmt, ...)
{
    if (!gLog)
        return;
    char stamp[32];
    time_t now = time(nullptr);
    strftime(stamp, sizeof stamp, "%F %T", localtime(&now));
    fprintf(gLog, "%s [%d] ", stamp, getpid());
    va_list ap;
    va_start(ap, fmt);
    vfprintf(gLog, fmt, ap);
    va_end(ap);
    fputc('\n', gLog);
    fflush(gLog);
}

// $XDG_STATE_HOME/resolve-vst (Standard ~/.local/state/resolve-vst), wird angelegt.
// Dieselbe Regel steht in resolve-vst.
std::string stateDir()
{
    std::string base;
    if (const char *xdg = getenv("XDG_STATE_HOME"); xdg && *xdg)
        base = xdg;
    else if (const char *home = getenv("HOME"))
        base = std::string(home) + "/.local/state";
    else
        return {};
    mkdir(base.c_str(), 0755);
    mkdir((base + "/resolve-vst").c_str(), 0755);
    return base + "/resolve-vst";
}

void openLog()
{
    std::string path;
    if (const char *env = getenv("RESOLVE_VST_LOG"))
        path = env;
    else if (const std::string dir = stateDir(); !dir.empty())
        path = dir + "/resolve-vst.log";
    if (!path.empty() && path != "-")
        gLog = fopen(path.c_str(), "a");
    else if (path == "-")
        gLog = stderr;
}

// Resolve übergibt Pfade als wchar_t (UTF-32). Nicht über wcstombs, das hängt am Locale.
std::string toUtf8(const wchar_t *w)
{
    std::string out;
    for (; w && *w; ++w) {
        uint32_t c = static_cast<uint32_t>(*w);
        if (c < 0x80) {
            out += char(c);
        } else if (c < 0x800) {
            out += char(0xC0 | (c >> 6));
            out += char(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += char(0xE0 | (c >> 12));
            out += char(0x80 | ((c >> 6) & 0x3F));
            out += char(0x80 | (c & 0x3F));
        } else {
            out += char(0xF0 | (c >> 18));
            out += char(0x80 | ((c >> 12) & 0x3F));
            out += char(0x80 | ((c >> 6) & 0x3F));
            out += char(0x80 | (c & 0x3F));
        }
    }
    return out;
}

bool isDirectory(const std::string &path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

} // namespace

struct AEffect;
using audioMasterCallback = long (*)(AEffect *, int, int, long, void *, float);
using Dispatcher = long (*)(AEffect *, int, int, long, void *, float);
using VstEntry = AEffect *(*)(audioMasterCallback);

struct AEffect {
    int32_t magic;
    Dispatcher dispatcher;
    // Rest der Struktur interessiert hier nicht.
};

namespace {

// Editorgröße: Plugins melden ihre Größe (effEditGetRect, audioMasterSizeWindow) in
// Bildschirmpixeln, Resolves Qt nimmt die Zahlen als logische Pixel und multipliziert sie
// mit dem Skalierungsfaktor. Darum werden die Größen auf dem Weg durch diesen Faktor geteilt.
// Dazu hängt sich der Lader zwischen Resolve und Plugin: eigener Dispatcher, eigener Callback.

constexpr int effClose = 1;
constexpr int effEditGetRect = 13;
constexpr int effEditOpen = 14;
constexpr int effVendorSpecific = 50;
constexpr int audioMasterSizeWindow = 15;
constexpr int kPreS = 0x50726553; // 'PreS'
constexpr long kAeCs = 0x41654373; // 'AeCs'

struct ERect {
    int16_t top, left, bottom, right;
};

struct Instance {
    std::atomic<AEffect *> effect{nullptr};
    Dispatcher dispatcher = nullptr;
    double scale = 1.0;
    ERect rect{};
};

// Feste Tabelle statt map: der Dispatcher läuft auch im Audio-Thread, Lesen ohne Lock.
Instance gInstances[512];
std::mutex gInstancesMutex;
std::atomic<audioMasterCallback> gHostCallback{nullptr};

Instance *findInstance(AEffect *effect)
{
    for (auto &in : gInstances)
        if (in.effect.load(std::memory_order_acquire) == effect)
            return &in;
    return nullptr;
}

template<typename T>
T qtFunction(const char *name)
{
    return reinterpret_cast<T>(dlsym(RTLD_DEFAULT, name));
}

// Skalierungsfaktor (devicePixelRatio) des Fensters, in das das Plugin zeichnet; ohne
// Fenster der des Hauptbildschirms. RESOLVE_VST_EDITOR_SCALE legt ihn fest (1 = aus).
double editorScale(unsigned long long windowId = 0)
{
    if (const char *env = getenv("RESOLVE_VST_EDITOR_SCALE")) {
        double s = atof(env);
        if (s > 0)
            return s;
    }
    using Find = void *(*)(unsigned long long);
    using WidgetFn = void *(*)(const void *);
    using Ratio = double (*)(const void *);
    if (windowId) {
        auto find = qtFunction<Find>("_ZN7QWidget4findEy");
        auto windowHandle = qtFunction<WidgetFn>("_ZNK7QWidget12windowHandleEv");
        auto topLevel = qtFunction<WidgetFn>("_ZNK7QWidget6windowEv");
        auto ratio = qtFunction<Ratio>("_ZNK7QWindow16devicePixelRatioEv");
        if (find && windowHandle && topLevel && ratio) {
            if (void *widget = find(windowId)) {
                void *window = windowHandle(widget);
                if (!window)
                    window = windowHandle(topLevel(widget));
                if (window)
                    return ratio(window);
            }
        }
    }
    auto primaryScreen = qtFunction<void *(*)()>("_ZN15QGuiApplication13primaryScreenEv");
    auto screenRatio = qtFunction<Ratio>("_ZNK7QScreen16devicePixelRatioEv");
    if (primaryScreen && screenRatio)
        if (void *screen = primaryScreen())
            return screenRatio(screen);
    return 1.0;
}

// Skalierung für den Inhalt: JUCE-Plugins (und andere) nehmen sie als VST2 nicht vom System,
// sondern vom Host, über die Erweiterung von Bitwig/Ableton: effVendorSpecific mit
// index 'PreS', value 'AeCs', Faktor in opt. Resolve sendet sie nicht, darum hier vor dem
// Öffnen des Editors. Plugins, die sie nicht kennen, ignorieren den Aufruf.
// RESOLVE_VST_CONTENT_SCALE legt den Faktor fest (0 = nicht senden).
void sendContentScale(Instance *in, AEffect *effect)
{
    double scale = in->scale;
    if (const char *env = getenv("RESOLVE_VST_CONTENT_SCALE"))
        scale = atof(env);
    if (scale <= 0)
        return;
    long result = in->dispatcher(effect, effVendorSpecific, kPreS, kAeCs, nullptr, float(scale));
    logf("  Inhaltsskalierung %.2f an Plugin %p (Antwort %ld)", scale, static_cast<void *>(effect), result);
}

short scaled(short v, double s)
{
    return static_cast<short>(lround(v / s));
}

long dispatch(AEffect *effect, int opcode, int index, long value, void *ptr, float opt)
{
    Instance *in = findInstance(effect);
    if (!in)
        return 0;
    switch (opcode) {
    case effEditOpen:
        in->scale = editorScale(reinterpret_cast<uintptr_t>(ptr));
        logf("Editor öffnen: Plugin %p, Fenster %#lx, Skalierung %.2f", static_cast<void *>(effect),
             static_cast<unsigned long>(reinterpret_cast<uintptr_t>(ptr)), in->scale);
        sendContentScale(in, effect);
        return in->dispatcher(effect, opcode, index, value, ptr, opt);
    case effEditGetRect: {
        long result = in->dispatcher(effect, opcode, index, value, ptr, opt);
        auto **rect = static_cast<ERect **>(ptr);
        if (rect && *rect) {
            const ERect r = **rect;
            if (in->scale != 1.0) {
                // Eigene Kopie: Das ERect gehört dem Plugin.
                in->rect = {scaled(r.top, in->scale), scaled(r.left, in->scale),
                            scaled(r.bottom, in->scale), scaled(r.right, in->scale)};
                *rect = &in->rect;
            }
            logf("Editorgröße: Plugin %p meldet %dx%d, an Resolve %dx%d", static_cast<void *>(effect),
                 r.right - r.left, r.bottom - r.top, (*rect)->right - (*rect)->left, (*rect)->bottom - (*rect)->top);
        }
        return result;
    }
    case effClose: {
        long result = in->dispatcher(effect, opcode, index, value, ptr, opt);
        in->effect.store(nullptr, std::memory_order_release);
        return result;
    }
    default:
        return in->dispatcher(effect, opcode, index, value, ptr, opt);
    }
}

long hostCallback(AEffect *effect, int opcode, int index, long value, void *ptr, float opt)
{
    if (opcode == audioMasterSizeWindow && effect) {
        if (Instance *in = findInstance(effect); in && in->scale != 1.0) {
            logf("Plugin %p will %dx%d, an Resolve %ldx%ld", static_cast<void *>(effect), index, int(value),
                 lround(index / in->scale), lround(value / in->scale));
            index = static_cast<int>(lround(index / in->scale));
            value = lround(value / in->scale);
        }
    }
    return gHostCallback.load(std::memory_order_relaxed)(effect, opcode, index, value, ptr, opt);
}

void wrap(AEffect *effect)
{
    std::lock_guard lock(gInstancesMutex);
    for (auto &in : gInstances) {
        if (in.effect.load(std::memory_order_relaxed))
            continue;
        in.dispatcher = effect->dispatcher;
        in.scale = editorScale();
        in.effect.store(effect, std::memory_order_release);
        effect->dispatcher = dispatch;
        return;
    }
    logf("  zu viele Plugin-Instanzen, Editorgröße wird nicht angepasst");
}

// Wine-DPI: Windows-Plugins über yabridge zeichnen mit der DPI ihres Wine-Prefix
// (HKCU\Control Panel\Desktop\LogPixels, Standard 96); JUCE-Plugins ignorieren sie (siehe
// sendContentScale), andere nicht. Wine kennt keine DPI pro Prozess, der Wert gilt für das
// ganze Prefix und wird beim Start des Plugin-Prozesses gelesen. Darum wird er vor dem
// Laden eines yabridge-Plugins auf 96 × Resolves Faktor gestellt, nur in resolve selbst
// (der Scanner zeigt keine Editoren) und je Prefix einmal. Den Vorwert merkt sich
// stateDir()/wine-dpi-restore; resolve-vst setzt ihn zurück, sobald Resolve beendet ist.

bool isResolveProcess()
{
    char exe[4096];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0)
        return false;
    exe[n] = 0;
    const char *base = strrchr(exe, '/');
    return strcmp(base ? base + 1 : exe, "resolve") == 0;
}

// yabridge legt neben Name.so die Name.dll ab, meist als Symlink in den Prefix.
std::string winePrefixFor(const std::string &so)
{
    if (so.size() < 3 || so.compare(so.size() - 3, 3, ".so") != 0)
        return {};
    const std::string dll = so.substr(0, so.size() - 3) + ".dll";
    char *real = realpath(dll.c_str(), nullptr);
    if (!real)
        return {};
    std::string path = real;
    free(real);
    size_t pos = path.find("/drive_c/");
    return pos == std::string::npos ? std::string() : path.substr(0, pos);
}

// Führt `wine <args>` im Prefix aus, über $WINELOADER wie yabridge (z. B. Runner aus
// bottle.yml). Mit `out` wird stdout gesammelt. Liefert den Exit-Code, -1 wenn Wine nicht
// startet.
int runWine(const std::string &prefix, std::vector<std::string> args, std::string *out = nullptr)
{
    const char *loader = getenv("WINELOADER");
    args.insert(args.begin(), loader && *loader ? loader : "wine");
    std::vector<char *> argv;
    for (auto &a : args)
        argv.push_back(a.data());
    argv.push_back(nullptr);

    std::vector<std::string> env;
    for (char **e = environ; *e; ++e)
        if (strncmp(*e, "WINEPREFIX=", 11) != 0 && strncmp(*e, "WINEDEBUG=", 10) != 0
            && strncmp(*e, "LD_PRELOAD=", 11) != 0)
            env.emplace_back(*e);
    env.push_back("WINEPREFIX=" + prefix);
    env.push_back("WINEDEBUG=-all");
    std::vector<char *> envp;
    for (auto &e : env)
        envp.push_back(e.data());
    envp.push_back(nullptr);

    int fds[2] = {-1, -1};
    if (out && pipe2(fds, O_CLOEXEC) != 0)
        return -1;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    if (out)
        posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
    else
        posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    pid_t pid;
    int err = posix_spawnp(&pid, argv[0], &actions, nullptr, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&actions);
    if (out) {
        close(fds[1]);
        char buf[4096];
        ssize_t n;
        while (err == 0 && (n = read(fds[0], buf, sizeof buf)) > 0)
            out->append(buf, n);
        close(fds[0]);
    }
    if (err != 0) {
        logf("  Wine-DPI: %s nicht startbar: %s", argv[0], strerror(err));
        return -1;
    }
    int status = 0;
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status))
        return -1;
    return WEXITSTATUS(status);
}

// Aktueller Wert über `wine reg query`, nicht aus user.reg: wineserver schreibt die Datei
// verzögert, direkt nach einem Zurücksetzen stünde dort noch der alte Wert.
// -1 = nicht gesetzt, -2 = Abfrage nicht möglich.
int queryWineDpi(const std::string &prefix)
{
    std::string out;
    int status = runWine(prefix, {"reg", "query", "HKCU\\Control Panel\\Desktop", "/v", "LogPixels"}, &out);
    if (status < 0)
        return -2;
    unsigned value;
    size_t pos = out.find("REG_DWORD");
    if (pos != std::string::npos && sscanf(out.c_str() + pos, "REG_DWORD %x", &value) == 1)
        return int(value);
    return -1;
}

// RESOLVE_VST_WINE_DPI: nicht gesetzt oder `auto` = 96 × Resolves Faktor, `0` = nicht
// anfassen, sonst der Wert.
int wantedWineDpi()
{
    const char *env = getenv("RESOLVE_VST_WINE_DPI");
    if (!env || !*env || strcmp(env, "auto") == 0)
        return int(lround(96 * editorScale()));
    return atoi(env);
}

// Merkt sich den Vorwert (Zeile "Prefix<Tab>DPI", -1 = war nicht gesetzt). Steht der Prefix
// schon drin, bleibt der ältere Eintrag: Er ist der Wert vor Resolve.
bool rememberWineDpi(const std::string &prefix, int previous)
{
    const std::string file = stateDir() + "/wine-dpi-restore";
    if (FILE *f = fopen(file.c_str(), "r")) {
        char line[4096];
        bool known = false;
        while (!known && fgets(line, sizeof line, f))
            known = strncmp(line, prefix.c_str(), prefix.size()) == 0 && line[prefix.size()] == '\t';
        fclose(f);
        if (known)
            return true;
    }
    FILE *f = fopen(file.c_str(), "a");
    if (!f)
        return false;
    bool ok = fprintf(f, "%s\t%d\n", prefix.c_str(), previous) > 0;
    return fclose(f) == 0 && ok;
}

void syncWineDpi(const std::string &so)
{
    static const bool inResolve = isResolveProcess();
    static std::mutex mutex;
    static std::vector<std::string> done;
    if (!inResolve)
        return;
    const std::string prefix = winePrefixFor(so);
    if (prefix.empty())
        return;
    std::lock_guard lock(mutex);
    for (const auto &p : done)
        if (p == prefix)
            return;
    done.push_back(prefix);

    const int wanted = wantedWineDpi();
    if (wanted <= 0)
        return;
    timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    const int current = queryWineDpi(prefix);
    if (current == -2) {
        logf("  Wine-DPI %s: Abfrage nicht möglich, bleibt", prefix.c_str());
        return;
    }
    if (current == wanted) {
        logf("  Wine-DPI %s: %d, passt", prefix.c_str(), current);
        return;
    }
    // Ohne gemerkten Vorwert kein Ändern, sonst ließe er sich nicht zurücksetzen.
    if (!rememberWineDpi(prefix, current)) {
        logf("  Wine-DPI %s: Vorwert nicht speicherbar, bleibt %d", prefix.c_str(), current);
        return;
    }
    bool ok = runWine(prefix, {"reg", "add", "HKCU\\Control Panel\\Desktop", "/v", "LogPixels", "/t", "REG_DWORD",
                               "/d", std::to_string(wanted), "/f"}) == 0;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    logf("  Wine-DPI %s: %d → %d %s (%.1f s); zurück nach Beenden von Resolve", prefix.c_str(), current, wanted,
         ok ? "gesetzt" : "NICHT gesetzt", double(t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9);
}

} // namespace

// Gleiche Signatur wie in Resolve, damit der Name (Itanium-Mangling) übereinstimmt:
// _ZN15VSTPluginLoader4LoadEPKwPFlP7AEffectiilPvfERb und _ZN15VSTPluginLoader5CloseEv.
// Das Objekt gehört Resolve; sein Konstruktor reserviert ein Byte (geladen ja/nein), mehr
// Platz gibt es nicht. Das dlopen-Handle wird darum nicht gespeichert (siehe Close).
class VSTPluginLoader {
public:
    EXPORT AEffect *Load(const wchar_t *path, audioMasterCallback callback, bool &failed);
    EXPORT void Close();

private:
    unsigned char loaded_;
};

AEffect *VSTPluginLoader::Load(const wchar_t *wpath, audioMasterCallback callback, bool &failed)
{
    const std::string path = toUtf8(wpath);
    logf("Load %s", path.c_str());

    if (isDirectory(path)) {
        // VST3-Bundles und Ordner wie carla.vst/ sind keine VST2-Bibliotheken.
        logf("  übersprungen: Ordner");
        failed = true;
        return nullptr;
    }

    // Der Scanner findet nur Namen auf *.vst, darum zeigen Symlinks Name.vst auf die echte
    // .so. Mit dem echten Pfad laden: Bridges wie yabridge suchen ihre Dateien neben
    // dem eigenen Pfad (Name.so → Name.dll).
    std::string file = path;
    if (char *real = realpath(path.c_str(), nullptr)) {
        file = real;
        free(real);
        if (file != path)
            logf("  → %s", file.c_str());
    }
    syncWineDpi(file);

    int flags = RTLD_NOW | RTLD_LOCAL;
    if (const char *deep = getenv("RESOLVE_VST_DEEPBIND"); deep && *deep == '1')
        flags |= RTLD_DEEPBIND;
    void *handle = dlopen(file.c_str(), flags);
    if (!handle) {
        logf("  dlopen: %s", dlerror());
        failed = true;
        return nullptr;
    }

    auto entry = reinterpret_cast<VstEntry>(dlsym(handle, "VSTPluginMain"));
    if (!entry)
        entry = reinterpret_cast<VstEntry>(dlsym(handle, "main"));
    if (!entry) {
        logf("  kein VSTPluginMain/main, kein VST2-Plugin");
        dlclose(handle);
        failed = true;
        return nullptr;
    }

    // Resolve übergibt immer denselben Callback (VSTHost::HostCallback).
    gHostCallback.store(callback, std::memory_order_relaxed);
    AEffect *effect = entry(hostCallback);
    if (!effect || effect->magic != kEffectMagic) {
        logf("  Einstiegspunkt lieferte %p, kein gültiger AEffect", static_cast<void *>(effect));
        // Handle bewusst offen lassen: manche Plugins hinterlassen nach einem
        // fehlgeschlagenen Start Threads oder atexit-Handler.
        failed = true;
        return nullptr;
    }

    wrap(effect);
    loaded_ = 1;
    logf("  ok, AEffect %p", static_cast<void *>(effect));
    return effect;
}

void VSTPluginLoader::Close()
{
    // Kein dlclose: Das Handle ist nicht gespeichert (siehe oben), und viele Plugins
    // vertragen das Entladen schlecht. dlopen zählt Referenzen, ein erneutes Load
    // bekommt dieselbe Bibliothek.
    loaded_ = 0;
}

namespace {

// Sucht ein Symbol in der Symboltabelle (.symtab) einer ELF-Datei. Nur für Executables
// ohne PIE (ET_EXEC), dort ist st_value die Laufzeitadresse.
uintptr_t findExecSymbol(const char *file, const char *name)
{
    int fd = open(file, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return 0;
    struct stat st;
    if (fstat(fd, &st) != 0) {
        close(fd);
        return 0;
    }
    void *map = mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED)
        return 0;

    uintptr_t result = 0;
    auto *base = static_cast<const unsigned char *>(map);
    auto *eh = reinterpret_cast<const Elf64_Ehdr *>(base);
    if (st.st_size >= off_t(sizeof(Elf64_Ehdr)) && memcmp(eh->e_ident, ELFMAG, SELFMAG) == 0
        && eh->e_ident[EI_CLASS] == ELFCLASS64 && eh->e_type == ET_EXEC) {
        auto *sh = reinterpret_cast<const Elf64_Shdr *>(base + eh->e_shoff);
        for (int i = 0; i < eh->e_shnum && !result; ++i) {
            if (sh[i].sh_type != SHT_SYMTAB)
                continue;
            auto *syms = reinterpret_cast<const Elf64_Sym *>(base + sh[i].sh_offset);
            auto *strs = reinterpret_cast<const char *>(base + sh[sh[i].sh_link].sh_offset);
            size_t count = sh[i].sh_size / sizeof(Elf64_Sym);
            for (size_t j = 0; j < count; ++j) {
                if (ELF64_ST_TYPE(syms[j].st_info) == STT_FUNC && syms[j].st_value
                    && strcmp(strs + syms[j].st_name, name) == 0) {
                    result = syms[j].st_value;
                    break;
                }
            }
        }
    }
    munmap(map, st.st_size);
    return result;
}

// Überschreibt den Anfang von `target` mit `movabs rax, dest; jmp rax` (12 Byte).
bool patchJump(uintptr_t target, void *dest)
{
    unsigned char code[12] = {0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xE0};
    uintptr_t d = reinterpret_cast<uintptr_t>(dest);
    memcpy(code + 2, &d, sizeof d);

    long pageSize = sysconf(_SC_PAGESIZE);
    uintptr_t page = target & ~uintptr_t(pageSize - 1);
    size_t len = (target + sizeof code) - page;
    if (mprotect(reinterpret_cast<void *>(page), len, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        logf("mprotect: %s", strerror(errno));
        return false;
    }
    memcpy(reinterpret_cast<void *>(target), code, sizeof code);
    mprotect(reinterpret_cast<void *>(page), len, PROT_READ | PROT_EXEC);
    __builtin___clear_cache(reinterpret_cast<char *>(target), reinterpret_cast<char *>(target + sizeof code));
    return true;
}

template<typename F>
void *memberAddress(F f)
{
    void *p;
    static_assert(sizeof f >= sizeof p);
    memcpy(&p, &f, sizeof p); // Itanium-ABI: nicht virtuelle Memberfunktion = Adresse
    return p;
}

void patchExecutable()
{
    char exe[4096];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0)
        return;
    exe[n] = 0;

    struct {
        const char *name;
        void *replacement;
    } const patches[] = {
        {"_ZN15VSTPluginLoader4LoadEPKwPFlP7AEffectiilPvfERb", memberAddress(&VSTPluginLoader::Load)},
        {"_ZN15VSTPluginLoader5CloseEv", memberAddress(&VSTPluginLoader::Close)},
    };
    for (const auto &p : patches) {
        uintptr_t addr = findExecSymbol(exe, p.name);
        if (!addr)
            continue;
        if (reinterpret_cast<void *>(addr) == p.replacement)
            continue;
        bool ok = patchJump(addr, p.replacement);
        logf("%s: %s bei %#lx %s", exe, p.name, static_cast<unsigned long>(addr), ok ? "umgeleitet" : "NICHT umgeleitet");
    }
}

__attribute__((constructor)) void init()
{
    openLog();
    patchExecutable();
}

} // namespace
