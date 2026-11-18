/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Guilherme Ruppert (OrganVave-1) */
/* BENCHMARK (0.11.0, GLOBAL > BENCHMARK): the runner. Eight kernels, each the work of one second of audio: the wheels,
 * the FIR and the DRIVE in integers and in single precision, the organ itself (float, ten keys held), and the rotary
 * (the Leslie's float engine, 400 / 340 rpm, no integer partner: 0.11.0 WIP). While it runs
 * the audio is muted (audio.c: the ISR writes silence and keeps its own time apart, bench_isr_ticks), the watchdog is fed
 * between blocks, and each kernel runs twice keeping the faster. The arithmetic is in bench_kernels.h. Outside the audio
 * path: no budget entry. The results are the device's own; the host test checks the kernels agree, not the timing. */
#include "bench_kernels.h"
#include "bench_state.h"                        /* bench_mute, bench_isr_ticks (also in audio.c: the guard keeps one) */

#define BENCH_KERNELS 8u
#define BENCH_REPS 2u

static uint32_t bench_ticks[BENCH_KERNELS];     /* ticks of one second of audio, the faster of the repeats */
static uint8_t bench_have;                      /* a run has finished: the screen shows its numbers */
static uint8_t bench_req;                       /* GLOBAL's ALGORITHM asked for a run: bench_poll starts it */
static volatile uint32_t bench_sink;            /* the kernels' results, kept so the work is not optimised away */
/* the DRIVE is called through a volatile pointer with the block length as a variable: a direct call with a constant lets
 * the compiler clone dr_orig for 32 samples, and the render's budget (tests/target_budget.txt) measures the clone, not the
 * function the audio path runs (0.11.0: 6129 against 1702). The organ's render is called directly, as the audio path calls it
 * (through a pointer its code changed: the budget of its wheels moved, with no change of theirs) */
static void (*volatile bench_drive)(dr_t *, const int32_t *, int32_t *, uint32_t) = dr_run_ex;
static volatile uint32_t bench_blk = BENCH_BLK;

static const uint8_t BENCH_KEYS[10] = {12, 19, 24, 28, 31, 36, 40, 43, 48, 55};   /* a chord, the organ's keys 0..60 */

static uint32_t bph[BENCH_N], binc[BENCH_N];
static int32_t bgi[BENCH_N];                    /* gains, Q9 */
static float bgf[BENCH_N];
static int32_t byi[BENCH_BLK], bout[BENCH_BLK], bx[BENCH_BLK], bc[BENCH_TAPS], bhist[64];
static float byf[BENCH_BLK], bxf[BENCH_BLK], bcf[BENCH_TAPS], bhistf[64];
static uint32_t bhp, bhpf;
static dr_t bdr;
/* the rotary's state: in .pool (the SRAM .bss is nearly full); the audio is muted while the benchmark runs */
static rot_t brot __attribute__((section(".pool")));

/* the inputs and the tables, once per run (outside the timing): the phases' steps for 55 Hz .. 830 Hz (inc = f * 2^32 / fs,
 * 2^32 / 44100 = 97391.4), the gains, the FIR's taps (a binomial of 33 terms, normalised, in Q10 and as floats) and a sine
 * at about -12 dBFS for the FIR and the DRIVE */
static void bench_data(void)
{
    uint32_t w, k;
    float t = 1.0f, sum = 0.0f;
    static float raw[BENCH_TAPS];
    for (w = 0; w < BENCH_N; w++) {
        binc[w] = (55u + 13u * w) * 97391u;
        bgi[w] = (int32_t)(60u + (w * 37u) % 400u);
        bgf[w] = (float)bgi[w] * (1.0f / 512.0f);
    }
    for (k = 0; k < BENCH_TAPS; k++) {
        raw[k] = t;
        sum += t;
        t = t * (float)(BENCH_TAPS - 1u - k) / (float)(k + 1u);
    }
    for (k = 0; k < BENCH_TAPS; k++) {
        bc[k] = (int32_t)(raw[k] / sum * 1024.0f + 0.5f);
        bcf[k] = (float)bc[k] * (1.0f / 1024.0f);
    }
    for (k = 0; k < BENCH_BLK; k++) {
        bx[k] = SINE[(k * 45u) & 1023u] / 4;
        bxf[k] = (float)bx[k];
    }
}

