// Eingriffe in Code und Datenstrukturen von Resolve, die kein Feature allein braucht: einen
// Sprung in Resolves Programmcode schreiben (Lader und AAC) und der Aufbau der Tabellen
// (std::map) in Resolves C++-Bibliothek. Die Byte-Muster zum Finden der Stellen stehen
// allein in sites.cpp.
#pragma once

#include <cstddef>
#include <cstdint>

constexpr size_t kJumpRaxLen = 12;      // movabs rax, dest; jmp rax
constexpr size_t kJumpIndirectLen = 14; // jmp [rip+0]; dq dest

// Baut `movabs rax, dest; jmp rax` (kJumpRaxLen Byte) nach out.
void buildJumpRax(void *dest, uint8_t *out);

// Baut `jmp [rip+0]; dq dest` (kJumpIndirectLen Byte) nach out, die restlichen Bytes bis
// len sind int3. Verändert kein Register. len muss mindestens kJumpIndirectLen sein.
void buildJumpIndirect(void *dest, uint8_t *out, size_t len);

// Schreibt `len` Bytes Code nach `addr`: Seiten beschreibbar machen, kopieren, wieder
// schützen, Befehlszwischenspeicher leeren. Bei false (mprotect fehlgeschlagen) steht der
// Fehler in errno und nichts ist geschrieben.
bool writeCode(uintptr_t addr, const uint8_t *code, size_t len);

// Überschreibt den Anfang von `target` mit buildJumpRax.
bool patchJumpRax(uintptr_t target, void *dest);

// Überschreibt `len` Bytes (14 bis 32) ab `addr` mit buildJumpIndirect.
bool patchJumpIndirect(uintptr_t addr, size_t len, void *dest);

// libc++ __tree_node<pair<const K, int>> und std::map: begin, end_node (nur left = Wurzel),
// size. K ist uint32_t (AAC) oder int (Kategorien des Laders); beide 4 Byte, gleiche Bits. Der Schlüssel liegt ab 0x1c, der Wert ab 0x20; stimmt das nicht mehr,
// bricht der Bau ab.
struct MapNode {
    MapNode *left, *right, *parent;
    bool black;
    uint32_t key;
    int value;
};
static_assert(offsetof(MapNode, key) == 0x1c && offsetof(MapNode, value) == 0x20);

struct MapHeader {
    MapNode *begin;
    MapNode *root;
    size_t size;
    MapNode *endNode() { return reinterpret_cast<MapNode *>(&root); }
};
