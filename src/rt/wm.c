/* Guest windows: the USER32 window manager the game (an MFC 4.2 program)
 * runs on.
 *
 * Every guest window is a record here: class, window procedure (guest
 * code), styles, parent, rectangle, properties, extra bytes.  Messages go
 * straight to the guest window procedures (SendMessage) or through the
 * guest message queue (PostMessage, input).  MFC's window-creation CBT hook
 * and subclassing work as on Windows: CreateWindowExA calls the WH_CBT hook
 * with HCBT_CREATEWND before WM_NCCREATE / WM_CREATE.
 *
 * The guest "screen" is 640x480: the top-level window covers it, input from
 * the host window (window.c) arrives in those coordinates and is routed to
 * the deepest visible child under the point (or the capture window). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "rt.h"

#define NWND 256
#define NCLS 64
#define NPROP 8
#define WBASE 0x00020000u
#define WSTEP 0x10u

typedef struct {
    int used;
    char name[64];
    u32 style, proc, extra, inst, icon, cursor, brush, menu;
    u32 atom;
} GClass;

typedef struct {
    int used;
    int cls;
    u32 proc, style, exstyle, parent, owner, id, inst, user;
    int x, y, w, h;           /* relative to the parent's client area (top level: screen) */
    char text[256];
    int dirty, destroying;
    u32 z;                    /* higher = on top among siblings */
    struct { char name[48]; u32 atom, val; } props[NPROP];
    u8 extra[64];
} GWnd;

static GClass cls[NCLS];
static GWnd wnd[NWND];
static u32 zcount = 1;
static u32 focus_wnd, capture_wnd, active_wnd;
static u32 hook_cbt, hook_cbt_h, hook_msgfilter;
static u32 defproc_thunk;     /* DefWindowProcA as a guest address */
extern u32 g_hwnd;            /* top-level game window (for window.c) */
extern int quit_requested;

u32 wm_class_cursor(u32 h);
void win_set_guest_cursor(u32 cursor);
void win_guest_shown(void);

static GWnd *W(u32 h)
{
    u32 i = (h - WBASE) / WSTEP;
    if (h < WBASE || (h - WBASE) % WSTEP || i >= NWND || !wnd[i].used) return NULL;
    return &wnd[i];
}
static u32 HW(GWnd *w) { return WBASE + (u32)(w - wnd) * WSTEP; }

int wm_is_window(u32 h) { return W(h) != NULL; }

static int cls_find(const char *name)
{
    for (int i = 0; i < NCLS; i++) if (cls[i].used && !_stricmp(cls[i].name, name)) return i;
    return -1;
}

static int cls_find_guest(u32 p)
{
    if (p < 0x10000) {
        for (int i = 0; i < NCLS; i++) if (cls[i].used && cls[i].atom == p) return i;
        return -1;
    }
    return cls_find(gstr(p));
}

/* ---------------- sending ---------------- */
u32 wm_send(u32 h, u32 msg, u32 wp, u32 lp)
{
    GWnd *w = W(h);
    if (!w || !w->proc) return 0;
    return guest_call(w->proc, 4, h, msg, wp, lp);
}

static int visible_chain(GWnd *w)
{
    while (w) { if (!(w->style & WS_VISIBLE)) return 0; w = W(w->parent); }
    return 1;
}

/* ---------------- classes ---------------- */
static u32 atom_next = 0xc001;

static u32 register_class(u32 wc)
{
    const char *name = gstr(RD32(wc + 36));
    char nb[64];
    if (RD32(wc + 36) < 0x10000) { snprintf(nb, sizeof nb, "#%u", RD32(wc + 36)); name = nb; }
    int i = cls_find(name);
    if (i >= 0) { rt_log("RegisterClassA %s: exists\n", name); return 0; }
    for (i = 0; i < NCLS && cls[i].used; i++) ;
    if (i == NCLS) rt_fatal("out of window classes");
    GClass *c = &cls[i];
    memset(c, 0, sizeof *c);
    c->used = 1;
    snprintf(c->name, sizeof c->name, "%s", name);
    c->style = RD32(wc);
    c->proc = RD32(wc + 4);
    c->extra = RD32(wc + 12);
    c->inst = RD32(wc + 16);
    c->icon = RD32(wc + 20);
    c->cursor = RD32(wc + 24);
    c->brush = RD32(wc + 28);
    c->menu = RD32(wc + 32);
    c->atom = atom_next++;
    rt_log("RegisterClassA %s proc %08x cursor %08x\n", c->name, c->proc, c->cursor);
    return c->atom;
}

void api_RegisterClassA(void) { RET(register_class(ARG(0)), 1); }

