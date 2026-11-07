// Guest threads on host fibers, and interrupt delivery.
//
// The SDK switches threads in SelectThread: OSSaveContext(current) returns 0 when it saves and 1 when
// the thread is later resumed; then OSLoadContext(next) "rfi"s into the next thread. Interrupt handlers
// also end with OSLoadContext(interrupted context). Here:
//   * every guest OSContext that ever runs gets a host fiber (own C stack);
//   * each fiber keeps a stack of resume points (setjmp + register snapshot): one pushed by our
//     SelectThread wrapper (resuming = "SelectThread returns 0") and one per delivered interrupt
//     (resuming = continue where the interrupt arrived);
//   * OSLoadContext(ctx) switches to ctx's fiber (creating it for a brand-new thread) or, if ctx is the
//     current one, unwinds to the newest resume point.
// Interrupts are polled on loop back-edges (g_irq_pending) and delivered when MSR[EE] is set.
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>
#include <ucontext.h>
#include <time.h>

void rt_halt(const char *why);
u32 g_irq_pending;

// ---------------------------------------------------------------- fibers
typedef struct Resume { jmp_buf jb; CPU regs; u32 irq_ctx; int kind; struct Resume *prev; } Resume;
typedef struct Fiber {
    u32 ctx;                 // guest OSContext address
    ucontext_t uc;
    Resume *top;             // newest resume point
    int started;
    int in_irq;              // >0 while this fiber is running an interrupt handler (no nesting)
    struct Fiber *next;
} Fiber;

static Fiber g_main_fiber = { .started = 1 };
static Fiber *g_cur = &g_main_fiber, *g_fibers = &g_main_fiber;
extern CPU *g_cpup;

static Fiber *fiber_of(u32 ctx) {
    for (Fiber *f = g_fibers; f; f = f->next) if (f->ctx == ctx) return f;
    return NULL;
}

static void load_regs_from_ctx(CPU *c, u32 ctx) {
    for (int i = 0; i < 32; i++) c->r[i] = R32(ctx + 4 * i);
    c->cr = R32(ctx + 0x80); c->lr = R32(ctx + 0x84); c->ctr = R32(ctx + 0x88);
    u32 xer = R32(ctx + 0x8C); c->so = xer >> 31; c->ov = (xer >> 30) & 1; c->ca = (xer >> 29) & 1;
    for (int i = 0; i < 32; i++) { c->f[i].ps0 = RF64(ctx + 0x90 + 8 * i); c->f[i].ps1 = RF64(ctx + 0x1C8 + 8 * i); }
    for (int i = 0; i < 8; i++) c->gqr[i] = R32(ctx + 0x1A4 + 4 * i);
    c->msr = R32(ctx + 0x19C);
}

static void fiber_entry(void) {
    CPU *c = g_cpup;
    Fiber *me = g_cur;
    load_regs_from_ctx(c, me->ctx);
    u32 pc = R32(me->ctx + 0x198), lr = c->lr;
    fprintf(stderr, "[thread] start context %08X at %08X\n", me->ctx, pc);
    rt_call(c, pc);
    c->lr = lr; rt_call(c, lr);      // a thread function returning falls into OSExitThread
    rt_halt("guest thread returned past OSExitThread");
}

static void switch_to(Fiber *t) {
    Fiber *me = g_cur;
    if (!t->started) {
        t->started = 1;
        getcontext(&t->uc);
        size_t sz = 4 << 20;
        t->uc.uc_stack.ss_sp = malloc(sz); t->uc.uc_stack.ss_size = sz; t->uc.uc_link = NULL;
        makecontext(&t->uc, fiber_entry, 0);
    }
    g_cur = t;
    swapcontext(&me->uc, &t->uc);
    // ---- we are `me` again: someone loaded our context. Continue at our newest resume point.
    Resume *r = me->top;
    if (!r) rt_halt("fiber resumed without a resume point");
    longjmp(r->jb, 1);
}

// ---------------------------------------------------------------- replaced SDK functions
// OSSaveContext(ctx): original stores the registers; we also remember that ctx belongs to this fiber.
void f_8021B5D0(CPU *c) {
    u32 ctx = c->r[3];
    if (!g_cur->ctx) g_cur->ctx = ctx;
    orig_8021B5D0(c);
}

// Guest backtrace of the last time each thread went to sleep (printed on timeout).
static u32 g_bt[8][16]; static u32 g_bt_ctx[8];
void rt_dump_threads(void) {
    for (int k = 0; k < 8; k++) if (g_bt_ctx[k]) {
        fprintf(stderr, "thread ctx %08X last slept from:", g_bt_ctx[k]);
        for (int i = 0; i < 16 && g_bt[k][i]; i++) fprintf(stderr, " %08X", g_bt[k][i]);
        fprintf(stderr, "\n");
    }
}
static void record_bt(CPU *c) {
    u32 ctx = g_cur->ctx; int slot = -1;
    for (int k = 0; k < 8; k++) if (g_bt_ctx[k] == ctx || (!g_bt_ctx[k] && slot < 0 && (slot = k, 0))) { slot = k; break; }
    if (slot < 0) return;
    g_bt_ctx[slot] = ctx;
    u32 fp = c->r[1]; g_bt[slot][0] = c->lr;
    for (int i = 1; i < 16; i++) { if (!fp || (fp & 0x1FFFFFFF) > 0x13FFFFFF) { g_bt[slot][i] = 0; break; } fp = R32(fp); g_bt[slot][i] = fp ? R32(fp + 4) : 0; }
}

