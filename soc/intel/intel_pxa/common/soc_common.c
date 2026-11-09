/*
 * Copyright (c) 2026 Takayuki Goto
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Support shared by the Intel/Marvell PXA2xx SoCs
 *
 * Interrupt control for the PXA2xx interrupt controller. PXA25x/26x have a
 * single 32-source bank; PXA27x adds a second bank for sources 32 and up, at
 * a completely separate set of register offsets.
 *
 * The controller is level sensitive and does not mask the active source for
 * the duration of the handler, so ISRs run with IRQs masked (see
 * CONFIG_ARMV5_IRQ_NESTING).
 */

#include <zephyr/kernel.h>
#include <zephyr/arch/cpu.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/sys_io.h>
#include <soc.h>

#define INTC_NODE     DT_NODELABEL(intc)
#define INTC_BASE     DT_REG_ADDR(INTC_NODE)
#define INTC_NUM_IRQS DT_PROP(INTC_NODE, num_irqs)

/* Bank 1: interrupt sources 0..31 */
#define ICIP (INTC_BASE + 0x00)
#define ICMR (INTC_BASE + 0x04)
#define ICLR (INTC_BASE + 0x08)
#define ICCR (INTC_BASE + 0x14)

/* Bank 2: interrupt sources 32.. (PXA27x and later) */
#define ICIP2 (INTC_BASE + 0x9c)
#define ICMR2 (INTC_BASE + 0xa0)
#define ICLR2 (INTC_BASE + 0xa4)

/* ICCR: idle mask enable - only unmasked interrupts bring the core out of idle */
#define ICCR_DIM BIT(0)

#define HAS_BANK2 (INTC_NUM_IRQS > 32)

BUILD_ASSERT(INTC_NUM_IRQS <= CONFIG_NUM_IRQS, "CONFIG_NUM_IRQS too small for the controller");

/* Clocks manager */
#define CKEN 0x41300004U

/* GPIO controller */
#define GPIO_BASE    0x40e00000U
#define GPDR(bank)   (GPIO_BASE + (((bank) < 3U) ? (0x0cU + ((bank) * 4U)) : 0x10cU))
#define GAFR_L(bank) (GPIO_BASE + 0x54U + ((bank) * 8U))

void pxa_gpio_set_af(unsigned int pin, unsigned int af, bool output)
{
	unsigned int bank = pin >> 5;
	unsigned int shift = (pin & 0xfU) * 2U;
	mem_addr_t gafr = GAFR_L(bank) + (((pin & 0x10U) != 0U) ? 4U : 0U);
	unsigned int key = irq_lock();
	uint32_t val = sys_read32(gafr);

	val &= ~(0x3U << shift);
	sys_write32(val | (af << shift), gafr);

	val = sys_read32(GPDR(bank));
	val = output ? (val | BIT(pin & 0x1fU)) : (val & ~BIT(pin & 0x1fU));
	sys_write32(val, GPDR(bank));
	irq_unlock(key);
}

void pxa_clock_enable(unsigned int bit)
{
	unsigned int key = irq_lock();

	sys_write32(sys_read32(CKEN) | BIT(bit), CKEN);
	irq_unlock(key);
}

static inline mem_addr_t icmr_of(unsigned int irq)
{
	return (irq < 32U) ? ICMR : ICMR2;
}

void z_soc_irq_init(void)
{
	/* Mask every source and route all of them to IRQ rather than FIQ. */
	sys_write32(0U, ICMR);
	sys_write32(0U, ICLR);

	if (HAS_BANK2) {
		sys_write32(0U, ICMR2);
		sys_write32(0U, ICLR2);
	}

	sys_write32(ICCR_DIM, ICCR);
}

void z_soc_irq_enable(unsigned int irq)
{
	mem_addr_t reg = icmr_of(irq);

	sys_write32(sys_read32(reg) | BIT(irq & 0x1fU), reg);
}

void z_soc_irq_disable(unsigned int irq)
{
	mem_addr_t reg = icmr_of(irq);

	sys_write32(sys_read32(reg) & ~BIT(irq & 0x1fU), reg);
}

int z_soc_irq_is_enabled(unsigned int irq)
{
	return (sys_read32(icmr_of(irq)) & BIT(irq & 0x1fU)) != 0U;
}

void z_soc_irq_priority_set(unsigned int irq, unsigned int prio, unsigned int flags)
{
	ARG_UNUSED(irq);
	ARG_UNUSED(prio);
	ARG_UNUSED(flags);

	/*
	 * The PXA27x priority registers only matter for the ICHP "highest
	 * priority pending" shortcut, which is not used here.
	 */
}

unsigned int z_soc_irq_get_active(void)
{
	uint32_t pending = sys_read32(ICIP);

	if (pending != 0U) {
		return (unsigned int)__builtin_ctz(pending);
	}

	if (HAS_BANK2) {
		pending = sys_read32(ICIP2);
		if (pending != 0U) {
			return 32U + (unsigned int)__builtin_ctz(pending);
		}
	}

	return CONFIG_NUM_IRQS;
}

void z_soc_irq_eoi(unsigned int irq)
{
	ARG_UNUSED(irq);

	/*
	 * ICIP mirrors the peripheral's own interrupt line, so servicing the
	 * peripheral is what ends the interrupt. There is nothing to write
	 * back here.
	 */
}
