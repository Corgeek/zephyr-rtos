/*
 * Copyright (c) 2026 Takayuki Goto
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Intel/Marvell PXA27x SoC support
 */

#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/arch/arm/arm9/mmu.h>
#include <zephyr/platform/hooks.h>
#include <soc.h>

/* Clocks manager */
#define CCCR         0x41300000U
#define CCCR_LCD_26  BIT(27)
#define CKEN         0x41300004U
#define CKEN_FFUART  BIT(6)
#define CKEN_OSTIMER BIT(9)
#define CCSR         0x4130000cU
#define CCSR_L_MASK  0x1fU

/* Power manager: sleep status register, write one to clear */
#define PSSR     0x40f00004U
#define PSSR_RDH BIT(5)
#define PSSR_PH  BIT(4)

uint32_t pxa_lcd_clock_hz(void)
{
	uint32_t l = sys_read32(CCSR) & CCSR_L_MASK;
	uint32_t k = (l <= 7U) ? 1U : (l <= 16U) ? 2U : 4U;

	/*
	 * LCLK is the run mode clock, 13 MHz x L, divided by K as L grows,
	 * or by a fixed 2 when CCCR[LCD_26] is set (as Linux computes it).
	 */
	if ((sys_read32(CCCR) & CCCR_LCD_26) != 0U) {
		k = 2U;
	}

	return 13000000U * l / k;
}

void soc_early_init_hook(void)
{
	/* A loader may have gated anything it had no use for. */
	sys_write32(sys_read32(CKEN) | CKEN_FFUART | CKEN_OSTIMER, CKEN);

	/* Release the pins from the state they were held in across sleep. */
	sys_write32(PSSR_RDH | PSSR_PH, PSSR);
}

/*
 * Only the SDRAM is worth caching; everything else in the 4 GB space is left
 * as the default identity device mapping, which covers the chip selects, the
 * PCMCIA windows and the whole 0x4xxxxxxx peripheral block. The cached map
 * runs from the bottom of bank 0 to the end of the image's memory, so an
 * image linked higher up, leaving low SDRAM free, still runs cached.
 */
#define SDRAM_NODE        DT_CHOSEN(zephyr_sram)
#define SDRAM_CACHED_SIZE (DT_REG_ADDR(SDRAM_NODE) + DT_REG_SIZE(SDRAM_NODE) - PXA_SDRAM_BANK0_BASE)

static const struct armv5_mmu_region pxa27x_mmu_regions[] = {
	ARMV5_MMU_REGION("sdram", PXA_SDRAM_BANK0_BASE, SDRAM_CACHED_SIZE, ARMV5_MMU_MEM_CACHED),
};

const struct armv5_mmu_config soc_mmu_config = {
	.regions = pxa27x_mmu_regions,
	.num_regions = ARRAY_SIZE(pxa27x_mmu_regions),
};
