// Nikon N-RAW (.nev) als R3D.
//
// Resolve wählt den Decoder nach der Endung. Unter Linux gibt es für .nev keinen (unter
// Windows und macOS ein SDK von Nikon), der RED-Decoder (REDR3D-x64.so) liest die Dateien
// aber, wenn sie auf .R3D enden. Statt umzubenennen zeigt der Lader Resolve jede Datei X.NEV
// als X.R3D:
//
//  - Ordnerlisten (readdir, readdir64, readdir_r): Aus X.NEV wird X.R3D, außer es liegt dort
//    eine echte X.R3D.
//  - Zugriffe über den Pfad (open, fopen, stat, access, realpath): Gibt es X.R3D nicht, aber
//    X.NEV, geht der Zugriff an X.NEV. Nur lesend; realpath liefert wieder den Namen mit .R3D.
//
// Die Schreibweise bleibt: DSC_0001.NEV ↔ DSC_0001.R3D, a.nev ↔ a.r3d. Nur im Prozess
// resolve, nicht im VstScanner; RESOLVE_VST_NEV=0 schaltet es ab. Resolve kennt die Clips
// danach unter X.R3D: Ohne den Lader sind sie offline.
//
// Resolve ist gegen glibc 2.28 gebaut, stat & Co. kommen dort als __xstat, __lxstat (und
// …64) an. Auf x86_64 sind struct stat und stat64, dirent und dirent64 gleich.

#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "vst2.h"

#define EXPORT extern "C" __attribute__((visibility("default"), used))

namespace {

bool gActive = false;

template <typename F>
F real(const char *name)
{
    return reinterpret_cast<F>(dlsym(RTLD_NEXT, name));
}

bool exists(const char *path)
{
    struct stat st;
    return fstatat(AT_FDCWD, path, &st, 0) == 0;
}

// Endet name (Länge n) auf ".<ext>", gleich welche Schreibung? ext klein, drei Zeichen.
bool hasExtension(const char *name, size_t n, const char *ext)
{
    if (n < 5 || name[n - 4] != '.')
        return false;
    for (int i = 0; i < 3; ++i) {
        char c = name[n - 3 + i];
        if (c >= 'A' && c <= 'Z')
            c = char(c - 'A' + 'a');
        if (c != ext[i])
            return false;
    }
    return true;
}

// Tauscht die drei Zeichen nach dem Punkt, Schreibung je Buchstabe; die Ziffer 3 nimmt die
// Schreibung des ersten Buchstabens.
void toNev(char *ext)
{
    bool upper = ext[0] == 'R';
    ext[0] = upper ? 'N' : 'n';
    ext[1] = upper ? 'E' : 'e';
    ext[2] = ext[2] == 'D' ? 'V' : 'v';
}

void toR3d(char *ext)
{
    bool upper = ext[0] == 'N';
    ext[0] = upper ? 'R' : 'r';
    ext[1] = '3';
    ext[2] = ext[2] == 'V' ? 'D' : 'd';
}

void logOnce(const char *what, const char *path)
{
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true))
        logMessage("NEV: %s %s (weitere ohne Log)", what, path);
}

// X.R3D → X.NEV, wenn es X.R3D nicht gibt, aber X.NEV. Sonst path selbst.
const char *mapPath(const char *path, char (&buf)[PATH_MAX])
{
    if (!gActive || !path)
        return path;
    size_t n = strlen(path);
    if (n >= sizeof buf || !hasExtension(path, n, "r3d") || exists(path))
        return path;
    memcpy(buf, path, n + 1);
    char *ext = buf + n - 3;
    toNev(ext);
    if (exists(buf))
        return buf;
    // Andere Schreibung als abgeleitet (a.R3D zu a.nev und umgekehrt)
    bool upper = ext[0] == 'N';
    memcpy(ext, upper ? "nev" : "NEV", 3);
    return exists(buf) ? buf : path;
}

// Eintrag X.NEV → X.R3D, wenn im Ordner keine echte X.R3D liegt.
void renameEntry(DIR *dir, dirent *entry)
{
    if (!gActive || !entry)
        return;
    if (entry->d_type != DT_REG && entry->d_type != DT_LNK && entry->d_type != DT_UNKNOWN)
        return;
    size_t n = strlen(entry->d_name);
    if (!hasExtension(entry->d_name, n, "nev"))
        return;
    char r3d[NAME_MAX + 1];
    memcpy(r3d, entry->d_name, n + 1);
    toR3d(r3d + n - 3);
    struct stat st;
    if (fstatat(dirfd(dir), r3d, &st, 0) == 0)
        return;
    memcpy(entry->d_name, r3d, n + 1);
    logOnce("zeige", r3d);
}

bool readOnly(int flags)
{
    return (flags & O_ACCMODE) == O_RDONLY && !(flags & (O_CREAT | O_TRUNC));
}

bool readOnly(const char *mode)
{
    return mode && mode[0] == 'r' && !strchr(mode, '+');
}

using OpenFunction = int (*)(const char *, int, ...);

