#include "editor_size.h"

#include <time.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include "editor_frame.h"
#include "editor_rect.h"
#include "log.h"
#include "plugin_conf.h"
#include "scale.h"

namespace {

// Editorgröße: Plugins melden ihre Größe (effEditGetRect, audioMasterSizeWindow) in
// Bildschirmpixeln, Resolves Qt nimmt die Zahlen als logische Pixel und multipliziert sie
// mit dem Skalierungsfaktor. Darum werden die Größen auf dem Weg durch diesen Faktor geteilt.
// Dazu hängt sich der Lader zwischen Resolve und Plugin: eigener Dispatcher, eigener Callback.

struct Instance {
    std::atomic<AEffect *> effect{nullptr};
    Dispatcher dispatcher = nullptr;
    Names names; // für plugins.conf
    double scale = 1.0;
    // Vergrößerung durch Wine (wine-zoom): Das Plugin meldet Größen in seinen Pixeln, auf dem
    // Bildschirm sind sie zoom-mal so groß.
    double zoom = 1.0;
    ERect rect{};
    // Rahmen (siehe fixFrame): Editorfenster von Resolve, Zielgröße in Bildschirmpixeln,
    // Zeitpunkt, bis zu dem bei effEditIdle nachgeprüft wird.
    std::atomic<unsigned long> window{0};
    std::atomic<int> targetW{0}, targetH{0};
    std::atomic<long> fixUntilMs{0};
    unsigned idleCount = 0;
};

// Feste Tabelle statt map: der Dispatcher läuft auch im Audio-Thread, Lesen ohne Lock.
Instance gInstances[512];
std::mutex gInstancesMutex;
std::atomic<audioMasterCallback> gHostCallback{nullptr};

Instance *findInstance(AEffect *effect)
{
    for (auto &in : gInstances)
        if (in.effect.load(std::memory_order_acquire) == effect)
            return &in;
    return nullptr;
}

// Skalierung für den Inhalt: JUCE-Plugins (und andere) nehmen sie als VST2 vom Host, über
// die Erweiterung von Bitwig/Ableton: effVendorSpecific mit index 'PreS', value 'AeCs',
// Faktor in opt. Resolve sendet sie nicht, darum hier vor dem Öffnen des Editors. Plugins,
// die sie nicht kennen, ignorieren den Aufruf. Manche nehmen die Zeichengröße trotzdem vom
// Bildschirm und rechnen nur die Fenstergröße mit dem Faktor (MPC Beats unter Wine): Dann
// bleibt ein Rand. Darum je Plugin abschaltbar.
// content-scale in plugins.conf, sonst RESOLVE_LINUX_VST_CONTENT_SCALE: auto (Standard) = Resolves
// Faktor, Zahl = fest, 0 = nicht senden.
void sendContentScale(Instance *in, AEffect *effect)
{
    std::string setting = pluginSetting(in->names, "content-scale");
    if (setting.empty())
        if (const char *env = getenv("RESOLVE_LINUX_VST_CONTENT_SCALE"))
            setting = env;
    if (setting.empty() && in->zoom != 1.0) {
        logMessage("  keine Inhaltsskalierung für %s: Wine vergrößert (%.2f)", in->names[0].c_str(), in->zoom);
        return;
    }
    const double scale = setting.empty() || setting == "auto" ? in->scale : atof(setting.c_str());
    if (scale <= 0) {
        logMessage("  Inhaltsskalierung für %s abgeschaltet", in->names[0].c_str());
        return;
    }
    long result = in->dispatcher(effect, effVendorSpecific, kPreS, kAeCs, nullptr, float(scale));
    logMessage("  Inhaltsskalierung %.2f an Plugin %p (Antwort %ld)", scale, static_cast<void *>(effect), result);
}

long nowMs()
{
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

// Rahmen: yabridge bettet das Wine-Fenster in ein eigenes Zwischenfenster im Editorfenster
// des Hosts ein und gibt ihm die Größe aus effEditGetRect beim Öffnen. Ändert das Plugin
// danach seine Größe (JUCE nach 'PreS'/'AeCs'), wächst das Zwischenfenster nicht mit und
// schneidet den Editor ab; JUCE-Plugins unter Wine melden nach dem Öffnen zudem wieder ihre
// Grundgröße. Darum bringt der Lader die Unterfenster von Resolves Editorfenster auf die
// Zielgröße: die Größe vor dem Öffnen bzw. die letzte von audioMasterSizeWindow.
// Das Fenstersystem spricht editor_frame.cpp. RESOLVE_LINUX_VST_FIX_FRAME=0 schaltet ab.

bool frameFixEnabled()
{
    const char *env = getenv("RESOLVE_LINUX_VST_FIX_FRAME");
    return !env || strcmp(env, "0") != 0;
}

void fixFrame(Instance *in)
{
    const uint32_t window = uint32_t(in->window.load());
    const int w = in->targetW.load(), h = in->targetH.load();
    if (!window || w <= 0 || h <= 0 || !frameFixEnabled())
        return;
    resizeChildWindows(window, w, h);
}

// Editorgröße abfragen: Größe des Plugins in Bildschirmpixeln, durch den Skalierungsfaktor
// geteilt an Resolve.
long editGetRect(Instance *in, AEffect *effect, int opcode, int index, long value, void *ptr, float opt)
{
    long result = in->dispatcher(effect, opcode, index, value, ptr, opt);
    auto **rect = static_cast<ERect **>(ptr);
    if (rect && *rect) {
        const ERect reported = **rect;
        // In Bildschirmpixeln (bei wine-zoom größer als gemeldet).
        ERect r = editorScreenRect(reported, in->zoom);
        const int w = r.right, h = r.bottom;
        bool replaced = false;
        if (!in->window.load()) {
            in->targetW.store(w);
            in->targetH.store(h);
        } else if (frameFixEnabled() && editorBelowTarget(w, h, in->targetW.load(), in->targetH.load())) {
            // Nach dem Öffnen kleiner als vorher, ohne audioMasterSizeWindow: JUCE unter Wine
            // meldet dann seine Grundgröße, gezeichnet wird aber in der Zielgröße.
            r = editorTargetRect(in->targetW.load(), in->targetH.load());
            replaced = true;
        }
        if (in->scale != 1.0 || in->zoom != 1.0 || replaced) {
            // Eigene Kopie: Das ERect gehört dem Plugin.
            in->rect = editorToResolveRect(r, in->scale);
            *rect = &in->rect;
        }
        logMessage("Editorgröße: Plugin %p meldet %dx%d%s, an Resolve %dx%d", static_cast<void *>(effect),
             reported.right - reported.left, reported.bottom - reported.top, replaced ? " (ersetzt)" : "",
             (*rect)->right - (*rect)->left, (*rect)->bottom - (*rect)->top);
    }
    return result;
}

long dispatch(AEffect *effect, int opcode, int index, long value, void *ptr, float opt)
{
    Instance *in = findInstance(effect);
    if (!in)
        return 0;
    switch (opcode) {
    case effEditOpen: {
        in->scale = editorScale(reinterpret_cast<uintptr_t>(ptr));
        logMessage("Editor öffnen: Plugin %p, Fenster %#lx, Skalierung %.2f", static_cast<void *>(effect),
             static_cast<unsigned long>(reinterpret_cast<uintptr_t>(ptr)), in->scale);
        sendContentScale(in, effect);
        long result = in->dispatcher(effect, opcode, index, value, ptr, opt);
        in->window.store(reinterpret_cast<uintptr_t>(ptr));
        in->fixUntilMs.store(nowMs() + 10000);
        fixFrame(in);
        return result;
    }
    case effEditClose:
        in->window.store(0);
        return in->dispatcher(effect, opcode, index, value, ptr, opt);
    case effEditIdle:
        // Kurz nach dem Öffnen bzw. einer Größenänderung den Rahmen nachprüfen (yabridge
        // setzt ihn beim Öffnen selbst und kann dem Lader zuvorkommen).
        if (in->window.load() && nowMs() < in->fixUntilMs.load() && ++in->idleCount % 5 == 0)
            fixFrame(in);
        return in->dispatcher(effect, opcode, index, value, ptr, opt);
    case effEditGetRect:
        return editGetRect(in, effect, opcode, index, value, ptr, opt);
    case effClose: {
        long result = in->dispatcher(effect, opcode, index, value, ptr, opt);
        in->effect.store(nullptr, std::memory_order_release);
        return result;
    }
    default:
        return in->dispatcher(effect, opcode, index, value, ptr, opt);
    }
}

} // namespace

long editorCallback(AEffect *effect, int opcode, int index, long value, void *ptr, float opt)
{
    if (opcode == audioMasterSizeWindow && effect) {
        Instance *in = findInstance(effect);
        if (in) {
            in->targetW.store(editorZoomed(index, in->zoom));
            in->targetH.store(editorZoomed(value, in->zoom));
            in->fixUntilMs.store(nowMs() + 5000);
        }
        if (in && (in->scale != 1.0 || in->zoom != 1.0)) {
            const long toW = editorToResolve(index, in->zoom, in->scale);
            const long toH = editorToResolve(value, in->zoom, in->scale);
            logMessage("Plugin %p will %dx%d, an Resolve %ldx%ld", static_cast<void *>(effect), index, int(value),
                 toW, toH);
            index = static_cast<int>(toW);
            value = toH;
        }
        long result = gHostCallback.load(std::memory_order_relaxed)(effect, opcode, index, value, ptr, opt);
        logMessage("  audioMasterSizeWindow: Resolve antwortet %ld", result);
        if (in)
            fixFrame(in);
        return result;
    }
    return gHostCallback.load(std::memory_order_relaxed)(effect, opcode, index, value, ptr, opt);
}

void wrapEditor(AEffect *effect, const Names &names, double zoom)
{
    std::lock_guard lock(gInstancesMutex);
    for (auto &in : gInstances) {
        if (in.effect.load(std::memory_order_relaxed))
            continue;
        in.dispatcher = effect->dispatcher;
        in.names = names;
        in.scale = editorScale();
        in.zoom = zoom;
        in.window.store(0);
        in.targetW.store(0);
        in.targetH.store(0);
        in.fixUntilMs.store(0);
        in.idleCount = 0;
        in.effect.store(effect, std::memory_order_release);
        effect->dispatcher = dispatch;
        return;
    }
    logMessage("  zu viele Plugin-Instanzen, Editorgröße wird nicht angepasst");
}

void setEditorHost(audioMasterCallback callback)
{
    gHostCallback.store(callback, std::memory_order_relaxed);
}