void api_UnregisterClassA(void)
{
    int i = cls_find_guest(ARG(0));
    if (i >= 0) cls[i].used = 0;
    RET(i >= 0, 2);
}

/* GetClassInfoA(inst, name, wc) */
void api_GetClassInfoA(void)
{
    int i = cls_find_guest(ARG(1));
    if (i < 0) RET(0, 3);
    GClass *c = &cls[i];
    u32 wc = ARG(2);
    WR32(wc, c->style); WR32(wc + 4, c->proc); WR32(wc + 8, 0); WR32(wc + 12, c->extra);
    WR32(wc + 16, c->inst); WR32(wc + 20, c->icon); WR32(wc + 24, c->cursor); WR32(wc + 28, c->brush);
    WR32(wc + 32, c->menu); WR32(wc + 36, ARG(1));
    RET(1, 3);
}

void api_GetClassNameA(void)
{
    GWnd *w = W(ARG(0));
    if (!w) RET(0, 3);
    const char *n = w->cls >= 0 ? cls[w->cls].name : "";
    u32 max = ARG(2);
    if (!max) RET(0, 3);
    snprintf((char *)MEM + ARG(1), max, "%s", n);
    RET(strlen((char *)MEM + ARG(1)), 3);
}

void api_GetClassLongA(void)
{
    GWnd *w = W(ARG(0));
    if (!w || w->cls < 0) RET(0, 2);
    GClass *c = &cls[w->cls];
    s32 i = (s32)ARG(1);
    RET(i == -12 ? c->cursor : i == -14 ? c->icon : i == -10 ? c->brush : i == -26 ? c->style : i == -24 ? c->proc : i == -32 ? c->atom : 0, 2);
}

/* ---------------- hooks ---------------- */
/* SetWindowsHookExA(id, proc, inst, thread) */
void api_SetWindowsHookExA(void)
{
    s32 id = (s32)ARG(0);
    u32 h = 0x7700 + (u32)(id + 2);
    if (id == 5) { hook_cbt = ARG(1); hook_cbt_h = h; }
    else if (id == -1) hook_msgfilter = ARG(1);
    else rt_log("SetWindowsHookExA %d %08x (ignored)\n", id, ARG(1));
    RET(h, 4);
}

void api_UnhookWindowsHookEx(void)
{
    if (ARG(0) == hook_cbt_h) hook_cbt = 0;
    else if (ARG(0) == 0x7700 + 1) hook_msgfilter = 0;
    RET(1, 1);
}

void api_CallNextHookEx(void) { RET(0, 4); }

/* ---------------- create / destroy ---------------- */
static GWnd *alloc_wnd(void)
{
    for (int i = 0; i < NWND; i++)
        if (!wnd[i].used) { memset(&wnd[i], 0, sizeof wnd[i]); wnd[i].used = 1; return &wnd[i]; }
    rt_fatal("out of windows");
    return NULL;
}

