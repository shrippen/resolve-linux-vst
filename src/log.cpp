#include "log.h"

#include <cstdarg>
#include <ctime>
#include <unistd.h>

namespace {

FILE *gLog = nullptr;

} // namespace

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
