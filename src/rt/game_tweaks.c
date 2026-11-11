/* Small game-specific changes on top of the original code.
 *
 * Briefing -> away mission (460e3b): after PROCEED the game lets the briefing
 * music play its closing "tag" (ATAGn/CTAGn.WAV, 4-6 s) and waits for it in
 * a Sleep(0) loop before it loads the mission.  A key press or a click during
 * that wait ends it (the music is cut), and the wait's length is logged. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "rt.h"

extern u32 win_input_count;   /* window.c: key presses and clicks so far */

/* 4db4f9(code, what, line, file): the sound manager's error report - the
   game formats it and throws it away; log it */
void N_snd_error(void)
{
    char what[120] = "", file[120] = "";
    u32 w = ARG(1), f = ARG(3);
    for (int i = 0; w && i < 119 && RD8(w + i); i++) what[i] = (char)RD8(w + i), what[i + 1] = 0;
    for (int i = 0; f && i < 119 && RD8(f + i); i++) file[i] = (char)RD8(f + i), file[i + 1] = 0;
    rt_log("sound manager error %d: %s (%s line %d) from %08x\n", (s32)ARG(0), what, file, (int)ARG(2), RD32(cpu.r[R_ESP]));
    rt_wrap_orig("N_snd_error", NULL)();
}

/* 4d1620: the same for the resource manager (DAT archives) */
void N_res_error(void)
{
    char what[120] = "";
    u32 w = ARG(1);
    for (int i = 0; w && i < 119 && RD8(w + i); i++) what[i] = (char)RD8(w + i), what[i + 1] = 0;
    rt_log("resource manager error %d: %s (line %d) from %08x\n", (s32)ARG(0), what, (int)ARG(2), RD32(cpu.r[R_ESP]));
    rt_wrap_orig("N_res_error", NULL)();
}

/* 4d6b16(sound): is it still playing? */
void N_sound_playing(void)
{
    static u32 wait_start, wait_input, waiting;
    u32 ra = RD32(cpu.r[R_ESP]);
    if (ra != 0x46110cu) { rt_wrap_orig("N_sound_playing", NULL)(); return; }
    if (!waiting) { waiting = 1; wait_start = now_ms(); wait_input = win_input_count; }
    rt_wrap_orig("N_sound_playing", NULL)();
    if (cpu.r[R_EAX] && win_input_count != wait_input) {
        rt_log("briefing music tag skipped after %u ms\n", now_ms() - wait_start);
        cpu.r[R_EAX] = 0;
    }
    if (!cpu.r[R_EAX]) {
        rt_log("briefing music tag wait: %u ms\n", now_ms() - wait_start);
        waiting = 0;
    }
}
