/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Guilherme Ruppert (Organvave) */
/* BENCHMARK's two globals (0.11.0): the mute the audio ISR reads (audio.c) and the ISR's own time while muted (bench.c
 * takes it out of its windows). Their own header, so that both the ISR (audio.c) and the runner (bench.c) see them whichever
 * of the two the host tests include: each test is one translation unit, and this is included once per one. */
#ifndef BENCH_STATE_H
#define BENCH_STATE_H

#include <stdint.h>

volatile uint8_t bench_mute;                   /* 1 while the BENCHMARK runs: the ISR writes silence and renders nothing */
volatile uint32_t bench_isr_ticks;             /* the ISR's time while muted (TIMER4 ticks) */

#endif
