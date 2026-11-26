// Threadpool des N-RAW-Decoders (siehe nraw_pool.h).
//
//   parallelFor ──► Auftrag {fn, ctx, n, next}  ◄── Arbeiter 1..T-1 holen sich Index für Index
//        └─ arbeitet selbst mit und wartet, bis kein Arbeiter mehr im Auftrag ist
#include "nraw_pool.h"

#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace {

constexpr const char *kEnvThreads = "RESOLVE_LINUX_VST_NRAW_THREADS";
constexpr unsigned kDefaultThreads = 12;
constexpr unsigned kMaxThreads = 64;
constexpr size_t kStackBytes = 256 * 1024;
constexpr size_t kNameBytes = 16; // inklusive Nullbyte (pthread_setname_np)

using Fn = void (*)(size_t, void *);

struct Job {
    Fn fn = nullptr;
    void *ctx = nullptr;
    size_t n = 0;
};

struct Pool {
    std::mutex lock;
    std::condition_variable wake; // neuer Auftrag
    std::condition_variable idle; // active == 0
    std::mutex runLock;           // ein Auftrag zur Zeit
    Job job;
    std::atomic<size_t> next{0};
    uint64_t generation = 0;
    unsigned active = 0;  // Arbeiter, die im aktuellen Auftrag stecken
    unsigned workers = 0; // gestartete Arbeiter
    unsigned limit = 0;   // so viele nehmen am Auftrag teil
    cpu_set_t mask;
};

struct WorkerArg {
    Pool *pool;
    unsigned index;
};

std::atomic<bool> gDead{false}; // im Kind nach fork()
std::atomic<unsigned> gThreads{0};
Pool *gPool = nullptr;
std::mutex gCreate;

void childAfterFork()
{
    gDead.store(true);
}

unsigned configuredThreads()
{
    unsigned n = gThreads.load();
    if (n != 0) {
        return n;
    }

    n = kDefaultThreads;
    const char *env = getenv(kEnvThreads);
    long value = env ? strtol(env, nullptr, 10) : 0;
    if (value >= 1 && value <= long(kMaxThreads)) {
        n = unsigned(value);
    }
    gThreads.store(n);
    return n;
}

void drain(Pool *p, const Job &job)
{
    while (true) {
        size_t i = p->next.fetch_add(1, std::memory_order_relaxed);
        if (i >= job.n) {
            return;
        }
        job.fn(i, job.ctx);
    }
}

void *workerMain(void *raw)
{
    WorkerArg arg = *static_cast<WorkerArg *>(raw);
    delete static_cast<WorkerArg *>(raw);
    Pool *p = arg.pool;

    char name[kNameBytes];
    snprintf(name, sizeof name, "nraw-%u", arg.index);
    pthread_setname_np(pthread_self(), name);

    uint64_t seen = 0;
    while (true) {
        Job job;
        {
            std::unique_lock<std::mutex> guard(p->lock);
            p->wake.wait(guard, [&] { return p->generation != seen; });
            seen = p->generation;
            if (arg.index > p->limit) {
                continue;
            }
            job = p->job;
            ++p->active;
        }

        drain(p, job);

        std::lock_guard<std::mutex> guard(p->lock);
        if (--p->active == 0) {
            p->idle.notify_all();
        }
    }
    return nullptr;
}

// Startet einen Arbeiter: alle Signale blockiert, Affinität des Prozesses, SCHED_OTHER, kleiner Stack.
bool spawn(Pool *p, unsigned index)
{
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, kStackBytes);
    pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&attr, SCHED_OTHER);
    sched_param param{};
    pthread_attr_setschedparam(&attr, &param);
    pthread_attr_setaffinity_np(&attr, sizeof p->mask, &p->mask);

    sigset_t all, old;
    sigfillset(&all);
    pthread_sigmask(SIG_SETMASK, &all, &old);

    pthread_t thread;
    int rc = pthread_create(&thread, &attr, workerMain, new WorkerArg{p, index});
    pthread_sigmask(SIG_SETMASK, &old, nullptr);
    pthread_attr_destroy(&attr);
    return rc == 0;
}

Pool *createPool()
{
    std::lock_guard<std::mutex> guard(gCreate);
    if (gPool) {
        return gPool;
    }

    Pool *p = new Pool; // nie freigegeben
    CPU_ZERO(&p->mask);
    if (sched_getaffinity(getpid(), sizeof p->mask, &p->mask) != 0) {
        CPU_ZERO(&p->mask);
        for (int i = 0; i < CPU_SETSIZE; i++) {
            CPU_SET(i, &p->mask);
        }
    }
    pthread_atfork(nullptr, nullptr, childAfterFork);
    gPool = p;
    return p;
}

void serial(size_t n, Fn fn, void *ctx)
{
    for (size_t i = 0; i < n; i++) {
        fn(i, ctx);
    }
}

void run(Pool *p, unsigned threads, size_t n, Fn fn, void *ctx)
{
    std::unique_lock<std::mutex> guard(p->lock);
    p->idle.wait(guard, [&] { return p->active == 0; }); // Nachzügler des letzten Auftrags

    while (p->workers + 1 < threads && spawn(p, p->workers + 1)) {
        ++p->workers;
    }
    p->limit = std::min(threads - 1, p->workers);
    p->job = Job{fn, ctx, n};
    p->next.store(0, std::memory_order_relaxed);
    ++p->generation;
    guard.unlock();
    p->wake.notify_all();

    drain(p, Job{fn, ctx, n});

    guard.lock();
    p->idle.wait(guard, [&] { return p->active == 0; });
}

} // namespace

void parallelFor(size_t n, Fn fn, void *ctx)
{
    unsigned threads = configuredThreads();
    if (n < 2 || threads < 2 || gDead.load()) {
        serial(n, fn, ctx);
        return;
    }

    Pool *p = gPool ? gPool : createPool();
    if (!p->runLock.try_lock()) {
        serial(n, fn, ctx);
        return;
    }
    run(p, threads, n, fn, ctx);
    p->runLock.unlock();
}

void poolSetThreads(unsigned threads)
{
    gThreads.store(threads < 1 ? 1 : (threads > kMaxThreads ? kMaxThreads : threads));
}
