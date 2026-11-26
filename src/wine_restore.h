// Wine-DPI zurücksetzen: Auswertung der Datei mit den Vorwerten ("Prefix<Tab>DPI" je Zeile,
// geschrieben von wine_dpi.cpp), rein gerechnet ohne Wine und Dateisystem. Das Zurücksetzen
// selbst übergibt der Aufrufer als Funktion; tools/wine-dpi-restore.cpp ruft Wine auf,
// tests/wine-restore-test.cpp zählt nur. Das Lesen einer Zeile nutzt auch wine_dpi.cpp. Eigene Datei, damit der Test sie ohne Wine baut.
#pragma once

#include <cerrno>
#include <cstdlib>
#include <functional>
#include <string>

constexpr int kWineDpiUnset = -1;    // LogPixels war nicht gesetzt (Vorwert in der Datei)
constexpr int kNoRestoreEntry = -2;  // kein Eintrag für das Prefix in der Datei

enum class RestoreResult {
    Done,   // zurückgesetzt: Zeile entfällt
    Skip,   // nichts zu tun (Prefix gibt es nicht mehr): Zeile entfällt
    Failed, // nicht zurückgesetzt: Zeile bleibt für den nächsten Lauf
};

// Setzt die DPI des Prefix auf den Wert des Textes (kWineDpiUnset = war nicht gesetzt).
using RestoreFn = std::function<RestoreResult(const std::string &prefix, const std::string &dpiText)>;

// Eine Zeile der Datei: "Prefix<Tab>DPI-Text". Ohne Tab ist der DPI-Text leer.
struct RestoreLine {
    std::string prefix;
    std::string dpiText;
    bool hasTab;
};

inline RestoreLine splitRestoreLine(const std::string &line)
{
    const size_t tab = line.find('\t');
    if (tab == std::string::npos) {
        return {line, std::string(), false};
    }
    return {line.substr(0, tab), line.substr(tab + 1), true};
}

// Strict: nur eine ganze Zahl gilt, sonst false (die Zeile bleibt dann für den nächsten Lauf).
// Lenient: wie atoi, Unlesbares wird 0. Die Bibliothek liest lenient, das Werkzeug strict;
// beides ist das Verhalten vor dem Zusammenführen und bleibt bewusst verschieden.
enum class DpiParse { Strict, Lenient };

inline bool parseDpiText(const std::string &text, DpiParse mode, int *dpi)
{
    if (mode == DpiParse::Lenient) {
        *dpi = atoi(text.c_str());
        return true;
    }

    char *end = nullptr;
    errno = 0;
    const long value = strtol(text.c_str(), &end, 10);
    if (text.empty() || *end != '\0' || errno != 0) {
        return false;
    }
    *dpi = int(value);
    return true;
}

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

        const auto [prefix, dpi, hasTab] = splitRestoreLine(line);
        if (prefix.empty()) {
            continue;
        }
        if (reset(prefix, dpi) == RestoreResult::Failed) {
            keep += prefix + '\t' + dpi + '\n';
        }
    }
    return keep;
}
