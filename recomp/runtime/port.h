// Host OS portability: clock, guest memory reservation, fibers, non-local jumps, directories.
#pragma once
#include <stddef.h>
#include <stdint.h>

// Monotonic host time in nanoseconds.
uint64_t host_now_ns(void);

// Reserve + commit the guest physical memory block (zero-filled).
void *host_alloc_guest(size_t size);

// mkdir that ignores "already exists".
void host_mkdir(const char *path);

// Non-local jumps that do not unwind (fibers jump between resume points on their own stacks, and a
// halt jumps from a fiber back to main). GCC's builtins only save sp/fp/ip, which is exactly that,
// and behave the same on Linux and Windows (mingw's longjmp would try an SEH unwind instead).
typedef void *rt_jmp_buf[5];
#define RT_SETJMP(b) __builtin_setjmp(b)
#define RT_LONGJMP(b) __builtin_longjmp(b, 1)

// Fibers: cooperative host stacks.
typedef struct HostFiber HostFiber;
HostFiber *host_fiber_main(void);                              // the calling thread, as a fiber
HostFiber *host_fiber_new(void (*entry)(void), size_t stack);  // starts on first switch
void host_fiber_switch(HostFiber *from, HostFiber *to);

// Crash and timeout hooks (debug aids; timeout only when asked for).
void host_install_crash_handler(void (*fn)(void));
void host_set_timeout(int seconds, void (*fn)(void));
