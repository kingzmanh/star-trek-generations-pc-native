/* DirectInput replacement (IDirectInputA / IDirectInputDeviceA).
 *
 * The game uses DirectInput for the mouse only: buffered relative motion
 * and buttons (c_dfDIMouse), with an event it waits on.  The host window
 * (window.c) feeds mouse motion in guest pixels, so the game's own cursor
 * follows the real one 1:1 at any window size.  The keyboard comes in as
 * window messages; a keyboard device works too (GetDeviceState, DIK codes). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include "rt.h"

#define DI_OK 0
#define DIERR_NOTACQUIRED 0x8007000Cu
#define DIERR_INVALIDPARAM 0x80070057u
#define DIERR_DEVICENOTREG 0x80040154u

typedef struct { u32 ofs, data, time, seq; } DOD;
#define QN 256

typedef struct {
    u32 obj;
    int kind;              /* 1 mouse, 2 keyboard */
    int acquired;
    u32 event;
    u32 bufsize;
    DOD q[QN];
    int qh, qt;
    int dx, dy, dz;        /* for GetDeviceState */
    u8 btn[4];
} Dev;

static Dev devs[4];
static u32 vt_di, vt_dev, obj_di;
static u32 seq;
extern u8 dik_state[256];
void thr_signal(u32 h);

static Dev *D(u32 obj) { u32 i = RD32(obj + 4); return i < 4 && devs[i].obj == obj ? &devs[i] : NULL; }

static void push(Dev *d, u32 ofs, u32 data)
{
    if (!d->acquired) return;
    int n = (d->qt + 1) % QN;
    if (n == d->qh) d->qh = (d->qh + 1) % QN;   /* overflow: drop the oldest */
    d->q[d->qt] = (DOD){ ofs, data, now_ms(), ++seq };
    d->qt = n;
    if (d->event) thr_signal(d->event);
}

static Dev *find(int kind) { for (int i = 0; i < 4; i++) if (devs[i].obj && devs[i].kind == kind) return &devs[i]; return NULL; }

/* from window.c: motion in guest pixels, buttons 0..2, wheel */
void di_mouse_move(int dx, int dy)
{
    Dev *d = find(1);
    if (!d) return;
    d->dx += dx; d->dy += dy;
    if (dx) push(d, 0, (u32)dx);
    if (dy) push(d, 4, (u32)dy);
}
void di_mouse_button(int b, int down)
{
    Dev *d = find(1);
    if (!d || b > 3) return;
    d->btn[b] = down ? 0x80 : 0;
    push(d, 12 + b, down ? 0x80 : 0);
}
void di_mouse_wheel(int dz)
{
    Dev *d = find(1);
    if (!d) return;
    d->dz += dz;
    push(d, 8, (u32)dz);
}
void di_key(int dik, int down)
{
    Dev *d = find(2);
    if (d) push(d, (u32)dik, down ? 0x80 : 0);
}

/* ---------------- IDirectInputA ---------------- */
static void di_QueryInterface(void) { WR32(ARG(2), ARG(0)); RET(DI_OK, 3); }
static void di_AddRef(void) { RET(2, 1); }
static void di_Release(void) { RET(1, 1); }

/* CreateDevice(this, guid, &dev, outer) */
static void di_CreateDevice(void)
{
    u32 g = ARG(1);
    int kind = RD32(g) == 0x6f1d2b60u ? 1 : RD32(g) == 0x6f1d2b61u ? 2 : 0;
    if (!kind) { rt_log("dinput: CreateDevice %08x: not available\n", RD32(g)); RET(DIERR_DEVICENOTREG, 4); }
    int i;
    for (i = 0; i < 4 && devs[i].obj; i++) ;
    if (i == 4) RET(DIERR_INVALIDPARAM, 4);
    Dev *d = &devs[i];
    memset(d, 0, sizeof *d);
    d->obj = gcalloc(8);
    WR32(d->obj, vt_dev);
    WR32(d->obj + 4, i);
    d->kind = kind;
    WR32(ARG(2), d->obj);
    rt_log("dinput: %s device\n", kind == 1 ? "mouse" : "keyboard");
    RET(DI_OK, 4);
}

/* EnumDevices(this, type, cb, ref, flags): mouse and keyboard only */
static void di_EnumDevices(void) { RET(DI_OK, 5); }
static void di_GetDeviceStatus(void) { RET(RD32(ARG(1)) == 0x6f1d2b60u || RD32(ARG(1)) == 0x6f1d2b61u ? DI_OK : 1, 2); }
static void di_ok2(void) { RET(DI_OK, 2); }
static void di_ok3(void) { RET(DI_OK, 3); }

