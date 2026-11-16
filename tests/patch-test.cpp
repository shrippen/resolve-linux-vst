// Test von src/patch.cpp ohne Resolve: die Bytes der beiden Sprünge für erfundene Ziele und
// das Schreiben in eine eigene, erst danach schreibgeschützte Seite. Läuft mit make test.
#include "../src/patch.h"

#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

int failures = 0;

void expect(bool ok, const char *what)
{
    printf("%s  %s\n", ok ? "ok   " : "FEHLER", what);
    failures += !ok;
}

constexpr uintptr_t kDest = 0x1122334455667788;
constexpr size_t kOffset = 0x10;
constexpr size_t kPadded = 24;

void *dest()
{
    return reinterpret_cast<void *>(kDest);
}

void testRaxBytes()
{
    uint8_t code[kJumpRaxLen];
    buildJumpRax(dest(), code);
    const uint8_t want[kJumpRaxLen] = {0x48, 0xB8, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0xFF, 0xE0};
    expect(memcmp(code, want, sizeof want) == 0, "movabs rax, dest; jmp rax");
}

void testIndirectBytes()
{
    uint8_t code[kPadded] = {};
    buildJumpIndirect(dest(), code, sizeof code);
    const uint8_t want[kPadded] = {0xff, 0x25, 0, 0, 0, 0, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11,
                                   0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc};
    expect(memcmp(code, want, sizeof want) == 0, "jmp [rip+0]; dq dest; Rest int3 (24 Byte)");

    uint8_t exact[kJumpIndirectLen];
    buildJumpIndirect(dest(), exact, sizeof exact);
    expect(memcmp(exact, want, sizeof exact) == 0, "jmp [rip+0]; dq dest (14 Byte)");
}

// Eigene Seite, nur lesbar und ausführbar wie Code; Nachbarbytes dürfen sich nicht ändern.
void testWrite()
{
    const size_t pageSize = size_t(sysconf(_SC_PAGESIZE));
    void *mem = mmap(nullptr, pageSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    expect(mem != MAP_FAILED, "Testseite angelegt");
    if (mem == MAP_FAILED) {
        return;
    }
    memset(mem, 0x90, pageSize);
    mprotect(mem, pageSize, PROT_READ | PROT_EXEC);

    uint8_t *base = static_cast<uint8_t *>(mem);
    expect(patchJumpRax(uintptr_t(base) + kOffset, dest()), "patchJumpRax schreibt");
    uint8_t want[kJumpRaxLen];
    buildJumpRax(dest(), want);
    expect(memcmp(base + kOffset, want, sizeof want) == 0, "patchJumpRax: Bytes stehen im Speicher");
    expect(base[kOffset - 1] == 0x90 && base[kOffset + kJumpRaxLen] == 0x90, "patchJumpRax: Nachbarbytes unverändert");

    expect(patchJumpIndirect(uintptr_t(base) + kOffset, kPadded, dest()), "patchJumpIndirect schreibt");
    uint8_t wantInd[kPadded];
    buildJumpIndirect(dest(), wantInd, sizeof wantInd);
    expect(memcmp(base + kOffset, wantInd, sizeof wantInd) == 0, "patchJumpIndirect: Bytes stehen im Speicher");
    expect(base[kOffset + kPadded] == 0x90, "patchJumpIndirect: Nachbarbyte unverändert");

    expect(!patchJumpIndirect(uintptr_t(base) + kOffset, kJumpIndirectLen - 1, dest()), "patchJumpIndirect: zu kurz abgelehnt");
    expect(!patchJumpIndirect(uintptr_t(base) + kOffset, 33, dest()), "patchJumpIndirect: zu lang abgelehnt");
    munmap(mem, pageSize);
}

// Rechte der Abbildung, die `addr` enthält, laut /proc/self/maps (z. B. "r-xp").
std::string protOf(const void *addr)
{
    FILE *f = fopen("/proc/self/maps", "r");
    char line[512];
    std::string prot;
    while (f && prot.empty() && fgets(line, sizeof line, f)) {
        unsigned long from, to;
        char perms[5];
        if (sscanf(line, "%lx-%lx %4s", &from, &to, perms) == 3 && uintptr_t(addr) >= from && uintptr_t(addr) < to) {
            prot = perms;
        }
    }
    if (f) {
        fclose(f);
    }
    return prot;
}

// Sprung über die Seitengrenze: beide Seiten müssen beschreibbar werden und danach wieder nur
// lesbar und ausführbar sein.
void testWriteAcrossPages()
{
    const size_t pageSize = size_t(sysconf(_SC_PAGESIZE));
    void *mem = mmap(nullptr, 2 * pageSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    expect(mem != MAP_FAILED, "zwei Testseiten angelegt");
    if (mem == MAP_FAILED) {
        return;
    }
    memset(mem, 0x90, 2 * pageSize);
    mprotect(mem, 2 * pageSize, PROT_READ | PROT_EXEC);

    uint8_t *base = static_cast<uint8_t *>(mem);
    uint8_t *at = base + pageSize - 5;
    expect(patchJumpRax(uintptr_t(at), dest()), "patchJumpRax über die Seitengrenze schreibt");
    uint8_t want[kJumpRaxLen];
    buildJumpRax(dest(), want);
    expect(memcmp(at, want, sizeof want) == 0, "über die Seitengrenze: Bytes stehen im Speicher");
    expect(protOf(base) == "r-xp" && protOf(base + pageSize) == "r-xp", "danach beide Seiten wieder r-x");
    munmap(mem, 2 * pageSize);
}

void testWriteFails()
{
    const uint8_t code[kJumpRaxLen] = {};
    errno = 0;
    expect(!writeCode(0x10, code, sizeof code) && errno != 0, "writeCode: ungültige Adresse liefert false mit errno");
}

} // namespace

// Wann leitet redirectFunctions eine Funktion um? Nur mit gefundenem Symbol, einem Ersatz,
// und wenn nicht schon der Ersatz an der Stelle liegt.
void testRedirectWanted()
{
    int replacement = 0;
    const uintptr_t other = 0x400000;
    expect(redirectWanted(other, &replacement), "Symbol gefunden, Ersatz da: umleiten");
    expect(!redirectWanted(0, &replacement), "Symbol fehlt: nicht umleiten");
    expect(!redirectWanted(other, nullptr), "kein Ersatz: nicht umleiten");
    expect(!redirectWanted(uintptr_t(&replacement), &replacement), "Ersatz liegt schon dort: nicht umleiten");
}

int main()
{
    testRedirectWanted();
    testRaxBytes();
    testIndirectBytes();
    testWrite();
    testWriteAcrossPages();
    testWriteFails();
    printf("%s\n", failures ? "FEHLGESCHLAGEN" : "alle Tests bestanden");
    return failures ? 1 : 0;
}
