/* Real higher internal resolution for space combat's viewscreen.
 *
 * The space engine draws the viewscreen 8-bit into its own buffer
 * (54f66c, 532x231, pitch 570238), then 494b64 converts it through a
 * 256-entry RGB565 table (56fb98) into the game's 16-bit screen at (52,10).
 * The 3D library (4e0000-4fd000) is parametric: its size 570230/570234,
 * pitch 570238, buffer 54f66c, z-buffer 5c9d24, and the window / centre it
 * derives in 4e7eb0 from fractions of that size (block 5a5140-5a5260).
 *
 * Per frame (494d18): the game clears the buffer and draws the star field
 * at 1x, then 4e2d70 starts the 3D.  There the library is switched to an
 * N x scratch buffer (seeded with the 1x picture upscaled) and an N x
 * z-buffer, and its window is re-derived from the fractions the game last
 * gave 4e7eb0 for this view.  At the conversion (494b64) everything is put
 * back, the game's buffer gets the centre sample of each N x block, and the
 * N x picture becomes the presenter's layer over the viewscreen (shown where
 * the screen still has the converted pixel - HUD drawn later wins). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include "rt.h"

#define HR_BUF  0x3a000000u      /* guest scratch: N x 8-bit picture */
#define HR_Z    0x3c000000u      /* guest scratch: N x z-buffer */
#define G_W     0x570230u
#define G_H     0x570234u
#define G_PITCH 0x570238u
#define G_BUF   0x54f66cu
#define G_ZBUF  0x5c9d24u
#define G_MODE  0x54a0dcu        /* 2: 8-bit buffer + conversion */
#define G_TAB   0x56fb98u        /* 8-bit -> RGB565 */
#define G_SCR   0x5700c4u        /* the game's 16-bit screen */
#define G_SCRPITCH 0x54a0d8u
#define BLK     0x5a5140u        /* the library's derived window block */
#define BLK_N   0xd8u        /* 5a5140-5a5217 (not the per-frame counters after it) */
#define MAIN_W  0x214
#define MAIN_H  0xe7

static u32 vp_args[8];
static int have_args, active, pass_n;
static u8 save_blk[BLK_N];
static u32 save_w, save_h, save_pitch, save_buf, save_z;
static u8 *lo_copy;
static u16 tab[256];
static int frame_n, out_x, out_y;
static u32 frame_ms, out_scr;

static int want_n(void)
{
    extern int opt_render;
    return opt_render < 1 ? 1 : opt_render > 4 ? 4 : opt_render;
}

/* 4e7eb0(left, top, right, bottom, cx, cy, ., .): the window as fractions of
   the size - remember the main view's, to derive the N x window from */
void N_sp_window(void)
{
    if (!active && RD32(G_W) == MAIN_W && RD32(G_H) == MAIN_H) {
        for (int i = 0; i < 8; i++) vp_args[i] = ARG(i);
        have_args = 1;
    }
    rt_wrap_orig("N_sp_window", NULL)();
}

/* 4e2d70: the 3D part of a frame begins */
void N_sp_begin(void)
{
    void (*orig)(void) = rt_wrap_orig("N_sp_begin", NULL);
    int n = want_n();
    if (active || n < 2 || !have_args || RD32(G_MODE) != 2 || RD32(G_W) != MAIN_W || RD32(G_H) != MAIN_H ||
        RD32(G_PITCH) != MAIN_W) { orig(); return; }
    int w = MAIN_W, h = MAIN_H, W = w * n, H = h * n;
    VirtualAlloc(MEM + HR_BUF, (size_t)W * H + 0x10000, MEM_COMMIT, PAGE_READWRITE);
    VirtualAlloc(MEM + HR_Z, (size_t)W * H * 4 + 0x10000, MEM_COMMIT, PAGE_READWRITE);
    save_w = RD32(G_W); save_h = RD32(G_H); save_pitch = RD32(G_PITCH); save_buf = RD32(G_BUF); save_z = RD32(G_ZBUF);
    memcpy(save_blk, MEM + BLK, BLK_N);
    /* seed: the 1x picture so far (cleared background, stars) */
    for (int y = 0; y < h; y++) {
        const u8 *s = MEM + save_buf + (u32)y * save_pitch;
        for (int yy = 0; yy < n; yy++) {
            u8 *d = MEM + HR_BUF + (u32)((y * n + yy) * W);
            for (int x = 0; x < w; x++) memset(d + x * n, s[x], n);
        }
    }
    WR32(G_W, (u32)W); WR32(G_H, (u32)H); WR32(G_PITCH, (u32)W); WR32(G_BUF, HR_BUF); WR32(G_ZBUF, HR_Z);
    guest_call(0x4e7eb0, 8, vp_args[0], vp_args[1], vp_args[2], vp_args[3], vp_args[4], vp_args[5], vp_args[6], vp_args[7]);
    active = 1;
    pass_n = n;
    orig();
}

