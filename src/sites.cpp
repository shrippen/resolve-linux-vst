// Suche der Stellen in Resolve über Byte-Muster (siehe sites.h).
//
// Ein Muster ist eine Folge von Bytes in Hex, `??` für beliebige (Sprungweiten, Adressen).
// Gesucht wird mit memmem über das längste feste Stück, der Rest wird an jeder Fundstelle
// verglichen; über die ganzen 160 MB Code von Resolve dauert das einige zehn Millisekunden.
// Ein Muster muss genau einmal vorkommen, sonst gilt die Stelle als nicht gefunden.

#include "sites.h"

#include <elf.h>
#include <fcntl.h>
#include <link.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

struct Pattern {
    uint8_t bytes[48];
    bool fixed[48];
    size_t size = 0;
    size_t runStart = 0, runSize = 0; // längstes festes Stück, für memmem

    explicit Pattern(const char *text)
    {
        for (const char *p = text; *p;) {
            if (*p == ' ') {
                ++p;
                continue;
            }
            if (size == sizeof bytes)
                abort();
            fixed[size] = p[0] != '?';
            bytes[size] = fixed[size] ? uint8_t(strtoul(std::string(p, 2).c_str(), nullptr, 16)) : 0;
            ++size;
            p += 2;
        }
        for (size_t i = 0; i < size;) {
            size_t j = i;
            while (j < size && fixed[j])
                ++j;
            if (j - i > runSize) {
                runStart = i;
                runSize = j - i;
            }
            i = j + 1;
        }
    }

    bool matches(const uint8_t *p) const
    {
        for (size_t i = 0; i < size; ++i)
            if (fixed[i] && p[i] != bytes[i])
                return false;
        return true;
    }
};

// Fundstellen von `pattern` mit Anfang in [from, to), höchstens `max`. Zählt weiter, damit der
// Aufrufer Mehrdeutigkeit erkennt.
size_t findAll(const CodeImage &code, const Pattern &pattern, uintptr_t from, uintptr_t to, uintptr_t *out,
               size_t max)
{
    const uintptr_t end = code.addr + code.size;
    from = from < code.addr ? code.addr : from;
    to = to > end - pattern.size ? end - pattern.size + 1 : to;
    if (from >= to)
        return 0;
    size_t count = 0;
    const uint8_t *p = code.data + (from - code.addr) + pattern.runStart;
    const uint8_t *last = code.data + (to - code.addr) + pattern.runStart; // exklusiv
    while (p < last) {
        auto *hit = static_cast<const uint8_t *>(
            memmem(p, size_t(last - p) + pattern.runSize - 1, pattern.bytes + pattern.runStart, pattern.runSize));
        if (!hit)
            break;
        const uint8_t *start = hit - pattern.runStart;
        if (pattern.matches(start)) {
            if (count < max)
                out[count] = code.addr + uintptr_t(start - code.data);
            ++count;
        }
        p = hit + 1;
    }
    return count;
}

const uint8_t *at(const CodeImage &code, uintptr_t addr)
{
    return code.data + (addr - code.addr);
}

int32_t rel32(const CodeImage &code, uintptr_t addr)
{
    int32_t v;
    memcpy(&v, at(code, addr), sizeof v);
    return v;
}

int8_t rel8(const CodeImage &code, uintptr_t addr)
{
    return int8_t(*at(code, addr));
}

bool fail(std::string *error, const char *what)
{
    if (error)
        *error = what;
    return false;
}

// Genau eine Fundstelle in [from, to).
bool findOne(const CodeImage &code, const char *text, uintptr_t from, uintptr_t to, uintptr_t *out)
{
    uintptr_t hit;
    if (findAll(code, Pattern(text), from, to, &hit, 1) != 1)
        return false;
    *out = hit;
    return true;
}

// Letzte Fundstelle in [from, to).
bool findLast(const CodeImage &code, const char *text, uintptr_t from, uintptr_t to, uintptr_t *out)
{
    uintptr_t hits[16];
    size_t n = findAll(code, Pattern(text), from, to, hits, 16);
    if (n == 0 || n > 16)
        return false;
    *out = hits[n - 1];
    return true;
}

} // namespace

bool runningImage(CodeImage *image)
{
    *image = {};
    // Das erste Objekt ist das Programm selbst.
    dl_iterate_phdr(
        [](dl_phdr_info *info, size_t, void *data) -> int {
            auto *image = static_cast<CodeImage *>(data);
            for (int i = 0; i < info->dlpi_phnum; ++i) {
                const ElfW(Phdr) &ph = info->dlpi_phdr[i];
                if (ph.p_type == PT_LOAD && (ph.p_flags & PF_X)) {
                    image->addr = info->dlpi_addr + ph.p_vaddr;
                    image->data = reinterpret_cast<const uint8_t *>(image->addr);
                    image->size = ph.p_filesz;
                    break;
                }
            }
            return 1;
        },
        image);
    return image->data != nullptr;
}

