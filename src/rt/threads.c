/* Guest threads and synchronisation objects.
 *
 * sgens.exe runs its sound manager in threads of its own (channel service,
 * delayed-sound queue, streaming), guarded by mutexes.  Each guest thread is
 * a host fiber with its own copy of the register file: threads switch only
 * inside the APIs that block or poll (Sleep, WaitFor..., PeekMessage, ...),
 * so the recompiled code never sees a thread change in the middle of a
 * function and needs no locking of its own.  Mutexes, events and thread
 * handles are plain records here; a waiting thread re-checks its condition
 * each time it is scheduled. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include "rt.h"

#define NTHR 16
#define NTLS 64
#define GSTACK (1u << 20)          /* guest stack of a new thread */

typedef struct {
    int used, finished, blocked;
    int suspend;
    void *fiber;
    CPU cpu;
    u32 tail_ret;
    u32 handle, id, exit_code;
    u32 fn, param;
    u32 teb, stack_lo, stack_hi;
    u32 tls[NTLS];
} GThread;

static GThread thr[NTHR];
static int cur;
static int nlive = 1;
static LARGE_INTEGER slice_start, qpf;
static int tls_used[NTLS];
extern u32 rt_tail_ret;

typedef struct { int used, kind; int owner, count; int manual, signaled; } Sync;
#define NSYNC 128
static Sync sync[NSYNC];

int thr_current(void) { return cur; }

static int sync_new(int kind)
{
    for (int i = 0; i < NSYNC; i++)
        if (!sync[i].used) { memset(&sync[i], 0, sizeof sync[i]); sync[i].used = 1; sync[i].kind = kind; sync[i].owner = -1; return i; }
    rt_fatal("out of sync objects");
    return -1;
}

/* ---------------- scheduler ---------------- */
static void switch_to(int n)
{
    if (n == cur) return;
    GThread *a = &thr[cur], *b = &thr[n];
    a->cpu = cpu;
    a->tail_ret = rt_tail_ret;
    cur = n;
    cpu = b->cpu;
    rt_tail_ret = b->tail_ret;
    SwitchToFiber(b->fiber);
    QueryPerformanceCounter(&slice_start);
    /* back on this thread */
}

static int runnable(int i) { return thr[i].used && !thr[i].finished && !thr[i].suspend; }

/* run the next thread in turn; when every live thread is waiting, sleep a
   moment first so a wait loop does not spin the host CPU */
void thr_yield(void)
{
    int all_blocked = 1, n = -1;
    for (int k = 1; k <= NTHR; k++) {
        int i = (cur + k) % NTHR;
        if (!runnable(i)) continue;
        if (!thr[i].blocked) all_blocked = 0;
        if (n < 0 && i != cur) n = i;
    }
    if (all_blocked && thr[cur].blocked) Sleep(1);
    if (n >= 0) switch_to(n);
}

/* ---------------- waiting ---------------- */
static int try_acquire(u32 h)
{
    GH *g = gh_get(h);
    if (!g) return 1;
    if (g->kind == GH_THREAD) return thr[g->obj].finished;
    if (g->kind != GH_MUTEX && g->kind != GH_EVENT) return 1;
    Sync *s = &sync[g->obj];
    if (s->kind == GH_MUTEX) {
        if (s->owner < 0 || s->owner == cur || !runnable(s->owner)) { s->owner = cur; s->count++; return 1; }
        return 0;
    }
    if (s->signaled) { if (!s->manual) s->signaled = 0; return 1; }
    return 0;
}

int thr_wait_handle(u32 h, u32 ms)
{
    if (try_acquire(h)) return 1;
    if (!ms) return 0;
    u32 until = now_ms() + ms;
    thr[cur].blocked = 1;
    for (;;) {
        if (cur == 0) win_pump(0);
        thr_yield();
        if (try_acquire(h)) { thr[cur].blocked = 0; return 1; }
        if (ms != 0xffffffffu && (s32)(now_ms() - until) >= 0) { thr[cur].blocked = 0; return 0; }
    }
}

