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

#include <dirent.h>
#include <dlfcn.h>
#include <elf.h>
#include <fnmatch.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <type_traits>
#include <vector>

#include "vst2.h"

#define EXPORT __attribute__((visibility("default"), used, noinline))

namespace {

FILE *gLog = nullptr;

void vlog(const char *fmt, va_list ap)
{
    if (!gLog)
        return;
    char stamp[32];
    time_t now = time(nullptr);
    strftime(stamp, sizeof stamp, "%F %T", localtime(&now));
    fprintf(gLog, "%s [%d] ", stamp, getpid());
    vfprintf(gLog, fmt, ap);
    fputc('\n', gLog);
    fflush(gLog);
}

__attribute__((format(printf, 1, 2))) void logf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog(fmt, ap);
    va_end(ap);
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

// Name eines Plugins für die Konfiguration: Dateiname der .so ohne Endung, bei VST3 der
// Name des Eintrags Name.vst (Name der Klasse).
std::string pluginName(const std::string &file)
{
    std::string name = file.substr(file.rfind('/') + 1);
    for (const char *ext : {".so", ".vst"})
        if (size_t n = strlen(ext); name.size() > n && name.compare(name.size() - n, n, ext) == 0)
            name.resize(name.size() - n);
    return name;
}

// Name mit dem Ordner davor, z. B. iZotope/Neoverb: yabridge legt die Plugins in Ordnern
// nach Hersteller ab, so passt ein Muster wie [iZotope/*] auf alle.
std::string pluginNameWithFolder(const std::string &file)
{
    const size_t slash = file.rfind('/');
    if (slash == std::string::npos || slash == 0)
        return pluginName(file);
    const size_t before = file.rfind('/', slash - 1);
    return file.substr(before == std::string::npos ? 0 : before + 1, slash - (before == std::string::npos ? 0 : before + 1))
           + "/" + pluginName(file);
}

std::string trim(const char *begin, const char *end)
{
    while (begin < end && (*begin == ' ' || *begin == '\t'))
        ++begin;
    while (end > begin && (end[-1] == ' ' || end[-1] == '\t'))
        --end;
    return std::string(begin, end);
}

// Namen, unter denen ein Plugin in plugins.conf steht: pluginName(), bei VST3 dazu der Name
// des Bundles ohne .vst3 (eine Waves-Shell enthält Hunderte Plugins), und der Name mit dem
// Ordner davor (pluginNameWithFolder, bei VST3 der des Bundles).
using Names = std::vector<std::string>;

// Eingebaute Vorgaben, wenn plugins.conf nichts sagt (Muster wie dort).
struct Default {
    const char *pattern, *key, *value;
};
constexpr Default kDefaults[] = {
    // Waves unter Wine zeichnet mit dem Faktor 240/DPI des Prefix, meldet aber die Größe ohne
    // ihn; bei 240 stimmen beide, die Skalierung kommt dann vom Host (Inhaltsskalierung).
    {"WaveShell*", "wine-dpi", "240"},
    // iZotope unter Wine kennt keine Inhaltsskalierung und zeichnet in 1/2,5 der Größe; nur
    // Wine selbst kann es vergrößern (wine-zoom).
    {"iZotope/*", "wine-zoom", "auto"},
};

bool matches(const std::string &pattern, const Names &names)
{
    for (const auto &n : names)
        if (fnmatch(pattern.c_str(), n.c_str(), 0) == 0)
            return true;
    return false;
}

// Einstellung je Plugin aus $XDG_CONFIG_HOME/resolve-vst/plugins.conf (Standard
// ~/.config/resolve-vst/plugins.conf), sonst aus kDefaults, leer wenn nicht gesetzt:
//
//   [MPC Beats64]          # Name (siehe Names) oder Shell-Muster wie [WaveShell*]
//   wine-dpi = 96
//   content-scale = 0
//
// Es gilt der erste passende Abschnitt mit dem Schlüssel. Wird bei jedem Aufruf gelesen
// (klein, selten gebraucht); Änderungen gelten ohne Neustart.
std::string pluginSetting(const Names &plugin, const char *key)
{
    std::string conf;
    if (const char *xdg = getenv("XDG_CONFIG_HOME"); xdg && *xdg)
        conf = xdg;
    else if (const char *home = getenv("HOME"))
        conf = std::string(home) + "/.config";
    std::string value;
    FILE *f = fopen((conf + "/resolve-vst/plugins.conf").c_str(), "r");
    bool inSection = false;
    char line[1024];
    while (f && fgets(line, sizeof line, f)) {
        line[strcspn(line, "#\r\n")] = 0;
        const char *end = line + strlen(line);
        if (const char *open = strchr(line, '['); open && trim(line, open).empty()) {
            const char *close = strchr(open, ']');
            inSection = close && matches(trim(open + 1, close), plugin);
            continue;
        }
        const char *eq = strchr(line, '=');
        if (inSection && eq && trim(line, eq) == key) {
            value = trim(eq + 1, end);
            break;
        }
    }
    if (f)
        fclose(f);
    if (value.empty())
        for (const auto &d : kDefaults)
            if (strcmp(d.key, key) == 0 && matches(d.pattern, plugin)) {
                value = d.value;
                break;
            }
    return value;
}

} // namespace

