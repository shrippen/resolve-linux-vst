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
#include <unistd.h>

#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

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

void openLog()
{
    std::string path;
    if (const char *env = getenv("RESOLVE_VST_LOG"))
        path = env;
    else if (const char *home = getenv("HOME")) {
        path = std::string(home) + "/.local/state/resolve-vst";
        mkdir((std::string(home) + "/.local/state").c_str(), 0755);
        mkdir(path.c_str(), 0755);
        path += "/resolve-vst.log";
    }
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

struct AEffect {
    int32_t magic;
    // Rest der Struktur interessiert hier nicht.
};

using audioMasterCallback = long (*)(AEffect *, int, int, long, void *, float);
using VstEntry = AEffect *(*)(audioMasterCallback);

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

    AEffect *effect = entry(callback);
    if (!effect || effect->magic != kEffectMagic) {
        logf("  Einstiegspunkt lieferte %p, kein gültiger AEffect", static_cast<void *>(effect));
        // Handle bewusst offen lassen: manche Plugins hinterlassen nach einem
        // fehlgeschlagenen Start Threads oder atexit-Handler.
        failed = true;
        return nullptr;
    }

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
