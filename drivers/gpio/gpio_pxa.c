/*
 * Copyright (c) 2026 Takayuki Goto
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Intel/Marvell PXA2xx GPIO, one instance per 32-line bank
 *
 * Levels are set and cleared through the write-one GPSR/GPCR pair, so only
 * the direction register needs a read-modify-write. Interrupts are not
 * supported yet.
 */

#define DT_DRV_COMPAT intel_pxa2xx_gpio

#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/gpio/gpio_utils.h>
#include <zephyr/irq.h>
#include <zephyr/sys/sys_io.h>
#include <soc.h>

#define GPLR 0x00U
#define GPDR 0x0cU
#define GPSR 0x18U
#define GPCR 0x24U

/* GPLR3 is 0x100 above GPLR0; banks 0-2 are four bytes apart */
#define GPLR3_OFFSET 0x100U

struct gpio_pxa_config {
	/* gpio_driver_config needs to be first */
	struct gpio_driver_config common;
	mem_addr_t base;
	unsigned int first_pin;
};

struct gpio_pxa_data {
	/* gpio_driver_data needs to be first */
	struct gpio_driver_data common;
};

static int gpio_pxa_pin_configure(const struct device *dev, gpio_pin_t pin, gpio_flags_t flags)
{
	const struct gpio_pxa_config *cfg = dev->config;
	bool output = (flags & GPIO_OUTPUT) != 0U;

	if ((flags & (GPIO_PULL_UP | GPIO_PULL_DOWN | GPIO_SINGLE_ENDED)) != 0U) {
		return -ENOTSUP;
	}

	/* Latch the level before the pin starts driving it */
	if ((flags & GPIO_OUTPUT_INIT_HIGH) != 0U) {
		sys_write32(BIT(pin), cfg->base + GPSR);
	} else if ((flags & GPIO_OUTPUT_INIT_LOW) != 0U) {
		sys_write32(BIT(pin), cfg->base + GPCR);
	}

	pxa_gpio_set_af(cfg->first_pin + pin, 0U, output);

	return 0;
}

static int gpio_pxa_port_get_raw(const struct device *dev, gpio_port_value_t *value)
{
	const struct gpio_pxa_config *cfg = dev->config;

	*value = sys_read32(cfg->base + GPLR);

	return 0;
}

static int gpio_pxa_port_set_masked_raw(const struct device *dev, gpio_port_pins_t mask,
					gpio_port_value_t value)
{
	const struct gpio_pxa_config *cfg = dev->config;

	sys_write32(mask & value, cfg->base + GPSR);
	sys_write32(mask & ~value, cfg->base + GPCR);

	return 0;
}

static int gpio_pxa_port_set_bits_raw(const struct device *dev, gpio_port_pins_t pins)
{
	const struct gpio_pxa_config *cfg = dev->config;

	sys_write32(pins, cfg->base + GPSR);

	return 0;
}

static int gpio_pxa_port_clear_bits_raw(const struct device *dev, gpio_port_pins_t pins)
{
	const struct gpio_pxa_config *cfg = dev->config;

	sys_write32(pins, cfg->base + GPCR);

	return 0;
}

static int gpio_pxa_port_toggle_bits(const struct device *dev, gpio_port_pins_t pins)
{
	const struct gpio_pxa_config *cfg = dev->config;
	unsigned int key = irq_lock();
	uint32_t level = sys_read32(cfg->base + GPLR);

	sys_write32(pins & ~level, cfg->base + GPSR);
	sys_write32(pins & level, cfg->base + GPCR);
	irq_unlock(key);

	return 0;
}

static DEVICE_API(gpio, gpio_pxa_api) = {
	.pin_configure = gpio_pxa_pin_configure,
	.port_get_raw = gpio_pxa_port_get_raw,
	.port_set_masked_raw = gpio_pxa_port_set_masked_raw,
	.port_set_bits_raw = gpio_pxa_port_set_bits_raw,
	.port_clear_bits_raw = gpio_pxa_port_clear_bits_raw,
	.port_toggle_bits = gpio_pxa_port_toggle_bits,
};

/* Bank number from the GPLR offset within the controller */
#define GPIO_PXA_OFFSET(n) (DT_INST_REG_ADDR(n) & 0xfffU)
#define GPIO_PXA_BANK(n)   ((GPIO_PXA_OFFSET(n) >= GPLR3_OFFSET) ? 3U : (GPIO_PXA_OFFSET(n) / 4U))

#define GPIO_PXA_INIT(n)                                                                           \
	static const struct gpio_pxa_config gpio_pxa_config_##n = {                                \
		.common = {.port_pin_mask = GPIO_PORT_PIN_MASK_FROM_DT_INST(n)},                   \
		.base = DT_INST_REG_ADDR(n),                                                       \
		.first_pin = GPIO_PXA_BANK(n) * 32U,                                               \
	};                                                                                         \
	static struct gpio_pxa_data gpio_pxa_data_##n;                                             \
	DEVICE_DT_INST_DEFINE(n, NULL, NULL, &gpio_pxa_data_##n, &gpio_pxa_config_##n,             \
			      PRE_KERNEL_1, CONFIG_GPIO_INIT_PRIORITY, &gpio_pxa_api);

DT_INST_FOREACH_STATUS_OKAY(GPIO_PXA_INIT)
