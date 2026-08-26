/* burnout3_ai_avoid.c -- the AI AVOIDANCE stage (retail `AI+0x2B0`).
 *
 * Every function here is a 1:1 transcription of one retail function and is
 * diffed against that function EXECUTED under Unicorn by
 * tools/validate_ai_avoid.py.  Nothing in this file is a paraphrase of a
 * behaviour; where a detail could not be executed it is marked [S] or [?].
 *
 *   b3_avoid_frame      FUN_00170260   the profile frame
 *   b3_avoid_band       FUN_00170100   the car's own footprint band
 *   b3_avoid_clear      FUN_0016D2F0 @0x0016D32F
 *   b3_avoid_strip      FUN_0016F400's index map
 *   b3_avoid_paint      FUN_0016F400
 *   b3_avoid_query      FUN_0016FCD0
 *   b3_avoid_aim_strip  FUN_0016F000
 *   b3_avoid_choose     FUN_0016C4B0
 */
#include "burnout3_ai_avoid.h"

#include <math.h>
#include <string.h>

/* retail Data/vdb.xml column, docs/RE_AI.md section 1                  [C] */
const B3AiAvoidParams B3_AI_AVOID_VDB = {
    20.0f, 2.5f, 101.0f, 0.0f, 200.0f, 100.0f, 5.0f, 0.2f,
    26.2f, 40.0f, 60.0f, 16.0f, 30.0f, 5.1f, 5.0f, 0.08f
};

float b3_avoid_time(const B3AiAvoid* a, int i) {
    return (float)a->time[i] * (B3_AV_TMAX / 255.0f);
}

float b3_avoid_dist(const B3AiAvoid* a, int i) {
    return (float)a->dist[i] * (1000.0f / 65536.0f);
}

static float av_dot3(const float u[3], const float v[3]) {
    return u[0] * v[0] + u[1] * v[1] + u[2] * v[2];
}

/* ------------------------------------------------------------------------
 * FUN_00170260 -- the profile FRAME.
 *
 * Retail interpolates the left and right road-edge points from the route's
 * per-node (leftVertexIndex, rightVertexIndex) records at the car's node and
 * fraction; that lerp is the CALLER's job here because it is pure data
 * lookup.  What this reproduces is the frame those two points define:
 *
 *   axis  = normalize(left - right) with y forced to 0        @0x001703C1
 *   width = |left - right|            -> avoid+0x444          @0x001703F1
 *   lat_right = dot(car - right, axis)  -> avoid+0x44C        @0x00170445
 *   lat_left  = -dot(car - left,  axis) -> avoid+0x448        @0x00170430
 *
 * lat_left + lat_right == width, so `lat_right` is simply how far the car
 * sits from the RIGHT edge measured toward the LEFT edge.              [C]
 * ---------------------------------------------------------------------- */
void b3_avoid_frame(B3AiAvoid* a, const float left[3], const float right[3],
                    const float car[3]) {
    int i;
    float d[3], len2, inv;
    for (i = 0; i < 3; i++) {
        a->left[i] = left[i];
        a->right[i] = right[i];
        a->car[i] = car[i];
    }
    d[0] = left[0] - right[0];
    d[1] = 0.0f;                       /* y is forced to 0     @0x001703B6 */
    d[2] = left[2] - right[2];
    len2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
    /* retail's degenerate fallback builds the axis from the road direction
     * (0x001703C9); the caller must not hand us a zero-width section. [S] */
    if (len2 < 2.3283064e-10f) {
        a->width = 0.0f;
        a->axis[0] = 0.0f; a->axis[1] = 0.0f; a->axis[2] = 0.0f;
        a->lat_left = 0.0f; a->lat_right = 0.0f;
        return;
    }
    a->width = sqrtf(len2);
    inv = 1.0f / a->width;
    a->axis[0] = d[0] * inv;
    a->axis[1] = d[1] * inv;
    a->axis[2] = d[2] * inv;
    {
        float cr[3], cl[3];
        for (i = 0; i < 3; i++) { cr[i] = car[i] - right[i];
                                  cl[i] = car[i] - left[i]; }
        a->lat_right = av_dot3(cr, a->axis);
        a->lat_left = -av_dot3(cl, a->axis);
    }
}

