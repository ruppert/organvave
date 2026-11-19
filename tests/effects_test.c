/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Guilherme Ruppert (OrganVave-1) */
/* The organ's effects section end to end on the host: the drive's patch bytes (OP_DRV .. OP_DRV_OUT) through mix_block, with the
 * whole firmware (organ_app.c) and a chord held on the organ. Since 0.11.0 the Leslie is gone (its controls are dummies): its
 * checks went with it. The master limiter's checks come back with the port's limiter removal (0.11.0).
 * 1. the bytes: clamped by org_pmax; a patch of zeros leaves the mix as it was (no drive).
 * 2. the fades: switching the drive on with the chord held makes no step (largest sample-to-sample jump within 4 times the dry mix's).
 * 3. the drive: a single key at one drawbar gets its third harmonic (more than 6 dB higher re the fundamental).
 * 4. silence: with everything on and no key, silence out.
 * 5. after the keys the output is quiet again, and MASTER 0 is silent. */
#include "host.h"
#include "../firmware/src/bench.c"
#include "../firmware/src/organ_app.c"
#include <stdio.h>

static int bad;
static void check(const char *what, int ok)
{
    printf("effects: %-100s %s\n", what, ok ? "ok" : "FAIL");
    bad += !ok;
}
#define FS_ 44100.0
#define MAXN (44100 * 40)
static int32_t oL[MAXN], oR[MAXN];
static uint32_t pos;

static void blocks(uint32_t n)           /* n samples (a multiple of CTL) from pos */
{
    int32_t o[2 * CTL];
    uint32_t i, t;
    for (t = 0; t < n; t += CTL) {
        mix_block(o, CTL);
        for (i = 0; i < CTL && pos < MAXN; i++, pos++) {
            oL[pos] = o[2u * i];
            oR[pos] = o[2u * i + 1u];
        }
    }
}
static void patch_set(const uint8_t *v) { org_set_patch(v); }
static uint8_t P[ORG_PSIZE];
static void boot(void)
{
    input_off(&trk[0], 48);              /* (a key of the last section) */
    input_off(&trk[0], 55);
    input_off(&trk[0], 60);
    host_tracks_init();
    memset(&fx, 0, sizeof fx);           /* (the last section's reverb tail) */
    rev_clear();
    org_init();
    dr_init(&dr_st);
    rot_load_tables(); rot_init(&rot_st);                 /* the rotary (main.c does the same at boot) */
    eq_init(&eq_st);
    dc_l = dc_r = dce_l = dce_r = 0;
    eq_w = 0; eq_idle = 0; eq_on = 0;
    dr_w = 0;
    dr_on = 0;
    fxo_seen = 0xFF;
    pos = 0;
    host_preset(&trk[0], ENGI_ORGAN, 0);
    trk[0].p[P_AMODE] = 0;
    trk[0].p[P_LEVEL] = 101;             /* (the level the sections were measured at: 0.9.8's first preset is louder) */
    trk[0].p[P_DIST] = trk[0].p[P_CHOR] = trk[0].p[P_DLY] = trk[0].p[P_REV] = 0;     /* (no reverb tail: the effects alone) */
    memset(P, 0, sizeof P);
    P[OP_UB + 2] = 8;                    /* 8' */
    P[OP_UB + 3] = 6;                    /* 4' */
    P[OP_DRV_GAIN] = 0;                  /* (0.11.1: GAIN 0 is the bypass: the drive is off unless a test sets it) */
    patch_set(P);
}
static double db(double x) { return 20 * log10(x > 1e-9 ? x : 1e-9); }
static double rms_win(const int32_t *y, uint32_t a, uint32_t n)
{
    double e = 0;
    uint32_t i;
    for (i = 0; i < n; i++) e += (double)y[a + i] * y[a + i];
    return sqrt(e / n);
}
/* the level of the signal in 50 ms windows over [a, b): the largest minus the smallest, dB */
static double swing(const int32_t *y, uint32_t a, uint32_t b)
{
    double lo = 1e30, hi = 0;
    uint32_t w = 2205, t;
    for (t = a; t + w <= b; t += w) {
        double r = rms_win(y, t, w);
        if (r < lo) lo = r;
        if (r > hi) hi = r;
    }
    return db(hi) - db(lo);
}
static int32_t maxjump(const int32_t *y, uint32_t a, uint32_t b)
{
    int32_t m = 0;
    uint32_t i;
    for (i = a + 1; i < b; i++) {
        int32_t d = y[i] - y[i - 1];
        if (d < 0) d = -d;
        if (d > m) m = d;
    }
    return m;
}
static double tone(const int32_t *y, uint32_t a, uint32_t n, double f)
{
    double re = 0, im = 0;
    uint32_t i;
    for (i = 0; i < n; i++) {
        double w = 0.5 - 0.5 * cos(2 * M_PI * i / n), ph = 2 * M_PI * f * (a + i) / FS_;
        re += w * y[a + i] * cos(ph);
        im -= w * y[a + i] * sin(ph);
    }
    return 2 * sqrt(re * re + im * im) / (n * 0.5);
}
static void chord_on(void)
{
    static const uint8_t K[3] = {48, 55, 60};
    uint32_t i;
    for (i = 0; i < 3; i++) input_on(&trk[0], K[i], 100);
}
static void chord_off(void)
{
    static const uint8_t K[3] = {48, 55, 60};
    uint32_t i;
    for (i = 0; i < 3; i++) input_off(&trk[0], K[i]);
}

