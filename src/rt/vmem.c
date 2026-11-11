/* Virtual memory: VirtualAlloc / VirtualFree / VirtualQuery with real
 * reserve / commit semantics in a guest address range of their own.
 *
 * The game's memory manager (SmartHeap 3.2) reserves 4 MB regions, commits
 * pages inside them and checks pointers with VirtualQuery (allocation base,
 * state), so the regions must look like Windows' own: 64 KB aligned
 * reservations, 4 KB pages, committed pages zero-filled. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include "rt.h"

#define VBASE 0x10000000u
#define VEND  0x30000000u
#define PG 0x1000u
#define NPG ((VEND - VBASE) / PG)

static u8 *pstate;            /* per page: 0 free, 1 reserved, 2 committed */
static u32 *pbase;            /* per page: allocation base of its reservation */

static void vinit(void)
{
    if (pstate) return;
    pstate = calloc(NPG, 1);
    pbase = calloc(NPG, 4);
}

static u32 pidx(u32 a) { return (a - VBASE) / PG; }
static int inrange(u32 a) { return a >= VBASE && a < VEND; }

static u32 find_free(u32 size)
{
    u32 n = (size + 0xffff) & ~0xffffu;
    for (u32 a = VBASE; a + n <= VEND; a += 0x10000) {
        u32 i = pidx(a), k;
        for (k = 0; k < n / PG; k++) if (pstate[i + k]) break;
        if (k == n / PG) return a;
        a += (k / 16) * 0x10000;   /* skip what we scanned */
    }
    return 0;
}

static u32 reserve(u32 a, u32 size)
{
    vinit();
    if (!a) a = find_free(size);
    else a &= ~0xffffu;
    if (!a || !inrange(a)) return 0;
    u32 n = (size + PG - 1) / PG, i = pidx(a);
    if (i + n > NPG) return 0;
    for (u32 k = 0; k < n; k++) if (pstate[i + k]) return 0;
    for (u32 k = 0; k < n; k++) { pstate[i + k] = 1; pbase[i + k] = a; }
    return a;
}

static int commit(u32 a, u32 size)
{
    u32 lo = a & ~(PG - 1), hi = (a + size + PG - 1) & ~(PG - 1);
    for (u32 p = lo; p < hi; p += PG) if (!inrange(p) || !pstate[pidx(p)]) return 0;
    for (u32 p = lo; p < hi; p += PG) {
        u32 i = pidx(p);
        if (pstate[i] != 2) { VirtualAlloc(MEM + p, PG, MEM_COMMIT, PAGE_READWRITE); memset(MEM + p, 0, PG); pstate[i] = 2; }
    }
    return 1;
}

/* VirtualAlloc(addr, size, type, protect) */
void api_VirtualAlloc(void)
{
    u32 a = ARG(0), size = ARG(1), type = ARG(2);
    vinit();
    if (a && !inrange(a)) {   /* inside memory we did not hand out (image, heap): usable already */
        rt_log("VirtualAlloc outside the region: %08x %x %x\n", a, size, type);
        RET(a, 4);
    }
    if ((type & MEM_RESERVE) || !a) {
        u32 r = reserve(a, size);
        if (!r) { rt_log("VirtualAlloc reserve %08x %x failed\n", a, size); RET(0, 4); }
        if (type & MEM_COMMIT || !(type & MEM_RESERVE)) commit(r, size);
        RET(r, 4);
    }
    if (type & MEM_COMMIT) {
        if (!commit(a, size)) { rt_log("VirtualAlloc commit %08x %x outside a reservation\n", a, size); RET(0, 4); }
        RET(a & ~(PG - 1), 4);
    }
    RET(0, 4);
}

/* VirtualFree(addr, size, type) */
void api_VirtualFree(void)
{
    u32 a = ARG(0), size = ARG(1), type = ARG(2);
    vinit();
    if (!inrange(a)) RET(1, 3);
    if (type & MEM_RELEASE) {
        if (pbase[pidx(a)] != a || !pstate[pidx(a)]) RET(0, 3);
        for (u32 i = pidx(a); i < NPG && pstate[i] && pbase[i] == a; i++) pstate[i] = 0;
        RET(1, 3);
    }
    if (type & MEM_DECOMMIT) {
        u32 lo = a & ~(PG - 1), hi = size ? (a + size + PG - 1) & ~(PG - 1) : lo;
        if (!size) {   /* whole reservation */
            u32 b = pbase[pidx(a)];
            for (u32 i = pidx(b); i < NPG && pstate[i] && pbase[i] == b; i++) pstate[i] = 1;
        } else for (u32 p = lo; p < hi; p += PG) if (pstate[pidx(p)] == 2) pstate[pidx(p)] = 1;
        RET(1, 3);
    }
    RET(0, 3);
}

/* MEMORY_BASIC_INFORMATION: base, allocbase, allocprot, size, state, prot, type */
static u32 query(u32 a, u32 mbi)
{
    vinit();
    u32 page = a & ~(PG - 1);
    if (inrange(a)) {
        u32 i = pidx(page), st = pstate[i], b = pbase[i], n = 0;
        while (i + n < NPG && pstate[i + n] == st && (st == 0 || pbase[i + n] == b)) n++;
        WR32(mbi, page);
        WR32(mbi + 4, st ? b : 0);
        WR32(mbi + 8, st ? PAGE_READWRITE : 0);
        WR32(mbi + 12, n * PG);
        WR32(mbi + 16, st == 2 ? MEM_COMMIT : st == 1 ? MEM_RESERVE : MEM_FREE);
        WR32(mbi + 20, st == 2 ? PAGE_READWRITE : 0);
        WR32(mbi + 24, st ? MEM_PRIVATE : 0);
        return 28;
    }
    /* everything else the game can see is committed memory */
    u32 base = a >= EXE_BASE && a < EXE_BASE + 0x1e8000 ? EXE_BASE : a >= HEAP_BASE && a < HEAP_END ? HEAP_BASE : page;
    u32 end = base == EXE_BASE ? EXE_BASE + 0x1e8000 : base == HEAP_BASE ? HEAP_END : page + PG;
    WR32(mbi, page);
    WR32(mbi + 4, base);
    WR32(mbi + 8, PAGE_READWRITE);
    WR32(mbi + 12, end - page);
    WR32(mbi + 16, MEM_COMMIT);
    WR32(mbi + 20, PAGE_READWRITE);
    WR32(mbi + 24, base == EXE_BASE ? MEM_IMAGE : MEM_PRIVATE);
    return 28;
}

void api_VirtualQuery(void) { RET(query(ARG(0), ARG(1)), 3); }
void api_VirtualQueryEx(void) { RET(query(ARG(1), ARG(2)), 4); }
void api_VirtualLock(void) { RET(1, 2); }
void api_VirtualUnlock(void) { RET(1, 2); }
void api_VirtualProtect(void) { if (ARG(3)) WR32(ARG(3), PAGE_READWRITE); RET(1, 4); }