/* ------------------------------------------------------------------------
 * FUN_00170100 -- the car's OWN FOOTPRINT band.
 *
 * This is the single most load-bearing correction to the previous port:
 * `avoid+0x441/+0x442` are the strips the CAR ITSELF covers -- roughly
 * 128 +- 5*half_extent -- and NOT a road-wide band.  Every aggregate in
 * FUN_0016C4B0 (the threat gate, dmin, the risk fraction, the mean window)
 * is taken over THIS band.                                             [C]
 *
 *   half = |road_fwd   . car_fwd| * 0.5 * racecar+0x2444   @0x001701BD/C5
 *        + |road_right . car_fwd| * 0.3 * racecar+0x2448   @0x001701D3/DB
 *   k  = (int)(5.0 * half)
 *   hi = k + 0x81
 *   if (hi < 0x100) { lo = (int)(-half*5.0) + 0x7F;
 *                     if (lo < 0) { lo = 0; hi = k*2; } }
 *   else            { hi = 0xFF; lo = 0xFF - k*2; }
 *   win = ((int)(5.0*half) + 1) * 2   == hi - lo            @0x001701E9
 *
 * WHICH EXTENT GOES WITH WHICH TERM is load-bearing and was inverted here
 * until 2026-08-22.  The disassembly pairs |cos| with racecar+0x2444 and
 * |sin| with +0x2448 (0x001701C5 / 0x001701DB, constants [0x003B1684] = 0.5
 * and [0x003B1750] = 0.3), and EXECUTION confirms it: with the car aligned to
 * the road only +0x2444 moves the band (100.0 -> half 50.0, i.e. 0.5x) and at
 * 90 degrees only +0x2448 does (100.0 -> half 30.0, i.e. 0.3x).  docs/RE_AI.md
 * 14.4 pins the two fields from unrelated retail functions: +0x2448 x 0.5 is
 * "half the car length" (FUN_00169D70) and +0x2444 is compared against the
 * LATERAL offset +0x38 (FUN_0016A620), so +0x2444 is the car WIDTH and
 * +0x2448 the car LENGTH -- both FULL extents.
 *
 * So an aligned car's band is 0.5 x its WIDTH, not 0.5 x its length.  The
 * port used to pass them the other way round, which made every car believe
 * its own footprint was 4.8 m wide instead of 2.4 m: `tmin`, `dmin`, the risk
 * fraction and both risk means in FUN_0016C4B0 are all taken over exactly
 * this band, so the car braked and swerved for traffic in the NEXT lane.
 * Measured on the real chooser over 20000 randomised profiles with the road
 * no-go stamped: the swap moved the avoidance aim in 69.2% of them (mean
 * 4.81 m), flipped the chosen SIDE in 13.2%, and entered AVOID where the
 * correct footprint stays CLEAR in 20.6%.
 * tools/validate_ai_avoid.py could not see it because tools/emulate_ai_avoid.py
 * seeded retail with the same swap; both are corrected with this change and
 * four asymmetric-extent cases now pin the assignment.                  [C]
 * ---------------------------------------------------------------------- */
void b3_avoid_band(B3AiAvoid* a, const float road_fwd[3],
                   const float road_right[3], const float car_fwd[3],
                   float ext_w, float ext_l) {
    float half = fabsf(av_dot3(road_right, car_fwd)) * 0.3f * ext_l
               + fabsf(av_dot3(road_fwd, car_fwd)) * 0.5f * ext_w;
    float sk = B3_AV_STRIPS_PER_M * half;
    int k = (int)sk;
    int hi = k + 0x81;
    int lo;
    if (hi < 0x100) {
        lo = (int)(-half * B3_AV_STRIPS_PER_M) + 0x7F;
        if (lo < 0) { lo = 0; hi = k * 2; }
    } else {
        hi = 0xFF;
        lo = 0xFF - k * 2;
    }
    a->hi = (unsigned char)hi;
    a->lo = (unsigned char)lo;
    /* measured: 5*half = 11.60 -> win 24, 8.75 -> 18, 13.75 -> 28, i.e.
     * TRUNCATION like every other conversion in the stage, and the
     * result is exactly hi - lo.                                    [C] */
    a->win = (unsigned char)((k + 1) * 2);
}

/* --- FUN_0016D2F0 @0x0016D32F: reset the three parallel arrays -------- */
void b3_avoid_clear(B3AiAvoid* a) {
    int i;
    for (i = 0; i < B3_AV_N; i++) {
        a->type[i] = 0;
        a->time[i] = 0xFF;             /* 8.0 s = clear                   */
        a->dist[i] = 32767;            /* 500 m = nothing in range        */
    }
}

