/* Internal interface of the PC runtime that replaces Windows 95, DirectDraw,
 * DirectSound and the rest of the platform under the recompiled game code
 * (sgens.exe, src/gen/sgens). */
#pragma once
#include <stdio.h>
#include <stdarg.h>
#include "cpu.h"

#include "game.h"
/* test / debug switches: environment variables NH_<x> */
#define ENV(x) GAME_ENV x

/* guest memory layout (a 4 GB reservation, MEM) */
#define GUEST_RESERVE   0x100000000ull
#define GUEST_COMMIT    0x20000000u      /* first 512 MB committed */
#define TEB_ADDR        0x00010000u      /* thread block: fs:[0] SEH chain, fs:[18h] self */
#define STRINGS_ADDR    0x00020000u      /* command line, environment, module names */
#define STACK_LO        0x00100000u
#define STACK_TOP       0x003ff000u      /* 3 MB main stack */
#define EXE_BASE        0x00400000u      /* sgens.exe image (to 005e8000) */
#define HEAP_BASE       0x01000000u      /* HeapAlloc / LocalAlloc / VirtualAlloc / COM objects */
#define HEAP_END        0x0ff00000u
#define THUNK_BASE      0x7f000000u      /* import thunks: exe 7f000000, COM / dynamic 7f200000 */
#define THUNK_END       0x7f400000u
#define MAGIC_RET       0x7f3ffff0u      /* return address of calls from natives into guest code */

void rt_log(const char *fmt, ...);
void rt_fatal(const char *fmt, ...);
extern FILE *rt_logf;
extern char rt_root[512];      /* folder holding hd\ (C:\STGENS) and cd\ (D:\) */
extern char rt_exe_dir[512];

/* code lookup / calls from native code into guest code */
void (*rt_lookup(u32 addr))(void);
u32 guest_call(u32 fn, int nargs, ...);          /* stdcall or cdecl, args left to right */

/* natives: arguments of the API being called (return address at [esp]) */
#define ARG(i)  RD32(cpu.r[R_ESP] + 4 + 4 * (i))
/* return from a stdcall API with n argument dwords */
#define RET(v, n) do { cpu.r[R_EAX] = (u32)(v); cpu.r[R_ESP] += 4 + 4 * (n); return; } while (0)
/* cdecl: caller pops */
#define RETC(v) do { cpu.r[R_EAX] = (u32)(v); cpu.r[R_ESP] += 4; return; } while (0)

/* thunks: native functions callable at a guest address */
typedef void (*NativeFn)(void);
u32 thunk_new(NativeFn fn, const char *name);
u32 api_resolve(const char *dll, const char *name);   /* -> thunk address (0 = unknown) */
void api_init(void);

/* guest heap */
u32 gmalloc(u32 size);
void gfree(u32 p);
u32 grealloc(u32 p, u32 size);
u32 gsize(u32 p);
u32 gcalloc(u32 size);
u32 gstrdup(const char *s);

/* PE images */
typedef struct {
    const char *file;          /* in rt_root\hd */
    u32 base, import_base;
    u32 entry, size;
    char dll[32];
} Image;
int pe_load(Image *im);
u32 pe_export(Image *im, const char *name);   /* "#ord" or name */
extern Image img_exe;

/* paths: guest DOS-style path -> host path */
void path_to_host(const char *g, char *out, size_t n);
const char *gstr(u32 a);          /* guest C string (NULL for 0) */

/* window / input / presentation (window.c) */
extern u32 g_hwnd;                /* the game's window (guest handle) */
extern u32 g_wndproc;
void win_create(u32 wndproc, const char *title);
int win_pump(int wait);           /* host messages -> guest queue; returns 0 on quit */
int msgq_get(u32 msg_addr, u32 hwnd, u32 lo, u32 hi, int remove);
void msgq_post(u32 hwnd, u32 msg, u32 wp, u32 lp);
void msgq_quit(int code);
void present_frame(const u8 *pix, int pitch, const u32 *pal);  /* 640x480 8-bit */
void present_frame16(const u8 *pix, int pitch, int w, int h);  /* RGB565 */
void present_frame32(const u32 *pix, int w, int h);
void win_cursor(int show);
u32 win_key_state(int vk);
extern int quit_requested;

/* timers (winmm timeSetEvent), run at API calls */
void timers_poll(void);
u32 now_ms(void);

/* DirectDraw / DirectSound (ddraw.c, dsound.c) */
void api_DirectDrawCreate(void);
void api_DirectSoundCreate(void);
void dsound_init(void);

/* handles (win32.c): files, finds, sync objects, threads, mappings */
typedef struct { int used, kind; void *h; int obj; u32 aux; } GH;
enum { GH_FILE = 1, GH_FIND, GH_OTHER, GH_MUTEX, GH_EVENT, GH_THREAD, GH_MAPPING };
u32 gh_new(int kind, void *h);
GH *gh_get(u32 h);

/* guest threads (threads.c): cooperative, one host fiber each; they switch
   only inside blocking / polling APIs */
void thr_init(void);
void thr_yield(void);             /* let the other guest threads run a slice */
int thr_current(void);
u32 thr_tls_get(u32 i);
void thr_sync_free(int s);           /* a mutex / event's last handle was closed */
void thr_wait_qpc(long long until);  /* wait for a QueryPerformanceCounter time, other threads run */
int thr_wait_handle(u32 h, u32 ms);   /* 1 = signalled (acquired), 0 = timeout */

/* an N x layer of live 3D drawn over part of the presented frame:
   pix = w x h ARGB (alpha 0 = keep the frame), over rectangle x,y,rw,rh of the frame */
typedef struct { const u32 *pix; int w, h; int x, y, rw, rh; int valid; } HiresLayer;
extern HiresLayer hires_layer;

/* registry (registry.c) */
void reg_init(void);
void reg_save(void);
