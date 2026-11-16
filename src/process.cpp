// Pfad und Name des laufenden Programms. Einmal gelesen beim ersten Aufruf (Funktions-static,
// unabhängig von der Reihenfolge der Konstruktoren) und dann gemerkt.
#include "process.h"

#include <limits.h>
#include <unistd.h>

#include <cstring>

namespace {

struct Exe {
    char path[PATH_MAX];
    const char *name;
};

const Exe &exe()
{
    static const Exe e = [] {
        Exe r = {};
        ssize_t n = readlink("/proc/self/exe", r.path, sizeof r.path - 1);
        if (n <= 0) {
            r.path[0] = 0;
            r.name = r.path;
            return r;
        }
        r.path[n] = 0;
        const char *slash = strrchr(r.path, '/');
        r.name = slash ? slash + 1 : r.path;
        return r;
    }();
    return e;
}

} // namespace

const char *exePath()
{
    return exe().path;
}

const char *programName()
{
    return exe().name;
}

bool isResolveProcess()
{
    const char *name = programName();
    return strcmp(name, kProgResolve) == 0 || strcmp(name, kProgScanner) == 0 || strcmp(name, kProgEditorTest) == 0;
}