void logMessage(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog(fmt, ap);
    va_end(ap);
}

namespace {

// Editorgröße: Plugins melden ihre Größe (effEditGetRect, audioMasterSizeWindow) in
// Bildschirmpixeln, Resolves Qt nimmt die Zahlen als logische Pixel und multipliziert sie
// mit dem Skalierungsfaktor. Darum werden die Größen auf dem Weg durch diesen Faktor geteilt.
// Dazu hängt sich der Lader zwischen Resolve und Plugin: eigener Dispatcher, eigener Callback.

constexpr int effClose = 1;
constexpr int effEditGetRect = 13;
constexpr int effEditOpen = 14;
constexpr int effEditClose = 15;
constexpr int effEditIdle = 19;
constexpr int effVendorSpecific = 50;
constexpr int audioMasterSizeWindow = 15;
constexpr int kPreS = 0x50726553; // 'PreS'
constexpr long kAeCs = 0x41654373; // 'AeCs'

struct Instance {
    std::atomic<AEffect *> effect{nullptr};
    Dispatcher dispatcher = nullptr;
    Names names; // für plugins.conf
    double scale = 1.0;
    // Vergrößerung durch Wine (wine-zoom): Das Plugin meldet Größen in seinen Pixeln, auf dem
    // Bildschirm sind sie zoom-mal so groß.
    double zoom = 1.0;
    ERect rect{};
    // Rahmen (siehe fixFrame): Editorfenster von Resolve, Zielgröße in Bildschirmpixeln,
    // Zeitpunkt, bis zu dem bei effEditIdle nachgeprüft wird.
    std::atomic<unsigned long> window{0};
    std::atomic<int> targetW{0}, targetH{0};
    std::atomic<long> fixUntilMs{0};
    unsigned idleCount = 0;
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

// Skalierung für den Inhalt: JUCE-Plugins (und andere) nehmen sie als VST2 vom Host, über
// die Erweiterung von Bitwig/Ableton: effVendorSpecific mit index 'PreS', value 'AeCs',
// Faktor in opt. Resolve sendet sie nicht, darum hier vor dem Öffnen des Editors. Plugins,
// die sie nicht kennen, ignorieren den Aufruf. Manche nehmen die Zeichengröße trotzdem vom
// Bildschirm und rechnen nur die Fenstergröße mit dem Faktor (MPC Beats unter Wine): Dann
// bleibt ein Rand. Darum je Plugin abschaltbar.
// content-scale in plugins.conf, sonst RESOLVE_VST_CONTENT_SCALE: auto (Standard) = Resolves
// Faktor, Zahl = fest, 0 = nicht senden.
void sendContentScale(Instance *in, AEffect *effect)
{
    std::string setting = pluginSetting(in->names, "content-scale");
    if (setting.empty())
        if (const char *env = getenv("RESOLVE_VST_CONTENT_SCALE"))
            setting = env;
    if (setting.empty() && in->zoom != 1.0) {
        logf("  keine Inhaltsskalierung für %s: Wine vergrößert (%.2f)", in->names[0].c_str(), in->zoom);
        return;
    }
    const double scale = setting.empty() || setting == "auto" ? in->scale : atof(setting.c_str());
    if (scale <= 0) {
        logf("  Inhaltsskalierung für %s abgeschaltet", in->names[0].c_str());
        return;
    }
    long result = in->dispatcher(effect, effVendorSpecific, kPreS, kAeCs, nullptr, float(scale));
    logf("  Inhaltsskalierung %.2f an Plugin %p (Antwort %ld)", scale, static_cast<void *>(effect), result);
}

long nowMs()
{
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

// Rahmen: yabridge bettet das Wine-Fenster in ein eigenes Zwischenfenster im Editorfenster
// des Hosts ein und gibt ihm die Größe aus effEditGetRect beim Öffnen. Ändert das Plugin
// danach seine Größe (JUCE nach 'PreS'/'AeCs'), wächst das Zwischenfenster nicht mit und
// schneidet den Editor ab; JUCE-Plugins unter Wine melden nach dem Öffnen zudem wieder ihre
// Grundgröße. Darum bringt der Lader die Unterfenster von Resolves Editorfenster auf die
// Zielgröße: die Größe vor dem Öffnen bzw. die letzte von audioMasterSizeWindow.
// Über xcb, nicht Xlib: Xlib beendet bei X-Fehlern (Fenster schon weg) sonst den Prozess,
// ein eigener Fehler-Handler gälte für ganz Resolve. RESOLVE_VST_FIX_FRAME=0 schaltet ab.

struct XcbCookie {
    unsigned sequence;
};
struct XcbQueryTreeReply {
    uint8_t responseType, pad0;
    uint16_t sequence;
    uint32_t length, root, parent;
    uint16_t childrenLen;
    uint8_t pad1[14];
};
struct XcbGeometryReply {
    uint8_t responseType, depth;
    uint16_t sequence;
    uint32_t length, root;
    int16_t x, y;
    uint16_t width, height, borderWidth;
    uint8_t pad0[2];
};

struct Xcb {
    void *(*connect)(const char *, int *);
    int (*hasError)(void *);
    XcbCookie (*queryTree)(void *, uint32_t);
    XcbQueryTreeReply *(*queryTreeReply)(void *, XcbCookie, void **);
    uint32_t *(*children)(const XcbQueryTreeReply *);
    int (*childrenLength)(const XcbQueryTreeReply *);
    XcbCookie (*getGeometry)(void *, uint32_t);
    XcbGeometryReply *(*getGeometryReply)(void *, XcbCookie, void **);
    XcbCookie (*configureWindow)(void *, uint32_t, uint16_t, const uint32_t *);
    void *(*pollForEvent)(void *);
    int (*flush)(void *);
    void *connection = nullptr;
};

bool frameFixEnabled()
{
    const char *env = getenv("RESOLVE_VST_FIX_FRAME");
    return !env || strcmp(env, "0") != 0;
}

Xcb *xcb()
{
    static Xcb x;
    static bool tried = false;
    if (tried)
        return x.connection ? &x : nullptr;
    tried = true;
    void *lib = dlopen("libxcb.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!lib)
        return nullptr;
    auto sym = [lib](auto &fn, const char *name) {
        fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(lib, name));
        return fn != nullptr;
    };
    if (!sym(x.connect, "xcb_connect") || !sym(x.hasError, "xcb_connection_has_error")
        || !sym(x.queryTree, "xcb_query_tree") || !sym(x.queryTreeReply, "xcb_query_tree_reply")
        || !sym(x.children, "xcb_query_tree_children") || !sym(x.childrenLength, "xcb_query_tree_children_length")
        || !sym(x.getGeometry, "xcb_get_geometry") || !sym(x.getGeometryReply, "xcb_get_geometry_reply")
        || !sym(x.configureWindow, "xcb_configure_window") || !sym(x.pollForEvent, "xcb_poll_for_event")
        || !sym(x.flush, "xcb_flush"))
        return nullptr;
    void *c = x.connect(nullptr, nullptr);
    if (!c || x.hasError(c)) {
        logf("Rahmen: keine X11-Verbindung");
        return nullptr;
    }
    x.connection = c;
    return &x;
}

void fixFrame(Instance *in)
{
    static std::mutex mutex;
    const uint32_t window = uint32_t(in->window.load());
    const int w = in->targetW.load(), h = in->targetH.load();
    if (!window || w <= 0 || h <= 0 || !frameFixEnabled())
        return;
    std::lock_guard lock(mutex);
    Xcb *x = xcb();
    if (!x)
        return;
    void *c = x->connection;
    void *error = nullptr;
    XcbQueryTreeReply *tree = x->queryTree ? x->queryTreeReply(c, x->queryTree(c, window), &error) : nullptr;
    free(error);
    if (!tree)
        return;
    const uint32_t *kids = x->children(tree);
    for (int i = 0, n = x->childrenLength(tree); i < n; ++i) {
        error = nullptr;
        XcbGeometryReply *g = x->getGeometryReply(c, x->getGeometry(c, kids[i]), &error);
        free(error);
        if (g && (g->width != w || g->height != h)) {
            const uint32_t size[] = {uint32_t(w), uint32_t(h)};
            x->configureWindow(c, kids[i], 4 | 8, size); // XCB_CONFIG_WINDOW_WIDTH | _HEIGHT
            logf("Rahmen: Unterfenster %#x von %dx%d auf %dx%d", kids[i], g->width, g->height, w, h);
        }
        free(g);
    }
    free(tree);
    x->flush(c);
    // Fehler unbestätigter Anfragen landen in der Ereigniswarteschlange: abräumen.
    while (void *event = x->pollForEvent(c))
        free(event);
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
    case effEditOpen: {
        in->scale = editorScale(reinterpret_cast<uintptr_t>(ptr));
        logf("Editor öffnen: Plugin %p, Fenster %#lx, Skalierung %.2f", static_cast<void *>(effect),
             static_cast<unsigned long>(reinterpret_cast<uintptr_t>(ptr)), in->scale);
        sendContentScale(in, effect);
        long result = in->dispatcher(effect, opcode, index, value, ptr, opt);
        in->window.store(reinterpret_cast<uintptr_t>(ptr));
        in->fixUntilMs.store(nowMs() + 10000);
        fixFrame(in);
        return result;
    }
    case effEditClose:
        in->window.store(0);
        return in->dispatcher(effect, opcode, index, value, ptr, opt);
    case effEditIdle:
        // Kurz nach dem Öffnen bzw. einer Größenänderung den Rahmen nachprüfen (yabridge
        // setzt ihn beim Öffnen selbst und kann dem Lader zuvorkommen).
        if (in->window.load() && nowMs() < in->fixUntilMs.load() && ++in->idleCount % 5 == 0)
            fixFrame(in);
        return in->dispatcher(effect, opcode, index, value, ptr, opt);
    case effEditGetRect: {
        long result = in->dispatcher(effect, opcode, index, value, ptr, opt);
        auto **rect = static_cast<ERect **>(ptr);
        if (rect && *rect) {
            const ERect reported = **rect;
            // In Bildschirmpixeln (bei wine-zoom größer als gemeldet).
            ERect r = {0, 0, short(lround((reported.bottom - reported.top) * in->zoom)),
                       short(lround((reported.right - reported.left) * in->zoom))};
            const int w = r.right, h = r.bottom;
            bool replaced = false;
            if (!in->window.load()) {
                in->targetW.store(w);
                in->targetH.store(h);
            } else if (frameFixEnabled() && in->targetW.load() > w && in->targetH.load() > h) {
                // Nach dem Öffnen kleiner als vorher, ohne audioMasterSizeWindow: JUCE unter Wine
                // meldet dann seine Grundgröße, gezeichnet wird aber in der Zielgröße.
                r = {0, 0, short(in->targetH.load()), short(in->targetW.load())};
                replaced = true;
            }
            if (in->scale != 1.0 || in->zoom != 1.0 || replaced) {
                // Eigene Kopie: Das ERect gehört dem Plugin.
                in->rect = {scaled(r.top, in->scale), scaled(r.left, in->scale),
                            scaled(r.bottom, in->scale), scaled(r.right, in->scale)};
                *rect = &in->rect;
            }
            logf("Editorgröße: Plugin %p meldet %dx%d%s, an Resolve %dx%d", static_cast<void *>(effect),
                 reported.right - reported.left, reported.bottom - reported.top, replaced ? " (ersetzt)" : "",
                 (*rect)->right - (*rect)->left, (*rect)->bottom - (*rect)->top);
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
        Instance *in = findInstance(effect);
        if (in) {
            in->targetW.store(int(lround(index * in->zoom)));
            in->targetH.store(int(lround(value * in->zoom)));
            in->fixUntilMs.store(nowMs() + 5000);
        }
        if (in && (in->scale != 1.0 || in->zoom != 1.0)) {
            const double f = in->zoom / in->scale;
            logf("Plugin %p will %dx%d, an Resolve %ldx%ld", static_cast<void *>(effect), index, int(value),
                 lround(index * f), lround(value * f));
            index = static_cast<int>(lround(index * f));
            value = lround(value * f);
        }
        long result = gHostCallback.load(std::memory_order_relaxed)(effect, opcode, index, value, ptr, opt);
        logf("  audioMasterSizeWindow: Resolve antwortet %ld", result);
        if (in)
            fixFrame(in);
        return result;
    }
    return gHostCallback.load(std::memory_order_relaxed)(effect, opcode, index, value, ptr, opt);
}

void wrap(AEffect *effect, const Names &names, double zoom)
{
    std::lock_guard lock(gInstancesMutex);
    for (auto &in : gInstances) {
        if (in.effect.load(std::memory_order_relaxed))
            continue;
        in.dispatcher = effect->dispatcher;
        in.names = names;
        in.scale = editorScale();
        in.zoom = zoom;
        in.window.store(0);
        in.targetW.store(0);
        in.targetH.store(0);
        in.fixUntilMs.store(0);
        in.idleCount = 0;
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
// Laden jedes yabridge-Plugins auf den Wert für dieses Plugin gestellt (wantedWineDpi), in
// resolve und im VstScanner: Plugin-Prozesse können ihn überleben (eine yabridge-Plugingruppe
// wie die von Waves läuft weiter und wird später von Resolve benutzt). Den Vorwert merkt sich
// stateDir()/wine-dpi-restore; resolve-vst setzt ihn zurück, sobald Resolve beendet ist.

bool isResolveProcess()
{
    char exe[4096];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0)
        return false;
    exe[n] = 0;
    const char *base = strrchr(exe, '/');
    base = base ? base + 1 : exe;
    return strcmp(base, "resolve") == 0 || strcmp(base, "VstScanner") == 0;
}

// Prefix, in dem eine Windows-Datei (meist ein Symlink von yabridge) liegt.
std::string winePrefixOf(const std::string &windowsFile)
{
    char *real = realpath(windowsFile.c_str(), nullptr);
    if (!real)
        return {};
    std::string path = real;
    free(real);
    size_t pos = path.find("/drive_c/");
    return pos == std::string::npos ? std::string() : path.substr(0, pos);
}

// VST2 über yabridge: neben Name.so liegt Name.dll.
std::string winePrefixFor(const std::string &so)
{
    if (so.size() < 3 || so.compare(so.size() - 3, 3, ".so") != 0)
        return {};
    return winePrefixOf(so.substr(0, so.size() - 3) + ".dll");
}

// VST3 über yabridge: im Bundle liegt Contents/x86_64-win/Name.vst3.
std::string winePrefixForBundle(const std::string &bundle)
{
    std::string prefix;
    if (DIR *d = opendir((bundle + "/Contents/x86_64-win").c_str())) {
        while (dirent *e = readdir(d))
            if (e->d_name[0] != '.' && prefix.empty())
                prefix = winePrefixOf(bundle + "/Contents/x86_64-win/" + e->d_name);
        closedir(d);
    }
    return prefix;
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

// Wert aus der Konfiguration: "auto" = 96 × Resolves Faktor, "0" = nicht anfassen, sonst
// die Zahl; leer oder unbekannt = -1.
int parseWineDpi(const char *v)
{
    while (*v == ' ' || *v == '\t')
        ++v;
    if (strncmp(v, "auto", 4) == 0)
        return int(lround(96 * editorScale()));
    char *end;
    long n = strtol(v, &end, 10);
    return end != v && n >= 0 ? int(n) : -1;
}

// Wine-Vergrößerung: Plugins, die weder DPI noch Inhaltsskalierung kennen (iZotope), zeichnet
// Wine mit dem Faktor kWineBaseDpi/DPI des Prefix (gemessen mit Wine 11; Waves verhält sich
// genauso, siehe kDefaults). Das Plugin meldet seine Größe aber ohne diesen Faktor; den rechnet
// der Lader dazu (Instance::zoom). Etwas weicher als echte Skalierung, Wine rechnet das Bild hoch.
constexpr int kWineBaseDpi = 240;

// wine-zoom in plugins.conf: auto = Resolves Faktor, Zahl = fest, 0 oder leer = aus.
double wineZoomSetting(const Names &names)
{
    const std::string setting = pluginSetting(names, "wine-zoom");
    if (setting == "auto")
        return editorScale();
    const double zoom = atof(setting.c_str());
    return zoom > 0 ? zoom : 0;
}

// DPI für ein Plugin: aus wine-zoom, sonst wine-dpi in plugins.conf, sonst
// RESOLVE_VST_WINE_DPI, sonst auto. Jedes yabridge-Plugin läuft in einem eigenen
// Wine-Prozess und liest die DPI bei dessen Start, darum kann sie je Plugin verschieden sein
// (nicht bei yabridge-Plugingruppen).
int wantedWineDpi(const Names &names)
{
    if (const double zoom = wineZoomSetting(names); zoom > 0)
        return std::max(1, int(lround(kWineBaseDpi / zoom)));
    std::string setting = pluginSetting(names, "wine-dpi");
    if (setting.empty())
        if (const char *env = getenv("RESOLVE_VST_WINE_DPI"))
            setting = env;
    int dpi = setting.empty() ? -1 : parseWineDpi(setting.c_str());
    return dpi >= 0 ? dpi : parseWineDpi("auto");
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

// Faktor für Instance::zoom: kWineBaseDpi / tatsächlich gesetzte DPI (gerundet), 1 ohne
// wine-zoom oder ohne Wine. `prefix` wie bei syncWineDpi.
double wineZoom(const Names &names, const std::string &prefix)
{
    if (prefix.empty() || wineZoomSetting(names) <= 0)
        return 1.0;
    return double(kWineBaseDpi) / wantedWineDpi(names);
}

// `prefix` aus winePrefixFor bzw. winePrefixForBundle.
void syncWineDpi(const Names &names, const std::string &prefix)
{
    struct Known {
        std::string prefix;
        int dpi;
    };
    static const bool inResolve = isResolveProcess();
    static std::mutex mutex;
    static std::vector<Known> known; // zuletzt gesetzter bzw. abgefragter Wert je Prefix
    if (!inResolve || prefix.empty())
        return;
    const int wanted = wantedWineDpi(names);
    if (wanted <= 0)
        return;
    std::lock_guard lock(mutex);

    timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    Known *k = nullptr;
    for (auto &entry : known)
        if (entry.prefix == prefix)
            k = &entry;
    if (!k) {
        const int current = queryWineDpi(prefix);
        if (current == -2) {
            logf("  Wine-DPI %s: Abfrage nicht möglich, bleibt", prefix.c_str());
            return;
        }
        known.push_back({prefix, current});
        k = &known.back();
    }
    const int current = k->dpi;
    if (current == wanted) {
        logf("  Wine-DPI %s: %d, passt", prefix.c_str(), current);
        return;
    }
    // Ohne gemerkten Vorwert kein Ändern, sonst ließe er sich nicht zurücksetzen. Nur der
    // erste Eintrag je Prefix zählt (Wert vor Resolve).
    if (!rememberWineDpi(prefix, current)) {
        logf("  Wine-DPI %s: Vorwert nicht speicherbar, bleibt %d", prefix.c_str(), current);
        return;
    }
    bool ok = runWine(prefix, {"reg", "add", "HKCU\\Control Panel\\Desktop", "/v", "LogPixels", "/t", "REG_DWORD",
                               "/d", std::to_string(wanted), "/f"}) == 0;
    if (ok)
        k->dpi = wanted;
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
        // Ordner Name.vst mit bundle und class: VST3-Plugin über den Adapter (vst3.cpp).
        // Andere Ordner wie carla.vst/ sind keine Plugins.
        if (!isVst3Entry(path)) {
            logf("  übersprungen: Ordner");
            failed = true;
            return nullptr;
        }
        const std::string bundle = vst3Bundle(path);
        const std::string bundleName = bundle.substr(0, bundle.size() - 5); // ohne .vst3
        const Names names{pluginName(path), pluginName(bundleName), pluginNameWithFolder(bundleName)};
        const std::string prefix = winePrefixForBundle(bundle);
        syncWineDpi(names, prefix);
        gHostCallback.store(callback, std::memory_order_relaxed);
        AEffect *effect = vst3Load(path, hostCallback);
        if (!effect) {
            failed = true;
            return nullptr;
        }
        wrap(effect, names, wineZoom(names, prefix));
        loaded_ = 1;
        logf("  ok, VST3 als AEffect %p", static_cast<void *>(effect));
        return effect;
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
    const Names names{pluginName(file), pluginNameWithFolder(file)};
    const std::string prefix = winePrefixFor(file);
    syncWineDpi(names, prefix);

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

    wrap(effect, names, wineZoom(names, prefix));
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

// Sucht eine Funktion oder Variable in der Symboltabelle (.symtab) einer ELF-Datei. Nur für Executables
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
                const int type = ELF64_ST_TYPE(syms[j].st_info);
                if ((type == STT_FUNC || type == STT_OBJECT) && syms[j].st_value
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

// Kategorien im VstScanner: Er liest <CategoryMask> beim Laden der Plugin-Liste über
// AudioPluginHost::GetPluginCategoryMaskIn und schreibt sie über ...MaskOut. Beide rechnen
// über Tabellen (std::map<int, int>, Bit der Datei ↔ Bit intern: Schlüssel & Maske ergibt
// den Wert), die PluginHost::Initialize füllt; im Scanner geschieht das nie, die Tabellen
// bleiben leer, und bei jedem Start von Resolve schreibt der Scanner alle Masken als 0 zurück
// (auch die von resolve-vst categories). Ersatz: wie das Original, bei leerer Tabelle aber
// die Maske unverändert.
struct MaskNode {
    MaskNode *left, *right, *parent;
    bool black;
    int key, value; // libc++ __tree_node<pair<const int, int>>: ab 0x1c
};
struct MaskMap {
    MaskNode *begin;
    MaskNode endNode; // nur left (Wurzel) gültig
};
static_assert(offsetof(MaskNode, key) == 0x1c && offsetof(MaskNode, value) == 0x20);

const MaskMap *gMaskIn = nullptr, *gMaskOut = nullptr;

int translateMask(const MaskNode *node, int mask)
{
    if (!node)
        return 0;
    return ((node->key & mask) ? node->value : 0) | translateMask(node->left, mask) | translateMask(node->right, mask);
}

int translateMask(const MaskMap *map, int mask)
{
    const MaskNode *root = map ? map->endNode.left : nullptr;
    return root ? translateMask(root, mask) : mask;
}

int categoryMaskIn(const void *, int mask)
{
    return translateMask(gMaskIn, mask);
}

int categoryMaskOut(const void *, int mask)
{
    return translateMask(gMaskOut, mask);
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
        {"_ZNK15AudioPluginHost23GetPluginCategoryMaskInEi", reinterpret_cast<void *>(&categoryMaskIn)},
        {"_ZNK15AudioPluginHost24GetPluginCategoryMaskOutEi", reinterpret_cast<void *>(&categoryMaskOut)},
    };
    // Vor dem Umleiten: Tabellen der Kategorie-Masken (nur im VstScanner im Executable).
    gMaskIn = reinterpret_cast<const MaskMap *>(findExecSymbol(exe, "_ZN15AudioPluginHost19_pluginCategoryMaskE"));
    gMaskOut = reinterpret_cast<const MaskMap *>(findExecSymbol(exe, "_ZN15AudioPluginHost18_pluginCategoryMapE"));
    for (const auto &p : patches) {
        uintptr_t addr = findExecSymbol(exe, p.name);
        if (!addr)
            continue;
        if (reinterpret_cast<void *>(addr) == p.replacement)
            continue;
        if ((p.replacement == reinterpret_cast<void *>(&categoryMaskIn) && !gMaskIn)
            || (p.replacement == reinterpret_cast<void *>(&categoryMaskOut) && !gMaskOut))
            continue;
        bool ok = patchJump(addr, p.replacement);
        logf("%s: %s bei %#lx %s", exe, p.name, static_cast<unsigned long>(addr), ok ? "umgeleitet" : "NICHT umgeleitet");
    }
}

// Zahlenformat: Resolve und VstScanner lesen die Version von FXConfiguration.xml ("1.4") mit
// dem Zahlenformat des Gebietsschemas; mit deutschem (Komma) wird daraus 1, und unter 1.3
// übergehen beide <CategoryMask>: alle Plugins „Unsortiert“, und der Scanner schreibt bei
// jedem Start 0 zurück. Darum LC_NUMERIC=C, bevor Qt das Gebietsschema setzt (der Scanner
// erbt es). LC_ALL ginge vor, sein Wert wandert darum nach LANG.
void fixNumericLocale()
{
    if (!isResolveProcess())
        return;
    if (const char *all = getenv("LC_ALL"); all && *all) {
        setenv("LANG", all, 1);
        unsetenv("LC_ALL");
    }
    setenv("LC_NUMERIC", "C", 1);
}

__attribute__((constructor)) void init()
{
    openLog();
    fixNumericLocale();
    patchExecutable();
}

} // namespace