/* ---------------- fibers ---------------- */
static void CALLBACK thread_main(void *p)
{
    GThread *t = p;
    u32 r = guest_call(t->fn, 1, t->param);
    t->exit_code = r;
    t->finished = 1;
    nlive--;
    rt_log("thread %u (%08x) returned %u\n", t->id, t->fn, r);
    for (;;) thr_yield();   /* never scheduled again */
}

/* preemption: a host timer raises rt_irq_pending; the recompiled code polls
   it at every loop back-edge (registers synced to cpu), so a guest thread
   that busy-waits on another one hands over after its time slice */
#define SLICE_MS 4
static DWORD WINAPI ticker(void *p)
{
    (void)p;
    for (;;) {
        Sleep(2);
        if (nlive > 1) rt_irq_pending = 1;
    }
    return 0;
}

static void *poll_ra[NTHR];
void irq_poll(void)
{
    rt_irq_pending = 0;
    poll_ra[cur] = __builtin_return_address(0);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    static int npre;
    if ((now.QuadPart - slice_start.QuadPart) * 1000 >= (s64)SLICE_MS * qpf.QuadPart) {
        if (npre++ < 0) rt_log("preempt thread %d\n", cur);
        thr_yield();
        QueryPerformanceCounter(&slice_start);
    }
}

void thr_init(void)
{
    QueryPerformanceFrequency(&qpf);
    QueryPerformanceCounter(&slice_start);
    CreateThread(NULL, 0, ticker, NULL, 0, NULL);
    thr[0].used = 1;
    thr[0].id = 0x44;
    thr[0].teb = TEB_ADDR;
    thr[0].fiber = ConvertThreadToFiber(NULL);
    if (!thr[0].fiber) rt_fatal("ConvertThreadToFiber failed");
}

/* CreateThread(sec, stack, fn, param, flags, idp) */
static void api_CreateThread(void)
{
    int i;
    for (i = 1; i < NTHR; i++) if (!thr[i].used) break;
    if (i == NTHR) RET(0, 6);
    GThread *t = &thr[i];
    memset(t, 0, sizeof *t);
    t->used = 1;
    t->id = 0x48 + 4 * i;
    t->fn = ARG(2);
    t->param = ARG(3);
    t->suspend = (ARG(4) & 4) ? 1 : 0;   /* CREATE_SUSPENDED */
    t->stack_lo = gmalloc(GSTACK);
    t->stack_hi = t->stack_lo + GSTACK - 64;
    t->teb = gcalloc(0x1000);
    WR32(t->teb, 0xffffffffu);
    WR32(t->teb + 4, t->stack_hi);
    WR32(t->teb + 8, t->stack_lo);
    WR32(t->teb + 0x18, t->teb);
    WR32(t->teb + 0x24, t->id);
    t->cpu = cpu;
    memset(t->cpu.r, 0, sizeof t->cpu.r);
    t->cpu.r[R_ESP] = t->stack_hi;
    t->cpu.sbase[S_FS] = t->teb;
    t->cpu.ftop = 0; t->cpu.fsw = 0; t->cpu.fcw = 0x27f;
    t->cpu.df = 0;
    t->fiber = CreateFiber(8u << 20, thread_main, t);
    if (!t->fiber) { t->used = 0; RET(0, 6); }
    t->handle = gh_new(GH_THREAD, NULL);
    nlive++;
    gh_get(t->handle)->obj = i;
    if (ARG(5)) WR32(ARG(5), t->id);
    rt_log("CreateThread %08x param %08x -> thread %d%s\n", t->fn, t->param, i, t->suspend ? " (suspended)" : "");
    RET(t->handle, 6);
}

static int thread_of(u32 h)
{
    if (h == 0xfffffffeu) return cur;
    GH *g = gh_get(h);
    return g && g->kind == GH_THREAD ? g->obj : -1;
}

static void api_ExitThread(void)
{
    rt_log("ExitThread %u (thread %d)\n", ARG(0), cur);
    thr[cur].exit_code = ARG(0);
    thr[cur].finished = 1;
    nlive--;
    for (;;) thr_yield();
}

