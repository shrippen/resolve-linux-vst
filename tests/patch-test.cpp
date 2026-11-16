// Test von src/patch.cpp ohne Resolve: die Bytes der beiden Sprünge für erfundene Ziele, das
// Schreiben in eine eigene, erst danach schreibgeschützte Seite und Auslesen, Suchen und
// Einfügen in eine selbst gebaute std::map-Tabelle. Läuft mit make test.
#include "../src/log.h"
#include "../src/patch.h"

#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <string>
#include <vector>

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

// Eine Tabelle wie in libc++: begin, Wurzel in end_node.left, size. Die Knoten legt mapInsert an.
struct Table {
    MapHeader map;
    Table() { map = MapHeader{nullptr, nullptr, 0}; map.begin = map.endNode(); }
    ~Table() { freeNodes(map.root); }
    Table(const Table &) = delete;
    Table &operator=(const Table &) = delete;

    static void freeNodes(MapNode *node)
    {
        if (!node) {
            return;
        }
        freeNodes(node->left);
        freeNodes(node->right);
        free(node);
    }
};

constexpr int kValueFactor = 3;

int valueOf(uint32_t key)
{
    return int(key) * kValueFactor + 1;
}

// Schwarzhöhe des Teilbaums, -1 bei einer verletzten Eigenschaft (zwei Rote in Folge,
// ungleiche Höhe, falscher parent, falsche Ordnung).
int blackHeight(const MapNode *node, const MapNode *parent, bool parentRed)
{
    if (!node) {
        return 1;
    }
    if (node->parent != parent || (parentRed && !node->black)) {
        return -1;
    }
    if ((node->left && node->left->key >= node->key) || (node->right && node->right->key < node->key)) {
        return -1;
    }
    int l = blackHeight(node->left, node, !node->black);
    int r = blackHeight(node->right, node, !node->black);
    if (l < 0 || r < 0 || l != r) {
        return -1;
    }
    return l + (node->black ? 1 : 0);
}

struct Visit {
    std::vector<uint32_t> keys;
    std::vector<int> values;
};

void collectVisit(uint32_t key, int value, void *ctx)
{
    auto *v = static_cast<Visit *>(ctx);
    v->keys.push_back(key);
    v->values.push_back(value);
}

// Fügt `keys` in dieser Reihenfolge ein und prüft danach alles, was libc++ von der Tabelle
// erwartet; `what` nennt die Reihenfolge.
void checkInsert(const std::vector<uint32_t> &keys, const char *what)
{
    Table t;
    for (uint32_t k : keys) {
        mapInsert(&t.map, k, valueOf(k));
    }
    std::string name(what);
    std::vector<uint32_t> sorted = keys;
    std::sort(sorted.begin(), sorted.end());

    Visit v;
    mapEach(&t.map, collectVisit, &v);
    bool values = true;
    for (size_t i = 0; i < v.keys.size(); ++i) {
        values = values && v.values[i] == valueOf(v.keys[i]);
    }
    expect(v.keys == sorted && values, (name + ": Inorder sortiert, Werte stimmen").c_str());
    expect(t.map.size == keys.size(), (name + ": size").c_str());
    expect(t.map.begin == (sorted.empty() ? t.map.endNode() : mapFind(&t.map, sorted.front())),
           (name + ": begin ist der kleinste Knoten").c_str());
    expect(!t.map.root || (t.map.root->black && t.map.root->parent == t.map.endNode()),
           (name + ": Wurzel schwarz, parent ist end_node").c_str());
    expect(blackHeight(t.map.root, t.map.endNode(), false) >= 0,
           (name + ": keine zwei Rote in Folge, gleiche Schwarzhöhe, parent und Ordnung").c_str());

    bool found = true;
    for (uint32_t k : keys) {
        const MapNode *n = mapFind(&t.map, k);
        found = found && n && n->key == k && n->value == valueOf(k);
    }
    expect(found, (name + ": Suchen findet jeden Schlüssel").c_str());
    expect(!mapFind(&t.map, sorted.empty() ? 0 : sorted.back() + 1), (name + ": Suchen findet fehlenden nicht").c_str());
}

