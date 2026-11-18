/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Guilherme Ruppert (OrganVave-1) */
/* The rotary speaker, CLASSIC model and its variants (ORGAN.md, "Rotary speaker"), in single precision (0.11.0, the port
 * back): setBfree's whirl.c (whirlProc2, GPL-2.0-or-later), sample for sample: the horn's filters A and B, the leakage, six
 * horn taps (two per reflection stage) read through the angle FIR, the two reflections, six drum taps on the unfiltered
 * input, the drum's high-shelf per mic. The rotors' angles advance per sample; their speeds ramp per block (setBfree's
 * exponential law, hornAcDc), in integers as before: a phase is 32 bits and a ramp's fraction is carried in Q30.
 *
 * What is float now: the audio. Samples are floats in units (1.0 = full scale; the organ's Q15 is divided by 32768 at the
 * input and multiplied back at the output). The delay lines, the reflections, the drum shelf, the coupled-form filters
 * (the five ROT_CPL sets, Q24 in the table, float here) and the horn FIRs' products are float. What stays integer, as
 * setBfree computes it from the angle: the geometry (the tap phases, the table index, the delays in Q12 samples, the angle
 * FIR at the nearest 16384th) and the tables themselves (eng_rotary_tables.h: displacements in Q12, FIRs in Q15, filters
 * in Q24). The tables are data quantised once; the arithmetic that runs per sample is float.
 *
 * The float port's accuracy is measured by tests/rotary_test.c against tools/setbfree_ref/whirl_render (the reference's
 * waveform SNR, left and right), not claimed here.
 *
 * The input is clamped to +-1.0 in rot_block. Only the geometry and the ramps are per block; the sample loop is float.
 */
#ifndef ENG_ROTARY_C
#define ENG_ROTARY_C
#include <stdint.h>
#include "eng_rotary_tables.h"

/* ROT_RAM (the firmware): the displacement tables (16 KB) are copied to SRAM at boot (rot_load_tables), in .bss (not
 * .pool: the POOL region is full, the SRAM has room). The FIRs (20 KB) stay in flash: neither fits in SRAM beside them,
 * and the FIR is read per tap at the angle's nearest 16384th (held per block in CLASSIC 2 .. 5). The host test reads the
 * constants */
#ifdef ROT_RAM
static int32_t rot_disp_ram[4][ROT_NA];
#define RT_DISP rot_disp_ram
#else
#define RT_DISP ROT_DISP
#endif
#define RT_FIR ROT_FIR

#define ROT_LINE 512u                  /* the delay lines: 512 samples (the default geometry needs at most about 360) */
#define ROT_MASK (ROT_LINE - 1u)
#define ROT_D 12                       /* the taps' delays (ROT_DISP, ROT_SPACE) are Q12 samples */
#define ROT_CTL 32u                    /* the speed ramp's block (setBfree's bufferLengthSamples) */
#define ROT_FS 44100u
/* the variants (rot_t.var, the classic engine's options; 0 is setBfree's CLASSIC, bit for bit in its structure):
 *   ROT_V_DRUM4 the drum taps' displacement refreshed every 4 samples
 *   ROT_V_TAP4  the horn's last tap pair (4, 5) left out (four horn taps)
 *   ROT_V_GEOM8 the horn taps' geometry (FIR and delay) computed every 8 samples, the delay ramped between them
 *   ROT_V_GEOM4 the same every 4 samples */
#define ROT_V_DRUM4 2u
#define ROT_V_TAP4 4u
#define ROT_V_GEOM8 16u
#define ROT_V_GEOM4 32u
#define RC_UNIT (1.0f / 32768.0f)      /* Q15 -> units */
#define RC_Q12 (1.0f / 4096.0f)        /* a delay's fraction */
#define RC_REFL (6554.0f / 16384.0f)   /* the reflection's 0.4 */
#define RC_HL (11469.0f / 16384.0f)    /* the horn level, 0.7 */
#define RC_TRIM 0.794f                 /* the output's trim, -2 dB: with the Leslie the mix is no louder than without it (0.11.0, no limiter) */
#define RC_LEAK (1720.0f / 16384.0f)   /* the leakage, 0.15 * 0.7 */

