/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Guilherme Ruppert (OrganVave) */
/* BENCHMARK kernels (0.11.0, GLOBAL > BENCHMARK): the arithmetic of the audio path, written twice: in integers (the Q15
 * sine table, int32 sums, Q9 gains, Q10 filter taps) and in single precision (the float sine table, float sums). Pure C
 * on purpose: no firmware state here, so tests/bench_test.c checks that the two agree. The runner that times them on the
 * FM-1 is bench.c. The interpolation is sine_i's and sine_f's (eng_organ.c, dsp.c), written out with the tables passed in. */
#ifndef BENCH_KERNELS_H
#define BENCH_KERNELS_H

#include <stdint.h>

#define BENCH_N 60u              /* wheels: about the organ's per-voice load of a full chord */
#define BENCH_TAPS 33u           /* the FIR: setBfree's 33 taps (the output filter's length) */
#define BENCH_BLK 32u            /* CTL: the samples of one block, as audio_block() renders them */
#define BENCH_BLOCKS 1378u       /* one second of audio: 1378 blocks, 44096 samples (0.99982 s at 44.1 kHz) */
#define BENCH_FS 44100u
#define BENCH_CLOCK 24000000u    /* TIMER4 ticks per second (fm1_time.h) */

/* the wheels, integer: per sample, every wheel's phase advances (inc) and its sine (linear, as sine_i) times its Q9 gain
 * is summed; the sum is Q15 (one shift per sample). The worst sum is 60 * 32767 * 512 = 1.0e9: fits int32. */
static void bench_wheels_i(const int16_t *sq, uint32_t *ph, const uint32_t *inc, const int32_t *g, int32_t *y)
{
    uint32_t s, w;
    for (s = 0; s < BENCH_BLK; s++) {
        int32_t acc = 0;
        for (w = 0; w < BENCH_N; w++) {
            uint32_t p = ph[w] += inc[w];
            uint32_t i = p >> 22;
            int32_t a = sq[i], b = sq[(i + 1u) & 1023u];
            int32_t v = a + (((b - a) * (int32_t)((p >> 7) & 0x7FFFu)) >> 15);
            acc += v * g[w];
        }
        y[s] = acc >> 9;
    }
}

/* the same wheels, single precision: the float sine table (units, as ORG_SINEF), the gains as floats in units (the Q9
 * gain / 512), the sum scaled to Q15 once per sample. The phases stay integers, as the organ's do; only the arithmetic is float. */
static void bench_wheels_f(const float *sf, uint32_t *ph, const uint32_t *inc, const float *g, float *y)
{
    uint32_t s, w;
    for (s = 0; s < BENCH_BLK; s++) {
        float acc = 0.0f;
        for (w = 0; w < BENCH_N; w++) {
            uint32_t p = ph[w] += inc[w];
            uint32_t i = p >> 22;
            float a = sf[i];
            float v = a + (sf[(i + 1u) & 1023u] - a) * (float)((p >> 7) & 0x7FFFu) * (1.0f / 32768.0f);
            acc += v * g[w];
        }
        y[s] = acc * 32768.0f;
    }
}

/* the FIR, integer: 33 taps in Q10, a 64-sample history ring, int32 sums (33 * 1024 * 32767 = 1.1e9: fits), >> 10 */
static void bench_fir_i(const int32_t *c, int32_t *hist, uint32_t *hp, const int32_t *x, int32_t *y)
{
    uint32_t s, k;
    for (s = 0; s < BENCH_BLK; s++) {
        int32_t acc = 0;
        *hp = (*hp + 1u) & 63u;
        hist[*hp] = x[s];
        for (k = 0; k < BENCH_TAPS; k++)
            acc += c[k] * hist[(*hp - k) & 63u];
        y[s] = acc >> 10;
    }
}

/* the FIR, single precision: the same taps as floats (c / 1024 exactly), the same ring, float sums */
static void bench_fir_f(const float *c, float *hist, uint32_t *hp, const float *x, float *y)
{
    uint32_t s, k;
    for (s = 0; s < BENCH_BLK; s++) {
        float acc = 0.0f;
        *hp = (*hp + 1u) & 63u;
        hist[*hp] = x[s];
        for (k = 0; k < BENCH_TAPS; k++)
            acc += c[k] * hist[(*hp - k) & 63u];
        y[s] = acc;
    }
}

/* the clock: the active time of a window is its ticks less the audio interrupt's time inside it (unsigned: a wrap of the
 * 32-bit counter is fine) */
static uint32_t bench_active(uint32_t t0, uint32_t t1, uint32_t isr0, uint32_t isr1)
{
    return (t1 - t0) - (isr1 - isr0);
}

/* ticks of one second of audio -> tenths of a percent of the real time (1000 = 100.0 %) */
static uint32_t bench_tenths(uint32_t ticks)
{
    return (uint32_t)(((uint64_t)ticks * BENCH_FS) / ((uint64_t)BENCH_BLOCKS * BENCH_BLK * BENCH_CLOCK / 1000u));
}

/* a float kernel's ticks against its integer partner, in hundredths (82 = 0.82 x: float takes 82 % of the integer time) */
static uint32_t bench_ratio(uint32_t ft, uint32_t it)
{
    return it ? (uint32_t)(((uint64_t)ft * 100u) / it) : 0u;
}

/* "12.3" from tenths of a percent, no libc */
static void bench_fmt(char *b, uint32_t t)
{
    char d[12];
    uint32_t n = 0, i = 0, v = t / 10u;
    do {
        d[n++] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v);
    while (n)
        b[i++] = d[--n];
    b[i++] = '.';
    b[i++] = (char)('0' + t % 10u);
    b[i] = 0;
}

#endif