ElfFile::ElfFile(const char *path)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return;
    struct stat st;
    if (fstat(fd, &st) == 0 && size_t(st.st_size) >= sizeof(Elf64_Ehdr)) {
        void *map = mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (map != MAP_FAILED) {
            auto *eh = static_cast<const Elf64_Ehdr *>(map);
            if (memcmp(eh->e_ident, ELFMAG, SELFMAG) == 0 && eh->e_ident[EI_CLASS] == ELFCLASS64) {
                base_ = static_cast<const uint8_t *>(map);
                size_ = st.st_size;
            } else {
                munmap(map, st.st_size);
            }
        }
    }
    close(fd);
}

ElfFile::~ElfFile()
{
    if (base_)
        munmap(const_cast<uint8_t *>(base_), size_);
}

bool ElfFile::isExecutable() const
{
    return base_ && reinterpret_cast<const Elf64_Ehdr *>(base_)->e_type == ET_EXEC;
}

uintptr_t ElfFile::symbol(const char *name, bool dynamic) const
{
    if (!base_)
        return 0;
    auto *eh = reinterpret_cast<const Elf64_Ehdr *>(base_);
    if (eh->e_shoff + eh->e_shnum * sizeof(Elf64_Shdr) > size_)
        return 0;
    auto *sh = reinterpret_cast<const Elf64_Shdr *>(base_ + eh->e_shoff);
    const uint32_t type = dynamic ? SHT_DYNSYM : SHT_SYMTAB;
    for (int i = 0; i < eh->e_shnum; ++i) {
        if (sh[i].sh_type != type || sh[i].sh_link >= eh->e_shnum)
            continue;
        auto *syms = reinterpret_cast<const Elf64_Sym *>(base_ + sh[i].sh_offset);
        auto *strs = reinterpret_cast<const char *>(base_ + sh[sh[i].sh_link].sh_offset);
        const size_t count = sh[i].sh_size / sizeof(Elf64_Sym);
        for (size_t j = 0; j < count; ++j) {
            const int t = ELF64_ST_TYPE(syms[j].st_info);
            if ((t == STT_FUNC || t == STT_OBJECT) && syms[j].st_value && strcmp(strs + syms[j].st_name, name) == 0)
                return syms[j].st_value;
        }
    }
    return 0;
}

bool ElfFile::imports(const char *name, const char *version) const
{
    if (!base_)
        return false;
    auto *eh = reinterpret_cast<const Elf64_Ehdr *>(base_);
    if (eh->e_shoff + eh->e_shnum * sizeof(Elf64_Shdr) > size_)
        return false;
    auto *sh = reinterpret_cast<const Elf64_Shdr *>(base_ + eh->e_shoff);
    const Elf64_Shdr *dynsym = nullptr, *versym = nullptr, *verneed = nullptr;
    for (int i = 0; i < eh->e_shnum; ++i) {
        if (sh[i].sh_type == SHT_DYNSYM)
            dynsym = &sh[i];
        else if (sh[i].sh_type == SHT_GNU_versym)
            versym = &sh[i];
        else if (sh[i].sh_type == SHT_GNU_verneed)
            verneed = &sh[i];
    }
    if (!dynsym || !versym || !verneed || dynsym->sh_link >= eh->e_shnum)
        return false;
    auto *syms = reinterpret_cast<const Elf64_Sym *>(base_ + dynsym->sh_offset);
    auto *strs = reinterpret_cast<const char *>(base_ + sh[dynsym->sh_link].sh_offset);
    auto *versions = reinterpret_cast<const Elf64_Half *>(base_ + versym->sh_offset);
    const size_t count = dynsym->sh_size / sizeof(Elf64_Sym);
    for (size_t j = 0; j < count; ++j) {
        if (syms[j].st_shndx != SHN_UNDEF || strcmp(strs + syms[j].st_name, name) != 0)
            continue;
        // Versionsindex → Name über .gnu.version_r (Verneed mit Vernaux-Einträgen).
        const Elf64_Half index = versions[j] & 0x7fff;
        auto *vn = base_ + verneed->sh_offset;
        for (unsigned k = 0; k < verneed->sh_info; ++k) {
            auto *need = reinterpret_cast<const Elf64_Verneed *>(vn);
            auto *aux = vn + need->vn_aux;
            for (unsigned a = 0; a < need->vn_cnt; ++a) {
                auto *vna = reinterpret_cast<const Elf64_Vernaux *>(aux);
                if (vna->vna_other == index)
                    return strcmp(strs + vna->vna_name, version) == 0;
                aux += vna->vna_next;
            }
            vn += need->vn_next;
        }
        return false;
    }
    return false;
}

bool ElfFile::code(CodeImage *image) const
{
    *image = {};
    if (!base_)
        return false;
    auto *eh = reinterpret_cast<const Elf64_Ehdr *>(base_);
    auto *ph = reinterpret_cast<const Elf64_Phdr *>(base_ + eh->e_phoff);
    for (int i = 0; i < eh->e_phnum; ++i)
        if (ph[i].p_type == PT_LOAD && (ph[i].p_flags & PF_X) && ph[i].p_offset + ph[i].p_filesz <= size_) {
            image->data = base_ + ph[i].p_offset;
            image->addr = ph[i].p_vaddr;
            image->size = ph[i].p_filesz;
            return true;
        }
    return false;
}