static void api_TerminateThread(void)
{
    int i = thread_of(ARG(0));
    if (i > 0 && !thr[i].finished) { thr[i].finished = 1; thr[i].exit_code = ARG(1); nlive--; }
    if (i == cur) for (;;) thr_yield();
    RET(1, 2);
}

static void api_SuspendThread(void)
{
    int i = thread_of(ARG(0));
    if (i < 0) RET(0xffffffffu, 1);
    u32 prev = thr[i].suspend++;
    if (i == cur) while (thr[cur].suspend) { thr[cur].blocked = 1; thr_yield(); }
    thr[cur].blocked = 0;
    RET(prev, 1);
}

static void api_ResumeThread(void)
{
    int i = thread_of(ARG(0));
    if (i < 0) RET(0xffffffffu, 1);
    u32 prev = thr[i].suspend;
    if (prev) thr[i].suspend--;
    RET(prev, 1);
}

static void api_GetExitCodeThread(void)
{
    int i = thread_of(ARG(0));
    if (i < 0) RET(0, 2);
    WR32(ARG(1), thr[i].finished ? thr[i].exit_code : 259 /* STILL_ACTIVE */);
    RET(1, 2);
}

static void api_GetCurrentThread(void) { RET(0xfffffffeu, 0); }
static void api_GetCurrentThreadId(void) { RET(thr[cur].id, 0); }
static void api_SetThreadPriority(void) { RET(1, 2); }
static void api_GetThreadPriority(void) { RET(0, 1); }

static void api_Sleep(void)
{
    u32 ms = ARG(0);
    u32 until = now_ms() + ms;
    timers_poll();
    if (!ms) {   /* Sleep(0): give the other threads a turn, never wait a whole tick */
        static u32 n0;
        if ((++n0 % 200000) == 0 && getenv(ENV("SLEEPDEBUG")))
            rt_log("sleep(0) #%u thread %d from %08x, caller %08x\n", n0, cur, RD32(cpu.r[R_ESP]), RD32(cpu.r[R_EBP] + 4));
        if (cur == 0) win_pump(0);
        thr_yield();
        RET(0, 1);
    }
    if (ms >= 20 && getenv(ENV("SLEEPDEBUG"))) rt_log("sleep %u ms thread %d from %08x\n", ms, cur, RD32(cpu.r[R_ESP]));
    thr[cur].blocked = 1;
    do {
        if (cur == 0) win_pump(0);
        thr_yield();
    } while ((s32)(now_ms() - until) < 0);
    thr[cur].blocked = 0;
    RET(0, 1);
}

/* ---------------- sync objects ---------------- */
static void api_CreateMutexA(void)
{
    int s = sync_new(GH_MUTEX);
    if (ARG(1)) { sync[s].owner = cur; sync[s].count = 1; }
    u32 h = gh_new(GH_MUTEX, NULL);
    gh_get(h)->obj = s;
    RET(h, 3);
}

static void api_OpenMutexA(void) { RET(0, 3); }

static void api_ReleaseMutex(void)
{
    GH *g = gh_get(ARG(0));
    if (!g || g->kind != GH_MUTEX) RET(0, 1);
    Sync *s = &sync[g->obj];
    if (s->owner != cur) RET(0, 1);
    if (--s->count <= 0) { s->count = 0; s->owner = -1; }
    RET(1, 1);
}

/* CreateEventA(sec, manual, initial, name) */
static void api_CreateEventA(void)
{
    int s = sync_new(GH_EVENT);
    sync[s].manual = ARG(1) != 0;
    sync[s].signaled = ARG(2) != 0;
    u32 h = gh_new(GH_EVENT, NULL);
    gh_get(h)->obj = s;
    RET(h, 4);
}

static Sync *event_of(u32 h)
{
    GH *g = gh_get(h);
    return g && g->kind == GH_EVENT ? &sync[g->obj] : NULL;
}
static void api_SetEvent(void) { Sync *s = event_of(ARG(0)); if (s) s->signaled = 1; RET(s != NULL, 1); }
static void api_ResetEvent(void) { Sync *s = event_of(ARG(0)); if (s) s->signaled = 0; RET(s != NULL, 1); }
static void api_PulseEvent(void) { Sync *s = event_of(ARG(0)); if (s) s->signaled = 1; RET(s != NULL, 1); }

