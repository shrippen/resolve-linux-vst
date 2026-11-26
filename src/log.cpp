#include "log.h"

#include <cstdarg>
#include <cstdlib>
#include <ctime>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {

FILE *gLog = nullptr;

} // namespace

// Dieselbe Regel steht in resolve-linux-vst.
std::string stateDir()
{
    std::string base;
    if (const char *xdg = getenv("XDG_STATE_HOME"); xdg && *xdg)
        base = xdg;
    else if (const char *home = getenv("HOME"))
        base = std::string(home) + "/.local/state";
    else
        return {};
    mkdir(base.c_str(), 0755);
    mkdir((base + "/resolve-linux-vst").c_str(), 0755);
    return base + "/resolve-linux-vst";
}

void openLog()
{
    std::string path;
    if (const char *env = getenv("RESOLVE_LINUX_VST_LOG"))
        path = env;
    else if (const std::string dir = stateDir(); !dir.empty())
        path = dir + "/resolve-linux-vst.log";
    if (!path.empty() && path != "-")
        setLogFile(fopen(path.c_str(), "a"));
    else if (path == "-")
        setLogFile(stderr);
}

void setLogFile(FILE *file)
{
    gLog = file;
}

void logMessage(const char *fmt, ...)
{
    if (!gLog) {
        return;
    }

    char stamp[32];
    time_t now = time(nullptr);
    strftime(stamp, sizeof stamp, "%F %T", localtime(&now));
    fprintf(gLog, "%s [%d] ", stamp, getpid());

    va_list ap;
    va_start(ap, fmt);
    vfprintf(gLog, fmt, ap);
    va_end(ap);

    fputc('\n', gLog);
    fflush(gLog);
}
