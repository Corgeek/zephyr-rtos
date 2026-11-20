/*
 * Copyright (c) 2026 Takayuki Goto
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/platform/hooks.h>
#include <zephyr/sys/poweroff.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/sys_io.h>
#include <soc.h>

/* FFUART on the IO port, through level shifters that invert TX and RX */
#define GPIO_IO_PORT_TXD 99U
#define GPIO_IO_PORT_RXD 102U
#define GPIO_IO_PORT_AF  3U

/* FFUART TX on pin 2 of the service connector under the battery cover */
#define GPIO_SERVICE_TXD    83U
#define GPIO_SERVICE_TXD_AF 2U

#if DT_NODE_HAS_STATUS_OKAY(DT_NODELABEL(usbh))
/* USB host: only port 2 is wired, as in Linux's spitz.c */
#define UHC_BASE      DT_REG_ADDR(DT_NODELABEL(usbh))
#define UHCRHDA       (UHC_BASE + 0x48U)
#define UHCRHDA_NOCP  BIT(12)
#define UHCRHDA_NPS   BIT(9)
#define UHCHR         (UHC_BASE + 0x64U)
#define UHCHR_SSEP3   BIT(11)
#define UHCHR_SSEP2   BIT(10)
#define UHCHR_SSEP1   BIT(9)
#define UP2OCR        0x40600020U
#define UP2OCR_HXOE   BIT(17)
#define UP2OCR_HXS    BIT(16)
#define UP2OCR_DMPDE  BIT(3)
#define UP2OCR_DPPDE  BIT(2)
#define GPIO_USB_HOST 37U

/*
 * Ports out of sleep standby, no power switching and no over-current
 * protection, port 2 on the host transceiver with its pull-downs, and the
 * 5 V supply of the connector on.
 */
static void zaurus_usbh_init(void)
{
	sys_write32(sys_read32(UHCHR) & ~(UHCHR_SSEP1 | UHCHR_SSEP2 | UHCHR_SSEP3), UHCHR);
	sys_write32(sys_read32(UHCRHDA) | UHCRHDA_NPS | UHCRHDA_NOCP, UHCRHDA);
	sys_write32(UP2OCR_HXS | UP2OCR_HXOE | UP2OCR_DPPDE | UP2OCR_DMPDE, UP2OCR);

	pxa_gpio_set(GPIO_USB_HOST, true);
	pxa_gpio_set_af(GPIO_USB_HOST, 0U, true);
}
#endif

void board_early_init_hook(void)
{
	pxa_gpio_set_af(GPIO_IO_PORT_TXD, GPIO_IO_PORT_AF, true);
	pxa_gpio_set_af(GPIO_IO_PORT_RXD, GPIO_IO_PORT_AF, false);
	pxa_gpio_set_af(GPIO_SERVICE_TXD, GPIO_SERVICE_TXD_AF, true);

#if DT_NODE_HAS_STATUS_OKAY(DT_NODELABEL(usbh))
	zaurus_usbh_init();
#endif
}

/*
 * Power off and reboot, as Linux's spitz.c: a pulse on GPIO89 makes the reset
 * generator reset the machine, and the boot loader then powers it off and
 * waits for the power key, unless MSC0 tells it to boot again.
 */
#define GPIO_ON_RESET 89U

/* The boot loader leaves 0x7ff0 in MSC0[31:16]; 0x7ee0 asks it to reboot */
#define MSC0             0x48000008U
#define MSC0_MAGIC_MASK  0xffff0000U
#define MSC0_BOOT_LOADER 0x7ff00000U
#define MSC0_REBOOT      0x7ee00000U

static FUNC_NORETURN void zaurus_gpio_reset(void)
{
	pxa_reset_status_clear();

	pxa_gpio_set(GPIO_ON_RESET, false);
	pxa_gpio_set_af(GPIO_ON_RESET, 0U, true);
	k_busy_wait(2000U);
	pxa_gpio_set(GPIO_ON_RESET, true);
	k_busy_wait(2000U);
	pxa_gpio_set(GPIO_ON_RESET, false);
	k_busy_wait(10000U);

	/* The reset generator did not reset the machine */
	pxa_watchdog_reset();
}

void z_sys_poweroff(void)
{
	zaurus_gpio_reset();
}

void sys_arch_reboot(int type)
{
	uint32_t msc0;

	ARG_UNUSED(type);

	(void)irq_lock();

	msc0 = sys_read32(MSC0);
	if ((msc0 & MSC0_MAGIC_MASK) == MSC0_BOOT_LOADER) {
		sys_write32((msc0 & ~MSC0_MAGIC_MASK) | MSC0_REBOOT, MSC0);
	}

	zaurus_gpio_reset();
}
