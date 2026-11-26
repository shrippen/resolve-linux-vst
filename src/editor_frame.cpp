#include "editor_frame.h"

#include <dlfcn.h>

#include <cstdlib>
#include <mutex>
#include <type_traits>

#include "log.h"

namespace {

// Über xcb, nicht Xlib: Xlib beendet bei X-Fehlern (Fenster schon weg) sonst den Prozess,
// ein eigener Fehler-Handler gälte für ganz Resolve. xcb wird zur Laufzeit geladen, die
// Strukturen sind nachgebaut (xcb/xproto.h), damit der Lader keine X11-Header braucht.

constexpr const char *kXcbLibrary = "libxcb.so.1";

// Wertemaske von xcb_configure_window: XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT.
constexpr uint16_t kXcbConfigWidth = 4;
constexpr uint16_t kXcbConfigHeight = 8;

constexpr size_t kQueryTreePad = 14;
constexpr size_t kGeometryPad = 2;

struct XcbCookie {
    unsigned sequence;
};
struct XcbQueryTreeReply {
    uint8_t responseType, pad0;
    uint16_t sequence;
    uint32_t length, root, parent;
    uint16_t childrenLen;
    uint8_t pad1[kQueryTreePad];
};
struct XcbGeometryReply {
    uint8_t responseType, depth;
    uint16_t sequence;
    uint32_t length, root;
    int16_t x, y;
    uint16_t width, height, borderWidth;
    uint8_t pad0[kGeometryPad];
};

struct Xcb {
    void *(*connect)(const char *, int *);
    int (*hasError)(void *);
    XcbCookie (*queryTree)(void *, uint32_t);
    XcbQueryTreeReply *(*queryTreeReply)(void *, XcbCookie, void **);
    uint32_t *(*children)(const XcbQueryTreeReply *);
    int (*childrenLength)(const XcbQueryTreeReply *);
    XcbCookie (*getGeometry)(void *, uint32_t);
    XcbGeometryReply *(*getGeometryReply)(void *, XcbCookie, void **);
    XcbCookie (*configureWindow)(void *, uint32_t, uint16_t, const uint32_t *);
    void *(*pollForEvent)(void *);
    int (*flush)(void *);
    void *connection = nullptr;
};

// Holt alle Funktionen aus der Bibliothek; false, wenn eine fehlt.
bool loadSymbols(Xcb &x, void *lib)
{
    auto sym = [lib](auto &fn, const char *name) {
        fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(lib, name));
        return fn != nullptr;
    };
    return sym(x.connect, "xcb_connect") && sym(x.hasError, "xcb_connection_has_error")
        && sym(x.queryTree, "xcb_query_tree") && sym(x.queryTreeReply, "xcb_query_tree_reply")
        && sym(x.children, "xcb_query_tree_children") && sym(x.childrenLength, "xcb_query_tree_children_length")
        && sym(x.getGeometry, "xcb_get_geometry") && sym(x.getGeometryReply, "xcb_get_geometry_reply")
        && sym(x.configureWindow, "xcb_configure_window") && sym(x.pollForEvent, "xcb_poll_for_event")
        && sym(x.flush, "xcb_flush");
}

// Lädt xcb und verbindet einmal; nullptr, wenn das nicht geht (der Versuch gilt dann als gescheitert).
Xcb *xcb()
{
    static Xcb x;
    static bool tried = false;
    if (tried)
        return x.connection ? &x : nullptr;
    tried = true;
    void *lib = dlopen(kXcbLibrary, RTLD_NOW | RTLD_LOCAL);
    if (!lib)
        return nullptr;
    if (!loadSymbols(x, lib))
        return nullptr;
    void *c = x.connect(nullptr, nullptr);
    if (!c || x.hasError(c)) {
        logMessage("Rahmen: keine X11-Verbindung");
        return nullptr;
    }
    x.connection = c;
    return &x;
}

} // namespace

void resizeChildWindows(uint32_t window, int w, int h)
{
    static std::mutex mutex;
    std::lock_guard lock(mutex);
    Xcb *x = xcb();
    if (!x)
        return;
    void *c = x->connection;
    void *error = nullptr;
    XcbQueryTreeReply *tree = x->queryTree ? x->queryTreeReply(c, x->queryTree(c, window), &error) : nullptr;
    free(error);
    if (!tree)
        return;
    const uint32_t *kids = x->children(tree);
    for (int i = 0, n = x->childrenLength(tree); i < n; ++i) {
        error = nullptr;
        XcbGeometryReply *g = x->getGeometryReply(c, x->getGeometry(c, kids[i]), &error);
        free(error);
        if (g && (g->width != w || g->height != h)) {
            const uint32_t size[] = {uint32_t(w), uint32_t(h)};
            x->configureWindow(c, kids[i], kXcbConfigWidth | kXcbConfigHeight, size);
            logMessage("Rahmen: Unterfenster %#x von %dx%d auf %dx%d", kids[i], g->width, g->height, w, h);
        }
        free(g);
    }
    free(tree);
    x->flush(c);
    // Fehler unbestätigter Anfragen landen in der Ereigniswarteschlange: abräumen.
    while (void *event = x->pollForEvent(c))
        free(event);
}