/* ------------------------------------------------------------------------
 * FUN_0016F400's world -> strip map, proved by execution:
 *   strip(p) = (int)((dot(p - right_edge, axis) - lat_right) * 5.0) + 128
 * The frame ORIGIN cancels against `lat_right`, so any lateral coordinate
 * along the same axis in metres gives the same strip.                  [C]
 * ---------------------------------------------------------------------- */
int b3_avoid_strip(const B3AiAvoid* a, const float p[3]) {
    float d[3];
    int i;
    for (i = 0; i < 3; i++) d[i] = p[i] - a->right[i];
    return (int)((av_dot3(d, a->axis) - a->lat_right) * B3_AV_STRIPS_PER_M)
           + B3_AV_MID;
}

/* ------------------------------------------------------------------------
 * FUN_0016F400 -- stamp [lo,hi] with (kind, time, dist).
 *
 * Time and dist are kept INDEPENDENTLY (each is a separate min) and the
 * TYPE follows the TIME only -- a later stamp with a worse time but a
 * better range updates the range and leaves the type alone.  Measured by
 * execution, not inferred.                                             [C]
 *
 * Quantisation, also measured: time is TRUNCATED at 255/8 with no clamp
 * (the `t < stored` guard is what keeps it in range), dist is truncated at
 * 32767*0.002 after an explicit +-500 clamp.                           [C]
 * ---------------------------------------------------------------------- */
void b3_avoid_paint_span(B3AiAvoid* a, int lo, int hi, int kind,
                         float t, float d) {
    int i;
    if (hi < 0) return;                            /* @0x0016F4E9 */
    if (lo >= B3_AV_N) return;
    if (lo < 0) lo = 0;
    if (hi > B3_AV_N - 1) hi = B3_AV_N - 1;
    for (i = lo; i <= hi; i++) {
        if (t < b3_avoid_time(a, i)) {
            a->time[i] = (unsigned char)(int)(t * B3_AV_TQ);
            a->type[i] = (unsigned char)kind;
        }
        if (d < b3_avoid_dist(a, i)) {
            float c = d;
            if (c <= -B3_AV_DMAX) c = -B3_AV_DMAX;
            if (c >= B3_AV_DMAX) c = B3_AV_DMAX;
            a->dist[i] = (short)(int)(c * B3_AV_DQ);
        }
    }
}

void b3_avoid_paint(B3AiAvoid* a, const float pl[3], const float pr[3],
                    int kind, float t, float d) {
    /* the "+0" record side is the LEFT edge and maps to the HIGH strip,
     * the "+2" side is the RIGHT edge and maps to the LOW strip     [C] */
    b3_avoid_paint_span(a, b3_avoid_strip(a, pr), b3_avoid_strip(a, pl),
                        kind, t, d);
}

/* ------------------------------------------------------------------------
 * FUN_0016FCD0 -- the risk query at a strip.
 *
 * `mean`  the mean RISK (8 - time) over the `win` strips the car would
 *         occupy if it sat at `strip`, walking AWAY from centre; a type-7
 *         (hard no-go) strip contributes ZERO risk here.
 * `total` the risk that has to be CROSSED to get from the car's own
 *         footprint edge to `strip` -- zero when `strip` is inside the
 *         footprint; only strips with time < 2.0 count.
 * `maxr`  the largest single-strip risk in the window.
 *
 * Verified numerically against execution (mean 6.1490 / total 98.3843 /
 * max 7.0275 at strip 100 for a road painted 103..183 at t=0.97255). [C]
 * ---------------------------------------------------------------------- */
void b3_avoid_query(const B3AiAvoid* a, int strip, float* mean,
                    float* total, float* maxr) {
    int n = a->win, k, u, step;
    *mean = 0.0f; *total = 0.0f; *maxr = 0.0f;
    if (a->width == 0.0f) return;                  /* avoid+0x444 gate */
    step = (strip < B3_AV_MID) ? 1 : -1;
    if (n == 0) {
        *mean = B3_AV_TMAX - b3_avoid_time(a, strip);
    } else {
        float sum = 0.0f;
        for (k = 0; k < n; k++) {
            int i = strip + step * k;
            float r;
            if (i < 0) i = 0;
            if (i > B3_AV_N - 1) i = B3_AV_N - 1;
            r = B3_AV_TMAX - b3_avoid_time(a, i);
            if (a->type[i] == B3_AVT_HARD) r = 0.0f;
            sum += r;
            if (r > *maxr) *maxr = r;
        }
        *mean = sum / (float)n;
    }
    if (strip < B3_AV_MID) {
        for (u = a->lo; u >= strip; u--) {
            if (u < 0 || u > B3_AV_N - 1) continue;
            if (b3_avoid_time(a, u) < B3_AV_SIDE_GATE
                && a->type[u] != B3_AVT_HARD)
                *total += B3_AV_TMAX - b3_avoid_time(a, u);
        }
    } else {
        for (u = a->hi; u <= strip; u++) {
            if (u < 0 || u > B3_AV_N - 1) continue;
            if (b3_avoid_time(a, u) < B3_AV_SIDE_GATE
                && a->type[u] != B3_AVT_HARD)
                *total += B3_AV_TMAX - b3_avoid_time(a, u);
        }
    }
}

