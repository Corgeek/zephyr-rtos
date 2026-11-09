/*
 * Copyright (c) 2026 Takayuki Goto
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Definitions shared by the Intel/Marvell PXA2xx SoCs
 */

#ifndef ZEPHYR_SOC_INTEL_INTEL_PXA_COMMON_SOC_COMMON_H_
#define ZEPHYR_SOC_INTEL_INTEL_PXA_COMMON_SOC_COMMON_H_

/* Static memory controller chip selects */
#define PXA_CS0_BASE 0x00000000U
#define PXA_CS1_BASE 0x04000000U
#define PXA_CS2_BASE 0x08000000U
#define PXA_CS3_BASE 0x0c000000U
#define PXA_CS4_BASE 0x10000000U
#define PXA_CS5_BASE 0x14000000U

/* PCMCIA / CompactFlash sockets */
#define PXA_PCMCIA0_BASE 0x20000000U
#define PXA_PCMCIA1_BASE 0x30000000U

/* SDRAM banks */
#define PXA_SDRAM_BANK0_BASE 0xa0000000U
#define PXA_SDRAM_BANK1_BASE 0xa4000000U
#define PXA_SDRAM_BANK2_BASE 0xa8000000U
#define PXA_SDRAM_BANK3_BASE 0xac000000U

#ifndef _ASMLANGUAGE

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Route a GPIO pin to an alternate function
 *
 * @param pin GPIO number
 * @param af Alternate function, 0 (plain GPIO) to 3
 * @param output Whether the pin drives the line
 */
void pxa_gpio_set_af(unsigned int pin, unsigned int af, bool output);

/**
 * @brief Turn on a peripheral clock
 *
 * @param bit Bit number in CKEN, as given by a node's @c cken-bit property
 */
void pxa_clock_enable(unsigned int bit);

#endif /* !_ASMLANGUAGE */

#endif /* ZEPHYR_SOC_INTEL_INTEL_PXA_COMMON_SOC_COMMON_H_ */
