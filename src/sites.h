// Stellen in Resolve, die der Lader im Speicher anpasst, gesucht über Byte-Muster statt über
// feste Adressen: Nach einem Update von Resolve liegen sie woanders, der Code drumherum bleibt
// meist gleich. Dieselbe Suche läuft beim Start in Resolve (aac.cpp) und offline im Werkzeug
// resolve-check (tools/resolve-check.cpp, `resolve-linux-vst check`), das ohne Resolve zu starten
// meldet, ob alle Stellen noch passen.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// Ausführbarer Teil eines Programms: Bytes ab Adresse addr (Laufzeitadresse, ohne PIE).
struct CodeImage {
    const uint8_t *data = nullptr;
    uintptr_t addr = 0;
    size_t size = 0;
};

// Code von Resolve im eigenen Prozess (Executable ohne PIE).
bool runningImage(CodeImage *image);

// Code einer geladenen Bibliothek (PIE/.so), deren Pfad auf `suffix` endet (z. B. "/REDR3D-x64.so"):
// Laufzeitadresse = Ladebasis + p_vaddr des ausführbaren Segments. false, wenn nicht geladen.
bool loadedImage(const char *suffix, CodeImage *image);

// Eine ELF-Datei, gelesen über mmap.
class ElfFile {
public:
    explicit ElfFile(const char *path);
    ~ElfFile();
    ElfFile(const ElfFile &) = delete;
    ElfFile &operator=(const ElfFile &) = delete;

    bool ok() const { return base_ != nullptr; }
    bool isExecutable() const; // ET_EXEC: Adressen der Symbole sind Laufzeitadressen
    // Wert eines definierten Symbols (Funktion oder Variable); `dynamic`: .dynsym statt .symtab.
    uintptr_t symbol(const char *name, bool dynamic = false) const;
    // Importiert die Datei `name` in der Version `version` (z. B. LIBAVFORMAT_60)?
    bool imports(const char *name, const char *version) const;
    bool code(CodeImage *image) const; // erstes ausführbares PT_LOAD

private:
    const uint8_t *base_ = nullptr;
    size_t size_ = 0;
};

// AAC in MP4/MOV (aac.cpp): Weiche des QuickTime-Lesers, Öffnen des FFmpeg-Decoders und
// die Tabelle FourCC → AVCodecID.
struct AacSites {
    uintptr_t switchAt = 0;      // 14 Byte: cmp r12d, 'alac'; setne cl; or cl, al; jne verwerfen
    uintptr_t switchAlac = 0;    // Zweig für alac (direkt dahinter)
    uintptr_t switchFfmpeg = 0;  // Zweig für .mp3 und flac, nach dem Holen der Quelle (rbx)
    uintptr_t switchDiscard = 0; // Spur verwerfen
    uintptr_t codecAt = 0;       // 24 Byte: mov ecx, [r13+0x20]; ALAC? FLAC? sonst weiter
    uintptr_t codecExtradata = 0; // Zusatzdaten der Spur als extradata (ALAC, FLAC)
    uintptr_t codecOther = 0;    // ohne Zusatzdaten weiter
    uintptr_t codecMap = 0;      // std::map<uint32_t, int> (libc++), in .bss
};

// Sucht alle Stellen; bei Fehlschlag steht in `error`, welches Muster nicht passte.
bool findAacSites(const CodeImage &code, AacSites *sites, std::string *error);

// N-RAW (nraw.cpp): die Dekodierfunktion von fastticolib in REDR3D-x64.so.
struct NrawSites {
    uintptr_t decode = 0; // Anfang; die ersten kNrawPrologLen Byte sind frei von RIP-Bezug
};

constexpr size_t kNrawPrologLen = 17; // sub rsp,0x60; pxor xmm0,xmm0; mov rax,fs:0x28

// Sucht die Stelle; bei Fehlschlag steht in `error`, welches Muster nicht passte.
bool findNrawSites(const CodeImage &code, NrawSites *sites, std::string *error);

// Fundstellen einer Konstante (4 Byte, Little Endian) im Code, als Hilfe zum Wiederfinden von
// Hand (objdump -d --start-address=…). Höchstens `max` Adressen, Rückgabe die Anzahl.
size_t findConstant(const CodeImage &code, uint32_t value, uintptr_t *out, size_t max);

// Symbole im VstScanner (.symtab des Executables), die die Bibliothek umleitet oder braucht.
// Eine Liste für Lader, Scanner-Teil und resolve-check: Benennt Blackmagic eine um, ändert
// sich nur diese Stelle.
constexpr const char *kSymLoad = "_ZN15VSTPluginLoader4LoadEPKwPFlP7AEffectiilPvfERb";
constexpr const char *kSymClose = "_ZN15VSTPluginLoader5CloseEv";
constexpr const char *kSymMaskIn = "_ZNK15AudioPluginHost23GetPluginCategoryMaskInEi";
constexpr const char *kSymMaskOut = "_ZNK15AudioPluginHost24GetPluginCategoryMaskOutEi";
constexpr const char *kSymMaskTable = "_ZN15AudioPluginHost19_pluginCategoryMaskE"; // Tabelle zu MaskIn
constexpr const char *kSymMapTable = "_ZN15AudioPluginHost18_pluginCategoryMapE";   // Tabelle zu MaskOut

constexpr const char *kScannerSymbols[] = {kSymLoad, kSymClose, kSymMaskIn, kSymMaskOut, kSymMaskTable, kSymMapTable};
