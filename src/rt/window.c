/* The game's window, its message queue, input and presentation.
 *
 * The game registers a window class and runs a classic PeekMessage /
 * GetMessage loop; its window procedure (guest code) gets the messages a
 * Windows 95 window would: WM_ACTIVATEAPP, keys, WM_CHAR, mouse in 640x480
 * coordinates.  The real window here is ours: resizable, the 640x480
 * picture scaled into it at 4:3.
 *
 * Test switches: NH_HIDDEN=1 (no window shown, no dialogs), NH_KEYS=
 * "ms:vk[:hold];..." (virtual-key presses), NH_CLICK="x,y,ms;..." (left
 * clicks at game coordinates), NH_SHOT="ms;ms..." (frames -> shots\),
 * NH_QUIT=ms (close after that time). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include "rt.h"

void wm_mouse(u32 msg, u32 keys, int x, int y);
void wm_key(u32 msg, u32 vk, u32 lp);
void wm_activate_app(int on);
void wm_close(void);
static HCURSOR host_cursor;
void di_mouse_move(int dx, int dy);
void di_mouse_button(int b, int down);
void di_mouse_wheel(int dz);
void di_key(int dik, int down);
u8 dik_state[256];
static int last_gx = -1, last_gy = -1;

/* DirectInput mouse: motion in guest pixels between host mouse messages */
void di_mouse_to(int x, int y);
static void di_motion(u32 glp)
{
    int gx = glp & 0xffff, gy = glp >> 16;
    di_mouse_to(gx, gy);
    last_gx = gx; last_gy = gy;
}

static void dik_event(LPARAM lp, int down)
{
    int sc = (lp >> 16) & 0xff;
    if (lp & (1 << 24)) sc |= 0x80;
    dik_state[sc] = down ? 0x80 : 0;
    di_key(sc, down);
}

u32 g_hwnd;
u32 g_wndproc;
int quit_requested;
static HWND hwnd;
static int hidden, cursor_hidden, game_cursor_hidden, in_menu;
static int guest_cursor_count = 0;
static u8 keys[256];
static u32 t0;

/* ---------------- message queue ---------------- */
typedef struct { u32 hwnd, msg, wp, lp, time, x, y; } GMsg;
#define QN 512
static GMsg q[QN];
static int qh, qt;
static int quit_posted, quit_code;

void msgq_post(u32 h, u32 msg, u32 wp, u32 lp)
{
    if ((qt + 1) % QN == qh) return;
    q[qt] = (GMsg){ h, msg, wp, lp, now_ms(), 0, 0 };
    qt = (qt + 1) % QN;
}

void msgq_quit(int code) { quit_posted = 1; quit_code = code; }

/* ---------------- scripted input / screenshots (tests) ---------------- */
typedef struct { u32 at, kind, a, b, hold; int done; } Ev;
static Ev evs[256];
static int nev;
static u32 shots[64];
static int nshots, shot_next;
static u32 quit_at;

static void script_init(void)
{
    const char *k = getenv(ENV("KEYS"));
    while (k && *k && nev < 256) {
        unsigned ms, vk, hold = 100;
        int used = 0;
        if (sscanf(k, "%u:%x%n", &ms, &vk, &used) < 2) break;
        k += used;
        if (*k == ':') { sscanf(k + 1, "%u%n", &hold, &used); k += 1 + used; }
        evs[nev++] = (Ev){ ms, 0, vk, 0, hold, 0 };
        if (*k == ';') k++;
    }
    const char *c = getenv(ENV("CLICK"));
    while (c && *c && nev < 256) {
        unsigned x, y, ms;
        int used = 0;
        if (sscanf(c, "%u,%u,%u%n", &x, &y, &ms, &used) < 3) break;
        c += used;
        evs[nev++] = (Ev){ ms, 1, x, y, 150, 0 };
        if (*c == ';') c++;
    }
    const char *s = getenv(ENV("SHOT"));
    while (s && *s && nshots < 64) {
        shots[nshots++] = (u32)strtoul(s, (char **)&s, 10);
        if (*s == ';' || *s == ',') s++; else break;
    }
    if (getenv(ENV("QUIT"))) quit_at = (u32)atoi(getenv(ENV("QUIT")));
}

