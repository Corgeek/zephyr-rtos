/*
 * Copyright (c) 2026 Takayuki Goto
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/platform/hooks.h>
#include <soc.h>

/* FFUART on the IO port, through level shifters that invert TX and RX */
#define GPIO_IO_PORT_TXD 99U
#define GPIO_IO_PORT_RXD 102U
#define GPIO_IO_PORT_AF  3U

/* FFUART TX on pin 2 of the service connector under the battery cover */
#define GPIO_SERVICE_TXD    83U
#define GPIO_SERVICE_TXD_AF 2U

void board_early_init_hook(void)
{
	pxa_gpio_set_af(GPIO_IO_PORT_TXD, GPIO_IO_PORT_AF, true);
	pxa_gpio_set_af(GPIO_IO_PORT_RXD, GPIO_IO_PORT_AF, false);
	pxa_gpio_set_af(GPIO_SERVICE_TXD, GPIO_SERVICE_TXD_AF, true);
}
