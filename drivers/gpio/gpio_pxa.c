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
 * the direction and edge-enable registers need a read-modify-write.
 *
 * Interrupts are edge only: GRER and GFER pick the edges, and GEDR latches
 * them until a one is written back. The interrupt controller has three
 * sources for all of them: one each for GPIO0 and GPIO1, and a third shared
 * by every other line in every bank. Each bank names the sources of its own
 * lines, the shared one last. Because the shared source cannot be connected
 * once per bank without shared interrupt support, one ISR serves every
 * source and every bank: it is connected to the shared source through the
 * first instance, and to the dedicated ones through the bank that owns them.
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
#define GRER 0x30U
#define GFER 0x3cU
#define GEDR 0x48U

/* GPLR3 is 0x100 above GPLR0; banks 0-2 are four bytes apart */
#define GPLR3_OFFSET 0x100U

struct gpio_pxa_config {
	/* gpio_driver_config needs to be first */
	struct gpio_driver_config common;
	mem_addr_t base;
	unsigned int first_pin;
	void (*irq_config)(void);
};

struct gpio_pxa_data {
	/* gpio_driver_data needs to be first */
	struct gpio_driver_data common;
	sys_slist_t callbacks;
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

static void gpio_pxa_rmw(mem_addr_t reg, uint32_t clear, uint32_t set)
{
	unsigned int key = irq_lock();

	sys_write32((sys_read32(reg) & ~clear) | set, reg);
	irq_unlock(key);
}

static int gpio_pxa_pin_interrupt_configure(const struct device *dev, gpio_pin_t pin,
					    enum gpio_int_mode mode, enum gpio_int_trig trig)
{
	const struct gpio_pxa_config *cfg = dev->config;
	uint32_t bit = BIT(pin);
	bool rising = false;
	bool falling = false;

	if (mode == GPIO_INT_MODE_LEVEL) {
		return -ENOTSUP;
	}

	if (mode != GPIO_INT_MODE_DISABLED) {
		rising = (trig & GPIO_INT_TRIG_HIGH) != 0U;
		falling = (trig & GPIO_INT_TRIG_LOW) != 0U;
	}

	/* Stop detecting before dropping an edge latched under the old setting */
	gpio_pxa_rmw(cfg->base + GRER, bit, 0U);
	gpio_pxa_rmw(cfg->base + GFER, bit, 0U);
	sys_write32(bit, cfg->base + GEDR);

	gpio_pxa_rmw(cfg->base + GRER, 0U, rising ? bit : 0U);
	gpio_pxa_rmw(cfg->base + GFER, 0U, falling ? bit : 0U);

