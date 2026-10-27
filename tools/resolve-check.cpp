// resolve-check: prüft, ob die Stellen, an denen sich libresolve-linux-vst.so in Resolve einhängt, in
// der installierten Version noch da sind, ohne Resolve zu starten. Für die Zeit nach einem
// Update von Resolve (`resolve-linux-vst check`).
//
//   build/resolve-check [Resolve-Ordner]    (Standard /opt/resolve)
//
// Geprüft wird alles, was nicht über Standard-Symbole läuft: die Symbole des VST-Laders in
// libFairlightPage.so und im VstScanner und die AAC-Stellen im Executable resolve (dieselbe
// Mustersuche wie beim Start, src/sites.cpp). Was sich erst zur Laufzeit zeigt (Inhalt der
// Codec-Tabelle), steht beim Start im Log. Exit-Code 1, wenn etwas fehlt.
#include "../src/sites.h"

#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <string>
#include <unistd.h>

namespace {

int failures = 0;

void report(bool ok, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void report(bool ok, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("  %s  ", ok ? "ok     " : "MISSING");
    vprintf(fmt, ap);
    putchar('\n');
    va_end(ap);
    failures += !ok;
}

void checkSymbols(const std::string &path, bool dynamic, const char *const *names, size_t count)
{
    printf("%s\n", path.c_str());
    ElfFile elf(path.c_str());
    if (!elf.ok()) {
        report(false, "not readable");
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        uintptr_t addr = elf.symbol(names[i], dynamic);
        report(addr != 0, "%s %#lx", names[i], static_cast<unsigned long>(addr));
    }
}

// Wo die Konstanten der Stellen im Code vorkommen, zum Weitersuchen mit objdump.
void hint(const CodeImage &code, const char *what, uint32_t value)
{
    uintptr_t hits[12];
    size_t n = findConstant(code, value, hits, 12);
    printf("        %s (%#x) in code: %zu×", what, value, n);
    for (size_t i = 0; i < n && i < 12; ++i)
        printf(" %#lx", static_cast<unsigned long>(hits[i]));
    printf("%s\n", n > 12 ? " …" : "");
}

void checkResolve(const std::string &path)
{
    printf("%s\n", path.c_str());
    ElfFile elf(path.c_str());
    CodeImage code;
    if (!elf.ok() || !elf.isExecutable() || !elf.code(&code)) {
        report(false, "not a non-PIE executable");
        return;
    }
    timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    AacSites s;
    std::string error;
    const bool ok = findAacSites(code, &s, &error);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    const double ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    if (!ok) {
        report(false, "AAC: %s", error.c_str());
        printf("        anchors to search by hand (objdump -d -M intel --start-address=… %s):\n", path.c_str());
        hint(code, "'alac'", 0x616c6163);
        hint(code, "'flac'", 0x666c6163);
        hint(code, "AV_CODEC_ID_ALAC", 0x15010);
        hint(code, "AV_CODEC_ID_FLAC", 0x1500c);
        return;
    }
    using UL = unsigned long;
    report(true, "AAC track switch %#lx (alac %#lx, FFmpeg %#lx, discard %#lx)", UL(s.switchAt), UL(s.switchAlac),
           UL(s.switchFfmpeg), UL(s.switchDiscard));
    report(true, "AAC decoder open %#lx (extradata %#lx, other %#lx)", UL(s.codecAt), UL(s.codecExtradata),
           UL(s.codecOther));
    report(true, "AAC codec table %#lx (contents are checked at startup)", UL(s.codecMap));
    printf("        search: %.0f ms\n", ms);
}

} // namespace

int main(int argc, char **argv)
{
    if (argc > 2) {
        fprintf(stderr, "usage: %s [resolve-dir]\n", argv[0]);
        return 2;
    }
    const std::string dir = argc == 2 ? argv[1] : "/opt/resolve";

    // resolve ruft Load/Close über die PLT von libFairlightPage.so (Preload gewinnt).
    const char *loader[] = {"_ZN15VSTPluginLoader4LoadEPKwPFlP7AEffectiilPvfERb", "_ZN15VSTPluginLoader5CloseEv"};
    checkSymbols(dir + "/libs/libFairlightPage.so", true, loader, 2);

    // VstScanner: direkte Aufrufe, umgeleitet über die Symboltabelle (resolve_linux_vst.cpp).
    const char *scanner[] = {
        "_ZN15VSTPluginLoader4LoadEPKwPFlP7AEffectiilPvfERb",
        "_ZN15VSTPluginLoader5CloseEv",
        "_ZNK15AudioPluginHost23GetPluginCategoryMaskInEi",
        "_ZNK15AudioPluginHost24GetPluginCategoryMaskOutEi",
        "_ZN15AudioPluginHost19_pluginCategoryMaskE",
        "_ZN15AudioPluginHost18_pluginCategoryMapE",
    };
    checkSymbols(dir + "/bin/VstScanner", false, scanner, sizeof scanner / sizeof *scanner);

    // Der AAC-Decoder (make aac) ist FFmpeg 6.0 und muss zu Resolves libavcodec passen.
    printf("%s/libs\n", dir.c_str());
    report(access((dir + "/libs/libavcodec.so.60").c_str(), F_OK) == 0, "libavcodec.so.60 (FFmpeg 6.x)");

    // Ersetzte FFmpeg-Funktionen (aac.cpp): Resolve bzw. seine libavformat rufen sie über die
    // PLT, die Strukturen im Lader sind die von FFmpeg 6.x (Version 60).
    struct Import {
        const char *file, *name, *version, *purpose;
    } const imports[] = {
        {"/bin/resolve", "avcodec_find_decoder", "LIBAVCODEC_60", "AAC import"},
        {"/libs/libavformat.so.60", "avcodec_find_decoder", "LIBAVCODEC_60", "AAC import"},
        {"/libs/libavformat.so.60", "av_parser_init", "LIBAVCODEC_60", "AAC import"},
        {"/bin/resolve", "avformat_write_header", "LIBAVFORMAT_60", "AAC render"},
    };
    for (const auto &i : imports) {
        ElfFile elf((dir + i.file).c_str());
        report(elf.imports(i.name, i.version), "%s%s imports %s@%s (%s)", dir.c_str(), i.file, i.name, i.version,
               i.purpose);
    }

    checkResolve(dir + "/bin/resolve");

    if (failures)
        printf("\n%d hook(s) missing; see the README, \"After a Resolve update\".\n", failures);
    return failures ? 1 : 0;
}
