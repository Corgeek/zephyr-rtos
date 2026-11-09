/*
 * Copyright (c) 2026 Takayuki Goto
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief PXA2xx I2C controller, polled bus master
 *
 * One byte at a time through IDBR: ICR[TB] starts the byte, ISR[ITE] or
 * ISR[IRF] says it has gone or arrived, and ISR[ACKNAK] holds the target's
 * answer to a transmitted byte. START and STOP ride on the byte they
 * precede or follow. A zero-length write, which a bus scan uses, is an
 * address byte with START alone followed by a master abort (ICR[MA]),
 * which sends the STOP without another byte. (START and STOP on the same
 * byte work under QEMU, but a real PXA270 left TB set and never started.)
 */

#define DT_DRV_COMPAT intel_pxa2xx_i2c

#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/sys_io.h>
#include <soc.h>

#include "i2c-priv.h"

LOG_MODULE_REGISTER(i2c_pxa, CONFIG_I2C_LOG_LEVEL);

#define IBMR 0x00U
#define IDBR 0x08U
#define ICR  0x10U
#define ISR  0x18U
#define ISAR 0x20U

#define ICR_START  BIT(0)
#define ICR_STOP   BIT(1)
#define ICR_ACKNAK BIT(2)
#define ICR_TB     BIT(3)
#define ICR_MA     BIT(4)
#define ICR_SCLE   BIT(5)
#define ICR_IUE    BIT(6)
#define ICR_UR     BIT(14)
#define ICR_FM     BIT(15)
#define ICR_GCD    BIT(7)

/* Unit and clock enabled, general call ignored, as Linux runs it */
#define ICR_BASE(data) (ICR_IUE | ICR_SCLE | ICR_GCD | (data)->speed_bits)

/* Own slave address; Linux uses 1, keeping clear of the general call */
#define SLAVE_ADDR 0x01U

#define ISR_ACKNAK BIT(1)
#define ISR_UB     BIT(2)
#define ISR_IBB    BIT(3)
#define ISR_ALD    BIT(5)
#define ISR_ITE    BIT(6)
#define ISR_IRF    BIT(7)
#define ISR_BED    BIT(10)
#define ISR_CLEAR  0x7ffU

/* Clock enables, for the warning on a stuck unit */
#define CKEN_REG 0x41300004U

/* Power manager configuration register: PWR_I2C pin enable */
#define PCFR         0x40f0001cU
#define PCFR_PI2C_EN BIT(6)

/* Ten byte times at 100 kHz, with margin */
#define BYTE_TIMEOUT_US 2000U

/* How long a STOP may keep the bus busy before the unit is reset */
#define BUS_FREE_TIMEOUT_US 1000U

/* The general purpose controller's pins on the PXA27x: SCL, SDA, AF1 */
#define I2C_SCL_PIN 117U
#define I2C_SDA_PIN 118U
#define I2C_PIN_AF  1U

struct i2c_pxa_config {
	mem_addr_t base;
	unsigned int cken_bit;
	uint32_t bitrate;
	bool pwr_i2c;
};

struct i2c_pxa_data {
	struct k_mutex lock;
	uint32_t speed_bits;
	/* Failures already reported, so a bus scan logs each kind once */
	bool told_timeout;
	bool told_lost;
};

static inline uint32_t rd(const struct i2c_pxa_config *cfg, uint32_t reg)
{
	return sys_read32(cfg->base + reg);
}

static inline void wr(const struct i2c_pxa_config *cfg, uint32_t reg, uint32_t val)
{
	sys_write32(val, cfg->base + reg);
}

static void i2c_pxa_reset(const struct device *dev)
{
	const struct i2c_pxa_config *cfg = dev->config;
	struct i2c_pxa_data *data = dev->data;

	wr(cfg, ICR, ICR_UR);
	wr(cfg, ISR, ISR_CLEAR);
	wr(cfg, ICR, 0U);
	wr(cfg, ISAR, SLAVE_ADDR);
	wr(cfg, ICR, ICR_BASE(data));
}

