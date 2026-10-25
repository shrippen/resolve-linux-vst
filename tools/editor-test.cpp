// editor-test: öffnet den Editor eines Plugins wie Resolve und nimmt ihn auf.
//
//   build/editor-test <plugin.so | Klasse.vst> <aufnahme.png> [Sekunden]
//
// VST3-Plugins über ihren Eintrag Klasse.vst aus resolve-vst link (VST3-Adapter). Vor dem
// Editor laufen einige Blöcke Audio (Sinus) durch das Plugin, die Pegel stehen auf stdout.
//
// Lädt über VSTPluginLoader::Load aus libresolve-vst.so (also mit Dispatcher, Callback,
// Inhaltsskalierung und Rahmen wie in Resolve). Größen sind wie bei Resolves Qt logische
// Pixel und werden mit RESOLVE_VST_EDITOR_SCALE multipliziert (ohne Qt gibt es sonst keinen
// Faktor). Am Ende: Fensterbaum auf stdout, Aufnahme des Editorfensters per ffmpeg.
// Die Wine-DPI stellt das Programm nicht ein (es heißt nicht resolve).
#include <X11/Xlib.h>
#include <glib.h>

#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>

#include "../src/vst2.h"
using Callback = audioMasterCallback;
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

    // X-Fehler melden statt beenden (gilt für alle Xlib-Verbindungen im Prozess, auch die
    // des Plugins; manche Plugins stolpern ohne Fenstermanager, z. B. in Xvfb).
    XSetErrorHandler([](Display *, XErrorEvent *e) {
        fprintf(stderr, "X-Fehler %d (Anfrage %d)\n", e->error_code, e->request_code);
        return 0;
    });
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
    e->dispatcher(e, 12, 0, 1, nullptr, 0);     // effMainsChanged an
    printf("%d Parameter, %d→%d Kanäle\n", e->numParams, e->numInputs, e->numOutputs);
    {
        // 20 Blöcke Sinus 1 kHz, -12 dBFS, auf allen Eingängen; Pegel des letzten Blocks.
        const int n = 512;
        std::vector<std::vector<float>> in(size_t(e->numInputs), std::vector<float>(n)), out(size_t(e->numOutputs), std::vector<float>(n));
        std::vector<float *> ip, op;
        for (auto &c : in)
            ip.push_back(c.data());
        for (auto &c : out)
            op.push_back(c.data());
        double rmsIn = 0, rmsOut = 0;
        for (int b = 0; b < 20; ++b) {
            for (auto &c : in)
                for (int i = 0; i < n; ++i)
                    c[size_t(i)] = 0.25f * float(sin(2 * M_PI * 1000 * (b * n + i) / 48000.0));
            e->processReplacing(e, ip.data(), op.data(), n);
        }
        for (auto &c : in)
            for (float v : c)
                rmsIn += double(v) * v;
        for (auto &c : out)
            for (float v : c)
                rmsOut += double(v) * v;
        auto db = [](double sum, size_t count) { return count ? 10 * log10(sum / double(count) + 1e-20) : -200.0; };
        printf("Audio: Eingang %.1f dBFS, Ausgang %.1f dBFS\n", db(rmsIn, in.size() * n), db(rmsOut, out.size() * n));
    }
    if (e->numParams > 0) {
        char name[256] = {}, display[256] = {}, label[256] = {};
        e->dispatcher(e, 8, 0, 0, name, 0);
        e->dispatcher(e, 7, 0, 0, display, 0);
        e->dispatcher(e, 6, 0, 0, label, 0);
        printf("Parameter 0: %s = %s %s (%.3f)\n", name, display, label, e->getParameter(e, 0));
    }
    void *chunk = nullptr;
    long chunkSize = e->dispatcher(e, 23, 0, 0, &chunk, 0); // effGetChunk
    if (chunkSize > 0) {
        std::vector<char> copy(static_cast<char *>(chunk), static_cast<char *>(chunk) + chunkSize);
        long ok = e->dispatcher(e, 24, 0, chunkSize, copy.data(), 0); // effSetChunk
        printf("Chunk: %ld Bytes, zurückgeschrieben: %ld\n", chunkSize, ok);
    }
    rect(e, "vor effEditOpen");
    e->dispatcher(e, 14, 0, 0, reinterpret_cast<void *>(win), 0); // effEditOpen
    rect(e, "nach effEditOpen");

    for (int i = 0; i < seconds * 50; ++i) {
        while (XPending(dpy)) {
            XEvent ev;
            XNextEvent(dpy, &ev);
        }
        e->dispatcher(e, 19, 0, 0, nullptr, 0); // effEditIdle
        while (g_main_context_iteration(nullptr, FALSE)) // IRunLoop des VST3-Adapters
            ;
        usleep(20000);
    }
    tree(win, 0);
    // Ausschnitt des Bildschirms an der Fensterposition (x11grab -window_id scheitert unter
    // Xwayland an Fenstern, die nicht ganz sichtbar sind).
    XWindowAttributes before;
    XGetWindowAttributes(dpy, win, &before);
    printf("Fenster %s\n", before.map_state == IsViewable ? "sichtbar" : "nicht sichtbar, wird gezeigt");
    XMapRaised(dpy, win);
    XSync(dpy, False);
    usleep(300000);
    XWindowAttributes a;
    XGetWindowAttributes(dpy, win, &a);
    int x = 0, y = 0;
    Window child;
    XTranslateCoordinates(dpy, win, DefaultRootWindow(dpy), 0, 0, &x, &y, &child);
    printf("Fenster bei %d,%d\n", x, y);
    const std::string cmd = "ffmpeg -loglevel error -y -f x11grab -video_size " + std::to_string(a.width) + "x"
                            + std::to_string(a.height) + " -i " + getenv("DISPLAY") + "+" + std::to_string(x) + ","
                            + std::to_string(y) + " -frames:v 1 '" + argv[2] + "'";
    int status = system(cmd.c_str());
    // Sauber schließen wie Resolve (eine yabridge-Plugingruppe hängt sonst beim nächsten
    // Editor). Manche Plugins (MPC Beats) hängen beim Schließen ohne Ereignisschleife des
    // Hosts: nach 10 s ohne Aufräumen beenden.
    signal(SIGALRM, [](int) { _Exit(3); });
    alarm(10);
    e->dispatcher(e, 15, 0, 0, nullptr, 0); // effEditClose
    e->dispatcher(e, 12, 0, 0, nullptr, 0); // effMainsChanged aus
    e->dispatcher(e, 1, 0, 0, nullptr, 0);  // effClose
    _Exit(status == 0 ? 0 : 1);
}