typedef struct {
    uint32_t ph_h, ph_d;               /* the rotors' angles, 2^32 = one turn */
    uint32_t inc_h, inc_d;             /* their speeds, phase per sample */
    uint32_t tgt_h, tgt_d;             /* their targets */
    int32_t k_ha, k_hd, k_da, k_dd;    /* the ramp's step per block, 1 - exp(-CTL / (FS * T)), Q30: acc / dec */
    int32_t rem_h, rem_d;              /* the ramps' carried fractions (Q30) */
    int8_t acdc_h, acdc_d;             /* +1 accelerating, -1 decelerating, 0 holding (setBfree's hornAcDc) */
    uint32_t mic_fw, mic_bw;           /* the mic angles' offsets (setBfree's micAngle * 0.25, and 1 - that), phase */
    uint32_t hset;                     /* the horn character: 0 default, 1 sharp (whirl-sharp.cfg: ROT_CPL sets 3 and 4 for 0 and 1) */
    float hl, leak;                    /* the horn level and the leakage (units) */
    float cpl[ROT_CPL_NSET][5];        /* the five coupled-form filters, units: ROT_CPL / 2^24 (s, w, c0, c1, d) */
    float hA[2], hB[2];                /* the horn filters' states (coupled form: u, v) */
    float dL[2], dR[2];                /* the drum output filters' states (coupled form: u, v) */
    float rf[4];                       /* the reflections' previous inputs: horn 0, 1; drum 2, 3 */
    float hh[3][4];                    /* the horn's history for the three tap pairs, newest first */
    float HL[ROT_LINE], HR[ROT_LINE], DL[ROT_LINE], DR[ROT_LINE];
    uint32_t outpos;
    uint8_t var;                       /* the variant (ROT_V_*), 0 CLASSIC */
    float vg;                          /* the variant's output gain (1.0 for CLASSIC): it matches CLASSIC's level */
    int32_t dc[6];                     /* the drum taps' displacements, Q12 samples (every sample, or every 4 with ROT_V_DRUM4) */
    int32_t geo_ok;                    /* CLASSIC 5: the anchors below are valid (0 after a block without ROT_V_GEOM8/4) */
    int32_t ga[6], gb[6];              /* CLASSIC 5: the horn delays (Q12) at the current anchor and at the next (8 samples ahead) */
    int32_t sl[6], cur[6];             /* CLASSIC 5: the delays' step per sample and their running value (Q12) */
    int16_t cA[6][5], cB[6][5];        /* CLASSIC 5: the horn FIRs at the current and the next anchor (Q15) */
} rot_t;

/* one of the five filters in coupled form (c = s, w, c0, c1, d; A = [[s, w], [-w, s]], B = (1, 0), C = (c0, c1), D = d),
 * z = {u, v} the state: the same filter as setBfree's biquad (tools/gen_rotary_tables.py derives the form) */
static inline float rot_cplf(float *z, const float *c, float x)
{
    float y = z[0] * c[2] + z[1] * c[3] + x * c[4];
    float u = z[0] * c[0] + z[1] * c[1] + x;
    float v = z[1] * c[0] - z[0] * c[1];
    z[0] = u;
    z[1] = v;
    return y;
}

/* the two-sample reflection (setBfree's FILTER_C with 0.4, 0.4): y = 0.4 (x + previous x) */
static inline float rot_refl(float *prev, float x)
{
    float y = RC_REFL * (x + *prev);
    *prev = x;
    return y;
}

/* the angle tables' values. h is the position in 16384ths of a turn, one table step is 16 of them. The displacement is
 * interpolated at the angle's fraction (f8, 256ths of a step): setBfree's h1 is a float with the same fraction. The FIR is
 * read at the nearest 16384th (setBfree's x_iroundf) and interpolated between the table entries at that integer position. */
static inline int32_t tab_disp(const int32_t *d, uint32_t h, uint32_t f8, uint32_t sh)
{
    uint32_t mask = (16384u >> sh) - 1u, k = h >> sh;
    int32_t a = d[k & mask], b = d[(k + 1u) & mask];
    return a + (((b - a) * (int32_t)f8) >> 8);
}
/* the angle's fraction in 256ths of a table step: the h bits below the step, then the two (sh 4) or four (sh 6)
 * bits of the angle below them */