/* ------------------------------------------------------------------------
 * FUN_0016F000 -- the clear-path aim strip.
 *
 * A 5-tap {s-3, s-1, s, s+1, s+3} box of the raw time bytes weighted by two
 * folded triangles: one peaking at strip 128 (the car) and one peaking at
 * the ROAD CENTRE.  The scan runs the road edges inset by 3 strips and the
 * `>=` keeps the LAST maximum.                                        [C]
 * ---------------------------------------------------------------------- */
int b3_avoid_aim_strip(const B3AiAvoid* a) {
    int i0 = (int)((0.0f - a->lat_right) * B3_AV_STRIPS_PER_M) + B3_AV_MID;
    int i1 = (int)((a->width - a->lat_right) * B3_AV_STRIPS_PER_M) + B3_AV_MID;
    int s, best = B3_AV_MID;
    float bestv = -0.1f;
    if (i0 < 0) i0 = 0; else if (i0 > 255) i0 = 255;
    if (i1 < 0) i1 = 0; else if (i1 > 255) i1 = 255;
    for (s = i0 + 3; s < i1 - 3; s++) {
        float wa = (float)s * 0.0078125f;
        float wb = (float)((s - i0) * 2) / (float)(i1 - i0);
        float v;
        int sum;
        if (wa > 1.0f) wa = 2.0f - wa;
        if (wb > 1.0f) wb = 2.0f - wb;
        sum = (int)a->time[s - 3] + (int)a->time[s - 1] + (int)a->time[s]
            + (int)a->time[s + 1] + (int)a->time[s + 3];
        v = wb * wa * (float)sum * 0.2f * (B3_AV_TMAX / 255.0f);
        if (v >= bestv) { bestv = v; best = s; }
    }
    return best;
}

/* ------------------------------------------------------------------------
 * FUN_0016C4B0 -- the chooser.
 * ---------------------------------------------------------------------- */
static float av_dmin(const B3AiAvoid* a) {
    int span = (int)a->hi - (int)a->lo;
    int q = (span + ((span >> 31) & 3)) >> 2;      /* /4 toward zero */
    int lo = (int)a->lo + q, hi = (int)a->hi - q, i;
    float d = B3_AV_DMAX;
    for (i = lo; i < hi; i++) {                    /* NB: `<`, exclusive */
        if (i < 0 || i > B3_AV_N - 1) continue;
        /* @0x0016CCB0: while avoid+0x48C < 50 every strip counts; at and
         * above 50 the RACECAR strips (type 6) drop out.  Note type 6 is a
         * racecar, not the road no-go -- docs/RE_AI.md 15.7 had this
         * inverted.                                                   [C] */
        if (a->spd_gate < 50.0f || a->type[i] != B3_AVT_RACER) {
            float v = b3_avoid_dist(a, i);
            if (v < d) d = v;
        }
    }
    return d;
}

/* the risk fraction: types 2..4 over the footprint, normalised by win*8 */
static float av_frac(const B3AiAvoid* a) {
    int i, n = a->win;
    float s = 0.0f;
    for (i = 0; i < n; i++) {
        int j = (int)a->lo + i;
        if (j < 0 || j > B3_AV_N - 1) continue;
        if (a->type[j] > 1 && a->type[j] < 5)
            s += B3_AV_TMAX - b3_avoid_time(a, j);
    }
    return n ? s / ((float)n * B3_AV_TMAX) : 0.0f;
}