static u32 do_create(u32 ex, u32 clsp, u32 title, u32 style, s32 x, s32 y, s32 cx, s32 cy, u32 parent, u32 menu, u32 inst, u32 param)
{
    int ci = cls_find_guest(clsp);
    char cname[64];
    if (clsp < 0x10000) snprintf(cname, sizeof cname, "#%u", clsp); else snprintf(cname, sizeof cname, "%s", gstr(clsp));
    GWnd *w = alloc_wnd();
    u32 h = HW(w);
    w->cls = ci;
    w->proc = ci >= 0 ? cls[ci].proc : defproc_thunk;
    if (ci < 0) rt_log("CreateWindowExA: unknown class %s (DefWindowProc)\n", cname);
    w->style = style;
    w->exstyle = ex;
    if (style & WS_CHILD) { w->parent = parent; w->id = menu; }
    else { w->owner = parent; w->parent = 0; }
    w->inst = inst;
    if (x == (s32)CW_USEDEFAULT) { x = 0; y = 0; }
    if (cx == (s32)CW_USEDEFAULT) { cx = 640; cy = 480; }
    /* top-level windows: the guest screen is 640x480 */
    if (!(style & WS_CHILD)) {
        if (cx > 640) cx = 640;
        if (cy > 480) cy = 480;
        if (x < 0 || x + cx > 640) x = 0;
        if (y < 0 || y + cy > 480) y = 0;
    }
    w->x = x; w->y = y; w->w = cx; w->h = cy;
    w->z = zcount++;
    if (title >= 0x10000) snprintf(w->text, sizeof w->text, "%s", gstr(title));
    rt_log("CreateWindowExA %s '%s' style %08x ex %08x %d,%d %dx%d parent %08x -> %08x\n",
           cname, w->text, style, ex, x, y, cx, cy, parent, h);
    /* CREATESTRUCT in guest memory */
    u32 cs = gcalloc(48);
    WR32(cs, param); WR32(cs + 4, inst); WR32(cs + 8, menu); WR32(cs + 12, parent);
    WR32(cs + 16, cy); WR32(cs + 20, cx); WR32(cs + 24, y); WR32(cs + 28, x);
    WR32(cs + 32, style); WR32(cs + 36, title); WR32(cs + 40, clsp); WR32(cs + 44, ex);
    if (hook_cbt) {
        u32 cbt = gcalloc(8);
        WR32(cbt, cs);
        guest_call(hook_cbt, 3, 3 /* HCBT_CREATEWND */, h, cbt);
        gfree(cbt);
    }
    if (!g_hwnd && !(style & WS_CHILD)) g_hwnd = h;
    u32 r = wm_send(h, WM_NCCREATE, 0, cs);
    if (!r) { rt_log("  WM_NCCREATE refused\n"); w->used = 0; gfree(cs); return 0; }
    r = wm_send(h, WM_CREATE, 0, cs);
    gfree(cs);
    if (r == 0xffffffffu) {
        rt_log("  WM_CREATE refused\n");
        wm_send(h, WM_DESTROY, 0, 0);
        wm_send(h, WM_NCDESTROY, 0, 0);
        w->used = 0;
        return 0;
    }
    wm_send(h, WM_SIZE, 0, (u32)w->w | ((u32)w->h << 16));
    wm_send(h, WM_MOVE, 0, (u32)(w->x & 0xffff) | ((u32)w->y << 16));
    if (style & WS_VISIBLE) {
        w->dirty = 1;
        wm_send(h, WM_SHOWWINDOW, 1, 0);
        if (!(style & WS_CHILD)) {
            win_guest_shown();
            active_wnd = h;
            wm_send(h, WM_ACTIVATEAPP, 1, 0);
            wm_send(h, WM_ACTIVATE, 1, 0);
            if (!focus_wnd) { focus_wnd = h; wm_send(h, WM_SETFOCUS, 0, 0); }
        }
    }
    return h;
}

/* CreateWindowExA(ex, class, title, style, x, y, w, h, parent, menu, inst, param) */
void api_CreateWindowExA(void)
{
    u32 h = do_create(ARG(0), ARG(1), ARG(2), ARG(3), (s32)ARG(4), (s32)ARG(5), (s32)ARG(6), (s32)ARG(7),
                      ARG(8), ARG(9), ARG(10), ARG(11));
    RET(h, 12);
}

static void destroy_rec(u32 h)
{
    GWnd *w = W(h);
    if (!w || w->destroying) return;
    w->destroying = 1;
    if (w->style & WS_VISIBLE) wm_send(h, WM_SHOWWINDOW, 0, 0);
    wm_send(h, WM_DESTROY, 0, 0);
    for (int i = 0; i < NWND; i++)
        if (wnd[i].used && (wnd[i].parent == h || wnd[i].owner == h)) destroy_rec(HW(&wnd[i]));
    wm_send(h, WM_NCDESTROY, 0, 0);
    if (focus_wnd == h) focus_wnd = 0;
    if (capture_wnd == h) capture_wnd = 0;
    if (active_wnd == h) active_wnd = 0;
    if (g_hwnd == h) g_hwnd = 0;
    w = W(h);
    if (w) w->used = 0;
}

void api_DestroyWindow(void)
{
    rt_log("DestroyWindow %08x\n", ARG(0));
    int ok = W(ARG(0)) != NULL;
    destroy_rec(ARG(0));
    RET(ok, 1);
}

/* ---------------- default procedure ---------------- */
static u32 def_proc(u32 h, u32 msg, u32 wp, u32 lp)
{
    GWnd *w = W(h);
    switch (msg) {
    case WM_NCCREATE:
        if (w && lp && RD32(lp + 36) >= 0x10000) snprintf(w->text, sizeof w->text, "%s", gstr(RD32(lp + 36)));
        return 1;
    case WM_CLOSE: destroy_rec(h); return 0;
    case WM_SETTEXT: if (w) snprintf(w->text, sizeof w->text, "%s", lp ? gstr(lp) : ""); return 1;
    case WM_GETTEXT:
        if (!w || !wp) return 0;
        snprintf((char *)MEM + lp, wp, "%s", w->text);
        return (u32)strlen((char *)MEM + lp);
    case WM_GETTEXTLENGTH: return w ? (u32)strlen(w->text) : 0;
    case WM_SETCURSOR:
        if (w && w->parent && wm_send(w->parent, WM_SETCURSOR, wp, lp)) return 1;
        if (w && LOWORD(lp) == HTCLIENT && w->cls >= 0) { win_set_guest_cursor(cls[w->cls].cursor); return 1; }
        return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: if (w) w->dirty = 0; return 0;
    case WM_MOUSEACTIVATE: return 1;   /* MA_ACTIVATE */
    case WM_NCHITTEST: return 1;       /* HTCLIENT */
    case WM_ACTIVATE: if (LOWORD(wp) && w && !(w->style & WS_CHILD)) { focus_wnd = h; } return 0;
    case WM_QUERYENDSESSION: return 1;
    case WM_SYSCOMMAND:
        if ((wp & 0xfff0) == SC_CLOSE) wm_send(h, WM_CLOSE, 0, 0);
        return 0;
    }
    return 0;
}

