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

int main()
{
    exePath();
    scribble();
    const bool ok = strcmp(programName(), "process-test") == 0 && strstr(exePath(), "/process-test") != nullptr;
    printf("%s  Programmname: \"%s\"\n", ok ? "ok   " : "FEHLER", programName());
    return ok ? 0 : 1;
}
