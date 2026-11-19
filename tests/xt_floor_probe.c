/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Guilherme Ruppert (OrganVave-1) */
/* DIAGNOSTIC (not in the suite): the sound and the cost of the crosstalk floor (OP_XT_FLOOR, the ladder of eng_organ.c,
 * -2 dB steps, index 3 is -96 dB, the default) and of the plain sine for every wheel (org_faint high: sine_n, no
 * interpolation). Factory bank 1 with the upper drawbars at 8, the benchmark's churn (25 keys, one released and one
 * pressed per block). Each case renders one second after a warm-up; the sound difference is the RMS of the difference
 * against the default, in dB relative to the default's RMS. Build like churn_probe.c. Changes nothing in the firmware. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "felucca_tables.h"
#define NVOICE 1
#define NPART 1
static inline int32_t clamp_(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }
#include "../firmware/engines/src/felucca_engine.h"
#define RING_PUBLISH() __asm__ volatile("" ::: "memory")
enum { P_E0, P_E1, P_E2, P_E3 };
struct track { int dummy; };
#include "../firmware/engines/src/dsp.c"
static void fm1_irq_off(void) {}
static void fm1_irq_on(void) {}
#include "../firmware/src/eng_organ.c"

#define SECS 1.0
#define WARM 0.5

/* one case: the floor index (3 = -96 dB), faint 0 = setBfree's interpolation everywhere, faint 1 = the plain sine everywhere */
static void render_case(uint32_t floor_idx, uint32_t faint, float *dst, double *active_avg)
{
    uint32_t nblk = (uint32_t)(SECS * 44100.0 / CTL), warm = (uint32_t)(WARM * 44100.0 / CTL), b, i, k;
    uint64_t act = 0, nm = 0;
    int32_t out[CTL];
    org_init();
    org_set_patch(ORG_FACTORY[1]);
    for (i = 0; i < 9u; i++)
        org_set(OP_UB + i, 8u);
    org_set(OP_VIB, 0u);
    org_set(OP_XTALK, 1u);
    org_set(OP_XT_FLOOR, floor_idx);
    org_faint = faint ? (1 << 30) : 0;
    org_block(0);
    org_adv_poll();
    org_all_off();
    for (i = 0; i < 25u; i++)
        org_key(36u + i, 1u);
    for (b = 0; b < warm + nblk; b++) {
        org_key(36u + b % 25u, 0u);
        org_key(36u + (b + 7u) % 25u, 1u);
        for (k = 0; k < CTL; k++)
            out[k] = 0;
        org_render(out, CTL);
        if (b >= warm) {
            for (k = 0; k < CTL; k++)
                dst[(b - warm) * CTL + k] = (float)out[k] / 32768.0f;
            act += org.nacl;
            nm++;
        }
    }
    *active_avg = nm ? (double)act / nm : 0.0;
}

int main(void)
{
    static const uint32_t FL[] = {3, 9, 15, 21, 27, 33, 39};
    uint32_t nsamp = (uint32_t)(SECS * 44100.0 / CTL) * CTL, n, c;
    float *ref = malloc(sizeof(float) * nsamp), *cur = malloc(sizeof(float) * nsamp);
    double ref_rms = 0.0, act;
    render_case(3u, 0u, ref, &act);
    for (n = 0; n < nsamp; n++)
        ref_rms += (double)ref[n] * ref[n];
    ref_rms = sqrt(ref_rms / nsamp);
    printf("reference: floor -96 dB (index 3), interpolation everywhere: RMS %.5f, active wheels %.1f\n", ref_rms, act);
    for (c = 0; c < sizeof FL / sizeof FL[0] + 1u; c++) {
        double d2 = 0.0, a = 0.0;
        int idx = c < sizeof FL / sizeof FL[0] ? (int)FL[c] : 3;
        int faint = c == sizeof FL / sizeof FL[0];
        render_case((uint32_t)idx, (uint32_t)faint, cur, &a);
        for (n = 0; n < nsamp; n++) {
            double dd = (double)cur[n] - ref[n];
            d2 += dd * dd;
        }
        if (faint)
            printf("plain sine, every wheel (no interpolation): active wheels %.1f, difference %.1f dB re the reference\n",
                   a, 20.0 * log10(sqrt(d2 / nsamp) / ref_rms + 1e-12));
        else
            printf("floor %3d dB (index %2d): active wheels %.1f, difference %.1f dB re the reference\n",
                   -2 * (51 - idx), idx, a, 20.0 * log10(sqrt(d2 / nsamp) / ref_rms + 1e-12));
    }
    free(ref);
    free(cur);
    return 0;
}