int openWith(OpenFunction f, const char *path, int flags, mode_t mode)
{
    char buf[PATH_MAX];
    const char *mapped = readOnly(flags) ? mapPath(path, buf) : path;
    if (mapped != path)
        logOnce("öffne", mapped);
    return f(mapped, flags, mode);
}

bool isResolve()
{
    char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0)
        return false;
    exe[n] = 0;
    const char *base = strrchr(exe, '/');
    return strcmp(base ? base + 1 : exe, "resolve") == 0;
}

__attribute__((constructor)) void init()
{
    const char *env = getenv("RESOLVE_VST_NEV");
    gActive = isResolve() && !(env && strcmp(env, "0") == 0);
}

} // namespace

EXPORT int open(const char *path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }
    static auto f = real<OpenFunction>("open");
    return openWith(f, path, flags, mode);
}

EXPORT int open64(const char *path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }
    static auto f = real<OpenFunction>("open64");
    return openWith(f, path, flags, mode);
}

EXPORT FILE *fopen(const char *path, const char *mode)
{
    static auto f = real<FILE *(*)(const char *, const char *)>("fopen");
    char buf[PATH_MAX];
    return f(readOnly(mode) ? mapPath(path, buf) : path, mode);
}

EXPORT FILE *fopen64(const char *path, const char *mode)
{
    static auto f = real<FILE *(*)(const char *, const char *)>("fopen64");
    char buf[PATH_MAX];
    return f(readOnly(mode) ? mapPath(path, buf) : path, mode);
}

// Ab glibc 2.33 gibt es __xstat nur noch für alte Programme (nicht über dlsym), darum direkt
// über fstatat. ver ist auf x86_64 immer 1 (_STAT_VER_LINUX), struct stat wie im Kernel.
// stat und lstat für Bibliotheken des Systems, die Resolve lädt.
EXPORT int stat(const char *path, struct stat *st)
{
    char buf[PATH_MAX];
    return fstatat(AT_FDCWD, mapPath(path, buf), st, 0);
}

EXPORT int stat64(const char *path, struct stat64 *st)
{
    char buf[PATH_MAX];
    return fstatat64(AT_FDCWD, mapPath(path, buf), st, 0);
}

EXPORT int lstat(const char *path, struct stat *st)
{
    char buf[PATH_MAX];
    return fstatat(AT_FDCWD, mapPath(path, buf), st, AT_SYMLINK_NOFOLLOW);
}

EXPORT int lstat64(const char *path, struct stat64 *st)
{
    char buf[PATH_MAX];
    return fstatat64(AT_FDCWD, mapPath(path, buf), st, AT_SYMLINK_NOFOLLOW);
}

EXPORT int __xstat(int, const char *path, struct stat *st)
{
    char buf[PATH_MAX];
    return fstatat(AT_FDCWD, mapPath(path, buf), st, 0);
}

EXPORT int __xstat64(int, const char *path, struct stat *st)
{
    char buf[PATH_MAX];
    return fstatat(AT_FDCWD, mapPath(path, buf), st, 0);
}

EXPORT int __lxstat(int, const char *path, struct stat *st)
{
    char buf[PATH_MAX];
    return fstatat(AT_FDCWD, mapPath(path, buf), st, AT_SYMLINK_NOFOLLOW);
}

EXPORT int __lxstat64(int, const char *path, struct stat *st)
{
    char buf[PATH_MAX];
    return fstatat(AT_FDCWD, mapPath(path, buf), st, AT_SYMLINK_NOFOLLOW);
}

EXPORT int access(const char *path, int mode)
{
    static auto f = real<int (*)(const char *, int)>("access");
    char buf[PATH_MAX];
    return f(mapPath(path, buf), mode);
}

EXPORT char *realpath(const char *path, char *resolved)
{
    static auto f = real<char *(*)(const char *, char *)>("realpath");
    char buf[PATH_MAX];
    const char *mapped = mapPath(path, buf);
    char *out = f(mapped, resolved);
    // Ergebnis wieder mit .R3D, damit Resolve beim Namen bleibt; über den Namen kommt es
    // wieder zur NEV-Datei (auch, wenn X.NEV ein Symlink war).
    if (out && mapped != path && hasExtension(out, strlen(out), "nev"))
        toR3d(out + strlen(out) - 3);
    return out;
}

EXPORT dirent *readdir(DIR *dir)
{
    static auto f = real<dirent *(*)(DIR *)>("readdir");
    dirent *entry = f(dir);
    renameEntry(dir, entry);
    return entry;
}

EXPORT dirent64 *readdir64(DIR *dir)
{
    static auto f = real<dirent64 *(*)(DIR *)>("readdir64");
    dirent64 *entry = f(dir);
    renameEntry(dir, reinterpret_cast<dirent *>(entry));
    return entry;
}

EXPORT int readdir_r(DIR *dir, dirent *entry, dirent **result)
{
    static auto f = real<int (*)(DIR *, dirent *, dirent **)>("readdir_r");
    int err = f(dir, entry, result);
    if (err == 0 && *result)
        renameEntry(dir, *result);
    return err;
}
