// Test der Auswertung der Vorwert-Datei (src/wine_restore.h): welche Zeilen nach dem
// Zurücksetzen bleiben. Läuft ohne Wine (make test).
#include "../src/wine_restore.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

int failures = 0;

void expect(bool ok, const char *what)
{
    printf("%s  %s\n", ok ? "ok   " : "FEHLER", what);
    failures += !ok;
}

void testSplit()
{
    const RestoreLine split = splitRestoreLine("/a b\t144");
    expect(split.prefix == "/a b" && split.dpiText == "144", "Zeile: Prefix und Text");
    expect(splitRestoreLine("/a").dpiText.empty() && splitRestoreLine("/a").prefix == "/a", "Zeile ohne Tab");
    expect(!splitRestoreLine("/a").hasTab && splitRestoreLine("/a\t").hasTab, "Tab erkannt, auch ohne Text");
    expect(splitRestoreLine("/a\t1\t2").dpiText == "1\t2", "nur der erste Tab trennt");
}

void testParse()
{
    int dpi = 0;
    expect(parseDpiText("192", DpiParse::Strict, &dpi) && dpi == 192, "strikt: Zahl");
    expect(parseDpiText("-1", DpiParse::Strict, &dpi) && dpi == kWineDpiUnset, "strikt: -1");
    expect(!parseDpiText("", DpiParse::Strict, &dpi), "strikt: leer scheitert");
    expect(!parseDpiText("abc", DpiParse::Strict, &dpi), "strikt: Text scheitert");
    expect(!parseDpiText("12abc", DpiParse::Strict, &dpi), "strikt: Rest scheitert");
    expect(parseDpiText("192", DpiParse::Lenient, &dpi) && dpi == 192, "locker: Zahl");
    expect(parseDpiText("-1\n", DpiParse::Lenient, &dpi) && dpi == kWineDpiUnset, "locker: -1 mit Zeilenende");
    expect(parseDpiText("abc", DpiParse::Lenient, &dpi) && dpi == 0, "locker: Text wird 0");
    expect(parseDpiText("", DpiParse::Lenient, &dpi) && dpi == 0, "locker: leer wird 0");
    expect(parseDpiText("12abc", DpiParse::Lenient, &dpi) && dpi == 12, "locker: Zahl vorn zählt");
}

} // namespace

int main()
{
    std::vector<std::string> seen;
    auto fake = [&](const std::string &prefix, const std::string &dpi) {
        seen.push_back(prefix + "=" + dpi);
        if (prefix == "/gone") {
            return RestoreResult::Skip;
        }
        return prefix.compare(0, 4, "/bad") == 0 ? RestoreResult::Failed : RestoreResult::Done;
    };

    expect(linesToKeep("", fake).empty() && seen.empty(), "leere Datei: nichts");

    const std::string all = linesToKeep("/a\t192\n/b\t-1\n", fake);
    expect(all.empty() && seen.size() == 2, "alle zurückgesetzt: nichts bleibt");
    expect(seen.size() == 2 && seen[0] == "/a=192" && seen[1] == "/b=-1", "Prefix und Wert (-1) kommen an");

    expect(linesToKeep("/gone\t96\n", fake).empty(), "Prefix weg: Zeile entfällt");
    expect(linesToKeep("/bad\t144\n", fake) == "/bad\t144\n", "Fehlschlag: Zeile bleibt");
    expect(linesToKeep("/a\t1\n/bad\t-1\n/b\t2\n/bad2\t3\n", fake) == "/bad\t-1\n/bad2\t3\n",
           "gemischt: nur die gescheiterten bleiben");
    expect(linesToKeep("\t96\n/a\t1\n", fake).empty(), "leerer Prefix: Zeile entfällt");
    expect(linesToKeep("/bad\t1", fake).empty(), "letzte Zeile ohne Zeilenende zählt nicht");

    seen.clear();
    linesToKeep("/a\n", fake);
    expect(seen.size() == 1 && seen[0] == "/a=", "Zeile ohne Tab: leerer Wert");

    testSplit();
    testParse();

    printf("%s\n", failures ? "FEHLGESCHLAGEN" : "alles ok");
    return failures ? 1 : 0;
}
