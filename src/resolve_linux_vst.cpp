// resolve-linux-vst: VST2-Lader für DaVinci Resolve unter Linux.
//
// Resolve bringt unter Linux einen vollständigen VST2-Host mit (VSTHost, VSTPlugin,
// VSTPluginManager), aber VSTPluginLoader::Load() ist ein leerer Rumpf, der immer nullptr
// liefert. Diese Bibliothek wird per LD_PRELOAD geladen und liefert die fehlende Funktion:
//
//  - In resolve (libFairlightPage.so) läuft der Aufruf über die PLT, das gleichnamige
//    Symbol hier gewinnt automatisch.
//  - Im VstScanner (Executable ohne PIE, direkter Aufruf) wird der Anfang der leeren
//    Funktion beim Laden im Speicher auf unsere Funktion umgebogen. Die Adresse steht in
//    der Symboltabelle von /proc/self/exe (ElfFile, sites.cpp).

#include <dirent.h>
#include <dlfcn.h>
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
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <type_traits>
#include <vector>

#include "editor_size.h"
#include "plugin_conf.h"
#include "process.h"
#include "log.h"
#include "patch.h"
#include "sites.h"
#include "vst2.h"
#include "wine_dpi.h"

#define EXPORT __attribute__((visibility("default"), used, noinline))

namespace {

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
    logMessage("Load %s", path.c_str());

    if (isDirectory(path)) {
        // Ordner Name.vst mit bundle und class: VST3-Plugin über den Adapter (vst3.cpp).
        // Andere Ordner wie carla.vst/ sind keine Plugins.
        if (!isVst3Entry(path)) {
            logMessage("  übersprungen: Ordner");
            failed = true;
            return nullptr;
        }
        const std::string bundle = vst3Bundle(path);
        const std::string bundleName = bundle.substr(0, bundle.size() - 5); // ohne .vst3
        const Names names{pluginName(path), pluginName(bundleName), pluginNameWithFolder(bundleName)};
        const std::string prefix = winePrefixForBundle(bundle);
        const double zoom = syncWineDpi(names, prefix);
        setEditorHost(callback);
        AEffect *effect = vst3Load(path, editorCallback);
        if (!effect) {
            failed = true;
            return nullptr;
        }
        wrapEditor(effect, names, zoom);
        loaded_ = 1;
        logMessage("  ok, VST3 als AEffect %p", static_cast<void *>(effect));
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
            logMessage("  → %s", file.c_str());
    }
    const Names names{pluginName(file), pluginNameWithFolder(file)};
    const std::string prefix = winePrefixFor(file);
    const double zoom = syncWineDpi(names, prefix);

    int flags = RTLD_NOW | RTLD_LOCAL;
    if (const char *deep = getenv("RESOLVE_LINUX_VST_DEEPBIND"); deep && *deep == '1')
        flags |= RTLD_DEEPBIND;
    void *handle = dlopen(file.c_str(), flags);
    if (!handle) {
        logMessage("  dlopen: %s", dlerror());
        failed = true;
        return nullptr;
    }

    auto entry = reinterpret_cast<VstEntry>(dlsym(handle, "VSTPluginMain"));
    if (!entry)
        entry = reinterpret_cast<VstEntry>(dlsym(handle, "main"));
    if (!entry) {
        logMessage("  kein VSTPluginMain/main, kein VST2-Plugin");
        dlclose(handle);
        failed = true;
        return nullptr;
    }

    // Resolve übergibt immer denselben Callback (VSTHost::HostCallback).
    setEditorHost(callback);
    AEffect *effect = entry(editorCallback);
    if (!effect || effect->magic != kEffectMagic) {
        logMessage("  Einstiegspunkt lieferte %p, kein gültiger AEffect", static_cast<void *>(effect));
        // Handle bewusst offen lassen: manche Plugins hinterlassen nach einem
        // fehlgeschlagenen Start Threads oder atexit-Handler.
        failed = true;
        return nullptr;
    }

    wrapEditor(effect, names, zoom);
    loaded_ = 1;
    logMessage("  ok, AEffect %p", static_cast<void *>(effect));
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

// Sprung nach `dest` (patch.cpp); der Grund eines Fehlschlags kommt ins Protokoll.
bool patchJump(uintptr_t target, void *dest)
{
    if (patchJumpRax(target, dest)) {
        return true;
    }
    logMessage("mprotect: %s", strerror(errno));
    return false;
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
// (auch die von resolve-linux-vst categories). Ersatz: wie das Original, bei leerer Tabelle aber
// die Maske unverändert.
const MapHeader *gMaskIn = nullptr, *gMaskOut = nullptr;

int translateMask(const MapNode *node, int mask)
{
    if (!node)
        return 0;
    return ((node->key & mask) ? node->value : 0) | translateMask(node->left, mask) | translateMask(node->right, mask);
}

int translateMask(const MapHeader *map, int mask)
{
    const MapNode *root = map ? map->root : nullptr;
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
    const char *exe = exePath();
    if (!*exe) {
        return;
    }

    struct {
        const char *name;
        void *replacement;
    } const patches[] = {
        {"_ZN15VSTPluginLoader4LoadEPKwPFlP7AEffectiilPvfERb", memberAddress(&VSTPluginLoader::Load)},
        {"_ZN15VSTPluginLoader5CloseEv", memberAddress(&VSTPluginLoader::Close)},
        {"_ZNK15AudioPluginHost23GetPluginCategoryMaskInEi", reinterpret_cast<void *>(&categoryMaskIn)},
        {"_ZNK15AudioPluginHost24GetPluginCategoryMaskOutEi", reinterpret_cast<void *>(&categoryMaskOut)},
    };
    // Symboltabelle (.symtab) des Executables; nur ohne PIE ist st_value die Laufzeitadresse.
    const ElfFile elf(exe);
    if (!elf.isExecutable())
        return;
    // Vor dem Umleiten: Tabellen der Kategorie-Masken (nur im VstScanner im Executable).
    gMaskIn = reinterpret_cast<const MapHeader *>(elf.symbol("_ZN15AudioPluginHost19_pluginCategoryMaskE"));
    gMaskOut = reinterpret_cast<const MapHeader *>(elf.symbol("_ZN15AudioPluginHost18_pluginCategoryMapE"));
    for (const auto &p : patches) {
        uintptr_t addr = elf.symbol(p.name);
        if (!addr)
            continue;
        if (reinterpret_cast<void *>(addr) == p.replacement)
            continue;
        if ((p.replacement == reinterpret_cast<void *>(&categoryMaskIn) && !gMaskIn)
            || (p.replacement == reinterpret_cast<void *>(&categoryMaskOut) && !gMaskOut))
            continue;
        bool ok = patchJump(addr, p.replacement);
        logMessage("%s: %s bei %#lx %s", exe, p.name, static_cast<unsigned long>(addr), ok ? "umgeleitet" : "NICHT umgeleitet");
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