static void api_WaitForSingleObject(void)
{
    RET(thr_wait_handle(ARG(0), ARG(1)) ? 0 : 0x102 /* WAIT_TIMEOUT */, 2);
}
static void api_WaitForSingleObjectEx(void)
{
    RET(thr_wait_handle(ARG(0), ARG(1)) ? 0 : 0x102, 3);
}

int msgq_pending(void);
/* MsgWaitForMultipleObjects(n, handles, all, ms, mask) */
static void api_MsgWaitForMultipleObjects(void)
{
    u32 n = ARG(0), hs = ARG(1), all = ARG(2), ms = ARG(3);
    u32 until = now_ms() + ms;
    thr[cur].blocked = 1;
    for (;;) {
        if (cur == 0) win_pump(0);
        if (!all) {
            for (u32 i = 0; i < n; i++) if (try_acquire(RD32(hs + 4 * i))) { thr[cur].blocked = 0; RET(i, 5); }
        } else {
            u32 k = 0;
            for (u32 i = 0; i < n; i++) { GH *g = gh_get(RD32(hs + 4 * i)); if (!g || g->kind != GH_MUTEX) k += try_acquire(RD32(hs + 4 * i)); else k++; }
            if (k == n) { thr[cur].blocked = 0; RET(0, 5); }
        }
        if (msgq_pending()) { thr[cur].blocked = 0; RET(n, 5); }
        if (ms != 0xffffffffu && (s32)(now_ms() - until) >= 0) { thr[cur].blocked = 0; RET(0x102, 5); }
        thr_yield();
    }
}

/* ---------------- TLS (per guest thread) ---------------- */
u32 thr_tls_get(u32 i) { return i < NTLS ? thr[cur].tls[i] : 0; }
static void api_TlsAlloc(void) { for (int i = 0; i < NTLS; i++) if (!tls_used[i]) { tls_used[i] = 1; for (int t = 0; t < NTHR; t++) thr[t].tls[i] = 0; RET(i, 0); } RET(0xffffffffu, 0); }
static void api_TlsFree(void) { if (ARG(0) < NTLS) tls_used[ARG(0)] = 0; RET(1, 1); }
static void api_TlsGetValue(void) { RET(thr_tls_get(ARG(0)), 1); }
static void api_TlsSetValue(void) { if (ARG(0) < NTLS) thr[cur].tls[ARG(0)] = ARG(1); RET(1, 2); }

/* ---------------- critical sections (one owner, cooperative) ---------------- */
/* CRITICAL_SECTION: [0] debug info, [4] lock count, [8] recursion, [12] owner thread id */
static void api_InitializeCriticalSection(void) { memset(MEM + ARG(0), 0, 24); WR32(ARG(0) + 4, 0xffffffffu); RET(0, 1); }
static void api_DeleteCriticalSection(void) { RET(0, 1); }
static void api_EnterCriticalSection(void)
{
    u32 cs = ARG(0);
    if (RD32(cs + 8) && RD32(cs + 12) != thr[cur].id) {
        thr[cur].blocked = 1;
        while (RD32(cs + 8) && RD32(cs + 12) != thr[cur].id) thr_yield();
        thr[cur].blocked = 0;
    }
    WR32(cs + 12, thr[cur].id);
    WR32(cs + 8, RD32(cs + 8) + 1);
    RET(0, 1);
}
static void api_LeaveCriticalSection(void)
{
    u32 cs = ARG(0);
    if (RD32(cs + 8)) WR32(cs + 8, RD32(cs + 8) - 1);
    if (!RD32(cs + 8)) WR32(cs + 12, 0);
    RET(0, 1);
}

static void api_InterlockedIncrement(void) { u32 a = ARG(0); WR32(a, RD32(a) + 1); RET(RD32(a), 1); }
static void api_InterlockedDecrement(void) { u32 a = ARG(0); WR32(a, RD32(a) - 1); RET(RD32(a), 1); }
static void api_InterlockedExchange(void) { u32 a = ARG(0), o = RD32(a); WR32(a, ARG(1)); RET(o, 2); }