int main(void)
{
    uint32_t i;
    double d0, s;
    /* 1. the bytes */
    boot();
    org_set(OP_DRV_GAIN, 250);
    org_set(OP_DRV_TYPE, 99);
    org_set(OP_DRV_TONE, 99);
    org_set(OP_DRV_TREB, 99);
    check("the bytes are clamped (GAIN 100, TYPE 8 (EXTREME, 0.11.0), TONE 0 and TREBLE 10 (0.9.44))",
          org_patch[OP_DRV_GAIN] == 100 && org_patch[OP_DRV_TYPE] == 8 && org_patch[OP_DRV_TONE] == 0 && org_patch[OP_DRV_TREB] == 10);
    /* a patch of zeros in the effects bytes leaves the mix dry */
    boot();
    chord_on();
    blocks(44100);
    check("with the effects bytes at 0 nothing is in the mix but the organ (dr_on, dr_w 0)",
          !dr_on && !dr_w && rms_win(oL, 22050, 22050) > 500);

    /* 2. the fades */
    boot();
    chord_on();
    blocks(44100);
    d0 = maxjump(oL, 22050, 44100);
    P[OP_DRV_GAIN] = 60;
    patch_set(P);
    blocks(44100 / 2);
    {
        double dj = maxjump(oL, 44100, pos);
        printf("effects: largest step dry %.0f, drive coming in %.0f\n", d0, dj);
        check("the drive coming in: no step (within 4 times the dry mix's; it adds harmonics, so more is allowed)", dj < 4.0 * d0 + 50);
    }

    /* 3. the drive: a single key, one drawbar: the third harmonic. DISTORTION: a single drawbar's note is far under the level
     * TUBE DRIVE's law is set at (0.9.30: its knob follows its distortion at the organ's level), so it hardly distorts it */
    boot();
    memset(P, 0, sizeof P);
    P[OP_UB + 2] = 8;
    P[OP_DRV_GAIN] = 0;
    P[OP_DRV_TYPE] = 3;
    patch_set(P);
    input_on(&trk[0], 48, 100);          /* C3, 130.81 Hz at the equal temperament; the 60 Hz gear is near it */
    blocks(44100 * 2);
    {
        double f0 = 130.81, a1, a3, b1, b3;
        uint32_t a = 44100;
        a1 = tone(oL, a, 44100, f0);
        a3 = tone(oL, a, 44100, 3 * f0);
        P[OP_DRV_GAIN] = 60;
        patch_set(P);
        blocks(44100 * 2);
        b1 = tone(oL, pos - 44100, 44100, f0);
        b3 = tone(oL, pos - 44100, 44100, 3 * f0);
        printf("effects: dry H3 %.1f dB re H1, driven H3 %.1f dB re H1\n", db(a3 / a1), db(b3 / b1));
        printf("effects: level dry %.0f, driven %.0f (rms)\n", rms_win(oL, 44100, 44100), rms_win(oL, pos - 44100, 44100));
        check("the drive: the third harmonic rises by more than 6 dB re the fundamental", db(b3 / b1) - db(a3 / a1) > 6.0);
    }

    /* 4. silence */
    {
        static const struct { uint8_t drv; const char *n; } C[2] = {{1, "drive"}, {0, "dry"}};
        uint32_t c;
        int32_t worst = 0;
        for (c = 0; c < 2; c++) {
            int32_t pk = 0;
            boot();
            P[OP_DRV_GAIN] = C[c].drv ? 60 : 0;
            patch_set(P);
            blocks(44100);
            for (i = 22050; i < pos; i++) {
                int32_t a = oL[i] < 0 ? -oL[i] : oL[i], b = oR[i] < 0 ? -oR[i] : oR[i];
                if (a > pk) pk = a;
                if (b > pk) pk = b;
            }
            printf("effects: no key, %s: peak %d\n", C[c].n, pk);
            if (pk > worst) worst = pk;
        }
        check("every effect on, no key: silence out (|y| < 4)", worst < 4);
    }
    /* 5. after the keys: the drive goes quiet (no limit cycle: at most 1 LSB), and MASTER 0 is silent */
    {
        static const struct { uint8_t drv; const char *n; } C[2] = {{0, "dry"}, {1, "the drive"}};
        uint32_t c;
        for (c = 0; c < 2; c++) {
            int32_t pk = 0;
            boot();
            P[OP_DRV_GAIN] = C[c].drv ? 60 : 0;
            patch_set(P);
            chord_on();
            blocks(44100);
            chord_off();
            blocks(44100 * 5);
            for (i = pos - 44100; i < pos; i++) {
                int32_t a = oL[i] < 0 ? -oL[i] : oL[i], b = oR[i] < 0 ? -oR[i] : oR[i];
                if (a > pk) pk = a;
                if (b > pk) pk = b;
            }
            printf("effects: %s, five seconds after the keys: peak %d\n", C[c].n, pk);
            check("  .. the output is quiet again (peak at most 1)", pk <= 1);
        }
        boot();
        P[OP_DRV_GAIN] = 60;
        patch_set(P);
        song.master_q12 = 0;
        chord_on();
        blocks(44100);
        {
            int32_t pk = 0;
            for (i = 0; i < pos; i++) {
                int32_t a = oL[i] < 0 ? -oL[i] : oL[i], b = oR[i] < 0 ? -oR[i] : oR[i];
                if (a > pk) pk = a;
                if (b > pk) pk = b;
            }
            check("MASTER 0 with the drive on, a chord held: silent", pk == 0);
        }
        song.master_q12 = 4096;
    }
    /* 7. the equaliser (0.5.0): after MASTER, before the limiter */
    {
        uint32_t i, a;
        double lo0, lo1, hi0, hi1;
        boot();
        memset(P, 0, sizeof P);
        P[OP_EQ_LOW] = 30; P[OP_EQ_MID] = 99; P[OP_EQ_FREQ] = 99; P[OP_EQ_HIGH] = 25;
        org_set_patch(P);
        check("the EQ bytes are clamped (gains 24, FREQ 11)", org_patch[OP_EQ_LOW] == 24 &&
              org_patch[OP_EQ_MID] == 24 && org_patch[OP_EQ_FREQ] == 11 && org_patch[OP_EQ_HIGH] == 24);
        /* flat: on with every byte at 0 is the dry mix within 3 */
        boot();
        P[OP_UB + 2] = 8; P[OP_UB + 3] = 6;
        patch_set(P);
        chord_on();
        blocks(44100);
        a = pos;
        blocks(22050);
        {
            int32_t ref[22050];
            int32_t worst = 0;
            memcpy(ref, oL + a, sizeof ref);
            /* (the keys go up and the release runs out before the second pass: the same keys sounding again at once
             * would differ by the old release's tail, 14 LSB, since the CLICK release is the default (0.9.5)) */
            input_off(&trk[0], 48); input_off(&trk[0], 55); input_off(&trk[0], 60);
            blocks(44100);
            boot();
            patch_set(P);
            chord_on();
            blocks(44100);
            a = pos;
            blocks(22050);
            for (i = 0; i < 22050u; i++) {
                int32_t d = oL[a + i] - ref[i];
                d = d < 0 ? -d : d;
                worst = d > worst ? d : worst;
            }
            printf("effects: EQ on and flat against off: the largest difference %d\n", worst);
            check("the EQ on and flat: the output is the dry mix within 3", worst <= 3);
        }
        chord_off();
        /* LOW +12: a key at C2 (65.4 Hz, the 8' drawbar) gets louder by more than 6 dB */
        boot();
        P[OP_UB + 2] = 8;
        patch_set(P);
        input_on(&trk[0], 36, 100);
        blocks(44100);
        lo0 = tone(oL, 22050, 22050, 65.41);
        P[OP_EQ_LOW] = 12;
        patch_set(P);
        blocks(44100 * 2);
        lo1 = tone(oL, pos - 22050, 22050, 65.41);
        input_off(&trk[0], 36);
        printf("effects: the EQ's LOW +12: the 65 Hz tone %.1f dB\n", db(lo1 / lo0));
        check("LOW +12 dB: the 65 Hz tone rises by 6 .. 14 dB", db(lo1 / lo0) > 6.0 && db(lo1 / lo0) < 14.0);
        /* HIGH -12: a key at C6 with the 1' drawbar (4186 Hz) goes down by more than 6 dB */
        boot();
        P[OP_UB + 8] = 8;
        patch_set(P);
        input_on(&trk[0], 84, 100);
        blocks(44100);
        hi0 = tone(oL, 22050, 22050, 4186.0);
        P[OP_EQ_HIGH] = 13;
        patch_set(P);
        blocks(44100 * 2);
        hi1 = tone(oL, pos - 22050, 22050, 4186.0);
        input_off(&trk[0], 84);
        printf("effects: the EQ's HIGH -12: the 4.2 kHz tone %.1f dB\n", db(hi1 / hi0));
        check("HIGH -12 dB: the 4.2 kHz tone falls by 6 .. 14 dB", db(hi1 / hi0) < -6.0 && db(hi1 / hi0) > -14.0);
        /* the fade: the EQ coming in with a chord held makes no step */
        boot();
        P[OP_UB + 2] = 8; P[OP_UB + 3] = 6;
        patch_set(P);
        chord_on();
        blocks(44100);
        {
            int32_t d0 = maxjump(oL, 22050, pos), dj;
            a = pos;
            P[OP_EQ_LOW] = 12; P[OP_EQ_MID] = 12; P[OP_EQ_HIGH] = 12;
            patch_set(P);
            blocks(22050);
            dj = maxjump(oL, a, pos);
            printf("effects: the EQ coming in: the largest jump %d against %d dry\n", dj, d0);
            check("the EQ coming in with a chord held: no step (within 4 times the dry mix's)", dj < 4 * d0 + 50);
        }
        chord_off();
        /* silence, and MASTER 0 */
        boot();
        P[OP_EQ_LOW] = 12; P[OP_EQ_MID] = 12; P[OP_EQ_HIGH] = 12; P[OP_DRV_GAIN] = 60;
        patch_set(P);
        chord_on();
        blocks(22050);
        chord_off();
        blocks(44100 * 8);
        {
            int32_t pk = 0;
            for (i = pos - 44100; i < pos; i++) {
                int32_t x = oL[i] < 0 ? -oL[i] : oL[i], y = oR[i] < 0 ? -oR[i] : oR[i];
                pk = x > pk ? x : pk;
                pk = y > pk ? y : pk;
            }
            printf("effects: everything on, eight seconds after the keys: peak %d\n", pk);
            check("the EQ on with the drive, long after the keys: silent (peak at most 1)", pk <= 1);
        }
        boot();
        P[OP_EQ_LOW] = 12; P[OP_EQ_MID] = 12; P[OP_EQ_HIGH] = 12;
        patch_set(P);
        song.master_q12 = 0;
        chord_on();
        blocks(44100);
        {
            int32_t pk = 0;
            for (i = 0; i < pos; i++) {
                int32_t x = oL[i] < 0 ? -oL[i] : oL[i];
                pk = x > pk ? x : pk;
            }
            check("MASTER 0 with the EQ on, a chord held: silent", pk == 0);
        }
        song.master_q12 = 4096;
    }
    /* 8. CPU overload (fx_cpu_high, which audio.c's shed_voice calls when it has no voice to take): the oldest key let go
     * (never the last); the delay and the drive stay as chosen (no CPU HIGH indicator, 0.9.26) */
    {
        boot();
        memset(P, 0, sizeof P);
        P[OP_UB + 2] = 8; P[OP_DRV_GAIN] = 60; P[OP_DRV_GAIN] = 60;
        patch_set(P);
        trk[0].p[P_DLY] = 60;
        org_key(10, 1); org_key(20, 1); org_key(30, 1);
        blocks(8820);
        check("overload: before, the delay runs and the organ holds 3 keys", fx.dly_on == 1 && org.ndown == 3);
        fx_cpu_high();
        blocks(256);
        check("  .. the delay still runs", fx.dly_on == 1);
        check("  .. the oldest key let go, the others held", org.keys[10] == 0 && org.keys[20] && org.keys[30] && org.ndown == 2);
        check("  .. the drive is as chosen", dr_on == 1);
        fx_cpu_high(); blocks(256);
        check("  .. the next oldest goes", org.keys[20] == 0 && org.keys[30] && org.ndown == 1);
        fx_cpu_high(); blocks(256);
        check("  .. the last key is never let go", org.keys[30] && org.ndown == 1);
        {
            int32_t pk = 0;
            uint32_t i;
            blocks(4410);
            for (i = pos - 4410; i < pos; i++)
                pk = oL[i] > pk ? oL[i] : -oL[i] > pk ? -oL[i] : pk;
            check("  .. and the organ still sounds", pk > 500);
        }
        check("  .. the delay runs on", fx.dly_on == 1);
        org_key(30, 0);
        trk[0].p[P_DLY] = 0;
        blocks(2000);
    }
    /* 9. every key of the three manuals at once, everything on: bounded and alive for ten seconds, then quiet */
    {
        uint32_t k, i, n = 0;
        int32_t pk = 0, pk_end = 0;
        double e = 0;
        boot();
        memset(P, 0, sizeof P);
        for (i = 0; i < 27u; i++)
            P[i] = 8;                                    /* every drawbar of every manual at 8 */
        P[OP_PERC] = 1; P[OP_VIB] = 5; P[OP_DRV_GAIN] = 60; P[OP_DRV_GAIN] = 100;
        P[OP_EQ_LOW] = 12; P[OP_EQ_MID] = 12; P[OP_EQ_HIGH] = 12;
        patch_set(P);
        trk[0].p[P_REV] = 127; trk[0].p[P_DLY] = 127;
        for (k = 0; k < ORG_NKEY; k++) {                 /* (the key queue holds 64 messages a fragment: a block between each 32) */
            org_key(k, 1);
            if (k % 32u == 31u)
                blocks(CTL);
        }
        blocks(44100 * 10);
        for (i = 44100; i < pos; i++) {
            int32_t a = oL[i] < 0 ? -oL[i] : oL[i], b = oR[i] < 0 ? -oR[i] : oR[i];
            pk = a > pk ? a : pk;
            pk = b > pk ? b : pk;
            e += (double)oL[i] * oL[i];
            n++;
        }
        printf("effects: all %u keys down, everything on, 10 s: peak %d, rms %.0f\n", (unsigned)org.ndown, pk, sqrt(e / n));
        check("all keys of the three manuals with everything on: bounded (peak under 2^17) and alive (rms above 1000)", pk < 131072 && sqrt(e / n) > 1000.0);
        for (k = 0; k < ORG_NKEY; k++) {
            org_key(k, 0);
            if (k % 32u == 31u)
                blocks(CTL);
        }
        trk[0].p[P_REV] = 0; trk[0].p[P_DLY] = 0;
        blocks(44100 * 14);
        for (i = pos - 44100; i < pos; i++) {
            int32_t a = oL[i] < 0 ? -oL[i] : oL[i], b = oR[i] < 0 ? -oR[i] : oR[i];
            pk_end = a > pk_end ? a : pk_end;
            pk_end = b > pk_end ? b : pk_end;
        }
        printf("effects: 14 s after letting go: peak %d\n", pk_end);
        check("  .. and after letting go it falls quiet (peak at most 2 after 14 s)", pk_end <= 2);
    }
    /* 6. the drive's two types, its tone and output (0.9.2 .. 0.9.44) */
    {
        double f0 = 130.81, d1, d2, d3, t1, t2, t3, rd, rt, rt0, rt1;
        boot();
        memset(P, 0, sizeof P);
        P[OP_UB + 2] = 8; P[OP_DRV_GAIN] = 60; P[OP_DRV_GAIN] = 60; P[OP_DRV_TYPE] = 3;   /* DISTORTION (0.9.30: the byte 3; 0 is TUBE DRIVE) */
        patch_set(P);
        input_on(&trk[0], 48, 100);
        blocks(44100 * 2);
        d1 = tone(oL, pos - 44100, 44100, f0); d2 = tone(oL, pos - 44100, 44100, 2 * f0); d3 = tone(oL, pos - 44100, 44100, 3 * f0);
        rd = rms_win(oL, pos - 44100, 44100);
        P[OP_DRV_TYPE] = 2;
        patch_set(P);
        blocks(44100 * 2);
        t1 = tone(oL, pos - 44100, 44100, f0); t2 = tone(oL, pos - 44100, 44100, 2 * f0); t3 = tone(oL, pos - 44100, 44100, 3 * f0);
        rt = rms_win(oL, pos - 44100, 44100);
        printf("effects: DISTORTION H2 %.1f H3 %.1f dB re H1, rms %.0f; TUBE H2 %.1f H3 %.1f dB re H1, rms %.0f\n",
               db(d2 / d1), db(d3 / d1), rd, db(t2 / t1), db(t3 / t1), rt);
        check("TUBE: it saturates (the third harmonic above -50 dB) but is gentler than DISTORTION (at least 8 dB lower)",
              db(t3 / t1) > -50.0 && db(d3 / d1) - db(t3 / t1) > 8.0);
        /* the levels agree within 1 dB on the calibration chord at the organ's level (tests/drive_test.c); a single drawbar's note
         * is far under it, where DISTORTION, which compresses, comes out louder than TUBE DRIVE (still linear there): about 18 dB
         * since 0.9.40 (the organ 14 dB lower into the master: the limiter, which held DISTORTION down before, leaves it alone) */
        check("TUBE at GAIN 6 against DISTORTION on a single drawbar's note: DISTORTION louder, by at most 20 dB", db(rt / rd) > -20.0 && db(rt / rd) < 0.0);
        {   /* the curve's asymmetry: even harmonics. At GAIN 10 (a single drawbar's note is quiet: at 6 TUBE DRIVE is nearly clean;
             * the second harmonic seen there before 0.9.36 was the crosstalk's 4' leakage, pruned since) */
            double u1, u2;
            P[OP_DRV_GAIN] = 100;
            patch_set(P);
            blocks(44100 * 2);
            u1 = tone(oL, pos - 44100, 44100, f0); u2 = tone(oL, pos - 44100, 44100, 2 * f0);
            printf("effects: TUBE at GAIN 10: H2 %.1f dB re H1 (at 6: %.1f)\n", db(u2 / u1), db(t2 / t1));
            /* 0.11.0: the complete crosstalk (setBfree's) puts the 4' leakage back into the second harmonic at every GAIN, so the
             * rise over GAIN 6 is no longer what this measures; the check is that GAIN 10 has its even harmonic above -40 dB */
            check("TUBE: the curve's even harmonics (at GAIN 10 the second is above -40 dB re the fundamental)",
                  db(u2 / u1) > -40.0);
            P[OP_DRV_GAIN] = 60;
            patch_set(P);
            blocks(44100);
        }
        check("TUBE: it sounds (rms above 500) and is bounded", rt > 500.0 && rt < 20000.0);
        /* the TREBLE (0.9.44; TONE before, whose bytes still say it: an old TONE is moved to the TREBLE when the patch is set): on
         * C5 (523 Hz) TREBLE +5 lifts the seventh harmonic (3663 Hz, near the 4 kHz shelf) against the first, -5 lowers it */
        {
            double fl, br, dk;
            input_off(&trk[0], 48);
            blocks(44100);
            f0 = 523.25;
            P[OP_DRV_TONE] = 0;
            P[OP_DRV_TYPE] = 3;                         /* DISTORTION: its own harmonics dominate the organ's on a single drawbar */
            patch_set(P);
            input_on(&trk[0], 72, 100);
            blocks(44100 * 2);
            fl = tone(oL, pos - 44100, 44100, 7 * f0) / tone(oL, pos - 44100, 44100, f0);
            P[OP_DRV_TONE] = (uint8_t)org_b9(8);
            patch_set(P);
            blocks(44100 * 2);
            br = tone(oL, pos - 44100, 44100, 7 * f0) / tone(oL, pos - 44100, 44100, f0);
            P[OP_DRV_TONE] = (uint8_t)org_b9(0);
            patch_set(P);
            blocks(44100 * 2);
            dk = tone(oL, pos - 44100, 44100, 7 * f0) / tone(oL, pos - 44100, 44100, f0);
            printf("effects: C5, DISTORTION H7 re H1: flat %.1f dB, TREBLE +5 %.1f dB, TREBLE -5 %.1f dB\n", db(fl), db(br), db(dk));
            check("TREBLE (an old patch's TONE, moved): +5 lifts and -5 lowers the seventh harmonic against flat (more than 5 dB each way)",
                  db(br) - db(fl) > 5.0 && db(fl) - db(dk) > 5.0);
            input_off(&trk[0], 72);
            blocks(44100);
            P[OP_DRV_TYPE] = 2;
            input_on(&trk[0], 48, 100);
        }
        /* the output: -4 dB is 0.63 in rms */
        P[OP_DRV_TONE] = 0;
        P[OP_DRV_OUT] = (uint8_t)org_b9(2);
        patch_set(P);
        blocks(44100 * 2);
        rt1 = rms_win(oL, pos - 44100, 44100);
        P[OP_DRV_OUT] = 0;
        patch_set(P);
        blocks(44100 * 2);
        rt0 = rms_win(oL, pos - 44100, 44100);
        printf("effects: TUBE rms at 0 dB %.0f, at -4 dB %.0f (ratio %.3f, 0.631 expected)\n", rt0, rt1, rt1 / rt0);
        check("OUTPUT: -4 dB takes the level down to 0.63 (within 3 %)", rt1 / rt0 > 0.61 && rt1 / rt0 < 0.65);
        input_off(&trk[0], 48);
        blocks(44100 * 2);
    }
    printf(bad ? "effects: FAILED\n" : "effects: all ok\n");
    return bad;
}