void api_DefWindowProcA(void) { RET(def_proc(ARG(0), ARG(1), ARG(2), ARG(3)), 4); }

/* ---------------- window longs, properties ---------------- */
void api_GetWindowLongA(void)
{
    GWnd *w = W(ARG(0));
    s32 i = (s32)ARG(1);
    if (!w) RET(0, 2);
    switch (i) {
    case -4: RET(w->proc, 2);
    case -6: RET(w->inst, 2);
    case -8: RET(w->parent, 2);
    case -12: RET(w->id, 2);
    case -16: RET(w->style, 2);
    case -20: RET(w->exstyle, 2);
    case -21: RET(w->user, 2);
    }
    if (i >= 0 && i + 4 <= 64) RET(*(u32 *)(w->extra + i), 2);
    RET(0, 2);
}

void api_SetWindowLongA(void)
{
    GWnd *w = W(ARG(0));
    s32 i = (s32)ARG(1);
    u32 v = ARG(2), old = 0;
    if (!w) RET(0, 3);
    switch (i) {
    case -4: old = w->proc; w->proc = v; break;
    case -6: old = w->inst; w->inst = v; break;
    case -8: old = w->parent; w->parent = v; break;
    case -12: old = w->id; w->id = v; break;
    case -16: old = w->style; w->style = v; break;
    case -20: old = w->exstyle; w->exstyle = v; break;
    case -21: old = w->user; w->user = v; break;
    default:
        if (i >= 0 && i + 4 <= 64) { old = *(u32 *)(w->extra + i); *(u32 *)(w->extra + i) = v; }
    }
    RET(old, 3);
}

static int prop_match(GWnd *w, int k, u32 name)
{
    if (name < 0x10000) return w->props[k].atom == name;
    return !w->props[k].atom && !_stricmp(w->props[k].name, gstr(name));
}

void api_SetPropA(void)
{
    GWnd *w = W(ARG(0));
    if (!w) RET(0, 3);
    for (int k = 0; k < NPROP; k++)
        if (w->props[k].name[0] || w->props[k].atom) { if (prop_match(w, k, ARG(1))) { w->props[k].val = ARG(2); RET(1, 3); } }
    for (int k = 0; k < NPROP; k++)
        if (!w->props[k].name[0] && !w->props[k].atom) {
            if (ARG(1) < 0x10000) w->props[k].atom = ARG(1);
            else snprintf(w->props[k].name, sizeof w->props[k].name, "%s", gstr(ARG(1)));
            w->props[k].val = ARG(2);
            RET(1, 3);
        }
    RET(0, 3);
}

void api_GetPropA(void)
{
    GWnd *w = W(ARG(0));
    if (!w) RET(0, 2);
    for (int k = 0; k < NPROP; k++)
        if ((w->props[k].name[0] || w->props[k].atom) && prop_match(w, k, ARG(1))) RET(w->props[k].val, 2);
    RET(0, 2);
}

void api_RemovePropA(void)
{
    GWnd *w = W(ARG(0));
    if (!w) RET(0, 2);
    for (int k = 0; k < NPROP; k++)
        if ((w->props[k].name[0] || w->props[k].atom) && prop_match(w, k, ARG(1))) {
            u32 v = w->props[k].val;
            memset(&w->props[k], 0, sizeof w->props[k]);
            RET(v, 2);
        }
    RET(0, 2);
}

/* ---------------- geometry ---------------- */
static void rect_out(u32 r, int l, int t, int rr, int b) { WR32(r, l); WR32(r + 4, t); WR32(r + 8, rr); WR32(r + 12, b); }

static void screen_pos(GWnd *w, int *sx, int *sy)
{
    int x = 0, y = 0;
    for (GWnd *p = w; p; p = W(p->parent)) { x += p->x; y += p->y; }
    *sx = x; *sy = y;
}

void api_GetClientRect(void)
{
    GWnd *w = W(ARG(0));
    if (!w) { rect_out(ARG(1), 0, 0, 640, 480); RET(ARG(0) == 0, 2); }
    rect_out(ARG(1), 0, 0, w->w, w->h);
    RET(1, 2);
}