static void finish(void)
{
    if (!active) return;
    active = 0;
    int n = pass_n, w = MAIN_W, h = MAIN_H, W = w * n;
    WR32(G_W, save_w); WR32(G_H, save_h); WR32(G_PITCH, save_pitch); WR32(G_BUF, save_buf); WR32(G_ZBUF, save_z);
    memcpy(MEM + BLK, save_blk, BLK_N);
    lo_copy = realloc(lo_copy, (size_t)w * h);
    for (int y = 0; y < h; y++) {
        const u8 *s = MEM + HR_BUF + (u32)((y * n + n / 2) * W) + n / 2;
        u8 *d = MEM + save_buf + (u32)y * save_pitch;
        for (int x = 0; x < w; x++) d[x] = s[x * n];
        memcpy(lo_copy + y * w, d, w);
    }
    memcpy(tab, MEM + G_TAB, sizeof tab);
    frame_n = n;
    frame_ms = now_ms();
}

/* 494b64(x, y): the 8-bit picture converted into the 16-bit screen */
void N_sp_convert(void)
{
    finish();
    if (frame_n > 1 && now_ms() - frame_ms < 100) {
        out_x = (s32)ARG(0); out_y = (s32)ARG(1);
        out_scr = RD32(G_SCR);
    }
    rt_wrap_orig("N_sp_convert", NULL)();
}

/* ddraw.c, presenting a 16-bit frame: the N x layer over the viewscreen */
static u32 *layer;
void space_compose(u32 mem, int pitch)
{
    int n = frame_n;
    if (n < 2 || !lo_copy || now_ms() - frame_ms > 250 || want_n() != n) return;
    int w = MAIN_W, h = MAIN_H, W = w * n, H = h * n;
    layer = realloc(layer, (size_t)W * H * 4);
    u32 argb[256];
    for (int i = 0; i < 256; i++) {
        u32 c = tab[i], r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
        argb[i] = 0xff000000u | (r << 3 | r >> 2) << 16 | (g << 2 | g >> 4) << 8 | (b << 3 | b >> 2);
    }
    int any = 0;
    for (int y = 0; y < h; y++) {
        int sy = out_y + y;
        const u16 *scr = sy >= 0 && sy < 480 ? (const u16 *)(MEM + mem + (u32)sy * pitch) : NULL;
        for (int x = 0; x < w; x++) {
            int sx = out_x + x;
            int on = scr && sx >= 0 && sx < 640 && scr[sx] == tab[lo_copy[y * w + x]];
            any |= on;
            for (int yy = 0; yy < n; yy++) {
                u32 *o = layer + (size_t)(y * n + yy) * W + x * n;
                if (!on) { for (int xx = 0; xx < n; xx++) o[xx] = 0; continue; }
                const u8 *s = MEM + HR_BUF + (u32)((y * n + yy) * W + x * n);
                for (int xx = 0; xx < n; xx++) o[xx] = argb[s[xx]];
            }
        }
    }
    if (any) hires_layer = (HiresLayer){ layer, W, H, out_x, out_y, w, h, 1 };
}
