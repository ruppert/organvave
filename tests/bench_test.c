/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Guilherme Ruppert (OrganVave) */
/* BENCHMARK kernels (0.11.0): firmware/src/bench_kernels.h on the host. The times come only from the device; this checks
 * that the two arithmetics compute the same thing, and that the clock's arithmetic is right with a fake clock.
 * 1. wheels: the integer and the float kernel, the same phases, sine and gains, agree within BENCH_N LSB over a second
 *    (each wheel's interpolation truncates in integers, one LSB at most, and the sum of 60 can drift that far), and the
 *    output is not silent.
 * 2. FIR: the integer and the float kernel agree within 1 LSB over a second (one truncation per sample; the Q10 taps are exact).
 * 3. clock: a window across the 32-bit counter's wrap, less the interrupt's time; one second at 24 MHz is 100.0 %; the
 *    float-to-integer ratio; the formatting of tenths. */
#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <string.h>
#include "bench_kernels.h"

static int bad;
static void check(const char *what, int ok)
{
    printf("bench: %-118s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok)
        bad++;
}

static int16_t sq[1025];
static float sf[1025];

int main(void)
{
    uint32_t ph_i[BENCH_N], ph_f[BENCH_N], inc[BENCH_N], b, s, w, k, n = 0, hp = 0, hpf = 0;
    int32_t gi[BENCH_N], yi[BENCH_BLK], c[BENCH_TAPS], hist[64], xb[BENCH_BLK];
    float gf[BENCH_N], yf[BENCH_BLK], cf[BENCH_TAPS], histf[64], xbf[BENCH_BLK];
    double worst_w = 0.0, worst_f = 0.0;
    int nonzero = 0;
    char buf[16];

    for (k = 0; k < 1025; k++) {
        sq[k] = (int16_t)lrint(32767.0 * sin(2.0 * M_PI * (double)k / 1024.0));
        sf[k] = (float)sin(2.0 * M_PI * (double)k / 1024.0);
    }

    /* 1. the wheels: 55 Hz .. 830 Hz in steps of 13 Hz, gains 60 .. 460 (Q9), a second in blocks of 32 */
    for (w = 0; w < BENCH_N; w++) {
        inc[w] = (55u + 13u * w) * 97391u;          /* the firmware's step (bench.c) */
        gi[w] = (int32_t)(60u + (w * 37u) % 400u);
        gf[w] = (float)gi[w] * (1.0f / 512.0f);
        ph_i[w] = ph_f[w] = 0u;
    }
    for (b = 0; b < BENCH_BLOCKS; b++) {
        bench_wheels_i(sq, ph_i, inc, gi, yi);
        bench_wheels_f(sf, ph_f, inc, gf, yf);
        for (s = 0; s < BENCH_BLK; s++) {
            double d = fabs((double)yi[s] - (double)yf[s]);
            if (d > worst_w)
                worst_w = d;
            if (yi[s])
                nonzero = 1;
        }
    }
    printf("bench: the wheels, integer against float: the largest difference %.2f LSB (Q15) over a second\n", worst_w);
    check("wheels: the integer and the float kernels agree within BENCH_N LSB (60 wheels, one truncation each)", worst_w <= BENCH_N);
    check("wheels: the output is not silent", nonzero);

    /* 2. the FIR: 33 binomial taps, normalised, Q10 and float; a sine (the sine table, /4) across the blocks */
    {
        float t1 = 1.0f, fs = 0.0f, raw[BENCH_TAPS];
        for (k = 0; k < BENCH_TAPS; k++) {
            raw[k] = t1;
            fs += t1;
            t1 = t1 * (float)(BENCH_TAPS - 1u - k) / (float)(k + 1u);
        }
        for (k = 0; k < BENCH_TAPS; k++) {
            c[k] = (int32_t)(raw[k] / fs * 1024.0f + 0.5f);
            cf[k] = (float)c[k] * (1.0f / 1024.0f);
        }
    }
    memset(hist, 0, sizeof hist);
    memset(histf, 0, sizeof histf);
    worst_f = 0.0;
    for (b = 0; b < BENCH_BLOCKS; b++) {
        for (s = 0; s < BENCH_BLK; s++) {
            xb[s] = sq[(n * 45u) & 1023u] / 4;
            xbf[s] = (float)xb[s];
            n++;
        }
        bench_fir_i(c, hist, &hp, xb, yi);
        bench_fir_f(cf, histf, &hpf, xbf, yf);
        for (s = 0; s < BENCH_BLK; s++) {
            double d = fabs((double)yi[s] - floor((double)yf[s]));
            if (d > worst_f)
                worst_f = d;
        }
    }
    printf("bench: the FIR, integer against float: the largest difference %.2f LSB (Q15) over a second\n", worst_f);
    check("FIR: the integer and the float kernels agree within 1 LSB (one truncation per sample)", worst_f <= 1.0);

    /* 3. the clock, with a fake counter */
    check("clock: a window across the 32-bit wrap less the interrupt's ticks: 0x200 - 100 = 412",
          bench_active(0xFFFFFF00u, 0x100u, 10u, 110u) == 412u);
    check("clock: a window with no interrupt inside is its ticks", bench_active(1000u, 1500u, 7u, 7u) == 500u);
    check("clock: one second at 24 MHz is 100.0 % (1000 tenths of a percent)", bench_tenths(24000000u) == 1000u);
    check("clock: half a second is 50.0 %", bench_tenths(12000000u) == 500u);
    check("clock: 2.4 ms of ticks per second of audio is 0.2 % (2 tenths)", bench_tenths(57600u) == 2u);
    check("ratio: float 6 against integer 8 ticks is 75 hundredths", bench_ratio(6u, 8u) == 75u);
    check("ratio: no integer time gives 0 (no division by zero)", bench_ratio(6u, 0u) == 0u);
    check("ratio: a float run longer than two seconds of ticks does not overflow (64-bit at run time)",
          bench_ratio(60000000u, 30000000u) == 200u);

    bench_fmt(buf, 1000u);
    check("format: 1000 tenths is \"100.0\"", strcmp(buf, "100.0") == 0);
    bench_fmt(buf, 99u);
    check("format: 99 tenths is \"9.9\"", strcmp(buf, "9.9") == 0);
    bench_fmt(buf, 5u);
    check("format: 5 tenths is \"0.5\"", strcmp(buf, "0.5") == 0);

    printf("bench: %s\n", bad ? "FAILED" : "all ok");
    return bad ? 1 : 0;
}
