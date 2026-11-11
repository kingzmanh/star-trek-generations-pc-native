/* Real higher internal resolution for the away missions' 3D (the DRE engine).
 *
 * DRE renders 8-bit into a buffer the game hands it each frame
 * (4b5a90(buffer, pitch)); its screen size is set by 4b58a0(width, height,
 * flip), which derives every projection constant and per-column / per-row
 * table from those two numbers.  So DRE is told it has an N x screen, renders
 * the frame once into an N x scratch buffer, and the game's own 1x buffer gets
 * the centre sample of every N x N block - the game (and its 8->16-bit copy to
 * the screen) sees the same picture as before.  The copy routine is watched to
 * learn where the view lands on the screen and with which 8->16-bit colour
 * table; the presenter then draws the N x picture over that rectangle wherever
 * the screen still shows what the copy put there (HUD sprites drawn later win). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include "rt.h"

#define HR_BUF 0x38000000u          /* guest scratch for the N x 8-bit picture */

static int base_w, base_h, base_flip, set_n = 1;   /* the game's own DRE size, the N it was set to */
static u32 lo_buf, lo_pitch;                       /* this frame's 1x buffer */
static int frame_n;
int dre_hires_debug;
static u8 *lo_copy;                 /* the 1x picture as the game got it */
static u32 frame_ms;
/* where the picture was blitted: destination surface memory, pitch, origin */
static u32 dst_mem;
static int dst_pitch, dst_x, dst_y;

static int want_n(void)
{
    extern int opt_render;
    int n = opt_render < 1 ? 1 : opt_render > 4 ? 4 : opt_render;
    return n;
}

/* 4b58a0(width, height, flip): DRE screen size */
void N_dre_setsize(void)
{
    void (*orig)(void) = rt_wrap_orig("N_dre_setsize", NULL);
    base_w = (int)(ARG(0) & 0xffff);
    base_h = (int)(ARG(1) & 0xffff);
    base_flip = (int)ARG(2);
    int n = want_n();
    if (base_w * n > 2560 || base_h * n > 2048) n = 1;
    set_n = n;
    rt_log("dre: screen %dx%d -> rendering at %dx\n", base_w, base_h, n);
    if (n > 1) {
        u32 sp = cpu.r[R_ESP];
        WR32(sp + 4, (u32)(base_w * n));
        WR32(sp + 8, (u32)(base_h * n));
    }
    orig();
}

/* 4b5a90(buffer, pitch): render a frame */
void N_dre_render(void)
{
    void (*orig)(void) = rt_wrap_orig("N_dre_render", NULL);
    int n = set_n;
    lo_buf = ARG(0);
    lo_pitch = ARG(1);
    if (n < 2) { frame_n = 1; orig(); return; }
    int W = base_w * n, H = base_h * n;
    VirtualAlloc(MEM + HR_BUF, (size_t)W * H + 0x10000, MEM_COMMIT, PAGE_READWRITE);
    u32 sp = cpu.r[R_ESP];
    WR32(sp + 4, HR_BUF);
    WR32(sp + 8, (u32)W);
    orig();
    /* the game's 1x picture: centre sample of every block */
    for (int y = 0; y < base_h; y++) {
        const u8 *s = MEM + HR_BUF + (u32)((y * n + n / 2) * W) + n / 2;
        u8 *d = MEM + lo_buf + (u32)y * lo_pitch;
        for (int x = 0; x < base_w; x++) d[x] = s[x * n];
    }
    frame_n = n;
    lo_copy = realloc(lo_copy, (size_t)base_w * base_h);
    for (int y = 0; y < base_h; y++) memcpy(lo_copy + y * base_w, MEM + lo_buf + (u32)y * lo_pitch, base_w);
    frame_ms = now_ms();
    dst_mem = 0;
}

/* ddraw.c: every blit.  The DRE picture goes to an offscreen composite
   first, which is then copied on - follow it to the surface that is shown. */
