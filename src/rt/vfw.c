/* Video for Windows (MSVFW32 ICLocate / ICSendMessage / ICDecompress) with
 * our own Cinepak decoder.
 *
 * The game reads its AVI movies itself and hands each Cinepak frame to
 * ICDecompress for the format it asks for (the 16-bit screen format).  The
 * decoder keeps a full-colour 24-bit picture of the movie as well: the
 * presenter can show that one instead of the 16-bit copy (vfw_last_frame,
 * Video > Movies: full colour). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include "rt.h"

#define MAXSTRIP 32
typedef struct { u32 px[4]; } CB;    /* four XRGB pixels of a 2x2 block */
typedef struct { CB v4[256], v1[256]; } Strip;

typedef struct {
    int used;
    int w, h;                /* movie size */
    u32 *rgb;                /* decoded picture, XRGB, top-down */
    Strip strips[MAXSTRIP];
    int out_bpp, out_bitfields, out_565, out_topdown;
    u32 frames;
} Codec;

static Codec codec[4];
u32 vfw_frame_counter;
u32 vfw_last_ms;      /* when the latest frame was decoded */

static inline u32 BE16(const u8 *p) { return (p[0] << 8) | p[1]; }
static inline u32 BE24(const u8 *p) { return (p[0] << 16) | (p[1] << 8) | p[2]; }
static inline u32 BE32(const u8 *p) { return ((u32)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
static inline int clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

static void decode_codebook(CB *cb, int id, int size, const u8 *d)
{
    const u8 *eod = d + size;
    int n = (id & 0x04) ? 4 : 6;
    u32 flag = 0, mask = 0;
    for (int i = 0; i < 256; i++) {
        if ((id & 0x01) && !(mask >>= 1)) {
            if (d + 4 > eod) break;
            flag = BE32(d); d += 4; mask = 0x80000000u;
        }
        if (!(id & 0x01) || (flag & mask)) {
            if (d + n > eod) break;
            int y[4], u = 0, v = 0;
            for (int k = 0; k < 4; k++) y[k] = *d++;
            if (n == 6) { u = (s8)*d++; v = (s8)*d++; }
            for (int k = 0; k < 4; k++) {
                int r = clamp8(y[k] + 2 * v), g = clamp8(y[k] - u / 2 - v), b = clamp8(y[k] + 2 * u);
                cb[i].px[k] = (u32)r << 16 | (u32)g << 8 | (u32)b;
            }
        }
    }
}

static int decode_vectors(Codec *c, Strip *s, int x1, int y1, int x2, int y2, int id, int size, const u8 *d)
{
    const u8 *eod = d + size;
    u32 flag = 0, mask = 0;
    for (int y = y1; y < y2; y += 4) {
        for (int x = x1; x < x2; x += 4) {
            if ((id & 0x01) && !(mask >>= 1)) {
                if (d + 4 > eod) return -1;
                flag = BE32(d); d += 4; mask = 0x80000000u;
            }
            if (!(id & 0x01) || (flag & mask)) {
                if (!(id & 0x02) && !(mask >>= 1)) {
                    if (d + 4 > eod) return -1;
                    flag = BE32(d); d += 4; mask = 0x80000000u;
                }
                u32 blk[16];
                if ((id & 0x02) || (~flag & mask)) {
                    if (d >= eod) return -1;
                    const CB *e = &s->v1[*d++];
                    /* each luma of the v1 entry covers a 2x2 quarter */
                    for (int k = 0; k < 16; k++) {
                        int bx = (k & 3) >> 1, by = (k >> 2) >> 1;
                        blk[k] = e->px[by * 2 + bx];
                    }
                } else if (flag & mask) {
                    if (d + 4 > eod) return -1;
                    for (int q = 0; q < 4; q++) {
                        const CB *e = &s->v4[*d++];
                        int qx = (q & 1) * 2, qy = (q >> 1) * 2;
                        blk[qy * 4 + qx] = e->px[0];
                        blk[qy * 4 + qx + 1] = e->px[1];
                        blk[(qy + 1) * 4 + qx] = e->px[2];
                        blk[(qy + 1) * 4 + qx + 1] = e->px[3];
                    }
                } else continue;
                for (int yy = 0; yy < 4; yy++) {
                    if (y + yy >= c->h) break;
                    u32 *row = c->rgb + (y + yy) * c->w;
                    for (int xx = 0; xx < 4; xx++) if (x + xx < c->w) row[x + xx] = blk[yy * 4 + xx];
                }
            }
        }
    }
    return 0;
}

static int decode_strip(Codec *c, Strip *s, int x1, int y1, int x2, int y2, const u8 *d, int size)
{
    const u8 *eod = d + size;
    if (x2 > c->w || y2 > c->h || x1 >= x2 || y1 >= y2) return -1;
    while (d + 4 <= eod) {
        int id = d[0], n = (int)BE24(d + 1) - 4;
        if (n < 0) return -1;
        d += 4;
        if (d + n > eod) n = (int)(eod - d);
        switch (id) {
        case 0x20: case 0x21: case 0x24: case 0x25: decode_codebook(s->v4, id, n, d); break;
        case 0x22: case 0x23: case 0x26: case 0x27: decode_codebook(s->v1, id, n, d); break;
        case 0x30: case 0x31: case 0x32: return decode_vectors(c, s, x1, y1, x2, y2, id, n, d);
        }
        d += n;
    }
    return -1;
}

static int decode_frame(Codec *c, const u8 *d, u32 size)
{
    if (size < 10) return -1;
    const u8 *eod = d + size;
    int flags = d[0];
    int nstrips = (int)BE16(d + 8);
    if (nstrips > MAXSTRIP) nstrips = MAXSTRIP;
    d += 10;
    int y0 = 0;
    for (int i = 0; i < nstrips; i++) {
        if (d + 12 > eod) return -1;
        int y2 = y0 + (int)BE16(d + 8);
        int n = (int)BE24(d + 1) - 12;
        if (n < 0) return -1;
        d += 12;
        if (d + n > eod) n = (int)(eod - d);
        if (i > 0 && !(flags & 0x01)) c->strips[i] = c->strips[i - 1];
        decode_strip(c, &c->strips[i], 0, y0, c->w, y2, d, n);
        d += n;
        y0 = y2;
    }
    return 0;
}

/* ---------------- output ---------------- */
static void output(Codec *c, u32 bi, u32 bits)
{
    int w = (s32)RD32(bi + 4), h = (s32)RD32(bi + 8);
    int bpp = RD16(bi + 14);
    u32 comp = RD32(bi + 16);
    int top = h < 0;
    if (h < 0) h = -h;
    if (w > c->w) w = c->w;
    if (h > c->h) h = c->h;
    int pitch = ((RD32(bi + 4) * bpp / 8) + 3) & ~3;
    int is565 = 0;
    if (bpp == 16 && comp == 3) is565 = RD32(bi + 40) == 0xf800;   /* BI_BITFIELDS red mask */
    for (int y = 0; y < h; y++) {
        const u32 *s = c->rgb + y * c->w;
        u8 *dst = MEM + bits + (top ? y : h - 1 - y) * pitch;
        if (bpp == 16) {
            u16 *o = (u16 *)dst;
            if (is565) for (int x = 0; x < w; x++) { u32 p = s[x]; o[x] = (u16)(((p >> 8) & 0xf800) | ((p >> 5) & 0x07e0) | ((p >> 3) & 0x1f)); }
            else for (int x = 0; x < w; x++) { u32 p = s[x]; o[x] = (u16)(((p >> 9) & 0x7c00) | ((p >> 6) & 0x03e0) | ((p >> 3) & 0x1f)); }
        } else if (bpp == 24) {
            for (int x = 0; x < w; x++) { u32 p = s[x]; dst[3 * x] = (u8)p; dst[3 * x + 1] = (u8)(p >> 8); dst[3 * x + 2] = (u8)(p >> 16); }
        } else if (bpp == 32) memcpy(dst, s, w * 4);
    }
}

/* the latest decoded movie picture (full colour) for the presenter */
const u32 *vfw_last_frame(int *w, int *h)
{
    for (int i = 0; i < 4; i++) if (codec[i].used && codec[i].rgb) { *w = codec[i].w; *h = codec[i].h; return codec[i].rgb; }
    return NULL;
}

static Codec *C(u32 h) { u32 i = h - 0x6c00; return i < 4 && codec[i].used ? &codec[i] : NULL; }

static void log_bi(const char *tag, u32 bi)
{
    if (!bi) { rt_log("vfw: %s (none)\n", tag); return; }
    u32 comp = RD32(bi + 16);
    rt_log("vfw: %s %dx%d %d-bit comp %.4s (%08x)\n", tag, (s32)RD32(bi + 4), (s32)RD32(bi + 8), RD16(bi + 14),
           comp > 0xffff ? (char *)&comp : "rgb", comp);
}

/* ICLocate(type, handler, in, out, flags) */
void api_ICLocate(void)
{
    u32 handler = ARG(1), in = ARG(2);
    u32 comp = in ? RD32(in + 16) : 0;
    u32 cvid = 'c' | 'v' << 8 | 'i' << 16 | 'd' << 24, CVID = 'C' | 'V' << 8 | 'I' << 16 | 'D' << 24;
    log_bi("ICLocate in", in);
    log_bi("ICLocate out", ARG(3));
    if (handler != cvid && handler != CVID && comp != cvid && comp != CVID) RET(0, 5);
    for (int i = 0; i < 4; i++)
        if (!codec[i].used) {
            Codec *c = &codec[i];
            memset(c, 0, sizeof *c);
            c->used = 1;
            c->w = in ? (s32)RD32(in + 4) : 640;
            c->h = in ? (s32)RD32(in + 8) : 480;
            if (c->h < 0) c->h = -c->h;
            c->rgb = calloc((size_t)c->w * c->h, 4);
            RET(0x6c00 + i, 5);
        }
    RET(0, 5);
}

#define ICM_USER 0x4000
void api_ICSendMessage(void)
{
    Codec *c = C(ARG(0));
    u32 msg = ARG(1), p1 = ARG(2), p2 = ARG(3);
    if (!c) RET(0xffffffffu, 4);
    switch (msg) {
    case ICM_USER + 10: /* ICM_DECOMPRESS_GET_FORMAT */
        if (!p2) RET(40, 4);
        memset(MEM + p2, 0, 40);
        WR32(p2, 40); WR32(p2 + 4, c->w); WR32(p2 + 8, c->h); WR16(p2 + 12, 1); WR16(p2 + 14, 24);
        WR32(p2 + 20, c->w * c->h * 3);
        RET(0, 4);
    case ICM_USER + 11: /* ICM_DECOMPRESS_QUERY */
        log_bi("query out", p2);
        RET(0, 4);
    case ICM_USER + 12: /* ICM_DECOMPRESS_BEGIN */
        log_bi("begin in", p1);
        log_bi("begin out", p2);
        RET(0, 4);
    case ICM_USER + 14: /* ICM_DECOMPRESS_END */
        RET(0, 4);
    case 4:             /* DRV_CLOSE */
        free(c->rgb);
        c->used = 0;
        RET(1, 4);
    }
    rt_log("vfw: ICSendMessage %x\n", msg);
    RET(0, 4);
}

/* ICDecompress(hic, flags, in, data, out, bits): cdecl (VFWAPIV) */
void api_ICDecompress(void)
{
    Codec *c = C(ARG(0));
    if (!c) RETC(0xffffffffu);
    u32 in = ARG(2), data = ARG(3), out = ARG(4), bits = ARG(5);
    u32 size = RD32(in + 20);
    if (!c->frames++) { log_bi("decompress in", in); log_bi("decompress out", out); }
    if (size) decode_frame(c, MEM + data, size);
    vfw_frame_counter++;
    vfw_last_ms = now_ms();
    if (!(ARG(1) & 0x80000000u) && bits) output(c, out, bits);   /* not ICDECOMPRESS_HURRYUP */
    RETC(0);
}

void api_ICClose(void) { Codec *c = C(ARG(0)); if (c) { free(c->rgb); c->rgb = NULL; c->used = 0; } RET(0, 1); }
void api_ICOpen(void) { RET(0, 3); }
