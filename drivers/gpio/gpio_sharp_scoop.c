/*
 * Copyright (c) 2026 Takayuki Goto
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief GPIO port of the Sharp SCOOP companion chip
 *
 * The registers are 16 bits wide on a 16-bit bus and must be accessed as
 * such. GPCR sets the direction (1 = output), GPWR is the output latch and
 * GPRR reads the pins. Pin n is bit n + 1.
 *
 * Nothing is reset at init: whatever ran before Zephyr (the boot loader,
 * or Linux before a kexec) leaves the lines it owns, such as the PCMCIA and
 * charger controls, in a working state, and only lines a Zephyr driver
 * configures are touched.
 */

#define DT_DRV_COMPAT sharp_scoop

#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/gpio/gpio_utils.h>
#include <zephyr/irq.h>
#include <zephyr/sys/sys_io.h>

#define SCOOP_GPCR 0x20U
#define SCOOP_GPWR 0x24U
#define SCOOP_GPRR 0x28U

#define PIN_BIT(pin) BIT((pin) + 1U)

struct gpio_scoop_config {
	/* gpio_driver_config needs to be first */
	struct gpio_driver_config common;
	mem_addr_t base;
};

struct gpio_scoop_data {
	/* gpio_driver_data needs to be first */
	struct gpio_driver_data common;
};

/* Port values are in pin order; the registers are one bit up */
static inline uint16_t to_reg(const struct device *dev, gpio_port_value_t pins)
{
	const struct gpio_scoop_config *cfg = dev->config;

	return (uint16_t)((pins & cfg->common.port_pin_mask) << 1);
}

static void gpio_scoop_update(const struct device *dev, uint16_t clear, uint16_t set,
			      uint16_t toggle)
{
	const struct gpio_scoop_config *cfg = dev->config;
	unsigned int key = irq_lock();
	uint16_t val = sys_read16(cfg->base + SCOOP_GPWR);

	sys_write16(((val & ~clear) | set) ^ toggle, cfg->base + SCOOP_GPWR);
	irq_unlock(key);
}

static void gpio_scoop_init_level(const struct device *dev, uint16_t bit, gpio_flags_t flags)
{
	if ((flags & GPIO_OUTPUT_INIT_HIGH) != 0U) {
		gpio_scoop_update(dev, 0U, bit, 0U);
	} else if ((flags & GPIO_OUTPUT_INIT_LOW) != 0U) {
		gpio_scoop_update(dev, bit, 0U, 0U);
	}
}

static int gpio_scoop_pin_configure(const struct device *dev, gpio_pin_t pin, gpio_flags_t flags)
{
	const struct gpio_scoop_config *cfg = dev->config;
	bool output = (flags & GPIO_OUTPUT) != 0U;
	uint16_t bit = PIN_BIT(pin);
	unsigned int key;
	uint16_t dir;

	if ((flags & (GPIO_PULL_UP | GPIO_PULL_DOWN | GPIO_SINGLE_ENDED)) != 0U) {
		return -ENOTSUP;
	}

	/* Latch first, so a new output never drives a stale level */
	if (output) {
		gpio_scoop_init_level(dev, bit, flags);
	}

	key = irq_lock();
	dir = sys_read16(cfg->base + SCOOP_GPCR);
	dir = output ? (dir | bit) : (dir & ~bit);
	sys_write16(dir, cfg->base + SCOOP_GPCR);
	irq_unlock(key);

	/* And again: QEMU's SCOOP drops GPWR bits of lines that are inputs */
	if (output) {
		gpio_scoop_init_level(dev, bit, flags);
	}

	return 0;
}

static int gpio_scoop_port_get_raw(const struct device *dev, gpio_port_value_t *value)
{
	const struct gpio_scoop_config *cfg = dev->config;

	*value = (sys_read16(cfg->base + SCOOP_GPRR) >> 1) & cfg->common.port_pin_mask;

	return 0;
}

static int gpio_scoop_port_set_masked_raw(const struct device *dev, gpio_port_pins_t mask,
					  gpio_port_value_t value)
{
	gpio_scoop_update(dev, to_reg(dev, mask), to_reg(dev, mask & value), 0U);

	return 0;
}

static int gpio_scoop_port_set_bits_raw(const struct device *dev, gpio_port_pins_t pins)
{
	gpio_scoop_update(dev, 0U, to_reg(dev, pins), 0U);

	return 0;
}

static int gpio_scoop_port_clear_bits_raw(const struct device *dev, gpio_port_pins_t pins)
{
	gpio_scoop_update(dev, to_reg(dev, pins), 0U, 0U);

	return 0;
}

static int gpio_scoop_port_toggle_bits(const struct device *dev, gpio_port_pins_t pins)
{
	gpio_scoop_update(dev, 0U, 0U, to_reg(dev, pins));

	return 0;
}

static DEVICE_API(gpio, gpio_scoop_api) = {
	.pin_configure = gpio_scoop_pin_configure,
	.port_get_raw = gpio_scoop_port_get_raw,
	.port_set_masked_raw = gpio_scoop_port_set_masked_raw,
	.port_set_bits_raw = gpio_scoop_port_set_bits_raw,
	.port_clear_bits_raw = gpio_scoop_port_clear_bits_raw,
	.port_toggle_bits = gpio_scoop_port_toggle_bits,
};

#define GPIO_SCOOP_INIT(n)                                                                         \
	BUILD_ASSERT(DT_INST_PROP(n, ngpios) <= 15, "SCOOP has at most 15 lines");                 \
	static const struct gpio_scoop_config gpio_scoop_config_##n = {                            \
		.common = {.port_pin_mask = GPIO_PORT_PIN_MASK_FROM_DT_INST(n)},                   \
		.base = DT_INST_REG_ADDR(n),                                                       \
	};                                                                                         \
	static struct gpio_scoop_data gpio_scoop_data_##n;                                         \
	DEVICE_DT_INST_DEFINE(n, NULL, NULL, &gpio_scoop_data_##n, &gpio_scoop_config_##n,         \
			      PRE_KERNEL_1, CONFIG_GPIO_INIT_PRIORITY, &gpio_scoop_api);

DT_INST_FOREACH_STATUS_OKAY(GPIO_SCOOP_INIT)
