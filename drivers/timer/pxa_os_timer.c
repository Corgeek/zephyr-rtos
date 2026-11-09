/*
 * Copyright (c) 2026 Takayuki Goto
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Kernel tick source based on the PXA2xx OS timer
 *
 * OSCR is a 32-bit up counter running at a fixed 3.25 MHz on PXA27x.
 * The kernel tick comes from a match on OSMR0, which is re-armed one tick
 * ahead on every interrupt; OSCR itself doubles as the cycle counter.
 */

#define DT_DRV_COMPAT intel_pxa2xx_timer

#include <zephyr/init.h>
#include <zephyr/drivers/timer/system_timer.h>
#include <zephyr/sys/clock.h>
#include <zephyr/irq.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/sys_io.h>

#define OST_BASE DT_INST_REG_ADDR(0)

#define OSMR0 (OST_BASE + 0x00)
#define OSCR  (OST_BASE + 0x10)
#define OSSR  (OST_BASE + 0x14)
#define OWER  (OST_BASE + 0x18)
#define OIER  (OST_BASE + 0x1c)

#define OSSR_M0 BIT(0)
#define OIER_E0 BIT(0)

#define CYCLES_PER_TICK (CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC / CONFIG_SYS_CLOCK_TICKS_PER_SEC)

BUILD_ASSERT(DT_INST_PROP(0, clock_frequency) == CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC,
	     "PXA OS timer clock-frequency must match CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC");

/*
 * A match only fires when OSCR passes OSMR0, so a value written too close to
 * OSCR can be missed, and then nothing happens until OSCR wraps 22 minutes
 * later. Keep at least this many cycles of margin.
 */
#define MIN_MATCH_CYCLES 16U

static struct k_spinlock lock;
/* OSCR value of the last tick boundary already handed to the kernel. */
static uint32_t last_tick_cycle;

static void pxa_ost_isr(const void *arg)
{
	ARG_UNUSED(arg);

	k_spinlock_key_t key = k_spin_lock(&lock);

	if ((sys_read32(OSSR) & OSSR_M0) == 0U) {
		k_spin_unlock(&lock, key);
		return;
	}

	/* Write-one-to-clear. */
	sys_write32(OSSR_M0, OSSR);

	/*
	 * Work out how many whole ticks have gone by rather than assuming one
	 * per interrupt. If the interrupt was serviced late - a slow handler,
	 * a long irq_lock(), or the host descheduling the emulator - the ticks
	 * in between still have to be accounted for, otherwise kernel time
	 * silently falls behind the same counter that k_busy_wait() and
	 * k_cycle_get_32() read.
	 *
	 * The subtraction is unsigned so that it stays correct across an OSCR
	 * wrap.
	 */
	uint32_t elapsed = sys_read32(OSCR) - last_tick_cycle;
	uint32_t ticks = elapsed / CYCLES_PER_TICK;

	last_tick_cycle += ticks * CYCLES_PER_TICK;

	uint32_t next = last_tick_cycle + CYCLES_PER_TICK;

	sys_write32(next, OSMR0);
	while ((int32_t)(next - sys_read32(OSCR)) < (int32_t)MIN_MATCH_CYCLES) {
		next += CYCLES_PER_TICK;
		sys_write32(next, OSMR0);
	}

	k_spin_unlock(&lock, key);

	if (ticks != 0U) {
		sys_clock_announce((int32_t)ticks);
	}
}

void sys_clock_set_timeout(uint32_t ticks, bool idle)
{
	ARG_UNUSED(ticks);
	ARG_UNUSED(idle);

	/* Tickful only: OSMR0 is always one tick ahead. */
}

uint32_t sys_clock_elapsed(void)
{
	/*
	 * Whole ticks that have passed since the last announcement but that
	 * the interrupt has not reported yet.
	 */
	k_spinlock_key_t key = k_spin_lock(&lock);
	uint32_t elapsed = sys_read32(OSCR) - last_tick_cycle;

	k_spin_unlock(&lock, key);

	return elapsed / CYCLES_PER_TICK;
}

uint32_t sys_clock_cycle_get_32(void)
{
	return sys_read32(OSCR);
}

static int pxa_ost_init(void)
{
	/* Disable and acknowledge all four match interrupts. */
	sys_write32(0U, OIER);
	sys_write32(0xfU, OSSR);
	sys_write32(0U, OWER);

	last_tick_cycle = sys_read32(OSCR);
	sys_write32(last_tick_cycle + CYCLES_PER_TICK, OSMR0);
	sys_write32(OIER_E0, OIER);

	IRQ_CONNECT(DT_INST_IRQN(0), DT_INST_IRQ(0, priority), pxa_ost_isr, NULL, 0);
	irq_enable(DT_INST_IRQN(0));

	return 0;
}

SYS_INIT(pxa_ost_init, PRE_KERNEL_2, CONFIG_SYSTEM_CLOCK_INIT_PRIORITY);
