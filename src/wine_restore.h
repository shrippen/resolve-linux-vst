// Wine-DPI zurücksetzen: Auswertung der Datei mit den Vorwerten ("Prefix<Tab>DPI" je Zeile,
// geschrieben von wine_dpi.cpp), rein gerechnet ohne Wine und Dateisystem. Das Zurücksetzen
// selbst übergibt der Aufrufer als Funktion; tools/wine-dpi-restore.cpp ruft Wine auf,
// tests/wine-restore-test.cpp zählt nur. Eigene Datei, damit der Test sie ohne Wine baut.
#pragma once

#include <functional>
#include <string>

enum class RestoreResult {
    Done,   // zurückgesetzt: Zeile entfällt
    Skip,   // nichts zu tun (Prefix gibt es nicht mehr): Zeile entfällt
    Failed, // nicht zurückgesetzt: Zeile bleibt für den nächsten Lauf
};

// Setzt die DPI des Prefix auf den Wert des Textes (-1 = war nicht gesetzt).
using RestoreFn = std::function<RestoreResult(const std::string &prefix, const std::string &dpiText)>;

// Geht alle Zeilen durch und liefert die, die stehen bleiben (mit Zeilenende). Eine Zeile ohne
// Tab hat einen leeren DPI-Text; leerer Prefix: Zeile entfällt. Eine unvollständige letzte
// Zeile (ohne Zeilenende) zählt nicht, wie bei `read` im bisherigen Starter.
inline std::string linesToKeep(const std::string &content, const RestoreFn &reset)
{
    std::string keep;
    size_t start = 0;
    size_t end;
    while ((end = content.find('\n', start)) != std::string::npos) {
        const std::string line = content.substr(start, end - start);
        start = end + 1;

        const size_t tab = line.find('\t');
        const std::string prefix = line.substr(0, tab);
        const std::string dpi = tab == std::string::npos ? std::string() : line.substr(tab + 1);
        if (prefix.empty()) {
            continue;
        }
        if (reset(prefix, dpi) == RestoreResult::Failed) {
            keep += prefix + '\t' + dpi + '\n';
        }
    }
    return keep;
}
