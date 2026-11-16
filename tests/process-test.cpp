// Test des Programmnamens (src/process.cpp). Gebaut mit -fno-elide-constructors: Der Name darf
// nicht auf eine Kopie zeigen, die nur durch weggelassene Kopien (NRVO) gültig bleibt.
#include "../src/process.h"

#include <cstdio>
#include <cstring>

namespace {

// Überschreibt den Stapel, auf dem das erste Lesen lief.
__attribute__((noinline)) void scribble()
{
    volatile char buf[2 * 4096 * 4];
    for (auto &c : buf) {
        c = 'X';
    }
}

} // namespace

// Erwartete Antworten je Programmname; make test startet dieselbe Datei unter jedem Namen.
struct Expect {
    const char *name;
    bool resolveMain;
    bool likeResolve;
};
constexpr Expect kExpect[] = {
    {"process-test", false, false},
    {kProgResolve, true, true},
    {kProgScanner, false, true},
    {kProgEditorTest, false, true},
};

int main()
{
    exePath();
    scribble();
    const char *name = programName();
    const char *slash = strrchr(exePath(), '/');
    const Expect *want = nullptr;
    for (const Expect &e : kExpect) {
        if (strcmp(name, e.name) == 0) {
            want = &e;
        }
    }
    const bool ok = want && slash && strcmp(slash + 1, name) == 0;
    printf("%s  Programmname: \"%s\"\n", ok ? "ok   " : "FEHLER", name);
    if (!ok) {
        return 1;
    }

    const bool askOk = isResolveMain() == want->resolveMain && loadsLikeResolve() == want->likeResolve;
    printf("%s  %s: isResolveMain %d, loadsLikeResolve %d\n", askOk ? "ok   " : "FEHLER", name, isResolveMain(),
           loadsLikeResolve());
    return askOk ? 0 : 1;
}