static inline uint32_t tab_f8(uint32_t h, uint32_t ang, uint32_t sh)
{
    uint32_t lo = 8u - sh;
    return ((h & ((1u << sh) - 1u)) << lo) | ((ang >> (18u - lo)) & ((1u << lo) - 1u));
}
static inline void tab_fir(const int16_t (*f)[5], uint32_t h, int16_t c[5], uint32_t sh)
{
    uint32_t mask = (16384u >> sh) - 1u, k = h >> sh, fr = h & ((1u << sh) - 1u), j;
    uint32_t k1 = (k + 1u) & mask;
    k &= mask;
    for (j = 0; j < 5; j++) {
        int32_t a = f[k][j], b = f[k1][j];
        c[j] = (int16_t)(a + (((b - a) * (int32_t)fr) >> sh));
    }
}

/* a horn tap's geometry (setBfree's HN_MOTION, first part): the FIR at the angle, the delay (Q12 samples) at the
 * displacement. A function of the angle only, so CLASSIC 5 holds it for 8 samples */
static inline void horn_geom(rot_t *r, uint32_t ang, uint32_t P, const int32_t *disp, const int16_t (*fir)[5], uint32_t sh,
                             int16_t c[5], int32_t *ts)
{
    uint32_t h = ((ang >> 18) + (uint32_t)ROT_PHASE[P]) & 16383u;
    tab_fir(fir, (h + ((ang >> 17) & 1u)) & 16383u, c, sh);
    *ts = ROT_SPACE[P] + tab_disp(disp, h, tab_f8(h, ang, sh), sh);
    (void)r;
}

/* a horn tap, second part: the FIR of the four last inputs (newest first, Q15 coefficients, summed in float), then the
 * write split linearly between the two samples around the delay (de-interpolating) */
static inline void horn_tap(float x, const float *hist, float *buf, const int16_t c[5], int32_t ts, uint32_t op)
{
    float xa = (x * (float)c[0] + hist[0] * (float)c[1] + hist[1] * (float)c[2] + hist[2] * (float)c[3] + hist[3] * (float)c[4]) * RC_UNIT;
    int32_t t = ts + (int32_t)(op << ROT_D);
    uint32_t n = ((uint32_t)(t >> ROT_D)) & ROT_MASK;
    float f = (float)(t & ((1 << ROT_D) - 1)) * RC_Q12;
    buf[n] += xa * (1.0f - f);
    buf[(n + 1u) & ROT_MASK] += xa * f;
}

/* a drum tap's displacement (setBfree's DR_MOTION): no FIR */
static inline int32_t drum_disp(uint32_t ang, uint32_t P, const int32_t *disp, uint32_t sh)
{
    uint32_t h = ((ang >> 18) + (uint32_t)ROT_PHASE[6u + P]) & 16383u;
    return tab_disp(disp, h, tab_f8(h, ang, sh), sh);
}
/* a drum tap: the same split write, at the displacement td (Q12 samples) */
static inline void drum_tap(rot_t *r, uint32_t P, int32_t td, float x, float *buf)
{
    int32_t t = ROT_SPACE[6u + P] + td + (int32_t)(r->outpos << ROT_D);
    uint32_t n = ((uint32_t)(t >> ROT_D)) & ROT_MASK;
    float f = (float)(t & ((1 << ROT_D) - 1)) * RC_Q12;
    buf[n] += x * (1.0f - f);
    buf[(n + 1u) & ROT_MASK] += x * f;
}

/* the speed's ramp for one rotor, once per block: setBfree's `inc += (1 - l) (target - inc)`, and the dead zone that
 * ends it (0.05 rpm, 81 phase units). The step's fraction is carried in *rem (Q30): a rounded or floored step stalls
 * where d * k < 0.5 (111 units below the horn's target, outside the dead zone), a carried one never stalls. */
static void rot_ramp(uint32_t *inc, uint32_t tgt, int8_t *acdc, int32_t k, int32_t *rem)
{
    int32_t d, dd, step;
    int64_t acc;
    if (!*acdc)
        return;
    d = (int32_t)(tgt - *inc);
    acc = (int64_t)d * k + *rem;
    step = (int32_t)(acc >> 30);
    *rem = (int32_t)(acc - ((int64_t)step << 30));
    *inc += (uint32_t)step;
    dd = (int32_t)(tgt - *inc);
    if (dd < 81 && dd > -81) {
        *inc = tgt;
        *acdc = 0;
    }
}