void api_GetWindowRect(void)
{
    GWnd *w = W(ARG(0));
    if (!w) { rect_out(ARG(1), 0, 0, 640, 480); RET(1, 2); }
    int x, y;
    screen_pos(w, &x, &y);
    rect_out(ARG(1), x, y, x + w->w, y + w->h);
    RET(1, 2);
}

void api_ClientToScreen(void)
{
    GWnd *w = W(ARG(0));
    int x = 0, y = 0;
    if (w) screen_pos(w, &x, &y);
    WR32(ARG(1), RD32(ARG(1)) + x);
    WR32(ARG(1) + 4, RD32(ARG(1) + 4) + y);
    RET(1, 2);
}

void api_ScreenToClient(void)
{
    GWnd *w = W(ARG(0));
    int x = 0, y = 0;
    if (w) screen_pos(w, &x, &y);
    WR32(ARG(1), RD32(ARG(1)) - x);
    WR32(ARG(1) + 4, RD32(ARG(1) + 4) - y);
    RET(1, 2);
}

/* MapWindowPoints(from, to, pts, n) */
void api_MapWindowPoints(void)
{
    int fx = 0, fy = 0, tx = 0, ty = 0;
    GWnd *a = W(ARG(0)), *b = W(ARG(1));
    if (a) screen_pos(a, &fx, &fy);
    if (b) screen_pos(b, &tx, &ty);
    int dx = fx - tx, dy = fy - ty;
    for (u32 i = 0; i < ARG(3); i++) {
        WR32(ARG(2) + 8 * i, RD32(ARG(2) + 8 * i) + dx);
        WR32(ARG(2) + 8 * i + 4, RD32(ARG(2) + 8 * i + 4) + dy);
    }
    RET((u32)(dx & 0xffff) | ((u32)dy << 16), 4);
}

static void move_wnd(GWnd *w, int x, int y, int cx, int cy, int move, int size)
{
    u32 h = HW(w);
    int moved = move && (x != w->x || y != w->y), sized = size && (cx != w->w || cy != w->h);
    if (move) { w->x = x; w->y = y; }
    if (size) { w->w = cx; w->h = cy; }
    if (sized) wm_send(h, WM_SIZE, 0, (u32)w->w | ((u32)w->h << 16));
    if (moved) wm_send(h, WM_MOVE, 0, (u32)(w->x & 0xffff) | ((u32)w->y << 16));
    w->dirty = 1;
}

void api_MoveWindow(void)
{
    GWnd *w = W(ARG(0));
    if (!w) RET(0, 6);
    move_wnd(w, (s32)ARG(1), (s32)ARG(2), (s32)ARG(3), (s32)ARG(4), 1, 1);
    RET(1, 6);
}

static void show(GWnd *w, int on);

/* SetWindowPos(h, after, x, y, cx, cy, flags) */
void api_SetWindowPos(void)
{
    GWnd *w = W(ARG(0));
    u32 f = ARG(6);
    if (!w) RET(0, 7);
    move_wnd(w, (s32)ARG(2), (s32)ARG(3), (s32)ARG(4), (s32)ARG(5), !(f & SWP_NOMOVE), !(f & SWP_NOSIZE));
    if (!(f & SWP_NOZORDER)) {
        u32 after = ARG(1);
        if (after == 1 /* HWND_BOTTOM */) w->z = 0;
        else w->z = zcount++;
    }
    if (f & SWP_SHOWWINDOW) show(w, 1);
    if (f & SWP_HIDEWINDOW) show(w, 0);
    RET(1, 7);
}

void api_BeginDeferWindowPos(void) { RET(0x7a00, 1); }
void api_DeferWindowPos(void)
{
    GWnd *w = W(ARG(1));
    u32 f = ARG(7);
    if (w) move_wnd(w, (s32)ARG(3), (s32)ARG(4), (s32)ARG(5), (s32)ARG(6), !(f & SWP_NOMOVE), !(f & SWP_NOSIZE));
    RET(ARG(0), 8);
}
void api_EndDeferWindowPos(void) { RET(1, 1); }

void api_GetWindowPlacement(void)
{
    GWnd *w = W(ARG(0));
    u32 p = ARG(1);
    if (!w) RET(0, 2);
    WR32(p, 44); WR32(p + 4, 0); WR32(p + 8, (w->style & WS_VISIBLE) ? SW_SHOWNORMAL : SW_HIDE);
    WR32(p + 12, 0); WR32(p + 16, 0); WR32(p + 20, -1); WR32(p + 24, -1);
    rect_out(p + 28, w->x, w->y, w->x + w->w, w->y + w->h);
    RET(1, 2);
}
void api_SetWindowPlacement(void) { RET(1, 2); }