static u32 chain_mem;          /* surface memory that currently holds the picture */
static int chain_x, chain_y;
void dre_note_blit(u32 src_mem, u32 dmem, int dpitch, int dx, int dy, int sx, int sy)
{
    if (!lo_buf || frame_n < 2) return;
    if (src_mem == lo_buf) {
        chain_mem = dmem; chain_x = dx - sx; chain_y = dy - sy;
        dst_mem = dmem; dst_pitch = dpitch; dst_x = chain_x; dst_y = chain_y;
    } else if (chain_mem && src_mem == chain_mem && dmem != chain_mem) {
        chain_x += dx - sx; chain_y += dy - sy; chain_mem = dmem;
        dst_mem = dmem; dst_pitch = dpitch; dst_x = chain_x; dst_y = chain_y;
    }
}

/* ddraw.c, Flip: the back buffer that held the picture is now the front */
void dre_note_flip(u32 old_back, u32 new_front)
{
    if (dst_mem == old_back) dst_mem = new_front;
}

/* ddraw.c, presenting an 8-bit frame: the N x layer over the view, shown
   where the frame still has the pixel the game's picture put there */
static u32 *layer;
void dre_compose(u32 mem, int pitch, const u32 *pal)
{
    hires_layer.valid = 0;
    int n = frame_n;
    if (n < 2 || !dst_mem || dst_mem != mem || now_ms() - frame_ms > 250 || !lo_copy) return;
    int W = base_w * n, H = base_h * n;
    layer = realloc(layer, (size_t)W * H * 4);
    u32 argb[256];
    for (int i = 0; i < 256; i++) { u32 c = pal[i]; argb[i] = 0xff000000u | (c & 0xff) << 16 | (c & 0xff00) | ((c >> 16) & 0xff); }
    int any = 0;
    for (int y = 0; y < base_h; y++) {
        int sy = dst_y + y;
        const u8 *scr = sy >= 0 && sy < 480 ? MEM + mem + (u32)sy * pitch : NULL;
        for (int x = 0; x < base_w; x++) {
            int sx = dst_x + x;
            int on = scr && sx >= 0 && sx < 640 && scr[sx] == lo_copy[y * base_w + x];
            any |= on;
            for (int yy = 0; yy < n; yy++) {
                u32 *o = layer + (size_t)(y * n + yy) * W + x * n;
                if (!on) { for (int xx = 0; xx < n; xx++) o[xx] = 0; continue; }
                const u8 *h = MEM + HR_BUF + (u32)((y * n + yy) * W + x * n);
                for (int xx = 0; xx < n; xx++) o[xx] = argb[h[xx]];
            }
        }
    }
    if (!any) return;
    hires_layer = (HiresLayer){ layer, W, H, dst_x, dst_y, base_w, base_h, 1 };
    static int shot_done;
    const char *hs = getenv(ENV("HRSHOT"));
    extern u32 win_time(void);
    if (hs && !shot_done && win_time() >= (u32)atoi(hs)) {   /* test: the N x layer as a BMP */
        shot_done = 1;
        char path[700];
        snprintf(path, sizeof path, "%s/shots/hires_%dx.bmp", rt_exe_dir, n);
        FILE *f = fopen(path, "wb");
        if (f) {
            BITMAPFILEHEADER fh = { 0x4d42, (DWORD)(sizeof fh + 40 + (size_t)W * H * 4), 0, 0, sizeof fh + 40 };
            BITMAPINFOHEADER ih = { 40, W, -H, 1, 32, 0, (DWORD)((size_t)W * H * 4), 0, 0, 0, 0 };
            fwrite(&fh, sizeof fh, 1, f); fwrite(&ih, 40, 1, f); fwrite(layer, 4, (size_t)W * H, f);
            fclose(f);
            rt_log("hires layer saved: %s (at %d,%d)\n", path, dst_x, dst_y);
        }
    }
}

/* ddraw.c: every blit.  The DRE picture goes to an offscreen composite
   first, which is then copied on - follow it to the surface that is shown. */
static u32 chain_mem;          /* surface memory that currently holds the picture */
static int chain_x, chain_y;
void dre_note_blit(u32 src_mem, u32 dmem, int dpitch, int dx, int dy, int sx, int sy)
{
    if (!lo_buf || frame_n < 2) return;
    if (src_mem == lo_buf) {
        chain_mem = dmem; chain_x = dx - sx; chain_y = dy - sy;
        dst_mem = dmem; dst_pitch = dpitch; dst_x = chain_x; dst_y = chain_y;
    } else if (chain_mem && src_mem == chain_mem && dmem != chain_mem) {
        chain_x += dx - sx; chain_y += dy - sy; chain_mem = dmem;
        dst_mem = dmem; dst_pitch = dpitch; dst_x = chain_x; dst_y = chain_y;
    }
}