/* 1 - exp(-CTL / (FS * T)) in Q30, T in microseconds: x - x^2/2 + x^3/6 (x < 0.1 for every time we use, error < x^4/24) */
static int32_t rot_k(uint32_t t_us)
{
    /* x = CTL / (FS * T), T = t_us * 1e-6 s, in Q30: CTL * 2^30 * 1e6 / (FS * t_us) (about 3e16: fits int64) */
    int64_t x30 = ((int64_t)ROT_CTL * ((int64_t)1 << 30) * 1000000LL) / ((int64_t)ROT_FS * (int64_t)t_us);
    int64_t x2 = (x30 * x30) >> 30;
    int64_t x3 = (x2 * x30) >> 30;
    return (int32_t)(x30 - (x2 >> 1) + x3 / 6);
}

/* a phase increment for a frequency in milli-hertz (2^32 per turn, per sample at FS) */
static uint32_t rot_inc(uint32_t mhz)
{
    return (uint32_t)((((uint64_t)mhz) << 32) / ((uint64_t)ROT_FS * 1000u));
}

/* the sample loop's body, float: in (Q15 mono, clamped to +-1.0 in rot_block) to outL and outR (Q15). The geometry (the
 * delays, the FIRs) is the integer part; each tap's FIR, write and read, the filters, the reflections and the shelf are float */