static void bench_setup(uint32_t k)             /* not timed: the states the kernel starts from */
{
    uint32_t i;
    if (k == 6u) {
        org_all_off();
        for (i = 0; i < 10u; i++)
            org_key(BENCH_KEYS[i], 1u);
    } else if (k == 7u) {
        rot_init(&brot);                        /* the cabinet at rest, the rotors at 400 / 340 rpm (649277 / 551885 per sample) */
        rot_target(&brot, 649277u, 551885u);
    } else if (k == 4u || k == 5u) {
        dr_init(&bdr);
        dr_user(&bdr, k == 4u ? 2u : 8u, 40u, 4u);  /* TUBE DRIVE (2) or ORIGINAL (8), GAIN 4.0 */
    } else {
        for (i = 0; i < BENCH_N; i++)
            bph[i] = 0u;
        for (i = 0; i < 64u; i++) {
            bhist[i] = 0;
            bhistf[i] = 0.0f;
        }
        bhp = bhpf = 0u;
    }
}

static void bench_teardown(uint32_t k)
{
    if (k == 6u)
        org_all_off();
}

static void bench_body(uint32_t k)              /* one second: BENCH_BLOCKS blocks of BENCH_BLK samples */
{
    uint32_t b;
    for (b = 0; b < BENCH_BLOCKS; b++) {
        switch (k) {
        case 0u:
            bench_wheels_i(SINE, bph, binc, bgi, byi);
            bench_sink += (uint32_t)byi[BENCH_BLK - 1u];
            break;
        case 1u:
            bench_wheels_f(ORG_SINEF, bph, binc, bgf, byf);
            bench_sink += (uint32_t)(int32_t)byf[BENCH_BLK - 1u];
            break;
        case 2u:
            bench_fir_i(bc, bhist, &bhp, bx, byi);
            bench_sink += (uint32_t)byi[BENCH_BLK - 1u];
            break;
        case 3u:
            bench_fir_f(bcf, bhistf, &bhpf, bxf, byf);
            bench_sink += (uint32_t)(int32_t)byf[BENCH_BLK - 1u];
            break;
        case 4u:
        case 5u:
            bench_drive(&bdr, bx, byi, bench_blk);
            bench_sink += (uint32_t)byi[BENCH_BLK - 1u];
            break;
        case 7u:
            rot_block(&brot, bx, byi, bout, BENCH_BLK);
            bench_sink += (uint32_t)bout[BENCH_BLK - 1u];
            break;
        default:                                /* 6: the organ's own render, as the audio path calls it */
            {
                uint32_t i;
                for (i = 0; i < BENCH_BLK; i++)
                    bout[i] = 0;
                org_render(bout, BENCH_BLK);
                bench_sink += (uint32_t)bout[BENCH_BLK - 1u];
            }
            break;
        }
        fm1_wdt_feed();
    }
}

/* the faster of BENCH_REPS runs of kernel k, its audio interrupt's time taken out */
static uint32_t bench_one(uint32_t k)
{
    uint32_t rep, best = 0xFFFFFFFFu;
    for (rep = 0; rep < BENCH_REPS; rep++) {
        uint32_t t0, i0, t1, i1, dt;
        bench_setup(k);
        t0 = fm1_ticks();
        i0 = bench_isr_ticks;
        bench_body(k);
        t1 = fm1_ticks();
        i1 = bench_isr_ticks;
        bench_teardown(k);
        dt = bench_active(t0, t1, i0, i1);
        if (dt < best)
            best = dt;
    }
    return best;
}

static void bench_show(uint32_t k)              /* "BENCHMARK 3/7", over the whole screen while it runs */
{
    char s[] = "BENCHMARK 0/8";
    s[10] = (char)('1' + k);
    s[12] = (char)('0' + BENCH_KERNELS);
    lcd_fill(0, 0, 240, 240, T_BG);
    draw_text_box(0, 110, 240, &AF_M, s, T_THEME, 1);
}

/* called by the main loop after the input: starts a requested run, muted, and asks for the screen again */
static void bench_poll(void)
{
    uint32_t k;
    if (!bench_req)
        return;
    bench_req = 0;
    bench_data();
    bench_mute = 1;
    for (k = 0; k < BENCH_KERNELS; k++) {
        bench_show(k);
        bench_ticks[k] = bench_one(k);
        fm1_wdt_feed();
    }
    bench_mute = 0;
    bench_have = 1;
    ui.force = 1;
}
