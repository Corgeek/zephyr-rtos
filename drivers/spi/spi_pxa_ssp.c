/*
 * Copyright (c) 2026 Takayuki Goto
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief PXA2xx SSP port as a polled SPI controller
 *
 * Motorola SPI frame format, one word in flight at a time. Chip selects are
 * GPIOs: the port's own SFRM pin toggles per word, which suits none of the
 * Zaurus peripherals.
 */

#define DT_DRV_COMPAT intel_pxa2xx_ssp

#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/sys_io.h>
#include <soc.h>

LOG_MODULE_REGISTER(spi_pxa_ssp, CONFIG_SPI_LOG_LEVEL);

#include "spi_context.h"

#define SSCR0 0x00U
#define SSCR1 0x04U
#define SSSR  0x08U
#define SSDR  0x10U

#define SSCR0_DSS_MASK  0xfU
#define SSCR0_SSE       BIT(7)
#define SSCR0_SCR_SHIFT 8U
#define SSCR0_SCR_MAX   0xfffU

#define SSCR1_SPO BIT(3)
#define SSCR1_SPH BIT(4)

#define SSSR_TNF BIT(2)
#define SSSR_RNE BIT(3)
#define SSSR_BSY BIT(4)

#define SSP_CLOCK_HZ 13000000U

/* Generous: a word at the slowest bit clock takes about 5 ms */
#define WAIT_LOOPS 1000000U

struct spi_pxa_ssp_config {
	mem_addr_t base;
	unsigned int cken_bit;
};

struct spi_pxa_ssp_data {
	struct spi_context ctx;
	uint8_t dfs;
};

static int spi_pxa_ssp_configure(const struct device *dev, const struct spi_config *config)
{
	const struct spi_pxa_ssp_config *cfg = dev->config;
	struct spi_pxa_ssp_data *data = dev->data;
	uint32_t bits = SPI_WORD_SIZE_GET(config->operation);
	uint32_t mode = SPI_MODE_GET(config->operation);
	uint32_t scr;
	uint32_t sscr1 = 0U;

	if (spi_context_configured(&data->ctx, config)) {
		return 0;
	}

	if ((config->operation & SPI_OP_MODE_PERIPHERAL) != 0U ||
	    (mode & (SPI_MODE_LOOP | SPI_TRANSFER_LSB)) != 0U ||
	    (config->operation & SPI_LINES_MASK) != SPI_LINES_SINGLE) {
		return -ENOTSUP;
	}

	if (bits < 4U || bits > 16U || config->frequency == 0U) {
		return -EINVAL;
	}

	/* Round the divider up, so the clock never exceeds the request */
	scr = DIV_ROUND_UP(SSP_CLOCK_HZ, config->frequency) - 1U;
	scr = MIN(scr, SSCR0_SCR_MAX);

	if ((mode & SPI_MODE_CPOL) != 0U) {
		sscr1 |= SSCR1_SPO;
	}
	if ((mode & SPI_MODE_CPHA) != 0U) {
		sscr1 |= SSCR1_SPH;
	}

	/* The port must be disabled while it is reconfigured */
	sys_write32(0U, cfg->base + SSCR0);
	sys_write32(sscr1, cfg->base + SSCR1);
	sys_write32((scr << SSCR0_SCR_SHIFT) | (bits - 1U), cfg->base + SSCR0);
	sys_write32((scr << SSCR0_SCR_SHIFT) | (bits - 1U) | SSCR0_SSE, cfg->base + SSCR0);

	data->dfs = (bits > 8U) ? 2U : 1U;
	data->ctx.config = config;

	return 0;
}

static int spi_pxa_ssp_wait(const struct spi_pxa_ssp_config *cfg, uint32_t mask, uint32_t want)
{
	for (uint32_t i = 0U; i < WAIT_LOOPS; i++) {
		if ((sys_read32(cfg->base + SSSR) & mask) == want) {
			return 0;
		}
	}

	return -ETIMEDOUT;
}

