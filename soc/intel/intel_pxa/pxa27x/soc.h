/*
 * Copyright (c) 2026 Takayuki Goto
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_SOC_INTEL_INTEL_PXA_PXA27X_SOC_H_
#define ZEPHYR_SOC_INTEL_INTEL_PXA_PXA27X_SOC_H_

#include <soc_common.h>

#ifndef _ASMLANGUAGE

/**
 * @brief Frequency of the LCD controller clock (LCLK), in Hz
 */
uint32_t pxa_lcd_clock_hz(void);

#endif /* !_ASMLANGUAGE */

#endif /* ZEPHYR_SOC_INTEL_INTEL_PXA_PXA27X_SOC_H_ */