/* ---------------- visibility / state ---------------- */
static void show(GWnd *w, int on)
{
    u32 h = HW(w);
    int was = (w->style & WS_VISIBLE) != 0;
    if (on == was) return;
    if (on) w->style |= WS_VISIBLE; else w->style &= ~WS_VISIBLE;
    wm_send(h, WM_SHOWWINDOW, on, 0);
    w->dirty = on;
    if (on && !(w->style & WS_CHILD)) {
        win_guest_shown();
        if (active_wnd != h) {
            active_wnd = h;
            wm_send(h, WM_ACTIVATEAPP, 1, 0);
            wm_send(h, WM_ACTIVATE, 1, 0);
        }
    }
}

void api_ShowWindow(void)
{
    GWnd *w = W(ARG(0));
    if (!w) RET(0, 2);
    int was = (w->style & WS_VISIBLE) != 0;
    u32 cmd = ARG(1);
    if (cmd == SW_HIDE) show(w, 0);
    else if (cmd != SW_MINIMIZE && cmd != SW_SHOWMINNOACTIVE && cmd != SW_SHOWMINIMIZED) show(w, 1);
    win_guest_shown();
    RET(was, 2);
}

void api_IsWindowVisible(void) { GWnd *w = W(ARG(0)); RET(w && visible_chain(w), 1); }
void api_IsWindow(void) { RET(W(ARG(0)) != NULL, 1); }
void api_IsWindowEnabled(void) { GWnd *w = W(ARG(0)); RET(w && !(w->style & WS_DISABLED), 1); }
void api_EnableWindow(void)
{
    GWnd *w = W(ARG(0));
    if (!w) RET(0, 2);
    int was_disabled = (w->style & WS_DISABLED) != 0;
    if (ARG(1)) w->style &= ~WS_DISABLED; else w->style |= WS_DISABLED;
    if (was_disabled != !ARG(1)) wm_send(ARG(0), WM_ENABLE, ARG(1) != 0, 0);
    RET(was_disabled, 2);
}
void api_IsIconic(void) { RET(0, 1); }
void api_IsZoomed(void) { RET(0, 1); }

void api_SetWindowTextA(void) { GWnd *w = W(ARG(0)); if (w) wm_send(ARG(0), WM_SETTEXT, 0, ARG(1)); RET(w != NULL, 2); }
void api_GetWindowTextA(void) { RET(W(ARG(0)) ? wm_send(ARG(0), WM_GETTEXT, ARG(2), ARG(1)) : 0, 3); }
void api_GetWindowTextLengthA(void) { RET(W(ARG(0)) ? wm_send(ARG(0), WM_GETTEXTLENGTH, 0, 0) : 0, 1); }

/* ---------------- relations ---------------- */
void api_GetParent(void) { GWnd *w = W(ARG(0)); RET(w ? (w->parent ? w->parent : w->owner) : 0, 1); }
void api_SetParent(void) { GWnd *w = W(ARG(0)); u32 o = w ? w->parent : 0; if (w) w->parent = ARG(1); RET(o, 2); }
void api_GetDesktopWindow(void) { RET(0x0001fff0u, 0); }
void api_IsChild(void)
{
    GWnd *w = W(ARG(1));
    while (w) { if (w->parent == ARG(0)) RET(1, 2); w = W(w->parent); }
    RET(0, 2);
}
void api_GetDlgCtrlID(void) { GWnd *w = W(ARG(0)); RET(w ? w->id : 0, 1); }
void api_GetDlgItem(void)
{
    for (int i = 0; i < NWND; i++) if (wnd[i].used && wnd[i].parent == ARG(0) && wnd[i].id == ARG(1)) RET(HW(&wnd[i]), 2);
    RET(0, 2);
}

/* children of `parent` (0 = top level) sorted top of the z-order first */
static int children(u32 parent, u32 *out, int max)
{
    int n = 0;
    for (int i = 0; i < NWND && n < max; i++)
        if (wnd[i].used && wnd[i].parent == parent && (parent || !(wnd[i].style & WS_CHILD))) out[n++] = HW(&wnd[i]);
    for (int a = 1; a < n; a++)
        for (int b = a; b > 0 && W(out[b])->z > W(out[b - 1])->z; b--) { u32 t = out[b]; out[b] = out[b - 1]; out[b - 1] = t; }
    return n;
}

void api_GetTopWindow(void)
{
    u32 l[NWND];
    int n = children(ARG(0) == 0x0001fff0u ? 0 : ARG(0), l, NWND);
    RET(n ? l[0] : 0, 1);
}