static int spi_pxa_ssp_xfer_word(const struct device *dev)
{
	const struct spi_pxa_ssp_config *cfg = dev->config;
	struct spi_pxa_ssp_data *data = dev->data;
	struct spi_context *ctx = &data->ctx;
	uint32_t tx = 0U;
	uint32_t rx;

	if (spi_context_tx_buf_on(ctx)) {
		tx = (data->dfs == 2U) ? sys_get_le16(ctx->tx_buf) : *ctx->tx_buf;
	}

	if (spi_pxa_ssp_wait(cfg, SSSR_TNF, SSSR_TNF) != 0) {
		return -ETIMEDOUT;
	}
	sys_write32(tx, cfg->base + SSDR);

	/* Exactly one word goes out, so exactly one comes back */
	if (spi_pxa_ssp_wait(cfg, SSSR_RNE, SSSR_RNE) != 0) {
		return -ETIMEDOUT;
	}
	rx = sys_read32(cfg->base + SSDR);

	if (spi_context_rx_buf_on(ctx)) {
		if (data->dfs == 2U) {
			sys_put_le16((uint16_t)rx, ctx->rx_buf);
		} else {
			*ctx->rx_buf = (uint8_t)rx;
		}
	}

	spi_context_update_tx(ctx, data->dfs, 1);
	spi_context_update_rx(ctx, data->dfs, 1);

	return 0;
}

static int spi_pxa_ssp_transceive(const struct device *dev, const struct spi_config *config,
				  const struct spi_buf_set *tx_bufs,
				  const struct spi_buf_set *rx_bufs)
{
	const struct spi_pxa_ssp_config *cfg = dev->config;
	struct spi_pxa_ssp_data *data = dev->data;
	struct spi_context *ctx = &data->ctx;
	int ret;

	spi_context_lock(ctx, false, NULL, NULL, config);

	ret = spi_pxa_ssp_configure(dev, config);
	if (ret != 0) {
		goto out;
	}

	/* Drop anything a previous user left in the receive FIFO */
	while ((sys_read32(cfg->base + SSSR) & SSSR_RNE) != 0U) {
		(void)sys_read32(cfg->base + SSDR);
	}

	spi_context_buffers_setup(ctx, tx_bufs, rx_bufs, data->dfs);
	spi_context_cs_control(ctx, true);

	while (spi_context_tx_on(ctx) || spi_context_rx_on(ctx)) {
		ret = spi_pxa_ssp_xfer_word(dev);
		if (ret != 0) {
			LOG_ERR("transfer timed out");
			break;
		}
	}

	/* Let the last bit leave the shifter before the chip select goes */
	(void)spi_pxa_ssp_wait(cfg, SSSR_BSY, 0U);

	if (ret != 0 || (config->operation & SPI_HOLD_ON_CS) == 0U) {
		spi_context_cs_control(ctx, false);
	}

out:
	spi_context_release(ctx, ret);

	return ret;
}

static int spi_pxa_ssp_release(const struct device *dev, const struct spi_config *config)
{
	struct spi_pxa_ssp_data *data = dev->data;

	ARG_UNUSED(config);

	spi_context_unlock_unconditionally(&data->ctx);

	return 0;
}

static DEVICE_API(spi, spi_pxa_ssp_api) = {
	.transceive = spi_pxa_ssp_transceive,
	.release = spi_pxa_ssp_release,
#ifdef CONFIG_SPI_RTIO
	.iodev_submit = spi_rtio_iodev_default_submit,
#endif
};

static int spi_pxa_ssp_init(const struct device *dev)
{
	const struct spi_pxa_ssp_config *cfg = dev->config;
	struct spi_pxa_ssp_data *data = dev->data;
	int ret;

	pxa_clock_enable(cfg->cken_bit);
	sys_write32(0U, cfg->base + SSCR0);

	ret = spi_context_cs_configure_all(&data->ctx);
	if (ret != 0) {
		return ret;
	}

	spi_context_unlock_unconditionally(&data->ctx);

	return 0;
}

#define SPI_PXA_SSP_INIT(n)                                                                        \
	static const struct spi_pxa_ssp_config spi_pxa_ssp_config_##n = {                          \
		.base = DT_INST_REG_ADDR(n),                                                       \
		.cken_bit = DT_INST_PROP(n, cken_bit),                                             \
	};                                                                                         \
	static struct spi_pxa_ssp_data spi_pxa_ssp_data_##n = {                                    \
		SPI_CONTEXT_INIT_LOCK(spi_pxa_ssp_data_##n, ctx),                                  \
		SPI_CONTEXT_INIT_SYNC(spi_pxa_ssp_data_##n, ctx),                                  \
		SPI_CONTEXT_CS_GPIOS_INITIALIZE(DT_DRV_INST(n), ctx)};                             \
	SPI_DEVICE_DT_INST_DEFINE(n, spi_pxa_ssp_init, NULL, &spi_pxa_ssp_data_##n,                \
				  &spi_pxa_ssp_config_##n, POST_KERNEL, CONFIG_SPI_INIT_PRIORITY,  \
				  &spi_pxa_ssp_api);

DT_INST_FOREACH_STATUS_OKAY(SPI_PXA_SSP_INIT)
