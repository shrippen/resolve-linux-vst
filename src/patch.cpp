#include "patch.h"

#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "log.h"
#include "sites.h"

namespace {

constexpr uint8_t kInt3 = 0xcc;
constexpr size_t kMaxJumpLen = 32;

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