	return 0;
}

static int gpio_pxa_manage_callback(const struct device *dev, struct gpio_callback *callback,
				    bool set)
{
	struct gpio_pxa_data *data = dev->data;

	return gpio_manage_callback(&data->callbacks, callback, set);
}

/* Latched edges on the lines that have detection enabled */
static uint32_t gpio_pxa_pending(const struct gpio_pxa_config *cfg)
{
	uint32_t enabled = sys_read32(cfg->base + GRER) | sys_read32(cfg->base + GFER);

	return sys_read32(cfg->base + GEDR) & enabled;
}

static uint32_t gpio_pxa_get_pending_int(const struct device *dev)
{
	return gpio_pxa_pending(dev->config);
}

static DEVICE_API(gpio, gpio_pxa_api) = {
	.pin_configure = gpio_pxa_pin_configure,
	.port_get_raw = gpio_pxa_port_get_raw,
	.port_set_masked_raw = gpio_pxa_port_set_masked_raw,
	.port_set_bits_raw = gpio_pxa_port_set_bits_raw,
	.port_clear_bits_raw = gpio_pxa_port_clear_bits_raw,
	.port_toggle_bits = gpio_pxa_port_toggle_bits,
	.pin_interrupt_configure = gpio_pxa_pin_interrupt_configure,
	.manage_callback = gpio_pxa_manage_callback,
	.get_pending_int = gpio_pxa_get_pending_int,
};

#define GPIO_PXA_DEV(n) DEVICE_DT_INST_GET(n),

static const struct device *const gpio_pxa_devs[] = {DT_INST_FOREACH_STATUS_OKAY(GPIO_PXA_DEV)};

static void gpio_pxa_isr(const void *arg)
{
	ARG_UNUSED(arg);

	for (size_t i = 0; i < ARRAY_SIZE(gpio_pxa_devs); i++) {
		const struct device *dev = gpio_pxa_devs[i];
		const struct gpio_pxa_config *cfg = dev->config;
		struct gpio_pxa_data *data = dev->data;
		uint32_t pending = gpio_pxa_pending(cfg);

		if (pending == 0U) {
			continue;
		}

		/* Clear first, so that an edge during the callbacks latches again */
		sys_write32(pending, cfg->base + GEDR);
		gpio_fire_callbacks(&data->callbacks, dev, pending);
	}
}

static int gpio_pxa_init(const struct device *dev)
{
	const struct gpio_pxa_config *cfg = dev->config;

	/*
	 * Whatever ran before (Linux, ahead of a kexec) may have left edge
	 * detection on; a stale latched edge would fire as soon as the shared
	 * source is unmasked.
	 */
	sys_write32(0U, cfg->base + GRER);
	sys_write32(0U, cfg->base + GFER);
	sys_write32(UINT32_MAX, cfg->base + GEDR);

	cfg->irq_config();

	return 0;
}

/* Bank number from the GPLR offset within the controller */
#define GPIO_PXA_OFFSET(n) (DT_INST_REG_ADDR(n) & 0xfffU)
#define GPIO_PXA_BANK(n)   ((GPIO_PXA_OFFSET(n) >= GPLR3_OFFSET) ? 3U : (GPIO_PXA_OFFSET(n) / 4U))

/* The shared source, last in every bank's list */
#define GPIO_PXA_SHARED_IDX(n) UTIL_DEC(DT_INST_NUM_IRQS(n))
#define GPIO_PXA_SHARED_IRQ(n) DT_INST_IRQ_BY_IDX(n, GPIO_PXA_SHARED_IDX(n), irq)

#define GPIO_PXA_SAME_SHARED_IRQ(n)                                                                \
	BUILD_ASSERT(GPIO_PXA_SHARED_IRQ(n) == GPIO_PXA_SHARED_IRQ(0),                             \
		     "every GPIO bank must list the same shared interrupt last");

DT_INST_FOREACH_STATUS_OKAY(GPIO_PXA_SAME_SHARED_IRQ)

#define GPIO_PXA_IRQ_CONNECT(idx, n)                                                               \
	IRQ_CONNECT(DT_INST_IRQ_BY_IDX(n, idx, irq), DT_INST_IRQ_BY_IDX(n, idx, priority),         \
		    gpio_pxa_isr, NULL, 0);                                                        \
	irq_enable(DT_INST_IRQ_BY_IDX(n, idx, irq));

/*
 * Bank n connects its dedicated sources, every entry but the last; the first
 * instance also connects the shared one.
 */
#define GPIO_PXA_IRQ_CONFIG(n)                                                                     \
	static void gpio_pxa_irq_config_##n(void)                                                  \
	{                                                                                          \
		LISTIFY(GPIO_PXA_SHARED_IDX(n), GPIO_PXA_IRQ_CONNECT, (), n)                       \
		IF_ENABLED(IS_EQ(n, 0), (GPIO_PXA_IRQ_CONNECT(GPIO_PXA_SHARED_IDX(n), n)))         \
	}

#define GPIO_PXA_INIT(n)                                                                           \
	GPIO_PXA_IRQ_CONFIG(n)                                                                     \
	static const struct gpio_pxa_config gpio_pxa_config_##n = {                                \
		.common = {.port_pin_mask = GPIO_PORT_PIN_MASK_FROM_DT_INST(n)},                   \
		.base = DT_INST_REG_ADDR(n),                                                       \
		.first_pin = GPIO_PXA_BANK(n) * 32U,                                               \
		.irq_config = gpio_pxa_irq_config_##n,                                             \
	};                                                                                         \
	static struct gpio_pxa_data gpio_pxa_data_##n;                                             \
	DEVICE_DT_INST_DEFINE(n, gpio_pxa_init, NULL, &gpio_pxa_data_##n, &gpio_pxa_config_##n,    \
			      PRE_KERNEL_1, CONFIG_GPIO_INIT_PRIORITY, &gpio_pxa_api);

DT_INST_FOREACH_STATUS_OKAY(GPIO_PXA_INIT)
