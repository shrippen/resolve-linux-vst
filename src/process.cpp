// Pfad und Name des laufenden Programms. Einmal gelesen beim ersten Aufruf (Funktions-static,
// unabhängig von der Reihenfolge der Konstruktoren) und dann gemerkt.
#include "process.h"

#include <limits.h>
#include <unistd.h>

#include <cstring>

namespace {

struct Exe {
    char path[PATH_MAX];
};

const Exe &exe()
{
    static const Exe e = [] {
        Exe r = {};
        ssize_t n = readlink("/proc/self/exe", r.path, sizeof r.path - 1);
        r.path[n > 0 ? n : 0] = 0;
        return r;
    }();
    return e;
}

} // namespace

const char *exePath()
{
    return exe().path;
}

// Aus dem gemerkten Pfad, nicht beim Lesen: ein Zeiger in die Kopie wäre nur mit NRVO gültig.
const char *programName()
{
    const char *path = exe().path;
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

bool isResolveProcess()
{
    const char *name = programName();
    return strcmp(name, kProgResolve) == 0 || strcmp(name, kProgScanner) == 0 || strcmp(name, kProgEditorTest) == 0;
}