typedef struct { const char *name; NativeFn fn; } ApiEnt;
const ApiEnt api_tab_threads[] = {
    {"CreateThread", api_CreateThread}, {"ExitThread", api_ExitThread}, {"TerminateThread", api_TerminateThread},
    {"SuspendThread", api_SuspendThread}, {"ResumeThread", api_ResumeThread}, {"GetExitCodeThread", api_GetExitCodeThread},
    {"GetCurrentThread", api_GetCurrentThread}, {"GetCurrentThreadId", api_GetCurrentThreadId},
    {"SetThreadPriority", api_SetThreadPriority}, {"GetThreadPriority", api_GetThreadPriority},
    {"Sleep", api_Sleep},
    {"CreateMutexA", api_CreateMutexA}, {"OpenMutexA", api_OpenMutexA}, {"ReleaseMutex", api_ReleaseMutex},
    {"CreateEventA", api_CreateEventA}, {"SetEvent", api_SetEvent}, {"ResetEvent", api_ResetEvent}, {"PulseEvent", api_PulseEvent},
    {"WaitForSingleObject", api_WaitForSingleObject}, {"WaitForSingleObjectEx", api_WaitForSingleObjectEx},
    {"MsgWaitForMultipleObjects", api_MsgWaitForMultipleObjects},
    {"TlsAlloc", api_TlsAlloc}, {"TlsFree", api_TlsFree}, {"TlsGetValue", api_TlsGetValue}, {"TlsSetValue", api_TlsSetValue},
    {"InitializeCriticalSection", api_InitializeCriticalSection}, {"DeleteCriticalSection", api_DeleteCriticalSection},
    {"EnterCriticalSection", api_EnterCriticalSection}, {"LeaveCriticalSection", api_LeaveCriticalSection},
    {"InterlockedIncrement", api_InterlockedIncrement}, {"InterlockedDecrement", api_InterlockedDecrement},
    {"InterlockedExchange", api_InterlockedExchange},
    {NULL, NULL}
};

/* set an event from native code (DirectInput notifications) */
void thr_signal(u32 h)
{
    GH *g = gh_get(h);
    if (g && g->kind == GH_EVENT) sync[g->obj].signaled = 1;
}

/* diagnostics: every guest thread's state and the code addresses on its stack */
void thr_dump(void)
{
    rt_log("  host image base %p\n", (void *)GetModuleHandleA(NULL));
    const char *m = getenv(ENV("DUMPMEM"));
    while (m && *m) {
        u32 a = (u32)strtoul(m, (char **)&m, 16), n = 64;
        if (*m == ':') n = (u32)strtoul(m + 1, (char **)&m, 16);
        rt_log("  mem %08x:", a);
        for (u32 k = 0; k < n; k += 4) rt_log("%s %08x", k % 32 ? "" : "\n   ", RD32(a + k));
        rt_log("\n");
        if (*m == ',') m++; else break;
    }
    for (int i = 0; i < NTHR; i++) {
        GThread *t = &thr[i];
        if (!t->used) continue;
        CPU *c = i == cur ? &cpu : &t->cpu;
        rt_log("  thread %d poll@%p fn %08x%s%s%s%s esp %08x:", i, poll_ra[i], t->fn, i == cur ? " (current)" : "", t->finished ? " finished" : "",
               t->blocked ? " blocked" : "", t->suspend ? " suspended" : "", c->r[R_ESP]);
        int n = 0;
        for (u32 k = 0; k < 256 && n < 14; k++) {
            u32 v = RD32(c->r[R_ESP] + 4 * k);
            if (v > 0x401000 && v < 0x52f000) { rt_log(" %08x", v); n++; }
        }
        rt_log("\n    eax %08x ecx %08x edx %08x ebx %08x ebp %08x esi %08x edi %08x [ebp-4] %08x [ebp+8] %08x\n",
               c->r[0], c->r[1], c->r[2], c->r[3], c->r[5], c->r[6], c->r[7], RD32(c->r[5] - 4), RD32(c->r[5] + 8));
    }
}