/* GetWindow(h, cmd): GW_HWNDFIRST 0, LAST 1, NEXT 2, PREV 3, OWNER 4, CHILD 5 */
void api_GetWindow(void)
{
    GWnd *w = W(ARG(0));
    u32 cmd = ARG(1);
    if (!w) RET(0, 2);
    if (cmd == 4) RET(w->owner, 2);
    u32 l[NWND];
    if (cmd == 5) { int n = children(ARG(0), l, NWND); RET(n ? l[0] : 0, 2); }
    int n = children(w->parent, l, NWND), k;
    for (k = 0; k < n && l[k] != ARG(0); k++) ;
    switch (cmd) {
    case 0: RET(n ? l[0] : 0, 2);
    case 1: RET(n ? l[n - 1] : 0, 2);
    case 2: RET(k + 1 < n ? l[k + 1] : 0, 2);
    case 3: RET(k > 0 ? l[k - 1] : 0, 2);
    }
    RET(0, 2);
}

void api_GetLastActivePopup(void) { RET(ARG(0), 1); }
void api_GetActiveWindow(void) { RET(active_wnd, 0); }
void api_GetForegroundWindow(void) { RET(active_wnd, 0); }
void api_SetActiveWindow(void) { u32 o = active_wnd; if (W(ARG(0))) active_wnd = ARG(0); RET(o, 1); }
void api_SetForegroundWindow(void) { if (W(ARG(0))) active_wnd = ARG(0); RET(1, 1); }

void api_GetFocus(void) { RET(focus_wnd, 0); }
void api_SetFocus(void)
{
    u32 o = focus_wnd, n = ARG(0);
    if (n && !W(n)) RET(0, 1);
    if (o != n) {
        if (o && W(o)) wm_send(o, WM_KILLFOCUS, n, 0);
        focus_wnd = n;
        if (n) wm_send(n, WM_SETFOCUS, o, 0);
    }
    RET(o, 1);
}

void api_GetCapture(void) { RET(capture_wnd, 0); }
void api_SetCapture(void) { u32 o = capture_wnd; capture_wnd = ARG(0); RET(o, 1); }
void api_ReleaseCapture(void)
{
    u32 o = capture_wnd;
    capture_wnd = 0;
    if (o && W(o)) wm_send(o, WM_CAPTURECHANGED, 0, 0);
    RET(1, 0);
}

/* EnumWindows(proc, lparam): top-level windows */
void api_EnumWindows(void)
{
    u32 l[NWND];
    int n = children(0, l, NWND);
    for (int i = 0; i < n; i++) if (!guest_call(ARG(0), 2, l[i], ARG(1))) break;
    RET(1, 2);
}

/* the deepest visible, enabled window at a guest screen point */
static u32 hit(u32 parent, int px, int py)
{
    u32 l[NWND];
    int n = children(parent, l, NWND);
    for (int i = 0; i < n; i++) {
        GWnd *w = W(l[i]);
        if (!(w->style & WS_VISIBLE)) continue;
        int sx, sy;
        screen_pos(w, &sx, &sy);
        if (px < sx || py < sy || px >= sx + w->w || py >= sy + w->h) continue;
        if (w->style & WS_DISABLED) return l[i];
        u32 c = hit(l[i], px, py);
        return c ? c : l[i];
    }
    return 0;
}

u32 wm_window_at(int x, int y) { return hit(0, x, y); }
void api_WindowFromPoint(void) { RET(hit(0, (s32)ARG(0), (s32)ARG(1)), 2); }
void api_ChildWindowFromPoint(void)
{
    GWnd *w = W(ARG(0));
    if (!w) RET(0, 3);
    int sx, sy;
    screen_pos(w, &sx, &sy);
    u32 l[NWND];
    int n = children(ARG(0), l, NWND);
    int px = (s32)ARG(1) + sx, py = (s32)ARG(2) + sy;
    for (int i = 0; i < n; i++) {
        GWnd *c = W(l[i]);
        int cx, cy;
        screen_pos(c, &cx, &cy);
        if (px >= cx && py >= cy && px < cx + c->w && py < cy + c->h) RET(l[i], 3);
    }
    RET(ARG(0), 3);
}

/* ---------------- painting ---------------- */
void api_InvalidateRect(void)
{
    if (!ARG(0)) { for (int i = 0; i < NWND; i++) if (wnd[i].used) wnd[i].dirty = 1; RET(1, 3); }
    GWnd *w = W(ARG(0));
    if (w) w->dirty = 1;
    RET(w != NULL, 3);
}
void api_ValidateRect(void) { GWnd *w = W(ARG(0)); if (w) w->dirty = 0; RET(1, 2); }
void api_UpdateWindow(void)
{
    GWnd *w = W(ARG(0));
    if (w && w->dirty && visible_chain(w)) wm_send(ARG(0), WM_PAINT, 0, 0);
    RET(1, 1);
}
void api_GetUpdateRect(void)
{
    GWnd *w = W(ARG(0));
    int d = w && w->dirty;
    if (ARG(1)) rect_out(ARG(1), 0, 0, d ? w->w : 0, d ? w->h : 0);
    RET(d, 3);
}

