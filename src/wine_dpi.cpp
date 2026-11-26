// Wine-DPI: Windows-Plugins über yabridge zeichnen mit der DPI ihres Wine-Prefix
// (HKCU\Control Panel\Desktop\LogPixels, Standard 96); JUCE-Plugins ignorieren sie (siehe
// sendContentScale), andere nicht. Wine kennt keine DPI pro Prozess, der Wert gilt für das
// ganze Prefix und wird beim Start des Plugin-Prozesses gelesen. Darum wird er vor dem
// Laden jedes yabridge-Plugins auf den Wert für dieses Plugin gestellt (pluginWineSetting), in
// resolve und im VstScanner: Plugin-Prozesse können ihn überleben (eine yabridge-Plugingruppe
// wie die von Waves läuft weiter und wird später von Resolve benutzt). Den Vorwert merkt sich
// die Datei, die resolve-linux-vst in RESOLVE_LINUX_VST_WINE_RESTORE mitgibt, und setzt ihn
// zurück, sobald Resolve beendet ist. Ohne die Variable bleibt die DPI unverändert.
// editor-test zählt mit: Es lädt wie Resolve, auch mit Wine-DPI (tests/wine-dpi.sh).

#include <dirent.h>
#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <vector>

#include "plugin_conf.h"
#include "log.h"
#include "process.h"
#include "scale.h"
#include "wine_base.h"
#include "wine_dpi.h"
#include "wine_reg.h"
#include "wine_restore.h"

namespace {

// Prefix, in dem eine Windows-Datei (meist ein Symlink von yabridge) liegt.
std::string winePrefixOf(const std::string &windowsFile)
{
    char *real = realpath(windowsFile.c_str(), nullptr);
    if (!real)
        return {};
    std::string path = real;
    free(real);
    size_t pos = path.find("/drive_c/");
    return pos == std::string::npos ? std::string() : path.substr(0, pos);
}

} // namespace

// VST2 über yabridge: neben Name.so liegt Name.dll.
std::string winePrefixFor(const std::string &so)
{
    if (so.size() < 3 || so.compare(so.size() - 3, 3, ".so") != 0)
        return {};
    return winePrefixOf(so.substr(0, so.size() - 3) + ".dll");
}

// VST3 über yabridge: im Bundle liegt Contents/x86_64-win/Name.vst3.
std::string winePrefixForBundle(const std::string &bundle)
{
    std::string prefix;
    if (DIR *d = opendir((bundle + "/Contents/x86_64-win").c_str())) {
        while (dirent *e = readdir(d))
            if (e->d_name[0] != '.' && prefix.empty())
                prefix = winePrefixOf(bundle + "/Contents/x86_64-win/" + e->d_name);
        closedir(d);
    }
    return prefix;
}

