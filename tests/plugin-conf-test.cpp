// Test des Lesens von plugins.conf und der eingebauten Vorgaben (src/plugin_conf.cpp) über
// pluginSetting, mit eigener Konfigurationsdatei in einem temporären Ordner (XDG_CONFIG_HOME).
// Läuft ohne Resolve und ohne Wine (make test).
#include "../src/plugin_conf.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

namespace {

namespace fs = std::filesystem;

int failures = 0;

void expect(bool ok, const char *what)
{
    printf("%s  %s\n", ok ? "ok   " : "FEHLER", what);
    failures += !ok;
}

constexpr const char *kConfDir = "resolve-linux-vst";
constexpr const char *kConfFile = "plugins.conf";
constexpr size_t kLongLine = 3000; // länger als der Lesepuffer von pluginSetting (1024)

fs::path root; // Ordner dieses Laufs, wird am Ende gelöscht

// Temporäre Dateien nach $TMPDIR, sonst ~/.cache, wie die anderen Tests (nie /tmp, das liegt im RAM).
fs::path tempBase()
{
    if (const char *tmp = getenv("TMPDIR"); tmp && *tmp) {
        return tmp;
    }
    const char *home = getenv("HOME");
    return fs::path(home ? home : ".") / ".cache";
}

fs::path confPath(const fs::path &config)
{
    return config / kConfDir / kConfFile;
}

void writeFile(const fs::path &path, const std::string &text)
{
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    FILE *f = fopen(path.c_str(), "w");
    if (!f) {
        printf("FEHLER  kann %s nicht schreiben\n", path.c_str());
        exit(1);
    }
    fputs(text.c_str(), f);
    fclose(f);
}

// Schreibt plugins.conf unter $XDG_CONFIG_HOME neu (pluginSetting liest bei jedem Aufruf).
void writeConf(const std::string &text)
{
    writeFile(confPath(root), text);
}

void removeConf()
{
    std::error_code ec;
    fs::remove(confPath(root), ec);
}

bool is(const Names &names, const char *key, const char *want)
{
    return pluginSetting(names, key) == want;
}

void testSections()
{
    writeConf("[MPC Beats64]\n"
              "wine-dpi = 96\n"
              "content-scale = 0\n"
              "\n"
              "[Other]\n"
              "wine-dpi = 120\n");
    expect(is({"MPC Beats64"}, "wine-dpi", "96"), "Abschnitt mit genauem Namen");
    expect(is({"MPC Beats64"}, "content-scale", "0"), "zweiter Schlüssel im Abschnitt");
    expect(is({"Other"}, "wine-dpi", "120"), "zweiter Abschnitt");
    expect(is({"Unknown"}, "wine-dpi", ""), "kein passender Abschnitt: leer");
    expect(is({"MPC Beats64"}, "wine-zoom", ""), "unbekannter Schlüssel: leer");
    expect(is({"mpc beats64"}, "wine-dpi", ""), "Name unterscheidet Groß und Klein");
    expect(is({"MPC"}, "wine-dpi", ""), "Name muss ganz passen");
    expect(is({"MPC Beats64"}, "Wine-Dpi", ""), "Schlüssel unterscheidet Groß und Klein");
}

void testPatterns()
{
    writeConf("[Vendor/*]\n"
              "wine-dpi = 111\n"
              "[Synth?]\n"
              "wine-dpi = 122\n"
              "[*]\n"
              "content-scale = auto\n");
    expect(is({"Vendor/Plug"}, "wine-dpi", "111"), "Muster Ordner/*");
    expect(is({"Vendor2/Plug"}, "wine-dpi", ""), "Muster Ordner/* passt nicht auf anderen Ordner");
    expect(is({"Synth1"}, "wine-dpi", "122"), "Muster mit ?");
    expect(is({"Anything"}, "content-scale", "auto"), "Muster * passt auf alles");
    expect(is({"Vendor/Plug"}, "content-scale", "auto"), "Muster * passt auch auf Name mit Ordner");
}

void testPrecedence()
{
    writeConf("[Plug]\n"
              "wine-dpi = 1\n"
              "[Plug]\n"
              "wine-dpi = 2\n"
              "content-scale = 7\n"
              "[*]\n"
              "wine-dpi = 3\n"
              "content-scale = 8\n"
              "wine-zoom = 9\n");
    expect(is({"Plug"}, "wine-dpi", "1"), "mehrere passende Abschnitte: der erste gewinnt");
    expect(is({"Plug"}, "content-scale", "7"), "Abschnitt ohne den Schlüssel: der nächste passende gilt");
    expect(is({"Plug"}, "wine-zoom", "9"), "Schlüssel nur im allgemeinen Abschnitt");
    expect(is({"Other"}, "wine-dpi", "3"), "anderes Plugin: allgemeiner Abschnitt");

    writeConf("[*]\n"
              "wine-dpi = 3\n"
              "[Plug]\n"
              "wine-dpi = 1\n");
    expect(is({"Plug"}, "wine-dpi", "3"), "Reihenfolge der Datei zählt, nicht die Genauigkeit");

    writeConf("[Plug]\n"
              "wine-dpi = 1\n"
              "wine-dpi = 2\n");
    expect(is({"Plug"}, "wine-dpi", "1"), "Schlüssel doppelt im Abschnitt: die erste Zeile gilt");
}

void testNames()
{
    writeConf("[MyPlug]\n"
              "wine-dpi = 10\n"
              "[Vendor/MyPlug]\n"
              "content-scale = 20\n"
              "[Bundle]\n"
              "wine-zoom = 30\n");
    const Names names{"file", "Bundle", "Vendor/MyPlug"};
    expect(is(names, "wine-zoom", "30"), "Name des VST3-Bundles");
    expect(is(names, "content-scale", "20"), "Name mit Ordner");
    expect(is(names, "wine-dpi", ""), "keiner der Namen passt: leer");
    expect(is({"MyPlug", "Bundle"}, "wine-dpi", "10"), "einer von mehreren Namen passt");

    writeConf("[Bundle]\n"
              "wine-dpi = 5\n"
              "[file]\n"
              "wine-dpi = 6\n");
    expect(is({"file", "Bundle"}, "wine-dpi", "5"), "Abschnittsreihenfolge geht der Namensreihenfolge vor");
    expect(is({}, "wine-dpi", ""), "keine Namen: leer");
}

void testSyntax()
{
    writeConf("# Kommentar am Anfang\n"
              "wine-dpi = 99\n" // vor dem ersten Abschnitt: ohne Wirkung
              "\n"
              "   [Plug]   # Kommentar am Abschnitt\n"
              "\n"
              "# wine-dpi = 1\n"
              "wine-dpi=2\n"
              "\t content-scale \t=\t 3 \t# Kommentar am Wert\n"
              "wine-zoom   =    auto   \n"
              "[Crlf]\r\n"
              "wine-dpi = 4\r\n"
              "[Space In Name]\n"
              "wine-dpi = 5\n"
              "[ Padded ]\n"
              "wine-dpi = 6\n"
              "[NoKey]\n"
              "just text\n"
              "[Empty]\n"
              "wine-dpi =\n");
    expect(is({"Plug"}, "wine-dpi", "2"), "Kommentarzeile und Zeile vor dem ersten Abschnitt ohne Wirkung");
    expect(is({"Plug"}, "content-scale", "3"), "Tabulatoren um = und Kommentar hinter dem Wert");
    expect(is({"Plug"}, "wine-zoom", "auto"), "Leerzeichen um = und am Zeilenende");
    expect(is({"Crlf"}, "wine-dpi", "4"), "Zeilenende CRLF");
    expect(is({"Space In Name"}, "wine-dpi", "5"), "Leerzeichen im Abschnittsnamen");
    expect(is({"Padded"}, "wine-dpi", "6"), "Leerzeichen innerhalb der Klammern werden entfernt");
    expect(is({"NoKey"}, "wine-dpi", ""), "Zeile ohne =: ohne Wirkung");
    expect(is({"Empty"}, "wine-dpi", ""), "leerer Wert: Treffer mit leerem Ergebnis");
}

void testDefaults()
{
    writeConf("");
    expect(is({"WaveShell1-VST3 14.0"}, "wine-dpi", "prefix"), "Vorgabe WaveShell*: wine-dpi prefix");
    expect(is({"WaveShell1-VST3 14.0"}, "wine-zoom", ""), "WaveShell*: kein wine-zoom");
    expect(is({"x", "WaveShell"}, "wine-dpi", "prefix"), "Vorgabe passt auf jeden der Namen");
    expect(is({"iZotope/Ozone 11"}, "wine-zoom", "auto"), "Vorgabe iZotope/*: wine-zoom auto");
    expect(is({"iZotope/Ozone 11"}, "wine-dpi", ""), "iZotope/*: kein wine-dpi");
    expect(is({"iZotope"}, "wine-zoom", ""), "iZotope ohne Ordner: keine Vorgabe");
    expect(is({"Other/Plug"}, "wine-zoom", ""), "anderes Plugin: keine Vorgabe");
    expect(is({"Other"}, "wine-dpi", ""), "anderes Plugin: leer");

    writeConf("[WaveShell*]\n"
              "wine-dpi = 96\n"
              "[iZotope/*]\n"
              "wine-zoom = 0\n"
              "[iZotope/Ozone 11]\n"
              "wine-dpi = 150\n");
    expect(is({"WaveShell1"}, "wine-dpi", "96"), "plugins.conf überschreibt die Vorgabe wine-dpi");
    expect(is({"iZotope/Ozone 11"}, "wine-zoom", "0"), "plugins.conf überschreibt die Vorgabe wine-zoom");
    expect(is({"iZotope/Ozone 11"}, "wine-dpi", "150"), "plugins.conf setzt zusätzlichen Schlüssel");

    writeConf("[WaveShell*]\n"
              "content-scale = 0\n");
    expect(is({"WaveShell1"}, "wine-dpi", "prefix"), "andere Zeile in plugins.conf lässt die Vorgabe stehen");
    expect(is({"WaveShell1"}, "content-scale", "0"), "Zeile in plugins.conf neben der Vorgabe");

    writeConf("[WaveShell*]\n"
              "wine-dpi =\n");
    expect(is({"WaveShell1"}, "wine-dpi", "prefix"), "leerer Wert in plugins.conf: Vorgabe gilt");
}

void testFile()
{
    removeConf();
    expect(is({"WaveShell1"}, "wine-dpi", "prefix"), "fehlende Datei: Vorgaben gelten");
    expect(is({"MPC Beats64"}, "wine-dpi", ""), "fehlende Datei: sonst leer");

    writeConf("[Plug]\nwine-dpi = 1\n");
    expect(is({"Plug"}, "wine-dpi", "1"), "Datei wird neu angelegt: gilt sofort");
    writeConf("[Plug]\nwine-dpi = 2\n");
    expect(is({"Plug"}, "wine-dpi", "2"), "Änderung der Datei gilt ohne Neustart");

    // Zeile länger als der Lesepuffer: darf weder abstürzen noch den Rest verschlucken.
    writeConf("[Plug]\n# " + std::string(kLongLine, 'x') + "\nwine-dpi = 8\n");
    expect(is({"Plug"}, "wine-dpi", "8"), "sehr lange Kommentarzeile");
}

// Ohne XDG_CONFIG_HOME (leer oder nicht gesetzt) liegt die Datei unter $HOME/.config.
void testHome()
{
    const fs::path home = root / "home";
    writeFile(confPath(home / ".config"), "[Plug]\nwine-dpi = 77\n");

    setenv("HOME", home.c_str(), 1);
    setenv("XDG_CONFIG_HOME", "", 1);
    expect(is({"Plug"}, "wine-dpi", "77"), "XDG_CONFIG_HOME leer: $HOME/.config");
    unsetenv("XDG_CONFIG_HOME");
    expect(is({"Plug"}, "wine-dpi", "77"), "XDG_CONFIG_HOME nicht gesetzt: $HOME/.config");
}

} // namespace

int main()
{
    std::error_code ec;
    const fs::path base = tempBase();
    fs::create_directories(base, ec);
    std::string pattern = (base / "plugin-conf-test-XXXXXX").string();
    if (!mkdtemp(pattern.data())) {
        printf("FEHLER  kein temporärer Ordner unter %s\n", base.c_str());
        return 1;
    }
    root = pattern;
    setenv("XDG_CONFIG_HOME", root.c_str(), 1);

    testSections();
    testPatterns();
    testPrecedence();
    testNames();
    testSyntax();
    testDefaults();
    testFile();
    testHome();

    fs::remove_all(root, ec);
    return failures ? 1 : 0;
}
