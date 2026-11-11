/* InstallShield 3 ".Z" archives (SUPP.Z on the Generations CD holds the
 * game's sgens.exe): a directory of files, each compressed with PKWARE DCL
 * "implode".  explode() is a small table-driven decoder written from the
 * format (literal / length / distance Shannon-Fano codes, 4-6 bit
 * dictionaries). */
#include "setup.h"
#include <stdlib.h>
#include <string.h>
#include "install.h"

#define MAXBITS 13

typedef struct { short count[MAXBITS + 1]; short sym[256]; } Huff;

typedef struct {
    const u8 *in;
    size_t len, pos;
    u32 bitbuf;
    int bitcnt;
    int err;
} Bits;

static u32 getbits(Bits *s, int need)
{
    u32 val = s->bitbuf;
    while (s->bitcnt < need) {
        if (s->pos >= s->len) { s->err = 1; return 0; }
        val |= (u32)s->in[s->pos++] << s->bitcnt;
        s->bitcnt += 8;
    }
    s->bitbuf = val >> need;
    s->bitcnt -= need;
    return val & ((1u << need) - 1);
}

/* codes are stored bit-inverted, first bit first */
static int decode(Bits *s, const Huff *h)
{
    int code = 0, first = 0, index = 0, len = 1;
    u32 bitbuf = s->bitbuf;
    int left = s->bitcnt;
    for (;;) {
        while (left--) {
            code |= (int)(bitbuf & 1) ^ 1;
            bitbuf >>= 1;
            int count = h->count[len];
            if (code < first + count) {
                s->bitbuf = bitbuf;
                s->bitcnt = (s->bitcnt - len) & 7;
                return h->sym[index + (code - first)];
            }
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
            len++;
        }
        left = (MAXBITS + 1) - len;
        if (left == 0) { s->err = 1; return 0; }
        if (s->pos >= s->len) { s->err = 1; return 0; }
        bitbuf = s->in[s->pos++];
        if (left > 8) left = 8;
    }
}

/* run-length coded code lengths -> canonical decoding tables */
static void construct(Huff *h, const u8 *rep, int n)
{
    u8 length[256];
    int symbol = 0;
    for (int i = 0; i < n; i++) {
        int len = rep[i] & 15, cnt = (rep[i] >> 4) + 1;
        while (cnt--) length[symbol++] = (u8)len;
    }
    memset(h->count, 0, sizeof h->count);
    for (int s = 0; s < symbol; s++) h->count[length[s]]++;
    short offs[MAXBITS + 1];
    offs[1] = 0;
    for (int l = 1; l < MAXBITS; l++) offs[l + 1] = (short)(offs[l] + h->count[l]);
    for (int s = 0; s < symbol; s++)
        if (length[s]) h->sym[offs[length[s]]++] = (short)s;
}

static const u8 LITLEN[] = {
    11, 124, 8, 7, 28, 7, 188, 13, 76, 4, 10, 8, 12, 10, 12, 10, 8, 23, 8, 9, 7, 6, 7, 8, 7, 6, 55, 8, 23, 24, 12, 11, 7, 9, 11, 12,
    6, 7, 22, 5, 7, 24, 6, 11, 9, 6, 7, 22, 7, 11, 38, 7, 9, 8, 25, 11, 8, 11, 9, 12, 8, 12, 5, 38, 5, 38, 5, 11, 7, 5, 6, 21, 6,
    10, 53, 8, 7, 24, 10, 27, 44, 253, 253, 253, 252, 252, 252, 13, 12, 45, 12, 45, 12, 61, 12, 45, 44, 173};
static const u8 LENLEN[] = {2, 35, 36, 53, 38, 23};
static const u8 DISTLEN[] = {2, 20, 53, 230, 247, 151, 248};
static const short BASE[16] = {3, 2, 4, 5, 6, 7, 8, 9, 10, 12, 16, 24, 40, 72, 136, 264};
static const u8 EXTRA[16] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8};

/* PKWARE DCL explode: in -> malloc'd out (expected size known) */
static u8 *explode(const u8 *in, size_t inlen, size_t outlen)
{
    static Huff litcode, lencode, distcode;
    static int ready;
    if (!ready) {
        construct(&litcode, LITLEN, sizeof LITLEN);
        construct(&lencode, LENLEN, sizeof LENLEN);
        construct(&distcode, DISTLEN, sizeof DISTLEN);
        ready = 1;
    }
    Bits s = {in, inlen, 0, 0, 0, 0};
    u8 *out = malloc(outlen ? outlen : 1);
    if (!out) return NULL;
    size_t o = 0;
    int lit = (int)getbits(&s, 8), dict = (int)getbits(&s, 8);
    if (s.err || lit > 1 || dict < 4 || dict > 6) { free(out); return NULL; }
    for (;;) {
        if (getbits(&s, 1)) {
            int sym = decode(&s, &lencode);
            int len = BASE[sym] + (int)getbits(&s, EXTRA[sym]);
            if (len == 519) break;   /* end code */
            int db = len == 2 ? 2 : dict;
            size_t dist = ((size_t)decode(&s, &distcode) << db) + getbits(&s, db) + 1;
            if (s.err || dist > o || o + len > outlen) { free(out); return NULL; }
            for (int i = 0; i < len; i++, o++) out[o] = out[o - dist];
        } else {
            int c = lit ? decode(&s, &litcode) : (int)getbits(&s, 8);
            if (s.err || o >= outlen) { free(out); return NULL; }
            out[o++] = (u8)c;
        }
        if (s.err) { free(out); return NULL; }
    }
    if (o != outlen) { free(out); return NULL; }
    return out;
}

static u16 rd16(const u8 *p) { return (u16)(p[0] | p[1] << 8); }
static u32 rd32(const u8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }

/* extract the files named in 'want' (NULL-terminated, case-insensitive)
   from an InstallShield 3 archive into outdir; returns the number written */
int is3_extract(const char *archive, const char *outdir, const char *const *want)
{
    size_t n;
    u8 *d = read_file(archive, &n);
    if (!d) { setup_log("cannot read %s", archive); return 0; }
    int got = 0;
    if (n < 0x40 || rd32(d) != 0x8c655d13u) { setup_log("%s is not an InstallShield archive", archive); free(d); return 0; }
    u32 nfiles = rd16(d + 0x0c), dirofs = rd32(d + 0x29), fofs = rd32(d + 0x33);
    size_t p = fofs;
    for (u32 i = 0; i < nfiles && p + 0x1e < n; i++) {
        u32 orig = rd32(d + p + 3), comp = rd32(d + p + 7), off = rd32(d + p + 11);
        u16 esz = rd16(d + p + 0x17);
        u8 nl = d[p + 0x1d];
        char name[256];
        memcpy(name, d + p + 0x1e, nl);
        name[nl] = 0;
        (void)dirofs;
        int wanted = 0;
        for (int k = 0; want[k]; k++) if (!_stricmp(want[k], name)) wanted = 1;
        if (wanted && (size_t)off + comp <= n) {
            u8 *raw = explode(d + off, comp, orig);
            char dst[MAX_PATH * 2];
            snprintf(dst, sizeof dst, "%s\\%s", outdir, name);
            if (raw && write_file(dst, raw, orig)) { got++; setup_log("unpacked %s (%u bytes)", name, orig); }
            else setup_log("cannot unpack %s", name);
            free(raw);
        }
        if (!esz) break;
        p += esz;
    }
    free(d);
    return got;
}
