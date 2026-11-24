/* SPDX-License-Identifier: GPL-3.0-only
 * The browser has no SMK-37 TRS jack: the UART of firmware/hal/smk_uart_tx.h (MMIO at the AC79's registers) is replaced by
 * no-ops, so the MIDI OUT queue, its settings and the keyboard's messages run as on the device and the bytes go nowhere. */
#pragma once
#include <stdint.h>
static inline void smk_uart1_tx_init(void) {}
static inline void smk_uart1_tx_byte(uint8_t b) { (void)b; }
static inline uint32_t smk_uart1_tx_done(void) { return 1u; }