static inline __attribute__((always_inline)) void rot_body(rot_t *r, const int32_t *in, int32_t *outL, int32_t *outR, uint32_t n,
                                                           uint32_t sh, const int32_t *const *D, const int16_t (*const *F)[5])
{
    uint32_t i, ntap = (r->var & ROT_V_TAP4) ? 4u : 6u;
    const float *ca = r->cpl[r->hset ? 3 : 0], *cb = r->cpl[r->hset ? 4 : 1];     /* the horn character's two filters */
    const float *cs = r->cpl[2];                                                   /* the drum's high-shelf */
    int16_t gc[6][5];
    int32_t gt[6];                                                                 /* the horn taps' delay (Q12) */
    const int16_t *gp[6];                                                          /* the FIR of each tap (gc, or the held one) */

    for (i = 0; i < n; i++) {
        float xx = (float)in[i] * RC_UNIT, x, leak, y, lo, ro;
        xx = xx > 1.0f ? 1.0f : xx < -1.0f ? -1.0f : xx;   /* the filters hold an input up to 1.0 */
        uint32_t op = r->outpos;
        uint32_t ang_hf = r->ph_h + r->mic_fw, ang_hb = r->ph_h + r->mic_bw;
        uint32_t ang_d = r->ph_d;

        /* 0) the horn taps' geometry. Every sample; or, with ROT_V_GEOM8 / GEOM4, the FIR is held and the delay ramps
         * linearly between this block's anchor and the next one, 8 (or 4) samples ahead */
        if (!(r->var & (ROT_V_GEOM8 | ROT_V_GEOM4))) {
            r->geo_ok = 0;
            horn_geom(r, ang_hf, 0, D[0], F[1], sh, gc[0], &gt[0]);
            horn_geom(r, ang_hb, 1, D[1], F[0], sh, gc[1], &gt[1]);
            horn_geom(r, ang_hf, 2, D[1], F[0], sh, gc[2], &gt[2]);
            horn_geom(r, ang_hb, 3, D[0], F[1], sh, gc[3], &gt[3]);
            if (!(r->var & ROT_V_TAP4)) {
                horn_geom(r, ang_hf, 4, D[0], F[1], sh, gc[4], &gt[4]);
                horn_geom(r, ang_hb, 5, D[1], F[0], sh, gc[5], &gt[5]);
            }
            for (uint32_t k = 0; k < ntap; k++) gp[k] = gc[k];
        } else {
            uint32_t gb_n = (r->var & ROT_V_GEOM4) ? 2u : 3u;      /* the block: 4 or 8 samples */
            if ((op & ((1u << gb_n) - 1u)) == 0) {                 /* the blocks run on the rotors' sample count */
                uint32_t k, af = ang_hf + (r->inc_h << gb_n), ab = ang_hb + (r->inc_h << gb_n);
                if (!r->geo_ok) {                                  /* the first block: the current anchor, computed here */
                    horn_geom(r, ang_hf, 0, D[0], F[1], sh, r->cA[0], &r->ga[0]);
                    horn_geom(r, ang_hb, 1, D[1], F[0], sh, r->cA[1], &r->ga[1]);
                    horn_geom(r, ang_hf, 2, D[1], F[0], sh, r->cA[2], &r->ga[2]);
                    horn_geom(r, ang_hb, 3, D[0], F[1], sh, r->cA[3], &r->ga[3]);
                    if (!(r->var & ROT_V_TAP4)) {
                        horn_geom(r, ang_hf, 4, D[0], F[1], sh, r->cA[4], &r->ga[4]);
                        horn_geom(r, ang_hb, 5, D[1], F[0], sh, r->cA[5], &r->ga[5]);
                    }
                    r->geo_ok = 1;
                } else {                                           /* the next anchor became the current one */
                    for (k = 0; k < ntap; k++) {
                        r->ga[k] = r->gb[k];
                        for (uint32_t j = 0; j < 5; j++) r->cA[k][j] = r->cB[k][j];
                    }
                }
                horn_geom(r, af, 0, D[0], F[1], sh, r->cB[0], &r->gb[0]);
                horn_geom(r, ab, 1, D[1], F[0], sh, r->cB[1], &r->gb[1]);
                horn_geom(r, af, 2, D[1], F[0], sh, r->cB[2], &r->gb[2]);
                horn_geom(r, ab, 3, D[0], F[1], sh, r->cB[3], &r->gb[3]);
                if (!(r->var & ROT_V_TAP4)) {
                    horn_geom(r, af, 4, D[0], F[1], sh, r->cB[4], &r->gb[4]);
                    horn_geom(r, ab, 5, D[1], F[0], sh, r->cB[5], &r->gb[5]);
                }
                for (k = 0; k < ntap; k++) {                       /* the step over the block, and the first sample's delay */
                    r->sl[k] = (r->gb[k] - r->ga[k]) >> gb_n;
                    r->cur[k] = r->ga[k] + r->sl[k];
                }
            }
            for (uint32_t k = 0; k < ntap; k++) {
                gt[k] = r->cur[k];
                r->cur[k] += r->sl[k];
                gp[k] = r->cA[k];
            }
        }

        /* 1) the horn's filters A and B, and the leakage (of the filtered input) */
        x = rot_cplf(r->hA, ca, xx);
        x = rot_cplf(r->hB, cb, x);
        leak = r->leak * x;

        /* 2) the horn: taps 0 and 1 (history 0), reflection 1, taps 2 and 3 (history 1), reflection 2, taps 4 and 5 */
        horn_tap(x, r->hh[0], r->HL, gp[0], gt[0], op);
        horn_tap(x, r->hh[0], r->HR, gp[1], gt[1], op);
        r->hh[0][3] = r->hh[0][2]; r->hh[0][2] = r->hh[0][1]; r->hh[0][1] = r->hh[0][0]; r->hh[0][0] = x;
        x = rot_refl(&r->rf[0], x);

        horn_tap(x, r->hh[1], r->HL, gp[2], gt[2], op);
        horn_tap(x, r->hh[1], r->HR, gp[3], gt[3], op);
        r->hh[1][3] = r->hh[1][2]; r->hh[1][2] = r->hh[1][1]; r->hh[1][1] = r->hh[1][0]; r->hh[1][0] = x;
        if (!(r->var & ROT_V_TAP4)) {                               /* ROT_V_TAP4 leaves this last pair out */
            x = rot_refl(&r->rf[1], x);
            horn_tap(x, r->hh[2], r->HL, gp[4], gt[4], op);
            horn_tap(x, r->hh[2], r->HR, gp[5], gt[5], op);
            r->hh[2][3] = r->hh[2][2]; r->hh[2][2] = r->hh[2][1]; r->hh[2][1] = r->hh[2][0]; r->hh[2][0] = x;
        }

        /* 3) the drum: the unfiltered input, the same two reflections, then its high-shelf per mic */
        if (!(r->var & ROT_V_DRUM4) || (i & 3u) == 0)              /* the six drum displacements: every sample, or every 4 (ROT_V_DRUM4) */
        {
            r->dc[0] = drum_disp(ang_d, 0, D[2], sh);
            r->dc[1] = drum_disp(ang_d, 1, D[3], sh);
            r->dc[2] = drum_disp(ang_d, 2, D[3], sh);
            r->dc[3] = drum_disp(ang_d, 3, D[2], sh);
            r->dc[4] = drum_disp(ang_d, 4, D[2], sh);
            r->dc[5] = drum_disp(ang_d, 5, D[3], sh);
        }
        drum_tap(r, 0, r->dc[0], xx, r->DL);
        drum_tap(r, 1, r->dc[1], xx, r->DR);
        x = rot_refl(&r->rf[2], xx);
        drum_tap(r, 2, r->dc[2], x, r->DL);
        drum_tap(r, 3, r->dc[3], x, r->DR);
        x = rot_refl(&r->rf[3], x);
        drum_tap(r, 4, r->dc[4], x, r->DL);
        drum_tap(r, 5, r->dc[5], x, r->DR);

        /* 4) the outputs: the drum's shelf and the horn, and the leakage on both */
        y = rot_cplf(r->dL, cs, r->DL[op]);
        lo = y + r->hl * r->HL[op] + leak;
        y = rot_cplf(r->dR, cs, r->DR[op]);
        ro = y + r->hl * r->HR[op] + leak;
        lo *= RC_TRIM; ro *= RC_TRIM;
        outL[i] = (int32_t)(lo * 32768.0f + (lo >= 0.0f ? 0.5f : -0.5f));
        outR[i] = (int32_t)(ro * 32768.0f + (ro >= 0.0f ? 0.5f : -0.5f));
        r->HL[op] = 0.0f; r->HR[op] = 0.0f; r->DL[op] = 0.0f; r->DR[op] = 0.0f;

        /* 5) the rotors advance */
        r->outpos = (op + 1u) & ROT_MASK;
        r->ph_h += r->inc_h;
        r->ph_d += r->inc_d;
    }
    if (r->var)                                                    /* the variants: their level matches CLASSIC's (r->vg) */
        for (i = 0; i < n; i++) {
            outL[i] = (int32_t)((float)outL[i] * r->vg);
            outR[i] = (int32_t)((float)outR[i] * r->vg);
        }
}

