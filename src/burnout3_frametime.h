/* Retail's wall-clock frame governor -- the thing the port was missing.
 *
 * Recovered 2026-08-19 from build/burnout3.elf.  The port's main loop carried
 * a comment claiming the Xbox title is FRAME-LOCKED ("wall-clock time is never
 * consumed by the sim").  That is wrong, and it is the whole bug: the sim
 * advanced exactly 1/60 s per RENDERED frame, so the game ran at
 * (achieved fps / 60) x real time -- 2.2x fast on a fast build, 0.24x on a
 * slow one.
 *
 * What retail actually does [C], FUN_000165F0 (the main frame dispatcher):
 *
 *   00016ABC  CMP  [EBP+0x2E20C], 0        ; ticks for this frame
 *   00016AC6  JLE  0x00016C10              ; <=0 -> render only, no sim step
 *   00016AD0: ---- inner loop, ONE 1/60 s sim tick per iteration ----
 *   00016ADC    INC [0x004A1EB4]           ; the frame counter, +1 per TICK
 *   00016AFC    CALL FUN_001B5AC0 (ECX=EBP+0x7040)   ; timer A
 *   00016B06    CALL FUN_001B5AC0 (ECX=0x0060EA00)   ; the game timer ->
 *                                          ;   DAT_0060EA1C / DAT_0060EA20
 *               ... the whole game update ...
 *   00016BED    i++ ; CMP i,[EBP+0x2E20C] ; JL 0x00016AD0
 *   00016C10  CALL FUN_000170B0            ; (once per RENDERED frame)
 *   00016C1F  LEA  ESI,[EBP+0x2E198]       ; = 0x004D5338, the governor
 *   00016C2D  PUSH 4 ; PUSH 1              ; base = 1, MAX TICKS = 4
 *   00016C31  CALL FUN_001B58E0            ; wall-clock accumulator
 *   00016C37  MOV  [EBP+0x2E20C], EAX      ; ticks for the NEXT frame
 *
 * (the alternate arm at 0x00016C27 pushes base = 2, max = 8, and pairs with
 *  FUN_00016E00 @0x00016ED1 setting the governor period to 33.333 ms -- the
 *  30 Hz render mode, still 60 sim ticks a second.)
 *
 * So retail is a textbook fixed-timestep accumulator: a fixed 1/60 s tick, a
 * 64-bit wall-clock residual, and a hard clamp of 4 ticks per rendered frame
 * with the overflow DISCARDED (the spiral-of-death guard, 0x001B59CA).
 *
 * The clock source is real [C]: FUN_001D20AC is `rdtsc`, FUN_001D20BD returns
 * the constant 0x2BB5C755 = 733,333,333 Hz (the Xbox CPU clock).  The period
 * is installed by FUN_001B5880(ms, 0) @0x001B5880 as
 *     [ESI+0x20] = (int64)(freq * ms * 0.001)
 * with ms = 16.666666f / 33.333332f (NTSC) or 20.0f / 40.0f (PAL), chosen at
 * 0x00016EAE / 0x00016EA7 / 0x00016ECC / 0x00016EC5.
 *
 * IMPORTANT, and the reason a governor ALONE does not fix the too-fast case:
 * `base` is 1, so FUN_001B58E0 never returns fewer than 1 tick per rendered
 * frame.  It is a CATCH-UP-only mechanism.  On Xbox the upper bound came from
 * the vsync-locked D3D Present -- the console could not render faster than
 * 60 Hz.  A desktop port must reproduce that bound itself, which is what
 * b3_frame_limit_wait_ms() below is for.
 *
 * This file is a 1:1 port of FUN_001B58E0 / FUN_001B5880's shape: same integer
 * ledger, same fields, same clamp, same backlog discard.  Only the clock
 * source differs (host performance counter instead of the 733 MHz TSC).
 */
#ifndef BURNOUT3_FRAMETIME_H
#define BURNOUT3_FRAMETIME_H

#include <stdint.h>

/* Field names carry retail's offsets in the 0x004D5338 object. */
typedef struct B3FrameGov {
    int     mode;        /* +0x00  0 = frame-lock (n := base), 2 = keep backlog */
    int64_t last;        /* +0x08  previous counter reading                     */
    int64_t residual;    /* +0x10  64-bit wall-clock residual                   */
    int64_t min_ticks;   /* +0x18  dead-band, 0 at both retail call sites       */
    int64_t period;      /* +0x20  one sim tick, in counter units               */
    int     last_n;      /* +0x28  ticks returned by the previous call          */
    int     consumed;    /* +0x2C  running ledger; starts at -1                 */
    int     extra;       /* +0x30  one-shot extra-tick request (0 in retail)    */
    char    frozen;      /* +0x34  suppress accumulation                        */
} B3FrameGov;

