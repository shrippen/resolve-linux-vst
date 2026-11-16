#include "scale.h"

#include <dlfcn.h>

#include <cstdlib>

namespace {

template<typename T>
T qtFunction(const char *name)
{
    return reinterpret_cast<T>(dlsym(RTLD_DEFAULT, name));
}

} // namespace

// Skalierungsfaktor (devicePixelRatio) des Fensters, in das das Plugin zeichnet; ohne
// Fenster der des Hauptbildschirms. RESOLVE_LINUX_VST_EDITOR_SCALE legt ihn fest (1 = aus).
double editorScale(unsigned long long windowId)
{
    if (const char *env = getenv("RESOLVE_LINUX_VST_EDITOR_SCALE")) {
        double s = atof(env);
        if (s > 0)
            return s;
    }
    using Find = void *(*)(unsigned long long);
    using WidgetFn = void *(*)(const void *);
    using Ratio = double (*)(const void *);
    if (windowId) {
        auto find = qtFunction<Find>("_ZN7QWidget4findEy");
        auto windowHandle = qtFunction<WidgetFn>("_ZNK7QWidget12windowHandleEv");
        auto topLevel = qtFunction<WidgetFn>("_ZNK7QWidget6windowEv");
        auto ratio = qtFunction<Ratio>("_ZNK7QWindow16devicePixelRatioEv");
        if (find && windowHandle && topLevel && ratio) {
            if (void *widget = find(windowId)) {
                void *window = windowHandle(widget);
                if (!window)
                    window = windowHandle(topLevel(widget));
                if (window)
                    return ratio(window);
            }
        }
    }
    auto primaryScreen = qtFunction<void *(*)()>("_ZN15QGuiApplication13primaryScreenEv");
    auto screenRatio = qtFunction<Ratio>("_ZNK7QScreen16devicePixelRatioEv");
    if (primaryScreen && screenRatio)
        if (void *screen = primaryScreen())
            return screenRatio(screen);
    return 1.0;
}