static void key_event(u32 vk, int down)
{
    keys[vk & 0xff] = down ? 0x80 : 0;
    wm_key(down ? WM_KEYDOWN : WM_KEYUP, vk, down ? 1 : 0xc0000001u);
    if (down) {
        u32 ch = 0;
        if (vk >= 'A' && vk <= 'Z') ch = vk + 32;
        else if ((vk >= '0' && vk <= '9') || vk == ' ' || vk == 13 || vk == 8 || vk == 27) ch = vk;
        if (ch) wm_key(WM_CHAR, ch, 1);
    }
}

static void save_shot(u32 t);
static int have_frame;
static void script_poll(void)
{
    u32 t = now_ms() - t0;
    while (have_frame && shot_next < nshots && t >= shots[shot_next]) { save_shot(shots[shot_next]); shot_next++; }
    for (int i = 0; i < nev; i++) {
        Ev *e = &evs[i];
        if (e->done == 2) continue;
        if (!e->done && t >= e->at) {
            e->done = 1;
            if (e->kind == 0) key_event(e->a, 1);
            else {   /* a click: move there, press 300 ms later, release after the hold */
                di_motion(e->a | (e->b << 16));
                wm_mouse(WM_MOUSEMOVE, 0, e->a, e->b);
            }
        } else if (e->done == 1 && e->kind == 1 && t >= e->at + 300) {
            e->done = 3;
            keys[VK_LBUTTON] = 0x80;
            di_mouse_button(0, 1);
            wm_mouse(WM_LBUTTONDOWN, 1, e->a, e->b);
        } else if ((e->done == 1 && e->kind == 0 && t >= e->at + e->hold) || (e->done == 3 && t >= e->at + 300 + e->hold)) {
            e->done = 2;
            if (e->kind == 0) key_event(e->a, 0);
            else { keys[VK_LBUTTON] = 0; di_mouse_button(0, 0); wm_mouse(WM_LBUTTONUP, 0, e->a, e->b); }
        }
    }
    if (quit_at && t >= quit_at) {
        void thr_dump(void);
        rt_log("test quit at %u ms\n", t);
        thr_dump();
        reg_save();
        if (rt_logf) fflush(rt_logf);
        TerminateProcess(GetCurrentProcess(), 0);
    }
}

/* ---------------- presentation (present.c: Direct3D 9; GDI fallback here) ---------------- */
int present_init(HWND h);
int present_rgb(const u32 *pix, int sw, int sh);
void present_dest_rect(int cw, int ch, int *dx, int *dy, int *dw, int *dh);
int present_menu_command(int id);
void present_set_fullscreen(int on);
void present_fps(double fps);
void settings_load(void);
extern int opt_fullscreen;
static int d3d_ok;
/* the last frame, 32-bit XRGB; fw x fh is 640x480 times the internal resolution */
#define FMAX (640 * 480 * 16)
static u32 *frame;
static int fw = 640, fh = 480;
static BITMAPINFOHEADER bmi;

static void dest_rect(int cw, int ch, int *x, int *y, int *w, int *h) { present_dest_rect(cw, ch, x, y, w, h); }

static void paint(HDC dc)
{
    if (!have_frame) return;
    if (d3d_ok && present_rgb(frame, fw, fh)) return;
    RECT r;
    GetClientRect(hwnd, &r);
    int x, y, w, h;
    dest_rect(r.right, r.bottom, &x, &y, &w, &h);
    bmi.biSize = sizeof bmi;
    bmi.biWidth = fw;
    bmi.biHeight = -fh;
    bmi.biPlanes = 1;
    bmi.biBitCount = 32;
    bmi.biCompression = BI_RGB;
    SetStretchBltMode(dc, COLORONCOLOR);
    StretchDIBits(dc, x, y, w, h, 0, 0, fw, fh, frame, (BITMAPINFO *)&bmi, DIB_RGB_COLORS, SRCCOPY);
    HBRUSH b = GetStockObject(BLACK_BRUSH);
    RECT bars[4] = { {0, 0, x, r.bottom}, {x + w, 0, r.right, r.bottom}, {x, 0, x + w, y}, {x, y + h, x + w, r.bottom} };
    for (int i = 0; i < 4; i++) if (bars[i].right > bars[i].left && bars[i].bottom > bars[i].top) FillRect(dc, &bars[i], b);
}