/* ---------------- IDirectInputDeviceA ---------------- */
static void dv_Release(void)
{
    Dev *d = D(ARG(0));
    if (d) { gfree(d->obj); d->obj = 0; }
    RET(0, 1);
}
static void dv_GetCapabilities(void)
{
    u32 c = ARG(1);
    Dev *d = D(ARG(0));
    u32 n = RD32(c);
    if (n > 4 && n < 0x100) memset(MEM + c + 4, 0, n - 4);
    WR32(c + 4, 1);                                  /* DIDC_ATTACHED */
    WR32(c + 8, d && d->kind == 1 ? 0x12 : 0x13);    /* DIDEVTYPE_MOUSE / KEYBOARD */
    WR32(c + 12, d && d->kind == 1 ? 3 : 0);         /* axes */
    WR32(c + 16, d && d->kind == 1 ? 3 : 128);       /* buttons */
    RET(DI_OK, 2);
}
static void dv_EnumObjects(void) { RET(DI_OK, 4); }
static void dv_GetProperty(void) { RET(DIERR_INVALIDPARAM, 3); }
/* SetProperty(this, guid, header): DIPROP_BUFFERSIZE = 1 */
static void dv_SetProperty(void)
{
    Dev *d = D(ARG(0));
    if (d && ARG(1) == 1) d->bufsize = RD32(ARG(2) + 16);
    RET(DI_OK, 3);
}
static void dv_Acquire(void) { Dev *d = D(ARG(0)); if (d) d->acquired = 1; RET(DI_OK, 1); }
static void dv_Unacquire(void) { Dev *d = D(ARG(0)); if (d) { d->acquired = 0; d->qh = d->qt = 0; } RET(DI_OK, 1); }

/* GetDeviceState(this, size, data) */
static void dv_GetDeviceState(void)
{
    Dev *d = D(ARG(0));
    u32 n = ARG(1), p = ARG(2);
    if (!d) RET(DIERR_INVALIDPARAM, 3);
    if (!d->acquired) RET(DIERR_NOTACQUIRED, 3);
    memset(MEM + p, 0, n);
    if (d->kind == 1) {
        if (n >= 16) {
            WR32(p, d->dx); WR32(p + 4, d->dy); WR32(p + 8, d->dz);
            memcpy(MEM + p + 12, d->btn, 4);
        }
        d->dx = d->dy = d->dz = 0;
    } else memcpy(MEM + p, dik_state, n < 256 ? n : 256);
    RET(DI_OK, 3);
}

/* GetDeviceData(this, cbObject, data, &count, flags) */
static void dv_GetDeviceData(void)
{
    Dev *d = D(ARG(0));
    u32 sz = ARG(1), out = ARG(2), pn = ARG(3), flags = ARG(4);
    if (!d) RET(DIERR_INVALIDPARAM, 5);
    if (!d->acquired) RET(DIERR_NOTACQUIRED, 5);
    u32 want = RD32(pn), got = 0;
    int h = d->qh;
    while (h != d->qt && got < want) {
        if (out) memcpy(MEM + out + got * sz, &d->q[h], sz < 16 ? sz : 16);
        h = (h + 1) % QN;
        got++;
    }
    if (!(flags & 1)) d->qh = h;   /* not DIGDD_PEEK */
    WR32(pn, got);
    RET(DI_OK, 5);
}

static void dv_SetDataFormat(void) { RET(DI_OK, 2); }
static void dv_SetEventNotification(void) { Dev *d = D(ARG(0)); if (d) d->event = ARG(1); RET(DI_OK, 2); }
static void dv_SetCooperativeLevel(void) { RET(DI_OK, 3); }
static void dv_GetObjectInfo(void) { RET(DIERR_INVALIDPARAM, 4); }
static void dv_GetDeviceInfo(void) { RET(DIERR_INVALIDPARAM, 2); }

static u32 make_vt(const NativeFn *fns, const char **names, int n)
{
    u32 vt = gmalloc(4 * n);
    for (int i = 0; i < n; i++) WR32(vt + 4 * i, thunk_new(fns[i], names[i]));
    return vt;
}

/* DirectInputCreateA(inst, version, &di, outer) */
void api_DirectInputCreateA(void)
{
    if (!vt_di) {
        static const NativeFn a[] = { di_QueryInterface, di_AddRef, di_Release, di_CreateDevice, di_EnumDevices,
            di_GetDeviceStatus, di_ok3, di_ok3 };
        static const char *an[] = { "DInput::QueryInterface", "DInput::AddRef", "DInput::Release", "DInput::CreateDevice",
            "DInput::EnumDevices", "DInput::GetDeviceStatus", "DInput::RunControlPanel", "DInput::Initialize" };
        vt_di = make_vt(a, an, 8);
        static const NativeFn b[] = { di_QueryInterface, di_AddRef, dv_Release, dv_GetCapabilities, dv_EnumObjects,
            dv_GetProperty, dv_SetProperty, dv_Acquire, dv_Unacquire, dv_GetDeviceState, dv_GetDeviceData,
            dv_SetDataFormat, dv_SetEventNotification, dv_SetCooperativeLevel, dv_GetObjectInfo, dv_GetDeviceInfo,
            di_ok3, di_ok3 };
        static const char *bn[] = { "DIDevice::QueryInterface", "DIDevice::AddRef", "DIDevice::Release",
            "DIDevice::GetCapabilities", "DIDevice::EnumObjects", "DIDevice::GetProperty", "DIDevice::SetProperty",
            "DIDevice::Acquire", "DIDevice::Unacquire", "DIDevice::GetDeviceState", "DIDevice::GetDeviceData",
            "DIDevice::SetDataFormat", "DIDevice::SetEventNotification", "DIDevice::SetCooperativeLevel",
            "DIDevice::GetObjectInfo", "DIDevice::GetDeviceInfo", "DIDevice::RunControlPanel", "DIDevice::Initialize" };
        vt_dev = make_vt(b, bn, 18);
        obj_di = gcalloc(8);
        WR32(obj_di, vt_di);
    }
    (void)di_ok2;
    WR32(ARG(2), obj_di);
    rt_log("DirectInputCreateA version %x\n", ARG(1));
    RET(DI_OK, 4);
}
