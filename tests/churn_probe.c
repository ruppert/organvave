/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Guilherme Ruppert (Organvave) */
/* DIAGNOSTIC (not in the suite): what a key change costs in the organ engine, with and without the crosstalk, under the
 * benchmark's churn (25 keys held, one key released and one pressed per block). Counts per second of audio, read from the
 * engine's own state: key messages, table entries they walk (org_contrib), wheels they touch (each is recalculated in
 * org_fragment), wheel-blocks on the envelope path (ORG_ADD ramps), and the active wheels. Build as organ_test (see
 * tests/run_tests.sh), run alone. Nothing here changes the firmware. */
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

static uint32_t stamp[ORG_NWHEEL], gen = 1;

/* one run of 'secs' seconds of churn with the crosstalk on (xt 1) or off (xt 0); the counts are per second */
static void probe(uint32_t xt, double secs)
{
    uint32_t nblk = (uint32_t)(secs * 44100.0 / CTL), b, i, k, nfrag = 0;
    uint64_t events = 0, entries = 0, recalcs = 0, envw = 0, active = 0, blocks = 0, faint2 = 0, faint32 = 0, nact = 0;
    int32_t out[CTL];
    uint32_t warm = (uint32_t)(0.5 * 44100.0 / CTL);
    org_init();
    for (i = 0; i < 9u; i++)
        org_set(OP_UB + i, 8u);                               /* the upper drawbars all out (8) */
    org_set(OP_XTALK, xt);
    org_set(OP_VIB, 0u);
    org_block(0);
    org_adv_poll();
    org_all_off();
    for (i = 0; i < 25u; i++)
        org_key(36u + i, 1u);
    for (b = 0; b < warm + nblk; b++) {
        int measure = b >= warm;
        org_key(36u + b % 25u, 0u);                           /* the churn: one released, one pressed, per block */
        org_key(36u + (b + 7u) % 25u, 1u);
        if (org.sub == 0u && measure) {                       /* a fragment starts: count what its queue will do */
            uint32_t q;
            gen++;
            for (q = 0; q < org.nq; q++) {
                uint32_t n, boff, j, xtm = org.q[q] >> 1, k2 = org.qk[q];
                const float *lf;
                const uint32_t *c = org_contrib(k2, xtm, &n, &boff, &lf);
                events++;
                entries += n;
                for (j = 0; j < n; j++) {
                    uint32_t wn = c[j] >> 25;
                    if (stamp[wn] != gen) {
                        stamp[wn] = gen;
                        recalcs++;
                    }
                }
            }
            nfrag++;
        }
        for (k = 0; k < CTL; k++)
            out[k] = 0;
        org_render(out, CTL);
        if (measure) {
            blocks++;
            active += org.nacl;
            for (i = 0; i < org.nacl; i++) {
                float v = org.w[org.acl[i]].sum;
                if (org.w[org.acl[i]].env)
                    envw++;
                if (v < 2.0f)
                    faint2++;
                if (v < 32.0f)
                    faint32++;
                nact++;
            }
        }
    }
    {
        double s = secs;
        printf("churn probe, crosstalk %s, %.0f s measured: key events %.0f/s, table entries %.0f/s, wheels recalculated %.0f/s,\n"
               "                envelope-path wheel-blocks %.0f/s, active wheels avg %.1f (fragments %u)\n",
               xt ? "ON " : "OFF", s, events / s, entries / s, recalcs / s, envw / s,
               blocks ? (double)active / blocks : 0.0, nfrag);
        printf("                active wheels with a sum below 2 (Q12): %.1f %%, below 32: %.1f %%\n",
               nact ? 100.0 * faint2 / nact : 0.0, nact ? 100.0 * faint32 / nact : 0.0);
    }
}

int main(void)
{
    probe(0u, 1.0);
    probe(1u, 1.0);
    return 0;
}