void present_shot(const char *path);
static void save_shot(u32 t)
{
    char dir[700], p[760];
    snprintf(dir, sizeof dir, "%s\\shots", rt_exe_dir);
    CreateDirectoryA(dir, NULL);
    snprintf(p, sizeof p, "%s\\shot_%06u.bmp", dir, t);
    if (getenv(ENV("SHOTOUT")) && d3d_ok) {
        present_rgb(frame, fw, fh);
        present_shot(p);
        rt_log("shot %s (presented)\n", p);
        return;
    }
    FILE *f = fopen(p, "wb");
    if (!f) return;
    BITMAPFILEHEADER fh2 = { 0x4d42, sizeof fh2 + 40 + fw * fh * 4, 0, 0, sizeof fh2 + 40 };
    BITMAPINFOHEADER ih = { 40, fw, -fh, 1, 32, 0, fw * fh * 4, 0, 0, 0, 0 };
    fwrite(&fh2, sizeof fh2, 1, f);
    fwrite(&ih, 40, 1, f);
    fwrite(frame, 4, (size_t)fw * fh, f);
    fclose(f);
    rt_log("shot %s\n", p);
}

static int frames;
static void frame_done(void)
{
    have_frame = 1;
    frames++;
    u32 t = now_ms() - t0;
    while (shot_next < nshots && t >= shots[shot_next]) { save_shot(shots[shot_next]); shot_next++; }
    if (hwnd && !hidden && !IsIconic(hwnd)) {
        if (!(d3d_ok && present_rgb(frame, fw, fh))) {
            HDC dc = GetDC(hwnd);
            paint(dc);
            ReleaseDC(hwnd, dc);
        }
    }
    static u32 last_fps;
    static int last_frames;
    if (t - last_fps >= 5000) {
        rt_log("fps %.1f\n", (frames - last_frames) * 1000.0 / (t - last_fps));
        present_fps((frames - last_frames) * 1000.0 / (t - last_fps));
        last_fps = t;
        last_frames = frames;
    }
    script_poll();
}

static void frame_alloc(void) { if (!frame) frame = calloc(FMAX, 4); }

/* 640x480 8-bit with a palette (0x00BBGGRR entries) */
void present_frame(const u8 *pix, int pitch, const u32 *pal)
{
    frame_alloc();
    fw = 640; fh = 480;
    for (int y = 0; y < 480; y++)
        for (int x = 0; x < 640; x++) {
            u32 c = pal[pix[y * pitch + x]];
            frame[y * 640 + x] = ((c & 0xff) << 16) | (c & 0xff00) | ((c >> 16) & 0xff);
        }
    frame_done();
}