/* FUN_001B5880 @0x001B5880: install the target period.
 * `freq` is the counter frequency (retail: 733,333,333). */
static inline void b3_frame_gov_init(B3FrameGov* g, double period_ms,
                                     double min_ms, int64_t freq)
{
    g->mode      = 1;
    g->last      = 0;
    g->residual  = 0;
    g->period    = (int64_t)((double)freq * period_ms * 0.001);   /* +0x20 */
    g->min_ticks = (int64_t)((double)freq * min_ms    * 0.001);   /* +0x18 */
    g->consumed  = -1;                                            /* +0x2C */
    g->last_n    = 0;
    g->extra     = 0;                                             /* +0x30 */
    g->frozen    = 0;
    if (g->period < 1) g->period = 1;
}

/* FUN_001B58E0 @0x001B58E0, thiscall ESI = the object, RET 0x8.
 * `base` = the floor (retail passes 1, or 2 in the 30 Hz render mode).
 * `max`  = the clamp  (retail passes 4, or 8).
 * `now`  = the host performance counter, retail's rdtsc.
 * Returns the number of 1/60 s sim ticks to run before the next render. */
static inline int b3_frame_gov_tick(B3FrameGov* g, int64_t now,
                                    int base, int max)
{
    int64_t elapsed;
    int     n;

    if (g->frozen) {
        elapsed = -1;                       /* 0x001B5968: OR EBP,-1          */
    } else {
        g->consumed += g->last_n;           /* 0x001B5901                     */
        if (g->consumed <= 0) {             /* 0x001B590A: the priming frames */
            g->last   = now;
            g->last_n = base;
            return base;
        }
        g->residual += now - g->last;       /* 0x001B5922                     */
        elapsed = g->residual - (int64_t)g->consumed * g->period; /* 0x001B5944 */
    }
    g->last = now;                          /* 0x001B597A / 0x001B5981        */

    n = base;                               /* 0x001B597D                     */
    if (elapsed > g->min_ticks)             /* 0x001B5984 / 0x001B598A        */
        n = base + (int)((elapsed + g->period - g->min_ticks - 1) / g->period);

    if (n > max) {                          /* 0x001B59B8                     */
        n = max;
        /* 0x001B59CA: DISCARD the backlog we are never going to simulate.
         * Without this a build that cannot hold 60 Hz death-spirals. */
        if (g->mode != 2 && !g->frozen)
            g->residual += (int64_t)(max - base) * g->period - elapsed;
    }
    if (!g->frozen) g->last_n = n;          /* 0x001B5A01                     */
    if (g->mode == 0) n = base;             /* 0x001B5A0D: frame-locked       */

    if (g->extra > 0 && n == base) {        /* 0x001B5A14 (never armed in retail) */
        int e = g->extra;
        g->residual += (int64_t)e * g->period;
        n += e;
    }
    g->extra = 0;                           /* 0x001B5A70                     */
    return n;
}

/* GLUE -- not retail code, but reproducing retail's HARDWARE bound.
 *
 * The Xbox could not present faster than the 60 Hz vblank, so the render loop
 * was capped at one frame per period and the governor above only ever had to
 * catch UP.  Off-console there is no such bound, so this returns how long to
 * sleep to land on the next period boundary.  `now`/`period` in counter units;
 * the return is milliseconds, 0 when we are already late.
 *
 * Real vsync (SDL_GL_SetSwapInterval(1)) is preferable when the driver honours
 * it; this is the fallback and the offscreen/headless answer.  Either way the
 * governor above stays correct: it consumes whatever wall time actually passed.
 */
static inline int b3_frame_limit_wait_ms(int64_t now, int64_t next_deadline,
                                         int64_t freq)
{
    int64_t d = next_deadline - now;
    if (d <= 0 || freq <= 0) return 0;
    /* Sleep the whole wait but one millisecond; the caller spins the rest, and
     * any residue is absorbed by the governor's 64-bit remainder anyway. */
    int ms = (int)((d * 1000) / freq);
    return ms > 1 ? ms - 1 : 0;
}

#endif /* BURNOUT3_FRAMETIME_H */
