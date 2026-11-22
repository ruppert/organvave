/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Guilherme Ruppert (Organvave) */
/* EXTREME (byte 8, 0.11.0): setBfree's overdrive (src/eng_drive.c dr_orig) on the host.
 * The reference is setBfree's own loop (b_overdrive/overdrive.c: mixFilterWeights and overdrive()) written again here in
 * double, with its default constants and its two 33-tap filters (drive_orig_ref.h). The firmware's float chain must follow it.
 * 1. reference: the firmware's output against the double model, a 441 Hz sine at 0.73 and GAIN 0.1 .. 10.0 (Q15): within 4 LSB.
 * 2. level: the organ-like chord at RMS 0.73 comes out within 1.5 dB of its dry level at every GAIN tenth (the trim).
 * 3. bounds: a full-scale square at every GAIN stays inside the output's 4.0 (Q15); silence in, silence out (exact).
 * 4. blocks: the output does not depend on the block length (1, 7, 64: bit-exact).
 * 5. DC: the output's mean under 1 % of its RMS.
 * 6. the byte: 8 is EXTREME (dr_type_of 6), 2 .. 7 keep their types; EXTREME after another type starts from empty states. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "../firmware/src/eng_drive.c"
#include "drive_orig_ref.h"

static int bad;
static void check(const char *what, int ok)
{
    printf("orig: %-118s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok)
        bad++;
}

#define N 8192
static int32_t in[N], out[N], blk1[N], blk7[N], blk64[N];
static dr_t D;

/* setBfree's overdrive() in double: mixFilterWeights (the four polyphase rows of ipwdef, summed per tap; aaldef normalised) */
static double RH[9], RA[33];
static void ref_setup(void)
{
    double s = 0.0, sa = 0.0, mix[33];
    int k, o;
    for (k = 0; k < 33; k++)
        s += fabs(ORIG_REF_IPW[k]);
    for (k = 0; k < 33; k++)
        mix[k] = ORIG_REF_IPW[k] / s;
    for (k = 0; k < 9; k++)
        RH[k] = 0.0;
    {
        static const int OFF[4] = {0, 3, 2, 1};            /* wi[0] = mix[0,4,..], wi[1] = mix[3,7,..], wi[2] = mix[2,..], wi[3] = mix[1,..] */
        for (o = 0; o < 4; o++)
            for (k = 0; OFF[o] + 4 * k < 33 && k < 9; k++)
                RH[k] += mix[OFF[o] + 4 * k];
    }
    for (k = 0; k < 33; k++)
        sa += fabs(ORIG_REF_AAL[k]);
    for (k = 0; k < 33; k++)
        RA[k] = ORIG_REF_AAL[k] / sa;
}

/* the reference loop: setBfree's overdrive() with inG (the input gain) and outG (the output gain), Q15 out */
static void ref_run(const int32_t *x, double *y, uint32_t n, double inG, double outG)
{
    double xz[64], yz[64], sag = 0.0, az = 0.0, az1 = 0.0, gfz = 0.0, bx = 0.0, by = 0.0, w;
    uint32_t i, k, xp = 0, yp = 0;
    memset(xz, 0, sizeof xz);
    memset(yz, 0, sizeof yz);
    for (i = 0; i < n; i++) {
        double xin = inG * ((double)x[i] / 8192.0), u = 0.0, v, bias, norm, t, x2, acc = 0.0;
        sag = 0.991 * sag + fabs(xin);                        /* sagFb, sagZ */
        bias = 0.5347 - 0.0094 * sag;                         /* biasBase, sagZgb */
        norm = 1.0 - 1.0 / (1.0 + bias * bias);
        xp = (xp + 1u) & 63u;
        xz[xp] = xin;
        for (k = 0; k < 9; k++)
            u += RH[k] * xz[(xp - k) & 63u];
        u -= -0.6214 * gfz;                                   /* adwGfb */
        t = u - az;                                           /* adwFb 0.5821 */
        az = u + az * 0.5821;
        u = t;
        if (u < 0.0) {
            x2 = u - bias;
            v = 1.0 / (1.0 + x2 * x2) - 1.0 + norm;
        } else {
            x2 = u + bias;
            v = 1.0 - norm - 1.0 / (1.0 + x2 * x2);
        }
        t = v + 0.999 * az1;                                  /* adwFb2 */
        v = t - az1;
        az1 = t;
        gfz = v;
        yp = (yp + 1u) & 63u;
        yz[yp] = v;
        for (k = 0; k < 33; k++)
            acc += RA[k] * yz[(yp - k) & 63u];
        w = outG * acc * 32768.0;
        by = w - bx + 0.998566 * by;                          /* the DC blocker (the firmware's, 0.11.0) */
        bx = w;
        y[i] = by;
    }
}

static double rms_of(const int32_t *v, uint32_t from, uint32_t n)
{
    double s = 0.0;
    uint32_t i;
    for (i = from; i < n; i++)
        s += (double)v[i] * (double)v[i];
    return sqrt(s / (double)(n - from));
}

/* the firmware: EXTREME at GAIN g, OUTPUT at 0 dB, blocks of blk samples in place */
static void run_fw(uint32_t g, uint32_t blk, const int32_t *x, int32_t *y, uint32_t n)
{
    uint32_t i = 0;
    dr_init(&D);
    dr_user(&D, 8u, g, 4u);
    memcpy(y, x, n * sizeof(int32_t));
    while (i < n) {
        uint32_t m = n - i < blk ? n - i : blk;
        dr_run_ex(&D, y + i, y + i, m);
        i += m;
    }
}