/* RGB565, w x h (640x480 times the internal resolution) */
void present_frame16(const u8 *pix, int pitch, int w, int h)
{
    frame_alloc();
    if (w * h > FMAX) return;
    fw = w; fh = h;
    for (int y = 0; y < h; y++) {
        const u16 *s = (const u16 *)(pix + y * pitch);
        u32 *d = frame + y * w;
        for (int x = 0; x < w; x++) {
            u32 c = s[x];
            u32 r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
            d[x] = ((r << 3 | r >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (b << 3 | b >> 2);
        }
    }
    frame_done();
}

/* already 32-bit XRGB (movies decoded at full colour) */
void present_frame32(const u32 *pix, int w, int h)
{
    frame_alloc();
    if (w * h > FMAX) return;
    fw = w; fh = h;
    memcpy(frame, pix, (size_t)w * h * 4);
    frame_done();
}

/* ---------------- host window ---------------- */
static int map_mouse(LPARAM lp, u32 *out)
{
    RECT r;
    GetClientRect(hwnd, &r);
    int x, y, w, h;
    dest_rect(r.right, r.bottom, &x, &y, &w, &h);
    int mx = (short)LOWORD(lp), my = (short)HIWORD(lp);
    int gx = w ? (mx - x) * 640 / w : 0, gy = h ? (my - y) * 480 / h : 0;
    if (gx < 0) gx = 0;
    if (gx > 639) gx = 639;
    if (gy < 0) gy = 0;
    if (gy > 479) gy = 479;
    *out = (u32)gx | ((u32)gy << 16);
    return mx >= x && mx < x + w && my >= y && my < y + h;
}

static LRESULT CALLBACK host_proc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    u32 glp;
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        paint(dc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_CLOSE: wm_close(); return 0;
    case WM_ACTIVATEAPP:   /* the game pauses when it loses focus (hidden test runs: never) */
        if (!hidden) wm_activate_app(wp ? 1 : 0);
        return 0;
    case WM_SYSKEYDOWN:
        if (wp == VK_RETURN) { present_set_fullscreen(!opt_fullscreen); return 0; }   /* Alt+Enter */
        break;
    case WM_SYSCHAR: if (wp == VK_RETURN) return 0; break;
    case WM_COMMAND: if (present_menu_command(LOWORD(wp))) { InvalidateRect(h, NULL, FALSE); return 0; } break;
    case WM_KEYDOWN: keys[wp & 0xff] = 0x80; dik_event(lp, 1); wm_key(WM_KEYDOWN, (u32)wp, (u32)lp); return 0;
    case WM_KEYUP: keys[wp & 0xff] = 0; dik_event(lp, 0); wm_key(WM_KEYUP, (u32)wp, (u32)lp); return 0;
    case WM_MOUSEWHEEL: di_mouse_wheel((short)HIWORD(wp)); return 0;
    case WM_CHAR: wm_key(WM_CHAR, (u32)wp, (u32)lp); return 0;
    case WM_MOUSEMOVE: map_mouse(lp, &glp); di_motion(glp); wm_mouse(WM_MOUSEMOVE, (u32)wp, glp & 0xffff, glp >> 16); return 0;
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_MBUTTONDOWN: case WM_MBUTTONUP:
        map_mouse(lp, &glp);
        di_motion(glp);
        di_mouse_button(m <= WM_LBUTTONUP ? 0 : m <= WM_RBUTTONUP ? 1 : 2, m == WM_LBUTTONDOWN || m == WM_RBUTTONDOWN || m == WM_MBUTTONDOWN);
        if (m == WM_LBUTTONDOWN || m == WM_RBUTTONDOWN) { SetCapture(h); keys[m == WM_LBUTTONDOWN ? VK_LBUTTON : VK_RBUTTON] = 0x80; }
        if (m == WM_LBUTTONUP || m == WM_RBUTTONUP) { ReleaseCapture(); keys[m == WM_LBUTTONUP ? VK_LBUTTON : VK_RBUTTON] = 0; }
        wm_mouse(m, (u32)wp, glp & 0xffff, glp >> 16);
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) { SetCursor(cursor_hidden ? NULL : host_cursor ? host_cursor : LoadCursorA(NULL, IDC_ARROW)); return TRUE; }
        break;
    case WM_SIZE: InvalidateRect(h, NULL, FALSE); return 0;
    case WM_ENTERMENULOOP: in_menu = 1; win_cursor(1); break;
    case WM_EXITMENULOOP: in_menu = 0; win_cursor(!game_cursor_hidden); break;
    }
    return DefWindowProcA(h, m, wp, lp);
}

void win_create(u32 wndproc, const char *title)
{
    g_wndproc = wndproc;
    hidden = getenv(ENV("HIDDEN")) != NULL;
    t0 = now_ms();
    script_init();
    settings_load();
    WNDCLASSA wc = { 0 };
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = host_proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hIcon = LoadIconA(wc.hInstance, MAKEINTRESOURCEA(1));
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    wc.lpszClassName = "GenerationsPC";
    RegisterClassA(&wc);
    /* default size: the largest whole multiple of 640x480 that fits the work area */
    RECT wa;
    SystemParametersInfoA(SPI_GETWORKAREA, 0, &wa, 0);
    int k = 1;
    while (640 * (k + 1) <= wa.right - wa.left - 40 && 480 * (k + 1) <= wa.bottom - wa.top - 80) k++;
    RECT r = { 0, 0, 640 * k, 480 * k };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, TRUE);
    hwnd = CreateWindowExA(0, "GenerationsPC", title, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                           r.right - r.left, r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
    rt_log("window %dx%d (x%d)%s\n", 640 * k, 480 * k, k, hidden ? " hidden" : "");
    d3d_ok = present_init(hwnd);
    int ww, wh;   /* NH_WINSIZE=WxH: client size for test runs / screenshots */
    const char *ws = getenv(ENV("WINSIZE"));
    if (ws && sscanf(ws, "%dx%d", &ww, &wh) == 2) {
        RECT c = { 0, 0, ww, wh };
        AdjustWindowRect(&c, (DWORD)GetWindowLongA(hwnd, GWL_STYLE), GetMenu(hwnd) != NULL);
        SetWindowPos(hwnd, NULL, 0, 0, c.right - c.left, c.bottom - c.top, SWP_NOMOVE | SWP_NOZORDER);
    }
}