namespace {

// Einstellung eines Plugins: wine-zoom, wine-dpi (je einmal aus plugins.conf), sonst
// RESOLVE_LINUX_VST_WINE_DPI, sonst auto. Jedes yabridge-Plugin läuft in einem eigenen
// Wine-Prozess und liest die DPI bei dessen Start, darum kann sie je Plugin verschieden sein
// (nicht bei yabridge-Plugingruppen).
WineSetting pluginWineSetting(const Names &names)
{
    const char *env = getenv("RESOLVE_LINUX_VST_WINE_DPI");
    return wineSettingFor(pluginSetting(names, "wine-zoom"), pluginSetting(names, "wine-dpi"), env ? env : "",
                          editorScale());
}

// Datei der Vorwerte; Ort bestimmt der Starter (leer: nicht gestartet über resolve-linux-vst).
std::string restoreFile()
{
    const char *path = getenv("RESOLVE_LINUX_VST_WINE_RESTORE");
    return path ? path : "";
}

// Gemerkter Vorwert eines Prefix (Wert vor Resolve, kWineDpiUnset = war nicht gesetzt), sonst
// kNoRestoreEntry.
int rememberedWineDpi(const std::string &prefix)
{
    const std::string file = restoreFile();
    FILE *f = file.empty() ? nullptr : fopen(file.c_str(), "r");
    if (!f) {
        return kNoRestoreEntry;
    }
    int value = kNoRestoreEntry;
    char line[4096];
    while (value == kNoRestoreEntry && fgets(line, sizeof line, f)) {
        const RestoreLine entry = splitRestoreLine(line);
        if (entry.hasTab && entry.prefix == prefix) {
            parseDpiText(entry.dpiText, DpiParse::Lenient, &value);
        }
    }
    fclose(f);
    return value;
}

// Merkt sich den Vorwert (Zeile "Prefix<Tab>DPI", kWineDpiUnset = war nicht gesetzt). Steht der
// Prefix schon drin, bleibt der ältere Eintrag: Er ist der Wert vor Resolve.
bool rememberWineDpi(const std::string &prefix, int previous)
{
    if (rememberedWineDpi(prefix) != kNoRestoreEntry) {
        return true;
    }
    const std::string file = restoreFile();
    FILE *f = file.empty() ? nullptr : fopen(file.c_str(), "a");
    if (!f)
        return false;
    bool ok = fprintf(f, "%s\t%d\n", prefix.c_str(), previous) > 0;
    return fclose(f) == 0 && ok;
}

// Läuft eine Wine-Sitzung des Prefix? wineserver legt dann
// /tmp/.wine-<uid>/server-<dev>-<inode des Prefix>/socket an (wie Wine, hexadezimal).
bool wineSessionRunning(const std::string &prefix)
{
    struct stat st;
    if (stat(prefix.c_str(), &st) != 0) {
        return false;
    }
    char socket[256];
    snprintf(socket, sizeof socket, "/tmp/.wine-%u/server-%llx-%llx/socket", unsigned(getuid()),
             (unsigned long long)st.st_dev, (unsigned long long)st.st_ino);
    return access(socket, F_OK) == 0;
}

// Wine-Vergrößerung: Wine vergrößert jedes Fenster mit Basis-DPI / DPI des Plugin-Prozesses.
// Die Basis ist die DPI beim Start der Wine-Sitzung des Prefix (wineserver, explorer.exe);
// spätere Änderungen sehen nur neue Prozesse. Gemessen mit Wine 9.21 an Neoverb (Faktor 3.75,
// DPI 64): Ruhewert 180 → 180/64 (75 % des Fensters), Ruhewert 240 → 240/64 (voll).
// Plugins ohne DPI und Inhaltsskalierung (iZotope) bekommen DPI = Basis / Faktor (wine-zoom),
// ihre gemeldete Größe rechnet der Lader hoch (Instance::zoom); etwas weicher als echte
// Skalierung. Waves zeichnet selbst mit dem Faktor des Hosts und braucht DPI = Basis.
struct WinePrefixState {
    std::string prefix;
    int dpi;  // zuletzt gesetzter bzw. abgefragter Wert
    int base; // DPI beim Start der laufenden Wine-Sitzung
};

constexpr int kSessionEndWaitMs = 10000;

// Setzt die DPI eines Prefix ohne laufende Sitzung und wartet, bis die dafür gestartete
// Sitzung endet (wineserver bleibt einige Sekunden): Die nächste startet dann mit `dpi`.
bool resetWineSession(const std::string &prefix, int dpi)
{
    if (!writeWineDpi(prefix, dpi)) {
        return false;
    }
    for (int ms = 0; ms < kSessionEndWaitMs; ms += 100) {
        if (!wineSessionRunning(prefix)) {
            return true;
        }
        usleep(100 * 1000);
    }
    return false;
}

// Vor einer neuen Sitzung: DPI auf die Basis zurück. Gelingt das nicht, startet die Sitzung
// mit dem aktuellen Wert, der dann die Basis ist.
void startAtBase(WinePrefixState &state)
{
    const bool ok = resetWineSession(state.prefix, state.base);
    logMessage("  Wine-DPI %s: neue Sitzung, zuerst %d → %d %s", state.prefix.c_str(), state.dpi, state.base,
         ok ? "zurückgesetzt" : "NICHT zurückgesetzt");
    if (ok) {
        state.dpi = state.base;
        return;
    }
    state.base = wineDpiOrDefault(state.dpi);
}

// Zustand eines Prefix vor dem Laden. Basis ist der Wert vor Resolve (gemerkter Vorwert, sonst
// der aktuelle). Läuft keine Sitzung, wird die DPI vorher auf die Basis zurückgesetzt, sonst
// startete die nächste Sitzung mit dem zuletzt gesetzten Wert und die DPI sänke mit jeder
// (Basis / Faktor / Faktor …). Eine laufende Sitzung, die dieser Prozess kennt, behält ihre
// Basis. nullptr, wenn Wine nicht abfragbar ist.
WinePrefixState *prefixState(std::vector<WinePrefixState> &known, const std::string &prefix)
{
    const WineSession session = wineSessionRunning(prefix) ? WineSession::Running : WineSession::Stopped;
    WinePrefixState *k = nullptr;
    for (auto &entry : known) {
        if (entry.prefix == prefix) {
            k = &entry;
        }
    }
    if (k && session == WineSession::Running) {
        return k;
    }

    // Aus der Registry, nicht k->dpi: Ein anderer Prozess (VstScanner) kann sie geändert haben.
    const int current = queryWineDpi(prefix);
    if (current == kWineQueryFailed) {
        return nullptr;
    }
    const int before = rememberedWineDpi(prefix);
    const WineBase decision = wineBaseFor(session, current,
                                          before == kNoRestoreEntry ? std::nullopt : std::optional<int>(before));
    WinePrefixState state{prefix, current, decision.base};
    if (decision.reset == WineReset::Yes) {
        startAtBase(state);
    }
    if (!k) {
        known.push_back(state);
        return &known.back();
    }
    *k = state;
    return k;
}

} // namespace

