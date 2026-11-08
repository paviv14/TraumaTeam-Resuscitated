#include "port.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <direct.h>

uint64_t host_now_ns(void) {
    static LARGE_INTEGER f; LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (uint64_t)(c.QuadPart / f.QuadPart) * 1000000000ull + (uint64_t)(c.QuadPart % f.QuadPart) * 1000000000ull / f.QuadPart;
}
void *host_alloc_guest(size_t size) {
    return VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);   // pages are zero, committed lazily by the OS
}
void host_mkdir(const char *path) { _mkdir(path); }

struct HostFiber { void *h; void (*entry)(void); };
static void WINAPI fiber_start(void *p) { ((HostFiber *)p)->entry(); }
HostFiber *host_fiber_main(void) {
    HostFiber *f = calloc(1, sizeof *f);
    f->h = ConvertThreadToFiber(NULL);
    if (!f->h) f->h = GetCurrentFiber();
    return f;
}
HostFiber *host_fiber_new(void (*entry)(void), size_t stack) {
    HostFiber *f = calloc(1, sizeof *f);
    f->entry = entry;
    f->h = CreateFiber(stack, fiber_start, f);
    if (!f->h) { fprintf(stderr, "CreateFiber failed\n"); exit(1); }
    return f;
}
void host_fiber_switch(HostFiber *from, HostFiber *to) { (void)from; SwitchToFiber(to->h); }

static void (*g_crash_fn)(void);
static LONG WINAPI crash_filter(EXCEPTION_POINTERS *e) {
    fprintf(stderr, "\n*** crash: exception %08lX at %p\n", e->ExceptionRecord->ExceptionCode, e->ExceptionRecord->ExceptionAddress);
    if (g_crash_fn) g_crash_fn();
    fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}
void host_install_crash_handler(void (*fn)(void)) { g_crash_fn = fn; SetUnhandledExceptionFilter(crash_filter); }

static void (*g_timeout_fn)(void); static int g_timeout_s;
static DWORD WINAPI timeout_thread(void *p) { (void)p; Sleep((DWORD)g_timeout_s * 1000); g_timeout_fn(); return 0; }
void host_set_timeout(int seconds, void (*fn)(void)) {
    if (seconds <= 0) return;
    g_timeout_fn = fn; g_timeout_s = seconds;
    CreateThread(NULL, 0, timeout_thread, NULL, 0, NULL);
}

#else   // POSIX
#include <time.h>
#include <signal.h>
#include <unistd.h>
#include <ucontext.h>
#include <sys/mman.h>
#include <sys/stat.h>

uint64_t host_now_ns(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec; }
void *host_alloc_guest(size_t size) {
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}
void host_mkdir(const char *path) { mkdir(path, 0755); }

struct HostFiber { ucontext_t uc; };
HostFiber *host_fiber_main(void) { return calloc(1, sizeof(HostFiber)); }
HostFiber *host_fiber_new(void (*entry)(void), size_t stack) {
    HostFiber *f = calloc(1, sizeof *f);
    getcontext(&f->uc);
    f->uc.uc_stack.ss_sp = malloc(stack); f->uc.uc_stack.ss_size = stack; f->uc.uc_link = NULL;
    makecontext(&f->uc, entry, 0);
    return f;
}
void host_fiber_switch(HostFiber *from, HostFiber *to) { swapcontext(&from->uc, &to->uc); }

static void (*g_crash_fn)(void);
static void on_crash(int sig) {
    fprintf(stderr, "\n*** crash: signal %d\n", sig);
    if (g_crash_fn) g_crash_fn();
    _exit(3);
}
void host_install_crash_handler(void (*fn)(void)) {
    g_crash_fn = fn;
    static char altstack[1 << 16]; stack_t ss = { .ss_sp = altstack, .ss_size = sizeof altstack };
    sigaltstack(&ss, NULL);
    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_handler = on_crash; sa.sa_flags = SA_ONSTACK;
    sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL);
}
static void (*g_timeout_fn)(void);
static void on_alarm(int sig) { (void)sig; g_timeout_fn(); }
void host_set_timeout(int seconds, void (*fn)(void)) {
    if (seconds <= 0) return;
    g_timeout_fn = fn; signal(SIGALRM, on_alarm); alarm(seconds);
}
#endif
