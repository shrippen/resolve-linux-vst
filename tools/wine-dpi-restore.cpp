// wine-dpi-restore: setzt die Wine-DPI der Prefixe zurück, die libresolve-linux-vst.so während
// Resolve geändert hat (aufgerufen von resolve-linux-vst nach dem Ende von Resolve und bei
// `resolve-linux-vst restore`). Wine-Befehl und Registry-Schlüssel kommen aus src/wine_reg.cpp;
// dessen Log geht auf stderr.
//
//   build/wine-dpi-restore <Datei mit den Vorwerten>
//
// Zeilen, die zurückgesetzt wurden oder deren Prefix es nicht mehr gibt, entfallen; die Datei
// verschwindet, wenn nichts bleibt. Gescheiterte Zeilen bleiben für den nächsten Lauf.
// Exit-Code 1, wenn Zeilen geblieben sind.
#include "../src/log.h"
#include "../src/wine_reg.h"
#include "../src/wine_restore.h"

#include <sys/stat.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

namespace {

constexpr const char *kMsgPrefix = "resolve-linux-vst: ";

bool isDirectory(const std::string &path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

RestoreResult resetOne(const std::string &prefix, const std::string &dpiText)
{
    if (!isDirectory(prefix)) {
        return RestoreResult::Skip;
    }

    int dpi = 0;
    if (parseDpiText(dpiText, DpiParse::Strict, &dpi) && restoreWineDpi(prefix, dpi)) {
        const std::string shown = dpi == kWineDpiUnset ? "default" : dpiText;
        fprintf(stderr, "%sWine DPI in %s back to %s\n", kMsgPrefix, prefix.c_str(), shown.c_str());
        return RestoreResult::Done;
    }
    fprintf(stderr, "%sWine DPI in %s not reset\n", kMsgPrefix, prefix.c_str());
    return RestoreResult::Failed;
}

} // namespace

int main(int argc, char **argv)
{
    setLogFile(stderr);

    if (argc != 2) {
        fprintf(stderr, "usage: %s <wine-dpi-restore file>\n", argv[0]);
        return 2;
    }
    const std::string file = argv[1];

    std::ifstream in(file);
    std::stringstream content;
    content << in.rdbuf();
    const std::string keep = linesToKeep(content.str(), resetOne);
    in.close();

    if (keep.empty()) {
        remove(file.c_str());
        return 0;
    }
    std::ofstream out(file, std::ios::trunc);
    out << keep;
    return 1;
}