int main(void)
{
    static double ry[N];
    uint32_t i, g, k;
    double diff_worst = 0.0, lvl_worst = 0.0, peak_worst = 0.0;
    int32_t peak = 0;

    ref_setup();
    check("byte 8 is EXTREME (dr_type_of 6); 2 .. 7 keep their types, 0 and 1 play as TUBE DRIVE",
          dr_type_of(8u) == DR_ORIG && dr_type_of(2u) == 0u && dr_type_of(3u) == 1u && dr_type_of(7u) == 5u && dr_type_of(1u) == 0u);

    /* 1. reference: a 441 Hz sine at 0.73 peak, GAIN 0.1 .. 10.0 (every 9th tenth), against the double loop */
    for (i = 0; i < N; i++)
        in[i] = (int32_t)lrint(0.73 * 8192.0 * sin(2.0 * M_PI * 441.0 * i / 44100.0));
    for (g = 1; g <= 100; g += 9) {
        double inG = 3.5675 / 40.0 * (double)g, outG = 0.8795 * (double)DR_OR_TRIM[g];
        run_fw(g, 64u, in, out, N);
        ref_run(in, ry, N, inG, outG);
        for (k = 0; k < N; k++) {
            double d = fabs((double)out[k] - ry[k]);
            if (d > diff_worst)
                diff_worst = d;
        }
    }
    printf("orig: the largest difference from the double loop: %.3f LSB (Q15)\n", diff_worst);
    check("reference: the firmware's output follows setBfree's loop within 4 LSB at GAIN 0.1 .. 10.0", diff_worst <= 4.0);

    /* 2. level: an organ-like chord at 0.73 RMS, within 1.5 dB of the dry level at every GAIN tenth */
    {
        static const double F[7] = {110.0, 165.0, 220.0, 330.0, 440.0, 660.0, 880.0};
        double a = 0.73 * sqrt(2.0) / sqrt(7.0), dry;
        for (i = 0; i < N; i++) {
            double s = 0.0;
            for (k = 0; k < 7; k++)
                s += sin(2.0 * M_PI * F[k] * i / 44100.0);
            in[i] = (int32_t)lrint(a * s * 8192.0);
        }
        dry = rms_of(in, N / 2, N) / 8192.0;
        for (g = 1; g <= 100; g++) {
            double db;
            run_fw(g, 64u, in, out, N);
            db = fabs(20.0 * log10((rms_of(out, N / 2, N) / 32768.0) / dry));
            if (db > lvl_worst)
                lvl_worst = db;
        }
        printf("orig: the chord's level against its dry level: worst %.2f dB\n", lvl_worst);
        check("level: the organ-like chord comes out within 1.5 dB of its dry level at every GAIN tenth (the trim)", lvl_worst <= 1.5);
    }

    /* 3. bounds and silence */
    for (i = 0; i < N; i++)
        in[i] = ((i / 37) & 1) ? 32767 : -32768;
    for (g = 1; g <= 100; g += 3) {
        run_fw(g, 64u, in, out, N);
        for (i = 0; i < N; i++) {
            int32_t v = out[i] < 0 ? -out[i] : out[i];
            if (v > peak)
                peak = v;
        }
    }
    peak_worst = peak;
    check("bounds: a full-scale square at every GAIN stays inside the output's 4.0 (Q15: -131072 .. 131071)", peak_worst <= 131072.0);
    memset(in, 0, sizeof in);
    run_fw(60u, 64u, in, out, N);
    peak = 0;
    for (i = 0; i < N; i++)
        if (out[i])
            peak = 1;
    check("silence: no input, no output (exact zero)", peak == 0);

    /* 4. blocks: bit-exact for any block length */
    for (i = 0; i < N; i++)
        in[i] = (int32_t)lrint(0.5 * 8192.0 * sin(2.0 * M_PI * 220.0 * i / 44100.0));
    run_fw(40u, 1u, in, blk1, N);
    run_fw(40u, 7u, in, blk7, N);
    run_fw(40u, 64u, in, blk64, N);
    check("blocks: the output is bit-exact for block lengths 1, 7 and 64",
          memcmp(blk1, blk7, sizeof blk1) == 0 && memcmp(blk1, blk64, sizeof blk1) == 0);

    /* 5. DC: the output's mean under 1 % of its RMS over 1 s (a 220 Hz sine at 0.5, GAIN 4.0; the blocker's 10 Hz settles in 0.1 s) */
    {
        static int32_t sin1[44100], o1[44100];
        double mean = 0.0, r;
        for (i = 0; i < 44100; i++)
            sin1[i] = (int32_t)lrint(0.5 * 8192.0 * sin(2.0 * M_PI * 220.0 * i / 44100.0));
        run_fw(40u, 64u, sin1, o1, 44100);
        for (i = 22050; i < 44100; i++)
            mean += o1[i];
        mean /= 22050.0;
        r = rms_of(o1, 22050, 44100);
        printf("orig: DC %.1f LSB against RMS %.1f LSB (%.2f %%) at GAIN 4.0, a 220 Hz sine at 0.5, the second second\n", mean, r, 100.0 * fabs(mean) / r);
        check("DC: the output's mean stays under 1 % of its RMS", fabs(mean) < 0.01 * r);
    }

    /* 6. switching: after another type, EXTREME starts from empty states */
    dr_init(&D);
    dr_user(&D, 2u, 40u, 4u);
    dr_run_ex(&D, in, out, 256);
    dr_user(&D, 8u, 40u, 4u);
    check("switch: EXTREME after another type starts with empty histories",
          D.osag == 0.0f && D.oaz == 0.0f && D.oaz1 == 0.0f && D.ogfz == 0.0f && D.oxp == 0u && D.oyp == 0u);

    printf("orig: %s\n", bad ? "FAILED" : "all ok");
    return bad ? 1 : 0;
}
