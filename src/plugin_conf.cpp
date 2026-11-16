#include "plugin_conf.h"

#include <fnmatch.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

std::string trim(const char *begin, const char *end)
{
    while (begin < end && (*begin == ' ' || *begin == '\t'))
        ++begin;
    while (end > begin && (end[-1] == ' ' || end[-1] == '\t'))
        --end;
    return std::string(begin, end);
}

// Eingebaute Vorgaben, wenn plugins.conf nichts sagt (Muster wie dort).
struct Default {
    const char *pattern, *key, *value;
};
constexpr Default kDefaults[] = {
    // Waves zeichnet mit dem Faktor des Hosts (Inhaltsskalierung); Wine darf nicht zusätzlich
    // vergrößern, also DPI = Basis (siehe WinePrefixState). Mit fest 240 bei Ruhewert 180: 75 %.
    {"WaveShell*", "wine-dpi", "prefix"},
    // iZotope unter Wine kennt keine Inhaltsskalierung und zeichnet in 1/2,5 der Größe; nur
    // Wine selbst kann es vergrößern (wine-zoom).
    {"iZotope/*", "wine-zoom", "auto"},
};

bool matches(const std::string &pattern, const Names &names)
{
    for (const auto &n : names)
        if (fnmatch(pattern.c_str(), n.c_str(), 0) == 0)
            return true;
    return false;
}

} // namespace

// Einstellung je Plugin aus $XDG_CONFIG_HOME/resolve-linux-vst/plugins.conf (Standard
// ~/.config/resolve-linux-vst/plugins.conf), sonst aus kDefaults, leer wenn nicht gesetzt:
//
//   [MPC Beats64]          # Name (siehe Names) oder Shell-Muster wie [WaveShell*]
//   wine-dpi = 96
//   content-scale = 0
//
// Es gilt der erste passende Abschnitt mit dem Schlüssel. Wird bei jedem Aufruf gelesen
// (klein, selten gebraucht); Änderungen gelten ohne Neustart.
std::string pluginSetting(const Names &plugin, const char *key)
{
    std::string conf;
    if (const char *xdg = getenv("XDG_CONFIG_HOME"); xdg && *xdg)
        conf = xdg;
    else if (const char *home = getenv("HOME"))
        conf = std::string(home) + "/.config";
    std::string value;
    FILE *f = fopen((conf + "/resolve-linux-vst/plugins.conf").c_str(), "r");
    bool inSection = false;
    char line[1024];
    while (f && fgets(line, sizeof line, f)) {
        line[strcspn(line, "#\r\n")] = 0;
        const char *end = line + strlen(line);
        if (const char *open = strchr(line, '['); open && trim(line, open).empty()) {
            const char *close = strchr(open, ']');
            inSection = close && matches(trim(open + 1, close), plugin);
            continue;
        }
        const char *eq = strchr(line, '=');
        if (inSection && eq && trim(line, eq) == key) {
            value = trim(eq + 1, end);
            break;
        }
    }
    if (f)
        fclose(f);
    if (value.empty())
        for (const auto &d : kDefaults)
            if (strcmp(d.key, key) == 0 && matches(d.pattern, plugin)) {
                value = d.value;
                break;
            }
    return value;
}