// Stellt die Wine-DPI für ein Plugin ein, liefert den Faktor für Instance::zoom (Basis / DPI
// bei wine-zoom, sonst 1). `prefix` aus winePrefixFor bzw. winePrefixForBundle.
double syncWineDpi(const Names &names, const std::string &prefix)
{
    static const bool inResolve = loadsLikeResolve();
    static std::mutex mutex;
    static std::vector<WinePrefixState> known;
    if (!inResolve || prefix.empty()) {
        return 1.0;
    }
    // "0" = nicht anfassen hängt nicht von der Basis ab: dann keine Wine-Aufrufe.
    const WineSetting setting = pluginWineSetting(names);
    if (setting.mode == WineMode::Leave) {
        return 1.0;
    }
    std::lock_guard lock(mutex);

    timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    WinePrefixState *k = prefixState(known, prefix);
    if (!k) {
        logMessage("  Wine-DPI %s: Abfrage nicht möglich, bleibt", prefix.c_str());
        return 1.0;
    }
    const int wanted = wineDpiFor(setting, k->base);
    const double zoom = wineZoomFor(setting, k->base, wanted);
    const int current = k->dpi;
    if (current == wanted) {
        logMessage("  Wine-DPI %s: %d, passt (Basis %d)", prefix.c_str(), current, k->base);
        return zoom;
    }
    // Ohne gemerkten Vorwert kein Ändern, sonst ließe er sich nicht zurücksetzen. Nur der
    // erste Eintrag je Prefix zählt (Wert vor Resolve).
    if (!rememberWineDpi(prefix, current)) {
        logMessage("  Wine-DPI %s: Vorwert nicht speicherbar, bleibt %d", prefix.c_str(), current);
        return 1.0;
    }
    bool ok = writeWineDpi(prefix, wanted);
    if (ok)
        k->dpi = wanted;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    logMessage("  Wine-DPI %s: %d → %d %s (Basis %d, %.1f s); zurück nach Beenden von Resolve", prefix.c_str(), current,
         wanted, ok ? "gesetzt" : "NICHT gesetzt", k->base,
         double(t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9);
    return ok ? zoom : 1.0;
}
