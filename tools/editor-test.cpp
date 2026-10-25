// editor-test: öffnet den Editor eines VST2-Plugins wie Resolve und nimmt ihn auf.
//
//   build/editor-test <plugin.so> <aufnahme.png> [Sekunden]
//
// Lädt über VSTPluginLoader::Load aus libresolve-vst.so (also mit Dispatcher, Callback,
// Inhaltsskalierung und Rahmen wie in Resolve). Größen sind wie bei Resolves Qt logische
// Pixel und werden mit RESOLVE_VST_EDITOR_SCALE multipliziert (ohne Qt gibt es sonst keinen
// Faktor). Am Ende: Fensterbaum auf stdout, Aufnahme des Editorfensters per ffmpeg.
// Die Wine-DPI stellt das Programm nicht ein (es heißt nicht resolve).
#include <X11/Xlib.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

struct AEffect;
using Callback = long (*)(AEffect *, int, int, long, void *, float);
struct AEffect {
    int32_t magic;
    Callback dispatcher;
};
struct ERect {
    int16_t top, left, bottom, right;
};
class VSTPluginLoader {
public:
    AEffect *Load(const wchar_t *path, Callback callback, bool &failed);
    unsigned char loaded;
};

namespace {

Display *dpy;
Window win;
double scale = 1.0;

void resize(int w, int h)
{
    XResizeWindow(dpy, win, int(w * scale), int(h * scale));
    XFlush(dpy);
}

long host(AEffect *, int opcode, int index, long value, void *, float)
{
    if (opcode == 1) // audioMasterVersion
        return 2400;
    if (opcode == 15) { // audioMasterSizeWindow
        printf("audioMasterSizeWindow %dx%ld (logisch)\n", index, value);
        resize(index, int(value));
        return 1;
    }
    return 0;
}

void tree(Window w, int depth)
{
    Window root, parent, *kids;
    unsigned n;
    XWindowAttributes a;
    XGetWindowAttributes(dpy, w, &a);
    printf("%*s%#lx %dx%d\n", depth * 2, "", w, a.width, a.height);
    if (depth < 2 && XQueryTree(dpy, w, &root, &parent, &kids, &n)) {
        for (unsigned i = 0; i < n; ++i)
            tree(kids[i], depth + 1);
        if (kids)
            XFree(kids);
    }
}

void rect(AEffect *e, const char *when)
{
    ERect *r = nullptr;
    e->dispatcher(e, 13, 0, 0, &r, 0); // effEditGetRect
    if (!r)
        return;
    printf("effEditGetRect %s: %dx%d (logisch)\n", when, r->right - r->left, r->bottom - r->top);
    resize(r->right - r->left, r->bottom - r->top);
}

} // namespace

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "Aufruf: %s <plugin.so> <aufnahme.png> [Sekunden]\n", argv[0]);
        return 2;
    }
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (const char *env = getenv("RESOLVE_VST_EDITOR_SCALE"))
        scale = atof(env);
    const int seconds = argc > 3 ? atoi(argv[3]) : 20;
    const std::wstring path(argv[1], argv[1] + strlen(argv[1]));

    dpy = XOpenDisplay(nullptr);
    if (!dpy) {
        fprintf(stderr, "keine X11-Verbindung\n");
        return 1;
    }
    win = XCreateSimpleWindow(dpy, DefaultRootWindow(dpy), 0, 0, 800, 600, 0, 0, 0x404040);
    XStoreName(dpy, win, "editor-test");
    XMapWindow(dpy, win);
    XFlush(dpy);

    VSTPluginLoader loader;
    bool failed = false;
    AEffect *e = loader.Load(path.c_str(), host, failed);
    if (!e) {
        fprintf(stderr, "Plugin nicht geladen (Log: ~/.local/state/resolve-vst/resolve-vst.log)\n");
        return 1;
    }
    e->dispatcher(e, 0, 0, 0, nullptr, 0);      // effOpen
    e->dispatcher(e, 10, 0, 0, nullptr, 48000); // effSetSampleRate
    e->dispatcher(e, 11, 0, 512, nullptr, 0);   // effSetBlockSize
    rect(e, "vor effEditOpen");
    e->dispatcher(e, 14, 0, 0, reinterpret_cast<void *>(win), 0); // effEditOpen
    rect(e, "nach effEditOpen");

    for (int i = 0; i < seconds * 50; ++i) {
        while (XPending(dpy)) {
            XEvent ev;
            XNextEvent(dpy, &ev);
        }
        e->dispatcher(e, 19, 0, 0, nullptr, 0); // effEditIdle
        usleep(20000);
    }
    tree(win, 0);
    const std::string cmd = "ffmpeg -loglevel error -y -f x11grab -window_id " + std::to_string(win) + " -i "
                            + getenv("DISPLAY") + " -frames:v 1 '" + argv[2] + "'";
    int status = system(cmd.c_str());
    // Ohne effClose beenden: Manche Plugins (MPC Beats über yabridge) hängen ohne
    // Ereignisschleife des Hosts beim Schließen; yabridge räumt den Wine-Prozess selbst ab.
    _Exit(status == 0 ? 0 : 1);
}
