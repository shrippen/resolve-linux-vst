// vst3-classes: listet die Effektklassen eines VST3-Bundles, je Zeile ID, Name und
// Unterkategorien (Tab-getrennt). resolve-linux-vst legt daraus die Einträge Name.vst an.
//
//   build/vst3-classes <Bundle.vst3>
#include "../src/vst2.h"

#include <cstdarg>
#include <cstdio>
#include <unistd.h>

void logMessage(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "Aufruf: %s <Bundle.vst3>\n", argv[0]);
        return 2;
    }
    // Ohne Aufräumen beenden: Module werden nie entladen, und manche (yabridge) hängen
    // sonst in ihren Destruktoren.
    _exit(vst3ListClasses(argv[1]));
}
