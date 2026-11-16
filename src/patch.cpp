#include "patch.h"

#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>

#include "log.h"
#include "sites.h"

namespace {

constexpr uint8_t kInt3 = 0xcc;
constexpr size_t kMaxJumpLen = 32;

// Drehungen und Balancierung wie libc++ __tree_balance_after_insert (Rot-Schwarz-Baum).
void rotateLeft(MapNode *x)
{
    MapNode *y = x->right;
    x->right = y->left;
    if (x->right) {
        x->right->parent = x;
    }
    y->parent = x->parent;
    if (x == x->parent->left) {
        x->parent->left = y;
    } else {
        x->parent->right = y;
    }
    y->left = x;
    x->parent = y;
}

void rotateRight(MapNode *x)
{
    MapNode *y = x->left;
    x->left = y->right;
    if (x->left) {
        x->left->parent = x;
    }
    y->parent = x->parent;
    if (x == x->parent->left) {
        x->parent->left = y;
    } else {
        x->parent->right = y;
    }
    y->right = x;
    x->parent = y;
}

// Wie libc++ __tree_balance_after_insert.
void balanceAfterInsert(MapNode *root, MapNode *x)
{
    x->black = x == root;
    while (x != root && !x->parent->black) {
        MapNode *grand = x->parent->parent;
        if (x->parent == grand->left) {
            MapNode *y = grand->right;
            if (y && !y->black) {
                x = x->parent;
                x->black = true;
                x = x->parent;
                x->black = x == root;
                y->black = true;
            } else {
                if (x != x->parent->left) {
                    x = x->parent;
                    rotateLeft(x);
                }
                x = x->parent;
                x->black = true;
                x = x->parent;
                x->black = false;
                rotateRight(x);
                break;
            }
        } else {
            MapNode *y = grand->left;
            if (y && !y->black) {
                x = x->parent;
                x->black = true;
                x = x->parent;
                x->black = x == root;
                y->black = true;
            } else {
                if (x == x->parent->left) {
                    x = x->parent;
                    rotateRight(x);
                }
                x = x->parent;
                x->black = true;
                x = x->parent;
                x->black = false;
                rotateLeft(x);
                break;
            }
        }
    }
}

struct Collect {
    uint32_t *keys;
    int *values;
    int max;
    int n;
};

void collectOne(uint32_t key, int value, void *ctx)
{
    auto *c = static_cast<Collect *>(ctx);
    if (c->n < c->max) {
        c->keys[c->n] = key;
        c->values[c->n] = value;
    }
    ++c->n;
}

// Inorder: aufsteigende Schlüssel.
void walk(const MapNode *node, MapVisit visit, void *ctx)
{
    if (!node) {
        return;
    }
    walk(node->left, visit, ctx);
    visit(node->key, node->value, ctx);
    walk(node->right, visit, ctx);
}

} // namespace

void buildJumpRax(void *dest, uint8_t *out)
{
    const uint8_t code[kJumpRaxLen] = {0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xE0};
    memcpy(out, code, sizeof code);
    uintptr_t d = reinterpret_cast<uintptr_t>(dest);
    memcpy(out + 2, &d, sizeof d);
}

void buildJumpIndirect(void *dest, uint8_t *out, size_t len)
{
    memset(out, kInt3, len);
    const uint8_t jump[6] = {0xff, 0x25, 0, 0, 0, 0};
    memcpy(out, jump, sizeof jump);
    uintptr_t d = reinterpret_cast<uintptr_t>(dest);
    memcpy(out + 6, &d, sizeof d);
}

bool writeCode(uintptr_t addr, const uint8_t *code, size_t len)
{
    long pageSize = sysconf(_SC_PAGESIZE);
    uintptr_t page = addr & ~uintptr_t(pageSize - 1);
    size_t span = addr + len - page;
    if (mprotect(reinterpret_cast<void *>(page), span, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        return false;
    }
    memcpy(reinterpret_cast<void *>(addr), code, len);
    mprotect(reinterpret_cast<void *>(page), span, PROT_READ | PROT_EXEC);
    __builtin___clear_cache(reinterpret_cast<char *>(addr), reinterpret_cast<char *>(addr + len));
    return true;
}

bool patchJumpRax(uintptr_t target, void *dest)
{
    uint8_t code[kJumpRaxLen];
    buildJumpRax(dest, code);
    return writeCode(target, code, sizeof code);
}

bool patchJumpIndirect(uintptr_t addr, size_t len, void *dest)
{
    uint8_t code[kMaxJumpLen];
    if (len < kJumpIndirectLen || len > sizeof code) {
        return false;
    }
    buildJumpIndirect(dest, code, len);
    return writeCode(addr, code, len);
}

bool patchJump(uintptr_t target, void *dest)
{
    if (patchJumpRax(target, dest)) {
        return true;
    }
    logMessage("mprotect: %s", strerror(errno));
    return false;
}

bool redirectWanted(uintptr_t addr, const void *replacement)
{
    if (!addr || !replacement) {
        return false;
    }
    return reinterpret_cast<const void *>(addr) != replacement;
}

void redirectFunctions(const ElfFile &elf, const char *exe, const Redirect *list, size_t count)
{
    // Symboltabelle (.symtab) des Executables; nur ohne PIE ist st_value die Laufzeitadresse.
    if (!elf.isExecutable()) {
        return;
    }

    for (size_t i = 0; i < count; ++i) {
        uintptr_t addr = elf.symbol(list[i].name);
        if (!redirectWanted(addr, list[i].replacement)) {
            continue;
        }

        bool ok = patchJump(addr, list[i].replacement);
        logMessage("%s: %s bei %#lx %s", exe, list[i].name, static_cast<unsigned long>(addr),
                   ok ? "umgeleitet" : "NICHT umgeleitet");
    }
}

void mapEach(const MapHeader *map, MapVisit visit, void *ctx)
{
    walk(map->root, visit, ctx);
}

int mapCollect(const MapHeader *map, uint32_t *keys, int *values, int max)
{
    Collect c{keys, values, max, 0};
    mapEach(map, collectOne, &c);
    return c.n > max ? -1 : c.n;
}

const MapNode *mapFind(const MapHeader *map, uint32_t key)
{
    const MapNode *node = map->root;
    while (node && node->key != key) {
        node = key < node->key ? node->left : node->right;
    }
    return node;
}

void mapInsert(MapHeader *map, uint32_t key, int value)
{
    auto *node = static_cast<MapNode *>(malloc(sizeof(MapNode)));
    *node = MapNode{nullptr, nullptr, nullptr, false, key, value};
    MapNode *parent = map->endNode();
    MapNode **link = &map->root;
    while (*link) {
        parent = *link;
        link = key < parent->key ? &parent->left : &parent->right;
    }
    node->parent = parent;
    *link = node;
    if (map->begin->left) {
        map->begin = map->begin->left;
    }
    balanceAfterInsert(map->root, node);
    ++map->size;
}
