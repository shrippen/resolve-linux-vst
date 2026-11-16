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

struct Translate {
    int mask;
    int result;
};

void translateOne(uint32_t key, int value, void *ctx)
{
    auto *t = static_cast<Translate *>(ctx);
    if (key & t->mask) {
        t->result |= value;
    }
}

int translateMask(const MapHeader *map, int mask)
{
    if (!map || !map->root) {
        return mask;
    }
    Translate t{mask, 0};
    mapEach(map, translateOne, &t);
    return t.result;
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

    // Vor dem Umleiten: Tabellen der Kategorie-Masken (nur im VstScanner im Executable).
    // Fehlt eine Tabelle, bleibt ihre Funktion unberührt (Ersatz nullptr).
    const ElfFile elf(exe);
    if (!elf.isExecutable()) {
        return;
    }

    gMaskIn = reinterpret_cast<const MapHeader *>(elf.symbol(kSymMaskTable));
    gMaskOut = reinterpret_cast<const MapHeader *>(elf.symbol(kSymMapTable));
    const Redirect patches[] = {
        {kSymMaskIn, gMaskIn ? reinterpret_cast<void *>(&categoryMaskIn) : nullptr},
        {kSymMaskOut, gMaskOut ? reinterpret_cast<void *>(&categoryMaskOut) : nullptr},
    };
    redirectFunctions(elf, exe, patches, sizeof patches / sizeof *patches);
}

// Zahlenformat: Resolve und VstScanner lesen die Version von FXConfiguration.xml ("1.4") mit
// dem Zahlenformat des Gebietsschemas; mit deutschem (Komma) wird daraus 1, und unter 1.3
// übergehen beide <CategoryMask>: alle Plugins „Unsortiert“, und der Scanner schreibt bei
// jedem Start 0 zurück. Darum LC_NUMERIC=C, bevor Qt das Gebietsschema setzt (der Scanner
// erbt es). LC_ALL ginge vor, sein Wert wandert darum nach LANG.
void fixNumericLocale()
{
    if (!loadsLikeResolve())
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