/* ddraw.c, Flip: the back buffer that held the picture is now the front */
void dre_note_flip(u32 old_back, u32 new_front)
{
    if (dst_mem == old_back) dst_mem = new_front;
}

/* ddraw.c, presenting an 8-bit frame: the N x layer over the view, shown
   where the frame still has the pixel the game's picture put there */
static u32 *layer;
void dre_compose(u32 mem, int pitch, const u32 *pal)
{
    hires_layer.valid = 0;
    int n = frame_n;
    if (n < 2 || !dst_mem || dst_mem != mem || now_ms() - frame_ms > 250 || !lo_copy) return;
    int W = base_w * n, H = base_h * n;
    layer = realloc(layer, (size_t)W * H * 4);
    u32 argb[256];
    for (int i = 0; i < 256; i++) { u32 c = pal[i]; argb[i] = 0xff000000u | (c & 0xff) << 16 | (c & 0xff00) | ((c >> 16) & 0xff); }
    int any = 0;
    for (int y = 0; y < base_h; y++) {
        int sy = dst_y + y;
        const u8 *scr = sy >= 0 && sy < 480 ? MEM + mem + (u32)sy * pitch : NULL;
        for (int x = 0; x < base_w; x++) {
            int sx = dst_x + x;
            int on = scr && sx >= 0 && sx < 640 && scr[sx] == lo_copy[y * base_w + x];
            any |= on;
            for (int yy = 0; yy < n; yy++) {
                u32 *o = layer + (size_t)(y * n + yy) * W + x * n;
                if (!on) { for (int xx = 0; xx < n; xx++) o[xx] = 0; continue; }
                const u8 *h = MEM + HR_BUF + (u32)((y * n + yy) * W + x * n);
                for (int xx = 0; xx < n; xx++) o[xx] = argb[h[xx]];
            }
        }
    }
    if (!any) return;
    hires_layer = (HiresLayer){ layer, W, H, dst_x, dst_y, base_w, base_h, 1 };
    static int shot_done;
    const char *hs = getenv(ENV("HRSHOT"));
    extern u32 win_time(void);
    if (hs && !shot_done && win_time() >= (u32)atoi(hs)) {   /* test: the N x layer as a BMP */
        shot_done = 1;
        char path[700];
        snprintf(path, sizeof path, "%s/shots/hires_%dx.bmp", rt_exe_dir, n);
        FILE *f = fopen(path, "wb");
        if (f) {
            BITMAPFILEHEADER fh = { 0x4d42, (DWORD)(sizeof fh + 40 + (size_t)W * H * 4), 0, 0, sizeof fh + 40 };
            BITMAPINFOHEADER ih = { 40, W, -H, 1, 32, 0, (DWORD)((size_t)W * H * 4), 0, 0, 0, 0 };
            fwrite(&fh, sizeof fh, 1, f); fwrite(&ih, 40, 1, f); fwrite(layer, 4, (size_t)W * H, f);
            fclose(f);
            rt_log("hires layer saved: %s (at %d,%d)\n", path, dst_x, dst_y);
        }
    }
}

/* probes: which 8->16-bit copy carries the DRE picture to the screen */
static void probe(const char *name)
{
    if (!dre_hires_debug || !lo_buf) return;
    for (int i = 0; i < 8; i++) {
        u32 v = ARG(i);
        if (v >= lo_buf && v < lo_buf + lo_pitch * (u32)base_h) {
            static char last[32]; static int cnt;
            if (strcmp(last, name)) { strcpy(last, name); cnt = 0; }
            if (cnt++ < 3)
                rt_log("dre probe %s this %08x args %08x %08x %08x %08x %08x %08x %08x %08x (arg %d = buffer+%x)\n", name, cpu.r[R_ECX],
                       ARG(0), ARG(1), ARG(2), ARG(3), ARG(4), ARG(5), ARG(6), ARG(7), i, v - lo_buf);
        }
    }
}