/* the ladder at 0x0016CCEF..0x0016CD48; returns 0 when nothing applies */
static int av_speed_ladder(const B3AiAvoid* a, const B3AiAvoidParams* p,
                           float dmin, float frac, float* out) {
    int span = (int)a->hi - (int)a->lo;
    int q = (span + ((span >> 31) & 3)) >> 2;
    if ((int)a->lo + q < (int)a->hi - q) {
        if (dmin < 10.0f) { *out = p->spd_10; return 1; }
        if (dmin < 20.0f) { *out = p->spd_20; return 1; }
        if (dmin < 30.0f) { *out = p->spd_30; return 1; }
    }
    if (frac > 0.95f) { *out = p->spd_r95; return 1; }
    if (frac > 0.9f)  { *out = p->spd_r90; return 1; }
    return 0;
}

void b3_avoid_choose(B3AiAvoid* a, const B3AiAvoidParams* p,
                     float corner_ceiling, int mode) {
    int lo = a->lo, hi = a->hi, i;
    float tmin;

    a->state = 0x10;                                   /* @0x0016C4D2 */
    a->risk_mean_r = a->risk_mean_l = 0.0f;
    a->risk_tot_r = a->risk_tot_l = 0.0f;
    a->valid = 1;

    /* the entry loop takes BOTH minima over the footprint band:
     * `tmin` gates the branch, and the DISTANCE minimum -- over the whole
     * band, not the middle half -- is what the AVOID branch's forward lead
     * divides by at @0x0016CBED.                                       [C] */
    tmin = b3_avoid_time(a, lo);
    a->dband = b3_avoid_dist(a, lo);
    for (i = lo + 1; i <= hi; i++) {
        float t = b3_avoid_time(a, i);
        float d = b3_avoid_dist(a, i);
        if (t < tmin) tmin = t;
        if (d < a->dband) a->dband = d;
    }
    a->dmin = av_dmin(a);
    a->frac = av_frac(a);
    a->clear_straight = 0;

    if (tmin >= B3_AV_THREAT) {
        /* --- CLEAR: nothing in our own footprint inside 4 s ---------- */
        float sp;
        int s = b3_avoid_aim_strip(a);
        a->aim_strip_cached = s;
        /* THE EDGE SCAN -- FUN_0016C4B0 @0x0016CE89..0x0016CF41.
         *
         * Even with its own footprint clear for 4 s, retail walks the four
         * strips BEYOND the low end of that footprint.  A strip occupied
         * inside 1.0 s (DAT_003B168C) raises that side's risk mean to 1.0
         * (@0x0016CECC), and if the clear-path aim strip lies beyond that
         * edge -- i.e. the chosen line would merge INTO the car alongside
         * -- the lateral offset is cancelled and the car aims straight
         * ahead (@0x0016CEC8 / @0x0016CF0B).  This is the rival-vs-rival
         * half of the lateral spread law: it is what stops two cars
         * picking the same slot.
         *
         * The mirror walk over the HIGH edge exists in the image at
         * 0x0016CF47..0x0016D005 and is DEAD CODE: its entry guard
         * `LEA EAX,[EBX+0x4] / CMP EBX,EAX / JL` @0x0016CF4E always jumps
         * past the loop, and the bottom test `CMP EBX,EAX / JGE`
         * @0x0016D003 can never continue it either -- a sign slip that
         * Ghidra also renders as the impossible `if (uVar9 + 4 <= uVar9)`.
         * PROVED by execution (scratchpad h4_pack/probe_edgescan.py):
         * an occupier at lo-1/-3/-4 sets avoid+0x494 to 1.0 while one at
         * hi+1/+3/+4 leaves avoid+0x490 at 0.0.  Porting the mirror would
         * be a divergence, not a fix, so it is deliberately absent.  [C] */
        for (i = lo; i >= lo - 4; i--) {
            if (i < 0) continue;
            if (b3_avoid_time(a, i) < B3_AV_EDGE_T) {  /* @0x0016CEBC */
                a->risk_mean_l = B3_AV_EDGE_T;         /* @0x0016CECC */
                if (s < lo) a->clear_straight = 1;     /* @0x0016CEC8 */
            }
        }
        /* the ladder, shared with the avoid branch (@0x0016D08F ->
         * 0x0016CCFC); the risk-fraction rungs are NOT reachable from this
         * branch, hence the literal 0 fraction.                       [C] */
        if (!av_speed_ladder(a, p, a->dmin, 0.0f, &sp)) sp = corner_ceiling;
        a->speed = sp;
        return;
    }

    /* --- AVOID -------------------------------------------------------- */
    {
        float thr = tmin - B3_AV_SLACK;
        float prev = tmin;
        int cur = lo, back, ra, rb;
        if (thr < 0.0f) thr = 0.0f;

        /* walk 1: out from `lo` toward the HIGH strips        @0x0016C5F8 */
        while (cur < B3_AV_N) {
            float t = b3_avoid_time(a, cur);
            int keep = 0;
            if (hi < cur - (int)a->win) {
                if (t < thr) { cur--; break; }
                keep = 1;
            } else if (thr <= t) {
                keep = 1;
            } else if (t < tmin) {
                break;
            }
            if (keep) {
                if (prev <= t) thr = t - B3_AV_SLACK;
            }
            prev = t;
            cur++;
        }
        if (cur > B3_AV_N - 1) cur = B3_AV_N - 1;
        back = cur;
        if (lo < cur) {
            while (back > lo) {
                if (b3_avoid_time(a, back) < thr) { back++; break; }
                back--;
            }
        }
        ra = (cur + back) / 2;
        if (ra < 1) ra = 1;
        if (ra > 255) ra = 255;

        /* walk 2: the mirror, out from `hi` toward the LOW strips.
         * Ghidra lost the array reads here (FUN_0016C370 is a stub in the
         * analysed image); the structure is the mirror of walk 1 and the
         * differential suite is what pins it.                        [S] */
        thr = tmin - B3_AV_SLACK;
        if (thr < 0.0f) thr = 0.0f;
        prev = tmin;
        cur = hi;
        while (cur >= 0) {
            float t = b3_avoid_time(a, cur);
            int keep = 0;
            if (cur + (int)a->win < lo) {
                if (t < thr) { cur++; break; }
                keep = 1;
            } else if (thr <= t) {
                keep = 1;
            } else if (t < tmin) {
                break;
            }
            if (keep) {
                if (prev <= t) thr = t - B3_AV_SLACK;
            }
            prev = t;
            cur--;
        }
        if (cur < 0) cur = 0;
        back = cur;
        if (cur < hi) {
            while (back < hi) {
                if (b3_avoid_time(a, back) < thr) { back--; break; }
                back++;
            }
        }
        rb = (cur + back) / 2;
        if (rb < 0) rb = 0;
        if (rb > 254) rb = 254;

        /* the four risk aggregates                          @0x0016C8AC */
        {
            /* the `win` strips the car would occupy AT the candidate,
             * walking back toward the car -- measured, not inferred: with
             * ra = 164 / win = 24 retail's mean is exactly the mean over
             * strips 141..164.                                        [C] */
            float s = 0.0f;
            for (i = 0; i < (int)a->win; i++) {
                int j = ra - i;
                if (j < 0) j = 0;
                s += B3_AV_TMAX - b3_avoid_time(a, j);
            }
            a->risk_mean_r = a->win ? s / (float)a->win : 0.0f;
        }
        {
            float s = 0.0f;
            for (i = lo; i <= ra && i < B3_AV_N; i++)
                if (b3_avoid_time(a, i) < B3_AV_SIDE_GATE)
                    s += B3_AV_TMAX - b3_avoid_time(a, i);
            a->risk_tot_r = s;
        }
        {
            float s = 0.0f;                       /* the mirror of the above */
            for (i = 0; i < (int)a->win; i++) {
                int j = rb + i;
                if (j > B3_AV_N - 1) j = B3_AV_N - 1;
                s += B3_AV_TMAX - b3_avoid_time(a, j);
            }
            a->risk_mean_l = a->win ? s / (float)a->win : 0.0f;
        }
        {
            float s = 0.0f;
            for (i = hi; i >= rb && i >= 0; i--)
                if (b3_avoid_time(a, i) < B3_AV_SIDE_GATE)
                    s += B3_AV_TMAX - b3_avoid_time(a, i);
            a->risk_tot_l = s;
        }

        /* the five-term side select                          @0x0016CB33 */
        {
            float mR = a->risk_mean_r, mL = a->risk_mean_l;
            float tR = a->risk_tot_r, tL = a->risk_tot_l;
            int two = (mL != B3_AV_TMAX && (mode == 4 || mR == B3_AV_TMAX))
                   || (hi == ra)
                   || ((mR == B3_AV_TMAX || (mode != 2 && mL != B3_AV_TMAX))
                       && lo != rb && (mL + tL <= tR + mR));
            a->state = (unsigned char)(two ? 2 : 1);
            a->target_strip = two ? rb : ra;
            a->cand_r = ra;
            a->cand_l = rb;
        }

        {
            float sp;
            if (!av_speed_ladder(a, p, a->dmin, a->frac, &sp))
                sp = corner_ceiling;
            a->speed = sp;
        }
    }
}
