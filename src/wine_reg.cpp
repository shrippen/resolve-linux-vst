#include "wine_reg.h"

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "log.h"
#include "wine_restore.h"

namespace {

// Umgebung für Wine: die eigene, ohne WINEPREFIX, WINEDEBUG und LD_PRELOAD, dazu das Prefix
// und WINEDEBUG=-all.
std::vector<std::string> wineEnv(const std::string &prefix)
{
    std::vector<std::string> env;
    for (char **e = environ; *e; ++e)
        if (strncmp(*e, "WINEPREFIX=", 11) != 0 && strncmp(*e, "WINEDEBUG=", 10) != 0
            && strncmp(*e, "LD_PRELOAD=", 11) != 0)
            env.emplace_back(*e);
    env.push_back("WINEPREFIX=" + prefix);
    env.push_back("WINEDEBUG=-all");
    return env;
}

// Führt `wine <args>` im Prefix aus, über $WINELOADER wie yabridge (z. B. Runner aus
// bottle.yml). Mit `out` wird stdout gesammelt. Liefert den Exit-Code, -1 wenn Wine nicht
// startet.
int runWine(const std::string &prefix, std::vector<std::string> args, std::string *out = nullptr)
{
    const char *loader = getenv("WINELOADER");
    args.insert(args.begin(), loader && *loader ? loader : "wine");
    std::vector<char *> argv;
    for (auto &a : args)
        argv.push_back(a.data());
    argv.push_back(nullptr);

    std::vector<std::string> env = wineEnv(prefix);
    std::vector<char *> envp;
    for (auto &e : env)
        envp.push_back(e.data());
    envp.push_back(nullptr);

    int fds[2] = {-1, -1};
    if (out && pipe2(fds, O_CLOEXEC) != 0)
        return -1;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    if (out)
        posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
    else
        posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    pid_t pid;
    int err = posix_spawnp(&pid, argv[0], &actions, nullptr, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&actions);
    if (out) {
        close(fds[1]);
        char buf[4096];
        ssize_t n;
        while (err == 0 && (n = read(fds[0], buf, sizeof buf)) > 0)
            out->append(buf, n);
        close(fds[0]);
    }
    if (err != 0) {
        logMessage("  Wine-DPI: %s nicht startbar: %s", argv[0], strerror(err));
        return -1;
    }
    int status = 0;
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status))
        return -1;
    return WEXITSTATUS(status);
}

constexpr const char *kDpiKey = "HKCU\\Control Panel\\Desktop";
constexpr const char *kDpiValue = "LogPixels";

// Löscht den Wert über `wine reg delete` (er war vor Resolve nicht gesetzt).
bool deleteWineDpi(const std::string &prefix)
{
    return runWine(prefix, {"reg", "delete", kDpiKey, "/v", kDpiValue, "/f"}) == 0;
}

} // namespace

// Aktueller Wert über `wine reg query`, nicht aus user.reg: wineserver schreibt die Datei
// verzögert, direkt nach einem Zurücksetzen stünde dort noch der alte Wert.
// kWineDpiUnset = nicht gesetzt, kWineQueryFailed = Abfrage nicht möglich.
int queryWineDpi(const std::string &prefix)
{
    std::string out;
    int status = runWine(prefix, {"reg", "query", kDpiKey, "/v", kDpiValue}, &out);
    if (status < 0)
        return kWineQueryFailed;
    unsigned value;
    size_t pos = out.find("REG_DWORD");
    if (pos != std::string::npos && sscanf(out.c_str() + pos, "REG_DWORD %x", &value) == 1)
        return int(value);
    return kWineDpiUnset;
}

// Schreibt die DPI des Prefix über `wine reg add`.
bool writeWineDpi(const std::string &prefix, int dpi)
{
    return runWine(prefix, {"reg", "add", kDpiKey, "/v", kDpiValue, "/t", "REG_DWORD", "/d", std::to_string(dpi),
                            "/f"}) == 0;
}

bool restoreWineDpi(const std::string &prefix, int dpi)
{
    return dpi == kWineDpiUnset ? deleteWineDpi(prefix) : writeWineDpi(prefix, dpi);
}