/* Wait for one of the bits in @p want, failing on bus errors */
static int i2c_pxa_wait(const struct i2c_pxa_config *cfg, uint32_t want)
{
	for (uint32_t us = 0U; us < BYTE_TIMEOUT_US; us++) {
		uint32_t isr = rd(cfg, ISR);

		if ((isr & ISR_ALD) != 0U) {
			wr(cfg, ISR, ISR_ALD);
			return -EAGAIN;
		}
		if ((isr & want) != 0U) {
			wr(cfg, ISR, isr & want);
			return 0;
		}
		k_busy_wait(1U);
	}

	return -ETIMEDOUT;
}

/* Send one byte; returns -EIO when the target does not acknowledge it */
static int i2c_pxa_send(const struct device *dev, uint8_t byte, uint32_t flags)
{
	const struct i2c_pxa_config *cfg = dev->config;
	struct i2c_pxa_data *data = dev->data;
	int ret;

	wr(cfg, IDBR, byte);
	wr(cfg, ICR, ICR_BASE(data) | ICR_TB | flags);

	ret = i2c_pxa_wait(cfg, ISR_ITE);
	if (ret != 0) {
		return ret;
	}

	return ((rd(cfg, ISR) & ISR_ACKNAK) != 0U) ? -EIO : 0;
}

static int i2c_pxa_recv(const struct device *dev, uint8_t *byte, uint32_t flags)
{
	const struct i2c_pxa_config *cfg = dev->config;
	struct i2c_pxa_data *data = dev->data;
	int ret;

	wr(cfg, ICR, ICR_BASE(data) | ICR_TB | flags);

	ret = i2c_pxa_wait(cfg, ISR_IRF);
	if (ret == 0) {
		*byte = (uint8_t)rd(cfg, IDBR);
	}

	return ret;
}

/* Generate a STOP after a failed byte, so the bus is left idle */
static void i2c_pxa_abort(const struct device *dev)
{
	const struct i2c_pxa_config *cfg = dev->config;
	struct i2c_pxa_data *data = dev->data;

	wr(cfg, ICR, ICR_BASE(data) | ICR_MA);
	for (uint32_t us = 0U; us < BYTE_TIMEOUT_US; us++) {
		if ((rd(cfg, ISR) & ISR_IBB) == 0U) {
			break;
		}
		k_busy_wait(1U);
	}
	wr(cfg, ICR, ICR_BASE(data));
}

static int i2c_pxa_msg(const struct device *dev, struct i2c_msg *msg, uint16_t addr, bool first)
{
	bool read = (msg->flags & I2C_MSG_RW_MASK) == I2C_MSG_READ;
	bool stop = (msg->flags & I2C_MSG_STOP) != 0U;
	int ret = 0;

	/* An address byte opens the first message and every restart */
	if (first || (msg->flags & I2C_MSG_RESTART) != 0U) {
		ret = i2c_pxa_send(dev, (uint8_t)((addr << 1) | (read ? 1U : 0U)), ICR_START);
		if (ret != 0) {
			return ret;
		}

		if (msg->len == 0U && stop) {
			i2c_pxa_abort(dev);
			return 0;
		}
	}

	for (uint32_t i = 0U; i < msg->len; i++) {
		bool last = (i + 1U == msg->len);
		uint32_t flags = (last && stop) ? ICR_STOP : 0U;

		if (read) {
			/* NAK the last byte of a read that ends here */
			if (last && stop) {
				flags |= ICR_ACKNAK;
			}
			ret = i2c_pxa_recv(dev, &msg->buf[i], flags);
		} else {
			ret = i2c_pxa_send(dev, msg->buf[i], flags);
		}

		if (ret != 0) {
			return ret;
		}
	}

	return 0;
}