// SelectThread(yield): run the original scheduler under a resume point.
void f_802222E0(CPU *c) {
    record_bt(c);
    Resume r; r.regs = *c; r.kind = 0; r.irq_ctx = 0; r.prev = g_cur->top;
    Fiber *me = g_cur;
    if (setjmp(r.jb)) {               // resumed: OSSaveContext "returned 1" -> SelectThread returns 0
        me->top = r.prev;
        *c = r.regs; c->r[3] = 0;
        return;
    }
    me->top = &r;
    orig_802222E0(c);
    me->top = r.prev;
}

// OSLoadContext(ctx): never returns to its caller.
void f_8021B650(CPU *c) {
    u32 ctx = c->r[3];
    Fiber *me = g_cur;
    // returning from an interrupt delivered to this fiber (possibly while idling on another context)
    if (me->top && me->top->kind == 1 && me->top->irq_ctx == ctx) longjmp(me->top->jb, 1);
    Fiber *t = fiber_of(ctx);
    if (t == me) { if (!me->top) rt_halt("OSLoadContext(self) with nothing to resume"); longjmp(me->top->jb, 1); }
    if (!t) { t = calloc(1, sizeof *t); t->ctx = ctx; t->next = g_fibers; g_fibers = t; }
    switch_to(t);
}

// ---------------------------------------------------------------- interrupts
static u64 now_ns(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (u64)ts.tv_sec * 1000000000ull + ts.tv_nsec; }
static u64 g_next_vi, g_dec_deadline; static int g_dec_armed;
u32 mmio_peek(u32 p); void mmio_poke(u32 p, u32 v);

void rt_set_dec(u32 v) {  // decrementer counts down at the timebase rate (60.75 MHz)
    g_dec_deadline = now_ns() + (u64)(s32)v * 1000ull / 61;  // ~16.46 ns per tick
    g_dec_armed = 1; g_irq_pending = 1;
}
u32 rt_get_dec(void) {
    if (!g_dec_armed) return 0xFFFFFFFFu;
    s64 left = (s64)g_dec_deadline - (s64)now_ns();
    return (u32)(s32)(left * 61 / 1000);
}

static void deliver(CPU *c, u32 handler, u32 exc) {
    u32 ctx = R32(0x800000D4);         // current OSContext (OSGetCurrentContext)
    Resume r; r.regs = *c; r.kind = 1; r.irq_ctx = ctx; r.prev = g_cur->top;
    Fiber *me = g_cur;
    if (setjmp(r.jb)) { me->top = r.prev; *c = r.regs; me->in_irq--; return; }
    me->top = &r;
    me->in_irq++;
    c->msr &= ~0x8000u;                // EE off inside the handler
    c->r[3] = exc; c->r[4] = ctx;
    rt_call(c, handler);
    me->top = r.prev; *c = r.regs;     // handler returned without OSLoadContext: just continue
    me->in_irq--;
}

void ios_service(CPU *c); void dsp_tick(void); void gx_tick(void); u32 gx_cause(void);
void rt_poll(CPU *c) {
    if (!(c->msr & 0x8000u) || g_cur->in_irq) return;   // MSR[EE], and never inside a handler
    g_cur->in_irq++;
    ios_service(c);
    g_cur->in_irq--;
    dsp_tick();
    gx_tick();
    u64 t = now_ns();
    if (!g_next_vi) g_next_vi = t + 16683333;
    if (g_dec_armed && t >= g_dec_deadline) {
        g_dec_armed = 0;
        deliver(c, 0x8021A1A0u, 8);    // DecrementerExceptionHandler
        return;
    }
    // GPU interrupt causes follow the device status; deliver until the handlers have acknowledged them
    // (one dispatch services the highest-priority source only).
    for (int k = 0; k < 4; k++) {
        u32 gpu = gx_cause();
        u32 cause = (mmio_peek(0x0C003000u) & ~0xE00u) | gpu;
        mmio_poke(0x0C003000u, cause);
        if (!(cause & ~0x100u)) break;
        deliver(c, 0x8021F9A0u, 4);    // ExternalInterruptHandler
        mmio_poke(0x0C003000u, (mmio_peek(0x0C003000u) & ~0xE00u) | gx_cause());
        if ((mmio_peek(0x0C003000u) & ~0x100u) == (cause & ~0x100u)) break;   // nothing acknowledged
    }
    if (t >= g_next_vi) {
        g_next_vi += 16683333;
        if (g_next_vi < t) g_next_vi = t + 16683333;
        // VI display interrupts DI0/DI1 (0x0C002030/34): raise bit 31 on each enabled one (bit 28),
        // and the VI cause bit in the PI interrupt cause register (0x0C003000).
        int any = 0;
        for (int k = 0; k < 4; k++) {
            u32 a = 0x0C002030u + 4 * k, v = mmio_peek(a);
            if (v & 0x10000000u) { mmio_poke(a, v | 0x80000000u); any = 1; }
        }
        if (any) {
            mmio_poke(0x0C003000u, mmio_peek(0x0C003000u) | 0x100);
            deliver(c, 0x8021F9A0u, 4);  // ExternalInterruptHandler
            mmio_poke(0x0C003000u, mmio_peek(0x0C003000u) & ~0x100u);
        }
    }
}