/* the sample loop: setBfree's 1024 angles (the table's shift 4) */
static void rot_samples(rot_t *r, const int32_t *in, int32_t *outL, int32_t *outR, uint32_t n)
{
    const int32_t *D[4] = {RT_DISP[0], RT_DISP[1], RT_DISP[2], RT_DISP[3]};
    const int16_t (*F[2])[5] = {RT_FIR[0], RT_FIR[1]};
    rot_body(r, in, outL, outR, n, 4u, D, F);
}

/* the speeds for this block (setBfree: once per call of whirlProc2, before its sample loop) */
static void rot_speeds(rot_t *r)
{
    rot_ramp(&r->inc_h, r->tgt_h, &r->acdc_h, r->acdc_h > 0 ? r->k_ha : r->k_hd, &r->rem_h);
    rot_ramp(&r->inc_d, r->tgt_d, &r->acdc_d, r->acdc_d > 0 ? r->k_da : r->k_dd, &r->rem_d);
}

/* one block of CTL samples (n <= ROT_CTL): in (Q15 mono) to outL and outR (Q15) */
static void rot_block(rot_t *r, const int32_t *in, int32_t *outL, int32_t *outR, uint32_t n)
{
    rot_speeds(r);
    rot_samples(r, in, outL, outR, n);
}

/* the models (the patch byte OP_ROT_MODEL): 0 CLASSIC (setBfree's whirl.c), 1 CLASSIC 2 (the horn's geometry every 8
 * samples), 2 CLASSIC 3 (four horn taps), 3 CLASSIC 4 (the drum every 4 samples and four horn taps), 4 CLASSIC 5 (the
 * horn's geometry every 4 samples). The variants change at once: the classic states carry over. Their levels match
 * CLASSIC's in dB (ROT_VG). The gains are the integer ROT_VG (Q24) as floats: rotary_test checks them against the level. */
