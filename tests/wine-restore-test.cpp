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

    printf("%s\n", failures ? "FEHLGESCHLAGEN" : "alles ok");
    return failures ? 1 : 0;
}
