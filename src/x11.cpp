// Fenstergrößen 0 von Plugins abfangen.
//
// X lehnt ConfigureWindow mit Breite oder Höhe 0 mit BadValue ab. Manche Plugins setzen ihr
// Fenster trotzdem auf 0×0: MiniMeters, wenn sein Zustand keine Fenstergröße enthält (Projekt
// gespeichert, bevor der Editor je offen war; die Variante Audio-Server speichert nie eine).
// Sein eigener X-Fehlerhandler ruft dann abort() und nimmt Resolve mit. Der Lader ersetzt
// XResizeWindow und XMoveResizeWindow und lässt solche Aufrufe weg; das Fenster behält seine
// Größe. Resolves Qt geht über xcb und kommt hier nicht vorbei.
//
// Ohne X11-Header (der Lader linkt nicht gegen libX11): Display und Window als void * und
// unsigned long, wie in Xlib.h.

#include <dlfcn.h>

#include <atomic>

#include "vst2.h"

#define EXPORT extern "C" __attribute__((visibility("default"), used))

namespace {

// Resolve lädt libX11 global, dort findet RTLD_NEXT sie. Sonst (Testprogramme, Plugins mit
// eigener libX11) die schon geladene Bibliothek.
template <typename F>
F real(const char *name)
{
    if (void *f = dlsym(RTLD_NEXT, name))
        return reinterpret_cast<F>(f);
    void *lib = dlopen("libX11.so.6", RTLD_LAZY | RTLD_NOLOAD);
    return lib ? reinterpret_cast<F>(dlsym(lib, name)) : nullptr;
}

void logOnce(const char *what, unsigned long window)
{
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true))
        logMessage("X11: %s(0x%lx) mit Größe 0 ausgelassen (weitere ohne Log)", what, window);
}

} // namespace

EXPORT int XResizeWindow(void *display, unsigned long window, unsigned width, unsigned height)
{
    if (width == 0 || height == 0) {
        logOnce("XResizeWindow", window);
        return 1;
    }
    static auto f = real<int (*)(void *, unsigned long, unsigned, unsigned)>("XResizeWindow");
    return f ? f(display, window, width, height) : 0;
}

EXPORT int XMoveResizeWindow(void *display, unsigned long window, int x, int y, unsigned width, unsigned height)
{
    if (width == 0 || height == 0) {
        logOnce("XMoveResizeWindow", window);
        return 1;
    }
    static auto f = real<int (*)(void *, unsigned long, int, int, unsigned, unsigned)>("XMoveResizeWindow");
    return f ? f(display, window, x, y, width, height) : 0;
}
