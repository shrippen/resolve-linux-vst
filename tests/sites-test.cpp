// Test der Mustersuche (src/sites.cpp) an selbst gebautem Code: dieselben Befehle wie in
// Resolve 21.1.1, an anderen Adressen und mit anderen Sprungweiten. Prüft, dass alle Ziele
// aus den Befehlen berechnet werden (nicht aus festen Abständen) und dass mehrdeutige oder
// veränderte Stellen abgelehnt werden. Läuft ohne Resolve (make test).
#include "../src/sites.h"

#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <vector>

namespace {

int failures = 0;

void expect(bool ok, const char *what)
{
    printf("%s  %s\n", ok ? "ok   " : "FEHLER", what);
    failures += !ok;
}

constexpr uintptr_t kBase = 0x400000;

struct Code {
    std::vector<uint8_t> bytes = std::vector<uint8_t>(0x4000, 0xcc);

    void put(uintptr_t addr, std::initializer_list<uint8_t> b) { memcpy(&bytes[addr - kBase], b.begin(), b.size()); }
    void rel32(uintptr_t at, uintptr_t target, uintptr_t next)
    {
        int32_t d = int32_t(target - next);
        memcpy(&bytes[at - kBase], &d, 4);
    }
    CodeImage image() const { return {bytes.data(), kBase, bytes.size()}; }
};

// Adressen im Testcode
constexpr uintptr_t kFlacCmp = 0x401000;   // cmp r12d, 'flac'; je kFlacBranch
constexpr uintptr_t kWindow = 0x401040;    // mov rbx, rax; test; sete; cmp r12d, 'alac' …
constexpr uintptr_t kSwitch = kWindow + 9; // ab dem cmp
constexpr uintptr_t kAlac = kSwitch + 14;
constexpr uintptr_t kFlacBranch = 0x401080; // test rax, rax; je verwerfen; mov rbx, rax
constexpr uintptr_t kFfmpeg = kFlacBranch + 8;
constexpr uintptr_t kDiscard = 0x4010a0;
constexpr uintptr_t kRootLoad = 0x402000; // mov r8, [rip+…]
constexpr uintptr_t kLea = 0x402020;      // lea r13, [rip+…]
constexpr uintptr_t kFindDecoder = 0x402100;
constexpr uintptr_t kCodec = 0x402180;
constexpr uintptr_t kCodecOther = 0x402300;
constexpr uintptr_t kMap = 0x29000000;

Code buildCode()
{
    Code c;
    c.put(kFlacCmp, {0x41, 0x81, 0xfc, 0x63, 0x61, 0x6c, 0x66, 0x74, uint8_t(kFlacBranch - (kFlacCmp + 9))});
    c.put(kWindow, {0x48, 0x89, 0xc3, 0x48, 0x85, 0xc0, 0x0f, 0x94, 0xc0, 0x41, 0x81, 0xfc, 0x63, 0x61, 0x6c, 0x61, 0x0f,
                    0x95, 0xc1, 0x08, 0xc1, 0x75, uint8_t(kDiscard - kAlac)});
    c.put(kAlac, {0xbf, 0xe0, 0x00, 0x00, 0x00});
    c.put(kFlacBranch, {0x48, 0x85, 0xc0, 0x74, uint8_t(kDiscard - (kFlacBranch + 5)), 0x48, 0x89, 0xc3});
    c.put(kFfmpeg, {0xbf, 0xe0, 0x00, 0x00, 0x00});
    c.put(kDiscard, {0xb0, 0x01});

    c.put(kRootLoad, {0x4c, 0x8b, 0x05});
    c.rel32(kRootLoad + 3, kMap + 8, kRootLoad + 7);
    c.put(kLea, {0x4c, 0x8d, 0x2d});
    c.rel32(kLea + 3, kMap + 8, kLea + 7);
    c.put(kFindDecoder, {0x41, 0x8b, 0x7d, 0x20, 0xe8, 0, 0, 0, 0});
    c.put(kCodec, {0x41, 0x8b, 0x4d, 0x20, 0x81, 0xf9, 0x10, 0x50, 0x01, 0x00, 0x74, 0x0c, 0x81, 0xf9, 0x0c, 0x50, 0x01,
                   0x00, 0x0f, 0x85});
    c.rel32(kCodec + 20, kCodecOther, kCodec + 24);
    return c;
}

} // namespace

int main()
{
    const Code code = buildCode();
    AacSites s;
    std::string error;
    const bool found = findAacSites(code.image(), &s, &error);
    expect(found, ("alle Stellen gefunden " + error).c_str());
    expect(s.switchAt == kSwitch, "Weiche");
    expect(s.switchAlac == kAlac, "Zweig alac");
    expect(s.switchFfmpeg == kFfmpeg, "Zweig FFmpeg (über den flac-Vergleich)");
    expect(s.switchDiscard == kDiscard, "Verwerfen (Sprungziel des jne)");
    expect(s.codecAt == kCodec, "Decoder");
    expect(s.codecExtradata == kCodec + 24, "extradata");
    expect(s.codecOther == kCodecOther, "sonst (Sprungziel des jne rel32)");
    expect(s.codecMap == kMap, "Tabelle (Endknoten − 8)");

    Code twice = code;
    twice.put(0x403000, {0x41, 0x8b, 0x4d, 0x20, 0x81, 0xf9, 0x10, 0x50, 0x01, 0x00, 0x74, 0x0c, 0x81, 0xf9, 0x0c, 0x50,
                         0x01, 0x00, 0x0f, 0x85, 0, 0, 0, 0});
    expect(!findAacSites(twice.image(), &s, &error), "doppelte Stelle abgelehnt");

    Code changed = code;
    changed.put(kAlac, {0xbf, 0xf0}); // anderer Decoder als im flac-Zweig
    expect(!findAacSites(changed.image(), &s, &error), "verschiedene Zweige abgelehnt");

    Code otherMap = code;
    otherMap.rel32(kRootLoad + 3, kMap + 0x100, kRootLoad + 7);
    expect(!findAacSites(otherMap.image(), &s, &error), "Wurzel und Endknoten verschieden abgelehnt");

    uintptr_t hits[4];
    expect(findConstant(code.image(), 0x616c6163, hits, 4) == 1 && hits[0] == kSwitch + 3, "Konstante 'alac'");

    return failures ? 1 : 0;
}
