// Eingriffe in Resolves Scanner und Zahlenformat beim Start: ein eigenes Feature, das keinen
// anderen Teil kennt. Startet sich selbst (Konstruktor), nach dem Lader, der das Protokoll öffnet.

#include <cstdint>
#include <cstdlib>

#include "log.h"
#include "patch.h"
#include "process.h"
#include "sites.h"

namespace {

constexpr int kInitScanner = 102;

// Kategorien im VstScanner: Er liest <CategoryMask> beim Laden der Plugin-Liste über
// AudioPluginHost::GetPluginCategoryMaskIn und schreibt sie über ...MaskOut. Beide rechnen
// über Tabellen (std::map<int, int>, Bit der Datei ↔ Bit intern: Schlüssel & Maske ergibt
// den Wert), die PluginHost::Initialize füllt; im Scanner geschieht das nie, die Tabellen
// bleiben leer, und bei jedem Start von Resolve schreibt der Scanner alle Masken als 0 zurück
// (auch die von resolve-linux-vst categories). Ersatz: wie das Original, bei leerer Tabelle aber
// die Maske unverändert.
const MapHeader *gMaskIn = nullptr, *gMaskOut = nullptr;

int translateMask(const MapNode *node, int mask)
{
    if (!node)
        return 0;
    return ((node->key & mask) ? node->value : 0) | translateMask(node->left, mask) | translateMask(node->right, mask);
}

int translateMask(const MapHeader *map, int mask)
{
    const MapNode *root = map ? map->root : nullptr;
    return root ? translateMask(root, mask) : mask;
}

int categoryMaskIn(const void *, int mask)
{
    return translateMask(gMaskIn, mask);
}

int categoryMaskOut(const void *, int mask)
{
    return translateMask(gMaskOut, mask);
}

void patchCategories()
{
    const char *exe = exePath();
    if (!*exe) {
        return;
    }

    struct {
        const char *name;
        void *replacement;
    } const patches[] = {
        {"_ZNK15AudioPluginHost23GetPluginCategoryMaskInEi", reinterpret_cast<void *>(&categoryMaskIn)},
        {"_ZNK15AudioPluginHost24GetPluginCategoryMaskOutEi", reinterpret_cast<void *>(&categoryMaskOut)},
    };
    // Symboltabelle (.symtab) des Executables; nur ohne PIE ist st_value die Laufzeitadresse.
    const ElfFile elf(exe);
    if (!elf.isExecutable())
        return;
    // Vor dem Umleiten: Tabellen der Kategorie-Masken (nur im VstScanner im Executable).
    gMaskIn = reinterpret_cast<const MapHeader *>(elf.symbol("_ZN15AudioPluginHost19_pluginCategoryMaskE"));
    gMaskOut = reinterpret_cast<const MapHeader *>(elf.symbol("_ZN15AudioPluginHost18_pluginCategoryMapE"));
    for (const auto &p : patches) {
        uintptr_t addr = elf.symbol(p.name);
        if (!addr)
            continue;
        if (reinterpret_cast<void *>(addr) == p.replacement)
            continue;
        if ((p.replacement == reinterpret_cast<void *>(&categoryMaskIn) && !gMaskIn)
            || (p.replacement == reinterpret_cast<void *>(&categoryMaskOut) && !gMaskOut))
            continue;
        bool ok = patchJump(addr, p.replacement);
        logMessage("%s: %s bei %#lx %s", exe, p.name, static_cast<unsigned long>(addr), ok ? "umgeleitet" : "NICHT umgeleitet");
    }
}

// Zahlenformat: Resolve und VstScanner lesen die Version von FXConfiguration.xml ("1.4") mit
// dem Zahlenformat des Gebietsschemas; mit deutschem (Komma) wird daraus 1, und unter 1.3
// übergehen beide <CategoryMask>: alle Plugins „Unsortiert“, und der Scanner schreibt bei
// jedem Start 0 zurück. Darum LC_NUMERIC=C, bevor Qt das Gebietsschema setzt (der Scanner
// erbt es). LC_ALL ginge vor, sein Wert wandert darum nach LANG.
void fixNumericLocale()
{
    if (!isResolveProcess())
        return;
    if (const char *all = getenv("LC_ALL"); all && *all) {
        setenv("LANG", all, 1);
        unsetenv("LC_ALL");
    }
    setenv("LC_NUMERIC", "C", 1);
}

__attribute__((constructor(kInitScanner))) void init()
{
    fixNumericLocale();
    patchCategories();
}

} // namespace