int win_pump(int wait)
{
    MSG m;
    if (wait && qh == qt && !quit_posted) MsgWaitForMultipleObjects(0, NULL, FALSE, 5, QS_ALLINPUT);
    while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&m);
        DispatchMessageA(&m);
    }
    script_poll();
    timers_poll();
    return !quit_posted;
}

void win_cursor(int show)
{
    if (!in_menu) game_cursor_hidden = !show;
    cursor_hidden = in_menu ? 0 : !show;
    if (hwnd) {
        POINT p;
        GetCursorPos(&p);
        SetCursorPos(p.x, p.y);   /* re-evaluates WM_SETCURSOR */
    }
}

u32 win_key_state(int vk) { return keys[vk & 0xff]; }

/* ---------------- USER32 natives (queue, cursor, keys) ---------------- */
void wm_mouse_fetched(u32 h, u32 msg);
int wm_paint_pending(u32 msg_addr);
void wm_get_cursor(int *x, int *y);

static void write_msg(u32 a, GMsg *m)
{
    WR32(a, m->hwnd); WR32(a + 4, m->msg); WR32(a + 8, m->wp); WR32(a + 12, m->lp);
    WR32(a + 16, m->time); WR32(a + 20, m->x); WR32(a + 24, m->y);
}

int msgq_pending(void) { return qh != qt || quit_posted; }

int msgq_get(u32 msg_addr, u32 h, u32 lo, u32 hi, int remove)
{
    (void)h;
    for (int i = qh; i != qt; i = (i + 1) % QN) {
        if ((lo || hi) && (q[i].msg < lo || q[i].msg > hi)) continue;
        GMsg m = q[i];
        int mx, my;
        wm_get_cursor(&mx, &my);
        m.x = mx; m.y = my;
        write_msg(msg_addr, &m);
        if (remove) {
            for (int j = i; j != qh; ) { int p = (j - 1 + QN) % QN; q[j] = q[p]; j = p; }
            qh = (qh + 1) % QN;
            if (m.msg >= WM_MOUSEFIRST && m.msg <= WM_MOUSELAST) wm_mouse_fetched(m.hwnd, m.msg);
        }
        return 1;
    }
    if (quit_posted && (!lo || lo <= WM_QUIT) && (!hi || hi >= WM_QUIT)) {
        GMsg m = { 0, WM_QUIT, (u32)quit_code, 0, now_ms(), 0, 0 };
        write_msg(msg_addr, &m);
        return 1;
    }
    if ((!lo && !hi) || (lo <= WM_PAINT && hi >= WM_PAINT)) return wm_paint_pending(msg_addr);
    return 0;
}

/* GetMessageA(msg, hwnd, min, max) */
void api_GetMessageA(void)
{
    for (;;) {
        win_pump(0);
        if (msgq_get(ARG(0), ARG(1), ARG(2), ARG(3), 1)) RET(RD32(ARG(0) + 4) != WM_QUIT, 4);
        thr_yield();
        if (!msgq_pending()) win_pump(1);
    }
}

/* PeekMessageA(msg, hwnd, min, max, remove) */
void api_PeekMessageA(void)
{
    static u32 n;
    win_pump(0);
    if ((++n & 3) == 0) thr_yield();
    RET(msgq_get(ARG(0), ARG(1), ARG(2), ARG(3), ARG(4) & 1), 5);
}