static const uint8_t ROT_VAR[5] = {0u, ROT_V_GEOM8, ROT_V_TAP4, ROT_V_DRUM4 | ROT_V_TAP4, ROT_V_GEOM4};
static const int32_t ROT_VG[5] = {1 << 24, 1 << 24, 16944154, 16928555, 1 << 24};   /* CLASSIC 3: +0.09 dB; CLASSIC 4: +0.078 dB */
static void rotary_model(rot_t *r, uint32_t model)
{
    r->var = ROT_VAR[model];
    r->vg = (float)ROT_VG[model] * (1.0f / 16777216.0f);
}

static void rot_init(rot_t *r)
{
    uint32_t i, k, j;
    for (i = 0; i < sizeof(*r) / sizeof(uint32_t); i++)
        ((uint32_t *)r)[i] = 0u;
    r->hl = RC_HL;
    r->leak = RC_LEAK;
    for (k = 0; k < ROT_CPL_NSET; k++)
        for (j = 0; j < 5; j++)
            r->cpl[k][j] = (float)ROT_CPL[k][j] * (1.0f / 16777216.0f);
    r->vg = 1.0f;
    r->k_ha = rot_k(1500000u);          /* hornAcc 1.5 s (setBfree's 0.161 s) */
    r->k_hd = rot_k(3000000u);          /* hornDec 3.0 s (setBfree's 0.321 s) */
    r->k_da = rot_k(4127000u);          /* drumAcc 4.127 s */
    r->k_dd = rot_k(1371000u);          /* drumDec 1.371 s */
}

/* the targets: the rotors ramp to them (setBfree's revControl sets hornAcDc / drumAcDc the same way) */
static void rot_target(rot_t *r, uint32_t horn_tgt, uint32_t drum_tgt)
{
    r->tgt_h = horn_tgt;
    r->tgt_d = drum_tgt;
    r->rem_h = 0;
    r->rem_d = 0;
    r->acdc_h = (int8_t)((int32_t)(horn_tgt - r->inc_h) > 0 ? 1 : (horn_tgt == r->inc_h ? 0 : -1));
    r->acdc_d = (int8_t)((int32_t)(drum_tgt - r->inc_d) > 0 ? 1 : (drum_tgt == r->inc_d ? 0 : -1));
}

/* the mic angle (setBfree's whirl.horn.mic.angle, 0..180 degrees, default 180: the two mics face the horn's two sides;
 * smaller angles bring them together: a narrower stereo image). micAngle = 1 - deg / 180, the forward mic turns by
 * micAngle / 4 and the backward one by -micAngle / 4 of a turn (hornAngle + fwAng, hornAngle + 1 - micAngle / 4) */
static void rot_mic(rot_t *r, uint32_t deg)
{
    uint32_t ma = 65536u - (deg > 180u ? 180u : deg) * 65536u / 180u;   /* micAngle, Q16 */
    r->mic_fw = ma << 14;
    r->mic_bw = (uint32_t)0 - (ma << 14);
}

static void rot_load_tables(void)
{
    uint32_t i, j;
#ifdef ROT_RAM
    for (i = 0; i < 4; i++)
        for (j = 0; j < ROT_NA; j++)
            rot_disp_ram[i][j] = ROT_DISP[i][j];
#endif
}

/* the cabinet emptied (the lines, the filters, the reflections), the rotors untouched: a model that has not run for a
 * while starts from silence */
static void rot_flush(rot_t *r)
{
    uint32_t i, j;
    for (i = 0; i < 2; i++) { r->hA[i] = 0.0f; r->hB[i] = 0.0f; r->dL[i] = 0.0f; r->dR[i] = 0.0f; }
    for (i = 0; i < 4; i++) r->rf[i] = 0.0f;
    for (i = 0; i < 3; i++)
        for (j = 0; j < 4; j++) r->hh[i][j] = 0.0f;
    for (i = 0; i < ROT_LINE; i++) { r->HL[i] = 0.0f; r->HR[i] = 0.0f; r->DL[i] = 0.0f; r->DR[i] = 0.0f; }
    r->outpos = 0;
}

#endif /* ENG_ROTARY_C */