static int i2c_pxa_transfer(const struct device *dev, struct i2c_msg *msgs, uint8_t num_msgs,
			    uint16_t addr)
{
	const struct i2c_pxa_config *cfg = dev->config;
	struct i2c_pxa_data *data = dev->data;
	int ret = 0;

	if (num_msgs == 0U) {
		return 0;
	}

	k_mutex_lock(&data->lock, K_FOREVER);

	/* The STOP that ended the previous transfer may still be going out */
	for (uint32_t us = 0U; (rd(cfg, ISR) & (ISR_IBB | ISR_UB)) != 0U; us++) {
		if (us == BUS_FREE_TIMEOUT_US) {
			LOG_WRN("bus busy, resetting the controller");
			i2c_pxa_reset(dev);
			break;
		}
		k_busy_wait(1U);
	}

	/*
	 * Start from clean status: a NAK leaves BED set alongside ACKNAK on
	 * real hardware, and nothing else clears it.
	 */
	wr(cfg, ISR, ISR_CLEAR);

	for (uint8_t i = 0U; i < num_msgs; i++) {
		if ((msgs[i].flags & I2C_MSG_ADDR_10_BITS) != 0U) {
			ret = -ENOTSUP;
			break;
		}

		ret = i2c_pxa_msg(dev, &msgs[i], addr, i == 0U);
		if (ret != 0) {
			break;
		}
	}

	if (ret != 0 && ret != -EIO) {
		/* A NAK is an ordinary answer; anything else means the unit is stuck */
		bool *told = (ret == -ETIMEDOUT) ? &data->told_timeout : &data->told_lost;

		if (!*told) {
			*told = true;
			LOG_WRN("%s at addr 0x%02x: ICR 0x%04x ISR 0x%04x IBMR 0x%x CKEN 0x%08x",
				(ret == -ETIMEDOUT) ? "timeout" : "lost arbitration", addr,
				rd(cfg, ICR), rd(cfg, ISR), rd(cfg, IBMR), sys_read32(CKEN_REG));
		}
	}

	if (ret != 0) {

		i2c_pxa_abort(dev);
		if (ret != -EIO) {
			/* Anything but a plain NAK may have left the unit confused */
			i2c_pxa_reset(dev);
		}
	}

	k_mutex_unlock(&data->lock);

	return ret;
}

static int i2c_pxa_configure(const struct device *dev, uint32_t dev_config)
{
	struct i2c_pxa_data *data = dev->data;

	if ((dev_config & I2C_MODE_CONTROLLER) == 0U || (dev_config & I2C_ADDR_10_BITS) != 0U) {
		return -ENOTSUP;
	}

	switch (I2C_SPEED_GET(dev_config)) {
	case I2C_SPEED_STANDARD:
		data->speed_bits = 0U;
		break;
	case I2C_SPEED_FAST:
		data->speed_bits = ICR_FM;
		break;
	default:
		return -ENOTSUP;
	}

	k_mutex_lock(&data->lock, K_FOREVER);
	i2c_pxa_reset(dev);
	k_mutex_unlock(&data->lock);

	return 0;
}

static DEVICE_API(i2c, i2c_pxa_api) = {
	.configure = i2c_pxa_configure,
	.transfer = i2c_pxa_transfer,
#ifdef CONFIG_I2C_RTIO
	.iodev_submit = i2c_iodev_submit_fallback,
#endif
};

static int i2c_pxa_init(const struct device *dev)
{
	const struct i2c_pxa_config *cfg = dev->config;
	struct i2c_pxa_data *data = dev->data;

	k_mutex_init(&data->lock);
	pxa_clock_enable(cfg->cken_bit);

	if (cfg->pwr_i2c) {
		sys_write32(sys_read32(PCFR) | PCFR_PI2C_EN, PCFR);
	} else {
		/* As Linux does; whatever ran before may have left them as GPIOs */
		pxa_gpio_set_af(I2C_SCL_PIN, I2C_PIN_AF, false);
		pxa_gpio_set_af(I2C_SDA_PIN, I2C_PIN_AF, false);
	}

	return i2c_pxa_configure(dev, I2C_MODE_CONTROLLER | i2c_map_dt_bitrate(cfg->bitrate));
}

#define I2C_PXA_INIT(n)                                                                            \
	static const struct i2c_pxa_config i2c_pxa_config_##n = {                                  \
		.base = DT_INST_REG_ADDR(n),                                                       \
		.cken_bit = DT_INST_PROP(n, cken_bit),                                             \
		.bitrate = DT_INST_PROP(n, clock_frequency),                                       \
		.pwr_i2c = DT_INST_PROP(n, pwr_i2c),                                               \
	};                                                                                         \
	static struct i2c_pxa_data i2c_pxa_data_##n;                                               \
	I2C_DEVICE_DT_INST_DEFINE(n, i2c_pxa_init, NULL, &i2c_pxa_data_##n, &i2c_pxa_config_##n,   \
				  POST_KERNEL, CONFIG_I2C_INIT_PRIORITY, &i2c_pxa_api);

DT_INST_FOREACH_STATUS_OKAY(I2C_PXA_INIT)