void testMapInsert()
{
    constexpr uint32_t kCount = 200;
    std::vector<uint32_t> asc, desc, zigzag, mixed;
    for (uint32_t i = 0; i < kCount; ++i) {
        asc.push_back(i * 2);
        desc.push_back((kCount - i) * 2);
        zigzag.push_back(i % 2 ? kCount - i / 2 : i / 2);
    }
    // Feste Mischung: i * 37 mod 211 durchläuft für Primzahl 211 alle Reste ohne Wiederholung.
    for (uint32_t i = 0; i < kCount; ++i) {
        mixed.push_back(i * 37 % 211);
    }

    checkInsert({}, "leer");
    checkInsert({5}, "ein Eintrag");
    checkInsert({10, 20, 30}, "drei aufsteigend");
    checkInsert({30, 20, 10}, "drei absteigend");
    checkInsert({10, 30, 20}, "drei: Zickzack rechts-links");
    checkInsert({30, 10, 20}, "drei: Zickzack links-rechts");
    checkInsert({0x2e6d7033, 0x61632d33, 0x616c6163, 0x666c6163, 0x61616320, 0x6d703461},
                "sechs Codec-Schlüssel wie in Resolve");
    checkInsert(asc, "200 aufsteigend");
    checkInsert(desc, "200 absteigend");
    checkInsert(zigzag, "200 Zickzack");
    checkInsert(mixed, "200 gemischt");
}

void testMapCollect()
{
    Table t;
    for (uint32_t k : {40u, 10u, 30u, 20u}) {
        mapInsert(&t.map, k, valueOf(k));
    }
    uint32_t keys[8];
    int values[8];
    expect(mapCollect(&t.map, keys, values, 8) == 4, "mapCollect: Anzahl");
    expect(keys[0] == 10 && keys[1] == 20 && keys[2] == 30 && keys[3] == 40 && values[0] == valueOf(10)
               && values[3] == valueOf(40),
           "mapCollect: Schlüssel und Werte in Reihenfolge");
    expect(mapCollect(&t.map, keys, values, 4) == 4, "mapCollect: genau max Einträge passen");
    expect(mapCollect(&t.map, keys, values, 3) < 0, "mapCollect: mehr als max liefert -1");

    Table empty;
    expect(mapCollect(&empty.map, keys, values, 8) == 0, "mapCollect: leere Tabelle");
}

// Was `run` ins Protokoll schreibt (Speicher statt Datei).
template <typename F> std::string logOf(F run)
{
    char *buf = nullptr;
    size_t size = 0;
    FILE *f = open_memstream(&buf, &size);
    setLogFile(f);
    run();
    setLogFile(nullptr);
    fclose(f);
    std::string text(buf ? buf : "", size);
    free(buf);
    return text;
}

// Beide Sprungarten melden einen Fehlschlag gleich: eine Zeile "mprotect: <Grund>".
void testJumpErrors()
{
    const uint8_t code[kJumpRaxLen] = {};
    errno = 0;
    writeCode(0x10, code, sizeof code);
    const std::string want = std::string("mprotect: ") + strerror(errno) + "\n";

    bool raxOk = true, indirectOk = true;
    std::string rax = logOf([&] { raxOk = patchJumpRax(0x10, dest()); });
    std::string indirect = logOf([&] { indirectOk = patchJumpIndirect(0x10, kPadded, dest()); });
    expect(!raxOk && rax.size() >= want.size() && rax.compare(rax.size() - want.size(), want.size(), want) == 0,
           "patchJumpRax: Fehlschlag meldet mprotect und Grund");
    expect(!indirectOk && indirect.size() >= want.size()
               && indirect.compare(indirect.size() - want.size(), want.size(), want) == 0,
           "patchJumpIndirect: Fehlschlag meldet mprotect und Grund");

    const size_t pageSize = size_t(sysconf(_SC_PAGESIZE));
    void *mem = mmap(nullptr, pageSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) {
        expect(false, "Testseite angelegt");
        return;
    }
    const uintptr_t at = uintptr_t(mem);
    std::string tooLong = logOf([&] { indirectOk = patchJumpIndirect(at, 33, dest()); });
    expect(!indirectOk && tooLong.find("Sprunglänge 33") != std::string::npos, "patchJumpIndirect: ungültige Länge wird gemeldet");
    std::string okLog = logOf([&] { raxOk = patchJumpRax(at, dest()) && patchJumpIndirect(at, kPadded, dest()); });
    expect(raxOk && okLog.empty(), "erfolgreiche Sprünge schreiben nichts ins Protokoll");
    munmap(mem, pageSize);
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
    testJumpErrors();
    testMapInsert();
    testMapCollect();
    printf("%s\n", failures ? "FEHLGESCHLAGEN" : "alle Tests bestanden");
    return failures ? 1 : 0;
}