// Die Muster stammen aus Resolve 21.1.1 (Adressen dort in Klammern). Registerwahl und
// Offsets gehören dazu: Die Ersatzstücke in aac.cpp (resolveVstSwitch, resolveVstCodec)
// verlassen sich auf r12d = FourCC, rbx = Quelle, r13 = Tabelleneintrag, rax = AVCodecContext.
bool findAacSites(const CodeImage &code, AacSites *s, std::string *error)
{
    *s = {};

    // Weiche (0x5deaec6): mov rbx, rax; test rax, rax; sete al; cmp r12d, 'alac';
    // setne cl; or cl, al; jne verwerfen. Ersetzt wird ab dem cmp.
    uintptr_t hit;
    if (!findOne(code, "48 89 c3 48 85 c0 0f 94 c0 41 81 fc 63 61 6c 61 0f 95 c1 08 c1 75 ??", code.addr,
                 code.addr + code.size, &hit))
        return fail(error, "track switch: cmp r12d, 'alac' not found or not unique");
    s->switchAt = hit + 9;
    s->switchAlac = s->switchAt + 14;
    s->switchDiscard = s->switchAlac + rel8(code, s->switchAt + 13);

    // Zweig für flac (0x5deae88): cmp r12d, 'flac'; je F, kurz vor der Weiche. Bei F holt er
    // die Quelle: …; test rax, rax; je verwerfen; mov rbx, rax; dahinter der Decoder.
    uintptr_t flac, flacTarget;
    if (findLast(code, "41 81 fc 63 61 6c 66 74 ??", s->switchAt - 0x100, s->switchAt, &flac))
        flacTarget = flac + 9 + rel8(code, flac + 8);
    else if (findLast(code, "41 81 fc 63 61 6c 66 0f 84 ?? ?? ?? ??", s->switchAt - 0x100, s->switchAt, &flac))
        flacTarget = flac + 13 + rel32(code, flac + 9);
    else
        return fail(error, "track switch: cmp r12d, 'flac' before it not found");
    uintptr_t source;
    if (!findOne(code, "48 85 c0 74 ?? 48 89 c3", flacTarget, flacTarget + 0x40, &source)
        || source + 5 + rel8(code, source + 4) != s->switchDiscard)
        return fail(error, "track switch: flac branch fetches the source differently");
    s->switchFfmpeg = source + 8;
    // Beide Zweige beginnen mit mov edi, Größe des Decoders (vor operator new).
    if (*at(code, s->switchAlac) != 0xbf || memcmp(at(code, s->switchAlac), at(code, s->switchFfmpeg), 5) != 0)
        return fail(error, "track switch: alac and flac branches differ");

    // Decoder öffnen (0x5ddfdeb): mov ecx, [r13+0x20]; cmp ecx, ALAC; je extradata;
    // cmp ecx, FLAC; jne weiter; extradata: …
    if (!findOne(code, "41 8b 4d 20 81 f9 10 50 01 00 74 0c 81 f9 0c 50 01 00 0f 85 ?? ?? ?? ??", code.addr,
                 code.addr + code.size, &hit))
        return fail(error, "decoder open: ALAC/FLAC comparison not found or not unique");
    s->codecAt = hit;
    s->codecExtradata = hit + 24;
    s->codecOther = hit + 24 + rel32(code, hit + 20);

    // Tabelle: Davor sucht dieselbe Funktion den Codec in der std::map (0x5ddfc19):
    // mov r8, [rip+Wurzel]; … lea r13, [rip+Endknoten]; … Der Endknoten (nur left = Wurzel)
    // liegt in libc++ 8 Byte hinter dem Anfang der map. Bis zum Vergleich bleibt r13 der
    // gefundene Eintrag: mov edi, [r13+0x20]; call avcodec_find_decoder.
    uintptr_t lea, root;
    if (!findLast(code, "4c 8d 2d ?? ?? ?? ??", s->codecAt - 0x400, s->codecAt, &lea))
        return fail(error, "codec table: lea r13, [rip+…] before the decoder not found");
    if (!findLast(code, "4c 8b 05 ?? ?? ?? ??", lea - 0x40, lea, &root))
        return fail(error, "codec table: mov r8, [rip+…] (map root) not found");
    const uintptr_t endNode = lea + 7 + rel32(code, lea + 3);
    if (root + 7 + rel32(code, root + 3) != endNode)
        return fail(error, "codec table: root and end node do not match");
    if (!findOne(code, "41 8b 7d 20 e8", lea, s->codecAt, &hit))
        return fail(error, "codec table: mov edi, [r13+0x20] before avcodec_find_decoder not found");
    s->codecMap = endNode - 8;
    return true;
}

size_t findConstant(const CodeImage &code, uint32_t value, uintptr_t *out, size_t max)
{
    char text[16];
    snprintf(text, sizeof text, "%02x %02x %02x %02x", value & 0xff, (value >> 8) & 0xff, (value >> 16) & 0xff,
             value >> 24);
    return findAll(code, Pattern(text), code.addr, code.addr + code.size, out, max);
}