#define DC_WINDOW 0x00030000u
/* BeginPaint(h, ps): PAINTSTRUCT hdc, erase, rcPaint, ... */
void api_BeginPaint(void)
{
    GWnd *w = W(ARG(0));
    u32 ps = ARG(1);
    memset(MEM + ps, 0, 64);
    WR32(ps, DC_WINDOW | ((ARG(0) - WBASE) / WSTEP));
    if (w) { rect_out(ps + 8, 0, 0, w->w, w->h); w->dirty = 0; }
    RET(RD32(ps), 2);
}
void api_EndPaint(void) { RET(1, 2); }

/* a dirty visible window gets WM_PAINT when the queue is empty */
int wm_paint_pending(u32 msg_addr)
{
    for (int i = 0; i < NWND; i++)
        if (wnd[i].used && wnd[i].dirty && visible_chain(&wnd[i])) {
            u32 h = HW(&wnd[i]);
            WR32(msg_addr, h); WR32(msg_addr + 4, WM_PAINT); WR32(msg_addr + 8, 0); WR32(msg_addr + 12, 0);
            WR32(msg_addr + 16, now_ms()); WR32(msg_addr + 20, 0); WR32(msg_addr + 24, 0);
            return 1;
        }
    return 0;
}

/* ---------------- input routing (from window.c, guest screen coordinates) ---------------- */
void msgq_post(u32 h, u32 msg, u32 wp, u32 lp);
static int last_mx, last_my;

void wm_get_cursor(int *x, int *y) { *x = last_mx; *y = last_my; }

void wm_mouse(u32 msg, u32 keys, int x, int y)
{
    last_mx = x; last_my = y;
    u32 t = capture_wnd && W(capture_wnd) ? capture_wnd : hit(0, x, y);
    if (!t) return;
    int sx, sy;
    screen_pos(W(t), &sx, &sy);
    u32 lp = (u32)((x - sx) & 0xffff) | ((u32)(y - sy) << 16);
    msgq_post(t, msg, keys, lp);
}

/* WM_SETCURSOR for the window under the mouse, sent when a mouse message is fetched */
void wm_mouse_fetched(u32 h, u32 msg)
{
    if (!W(h) || capture_wnd) return;
    wm_send(h, WM_SETCURSOR, h, 1 /* HTCLIENT */ | (msg << 16));
}

void wm_key(u32 msg, u32 vk, u32 lp)
{
    u32 t = focus_wnd && W(focus_wnd) ? focus_wnd : active_wnd ? active_wnd : g_hwnd;
    if (t) msgq_post(t, msg, vk, lp);
}

void wm_activate_app(int on)
{
    u32 l[NWND];
    int n = children(0, l, NWND);
    for (int i = 0; i < n; i++) msgq_post(l[i], WM_ACTIVATEAPP, on, 0);
}

void wm_close(void)
{
    if (g_hwnd) msgq_post(g_hwnd, WM_CLOSE, 0, 0);
    else msgq_quit(0);
}

/* ---------------- messages ---------------- */
void api_SendMessageA(void) { RET(wm_send(ARG(0), ARG(1), ARG(2), ARG(3)), 4); }
void api_SendDlgItemMessageA(void)
{
    for (int i = 0; i < NWND; i++)
        if (wnd[i].used && wnd[i].parent == ARG(0) && wnd[i].id == ARG(1)) RET(wm_send(HW(&wnd[i]), ARG(2), ARG(3), ARG(4)), 5);
    RET(0, 5);
}
void api_PostMessageA(void)
{
    if (ARG(0) == 0xffff) { wm_activate_app(1); RET(1, 4); }
    msgq_post(ARG(0), ARG(1), ARG(2), ARG(3));
    RET(1, 4);
}
void api_CallWindowProcA(void)
{
    u32 r = guest_call(ARG(0), 4, ARG(1), ARG(2), ARG(3), ARG(4));
    RET(r, 5);
}

void api_DispatchMessageA(void)
{
    u32 m = ARG(0);
    u32 h = RD32(m), msg = RD32(m + 4);
    u32 r = 0;
    if (msg == WM_QUIT) RET(0, 1);
    if (!h) RET(0, 1);
    r = wm_send(h, msg, RD32(m + 8), RD32(m + 12));
    if (msg == WM_PAINT) { GWnd *w = W(h); if (w) w->dirty = 0; }
    RET(r, 1);
}

void wm_init(void)
{
    defproc_thunk = api_resolve("USER32.DLL", "DefWindowProcA");
}