void api_TranslateMessage(void) { RET(0, 1); }   /* WM_CHAR comes from the host window */
void api_PostQuitMessage(void) { rt_log("PostQuitMessage(%d)\n", ARG(0)); msgq_quit((int)ARG(0)); RET(0, 1); }
void api_GetMessageTime(void) { RET(now_ms(), 0); }
void api_GetMessagePos(void) { int x, y; wm_get_cursor(&x, &y); RET((u32)(x & 0xffff) | ((u32)y << 16), 0); }

/* show the host window when the game first shows one of its own */
void win_guest_shown(void)
{
    static int shown;
    if (shown) return;
    shown = 1;
    if (!hwnd) win_create(0, GAME_TITLE);
    if (hwnd && !hidden) { ShowWindow(hwnd, SW_SHOWNORMAL); SetForegroundWindow(hwnd); }
}

/* cursors: the game's own (resources of sgens.exe) become host cursors */
HCURSOR res_cursor(u32 id);
static u32 guest_cursor;
void win_set_guest_cursor(u32 c)
{
    guest_cursor = c;
    host_cursor = (c >= 0x00050000u && c < 0x00060000u) ? res_cursor(c & 0xffff) : c ? LoadCursorA(NULL, IDC_ARROW) : NULL;
    win_cursor(c != 0 && guest_cursor_count >= 0);
}
void api_SetCursor(void) { u32 o = guest_cursor; win_set_guest_cursor(ARG(0)); RET(o, 1); }
void api_ShowCursor(void) { guest_cursor_count += ARG(0) ? 1 : -1; win_cursor(guest_cursor_count >= 0); RET(guest_cursor_count, 1); }
/* cursor handles: 0x5xxxx = resource id of sgens.exe, 0x6xxxx = system cursor */
void api_LoadCursorA(void) { u32 id = ARG(1); RET(ARG(0) ? 0x00050000u | (id & 0xffff) : 0x00060000u | (id & 0xffff), 2); }
void api_LoadIconA(void) { RET(0x00070000u | (ARG(1) & 0xffff), 2); }
void api_GetKeyState(void) { u32 s = keys[ARG(0) & 0xff]; RET(s ? 0xff80u : 0, 1); }
void api_GetAsyncKeyState(void) { u32 s = keys[ARG(0) & 0xff]; RET(s ? 0x8000u : 0, 1); }
void api_GetKeyboardState(void) { memcpy(MEM + ARG(0), keys, 256); RET(1, 1); }
void api_GetCursorPos(void) { int x, y; wm_get_cursor(&x, &y); WR32(ARG(0), x); WR32(ARG(0) + 4, y); RET(1, 1); }
void api_SetCursorPos(void)
{
    int gx = (s32)ARG(0), gy = (s32)ARG(1);
    if (hwnd && !hidden && GetForegroundWindow() == hwnd) {
        RECT r;
        GetClientRect(hwnd, &r);
        int x, y, w, h;
        dest_rect(r.right, r.bottom, &x, &y, &w, &h);
        POINT p = { x + gx * w / 640, y + gy * h / 480 };
        ClientToScreen(hwnd, &p);
        SetCursorPos(p.x, p.y);
    }
    wm_mouse(WM_MOUSEMOVE, 0, gx, gy);
    RET(1, 2);
}
void api_GetWindowThreadProcessId(void) { if (ARG(1)) WR32(ARG(1), 0x40); RET(0x44, 2); }
void api_GetSystemMetrics(void)
{
    u32 i = ARG(0);
    RET(i == SM_CXSCREEN ? 640 : i == SM_CYSCREEN ? 480 : i == SM_CXCURSOR || i == SM_CYCURSOR ? 32 :
        i == SM_MOUSEPRESENT ? 1 : i == SM_CXFULLSCREEN ? 640 : i == SM_CYFULLSCREEN ? 460 : i == SM_CMOUSEBUTTONS ? 3 : 0, 1);
}

/* whole-number scale of the presented picture (cursor size) */
int win_scale(void)
{
    if (!hwnd) return 1;
    RECT r;
    GetClientRect(hwnd, &r);
    int x, y, w, h;
    dest_rect(r.right, r.bottom, &x, &y, &w, &h);
    int s = (w + 320) / 640;
    return s < 1 ? 1 : s > 4 ? 4 : s;
}
