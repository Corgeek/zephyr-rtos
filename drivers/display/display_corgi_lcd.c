/*
 * Copyright (c) 2026 Takayuki Goto
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Sharp corgi LCD timing generator and backlight
 *
 * Follows Linux's drivers/video/backlight/corgi_lcd.c: the same power-up
 * sequence, the same bit-banged I2C to the M62332FP common voltage DAC
 * through power register 0, and the same backlight scale.
 *
 * Two values come from the Sharp boot loader's parameter block at the bottom
 * of SDRAM: the common voltage (COMADJ) and the VGA phase adjustment (PHAD).
 * Linux leaves that block alone, so it normally survives a kexec; when its
 * magic is missing the driver uses Linux's fallbacks.
 */

#define DT_DRV_COMPAT sharp_corgi_lcd

#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/sys_io.h>

LOG_MODULE_REGISTER(corgi_lcd, CONFIG_DISPLAY_LOG_LEVEL);

/* Timing generator registers */
#define RESCTL_ADRS    0x00U
#define PHACTRL_ADRS   0x01U
#define DUTYCTRL_ADRS  0x02U
#define POWERREG0_ADRS 0x03U
#define POWERREG1_ADRS 0x04U
#define PICTRL_ADRS    0x06U
#define POLCTRL_ADRS   0x07U

#define RESCTL_QVGA 0x01U
#define RESCTL_VGA  0x00U

#define POWER1_VW_ON   0x01U
#define POWER1_GVSS_ON 0x02U
#define POWER1_VDD_ON  0x04U

#define POWER0_COM_DCLK 0x01U
#define POWER0_COM_DOUT 0x02U
#define POWER0_DAC_ON   0x04U
#define POWER0_COM_ON   0x08U
#define POWER0_VCC5_ON  0x10U

#define PICTRL_INIT_STATE     0x01U
#define PICTRL_INIOFF         0x02U
#define PICTRL_POWER_DOWN     0x04U
#define PICTRL_COM_SIGNAL_OFF 0x08U
#define PICTRL_DAC_SIGNAL_OFF 0x10U

/* Rising edges, active low sync, active high enable */
#define POLCTRL_DEFAULT 0x00U

#define PHACTRL_PHASE_MANUAL 0x01U
#define DEFAULT_PHAD_QVGA    9U
#define DEFAULT_COMADJ       125U

/* M62332FP address byte, as Linux sends it */
#define M62332_ADDRESS 0x9cU

/* Sharp boot loader parameter block (Linux: sharpsl_param.c) */
#define SHARPSL_PARAM_BASE 0xa0000a00U
#define PARAM_MAGIC(a, b, c, d)                                                                    \
	((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
#define COMADJ_MAGIC     PARAM_MAGIC('C', 'M', 'A', 'D')
#define PHAD_MAGIC       PARAM_MAGIC('P', 'H', 'A', 'D')
#define PARAM_COMADJ_KEY 0x00U
#define PARAM_COMADJ     0x04U
#define PARAM_PHAD_KEY   0x3cU
#define PARAM_PHAD       0x40U

#define BRIGHTNESS_MAX 0x2fU

struct corgi_lcd_config {
	struct spi_dt_spec bus;
	struct gpio_dt_spec backlight_on;
	struct gpio_dt_spec backlight_cont;
	uint8_t brightness;
	bool qvga;
};

static int corgi_send(const struct corgi_lcd_config *cfg, uint8_t reg, uint8_t val)
{
	uint8_t byte = (uint8_t)(((reg & 0x07U) << 5) | (val & 0x1fU));
	const struct spi_buf buf = {.buf = &byte, .len = 1};
	const struct spi_buf_set tx = {.buffers = &buf, .count = 1};

	return spi_write_dt(&cfg->bus, &tx);
}

/* The DAC hangs off two bits of power register 0, bit-banged as I2C */
static void dac_send(const struct corgi_lcd_config *cfg, uint8_t data)
{
	(void)corgi_send(cfg, POWERREG0_ADRS, data);
	k_busy_wait(10);
}

static void dac_bit(const struct corgi_lcd_config *cfg, uint8_t data)
{
	dac_send(cfg, data);
	dac_send(cfg, data | POWER0_COM_DCLK);
	dac_send(cfg, data);
}

static void dac_byte(const struct corgi_lcd_config *cfg, uint8_t base, uint8_t data)
{
	for (int i = 0; i < 8; i++) {
		dac_bit(cfg, ((data & 0x80U) != 0U) ? (base | POWER0_COM_DOUT) : base);
		data <<= 1;
	}
	/* Acknowledge clock */
	dac_bit(cfg, base);
}

static void set_common_voltage(const struct corgi_lcd_config *cfg, uint8_t base, uint8_t value)
{
	/* Start */
	dac_send(cfg, base | POWER0_COM_DCLK | POWER0_COM_DOUT);
	dac_send(cfg, base | POWER0_COM_DCLK);
	dac_send(cfg, base);

	dac_byte(cfg, base, M62332_ADDRESS);
	dac_byte(cfg, base, 0x00U);
	dac_byte(cfg, base, value);

	/* Stop */
	dac_send(cfg, base);
	dac_send(cfg, base | POWER0_COM_DCLK);
	dac_send(cfg, base | POWER0_COM_DCLK | POWER0_COM_DOUT);
}

static uint8_t param_comadj(void)
{
	if (sys_read32(SHARPSL_PARAM_BASE + PARAM_COMADJ_KEY) != COMADJ_MAGIC) {
		LOG_WRN("no COMADJ from the boot loader, using %u", DEFAULT_COMADJ);
		return DEFAULT_COMADJ;
	}

	return (uint8_t)sys_read32(SHARPSL_PARAM_BASE + PARAM_COMADJ);
}

static uint8_t phase_adjust(const struct corgi_lcd_config *cfg)
{
	if (cfg->qvga) {
		return (DEFAULT_PHAD_QVGA << 1) | PHACTRL_PHASE_MANUAL;
	}

	if (sys_read32(SHARPSL_PARAM_BASE + PARAM_PHAD_KEY) != PHAD_MAGIC) {
		LOG_WRN("no PHAD from the boot loader, using none");
		return PHACTRL_PHASE_MANUAL;
	}

	return PHACTRL_PHASE_MANUAL | ((sys_read32(SHARPSL_PARAM_BASE + PARAM_PHAD) & 0xfU) << 1);
}

static int corgi_power_on(const struct corgi_lcd_config *cfg)
{
	uint8_t comadj = param_comadj();
	uint8_t phad = phase_adjust(cfg);
	int ret;

	LOG_INF("COMADJ %u, PHAD 0x%02x", comadj, phad);

	/* Initialise the internal logic and ports, everything off */
	ret = corgi_send(cfg, PICTRL_ADRS,
			 PICTRL_POWER_DOWN | PICTRL_INIOFF | PICTRL_INIT_STATE |
				 PICTRL_COM_SIGNAL_OFF | PICTRL_DAC_SIGNAL_OFF);
	if (ret != 0) {
		return ret;
	}
	(void)corgi_send(cfg, POWERREG0_ADRS, POWER0_COM_DCLK | POWER0_COM_DOUT);
	(void)corgi_send(cfg, POWERREG1_ADRS, 0U);

	/* VDD (+8 V), SVSS (-4 V) on */
	(void)corgi_send(cfg, POWERREG1_ADRS, POWER1_VDD_ON);
	k_busy_wait(3000);

	/* DAC on */
	(void)corgi_send(cfg, POWERREG0_ADRS, POWER0_COM_DCLK | POWER0_COM_DOUT | POWER0_DAC_ON);

	/* INIB high, INI low */
	(void)corgi_send(cfg, PICTRL_ADRS, PICTRL_INIT_STATE | PICTRL_COM_SIGNAL_OFF);

	set_common_voltage(cfg, POWER0_DAC_ON, comadj);

	/* VCC5 on, DAC on */
	(void)corgi_send(cfg, POWERREG0_ADRS,
			 POWER0_COM_DCLK | POWER0_COM_DOUT | POWER0_DAC_ON | POWER0_VCC5_ON);

	/* GVSS (-8 V) on, VDD on */
	(void)corgi_send(cfg, POWERREG1_ADRS, POWER1_GVSS_ON | POWER1_VDD_ON);
	k_busy_wait(2000);

	/* COM signal on */
	(void)corgi_send(cfg, PICTRL_ADRS, PICTRL_INIT_STATE);

	/* COM, DAC, VCC5 on */
	(void)corgi_send(cfg, POWERREG0_ADRS,
			 POWER0_COM_DCLK | POWER0_COM_DOUT | POWER0_DAC_ON | POWER0_COM_ON |
				 POWER0_VCC5_ON);

	/* VW, GVSS, VDD on */
	(void)corgi_send(cfg, POWERREG1_ADRS, POWER1_VW_ON | POWER1_GVSS_ON | POWER1_VDD_ON);

	/* Signal outputs on */
	(void)corgi_send(cfg, PICTRL_ADRS, 0U);

	(void)corgi_send(cfg, PHACTRL_ADRS, phad);
	(void)corgi_send(cfg, POLCTRL_ADRS, POLCTRL_DEFAULT);
	k_busy_wait(1000);

	return corgi_send(cfg, RESCTL_ADRS, cfg->qvga ? RESCTL_QVGA : RESCTL_VGA);
}

/* Linux's corgi_bl_set_intensity(), on the same 0..0x2f scale */
static int corgi_set_backlight(const struct corgi_lcd_config *cfg, uint8_t level)
{
	uint32_t duty = MIN(level, BRIGHTNESS_MAX);
	int ret;

	if (duty > 0x10U) {
		duty += 0x10U;
	}

	ret = corgi_send(cfg, DUTYCTRL_ADRS, (uint8_t)duty);
	if (ret != 0) {
		return ret;
	}

	ret = gpio_pin_set_dt(&cfg->backlight_cont, (duty & 0x20U) != 0U);
	if (ret != 0) {
		return ret;
	}

	return gpio_pin_set_dt(&cfg->backlight_on, duty != 0U);
}

static int corgi_lcd_init(const struct device *dev)
{
	const struct corgi_lcd_config *cfg = dev->config;
	int ret;

	if (!spi_is_ready_dt(&cfg->bus) || !gpio_is_ready_dt(&cfg->backlight_on) ||
	    !gpio_is_ready_dt(&cfg->backlight_cont)) {
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&cfg->backlight_on, GPIO_OUTPUT_INACTIVE);
	if (ret == 0) {
		ret = gpio_pin_configure_dt(&cfg->backlight_cont, GPIO_OUTPUT_INACTIVE);
	}
	if (ret == 0) {
		ret = corgi_power_on(cfg);
	}
	if (ret == 0) {
		ret = corgi_set_backlight(cfg, cfg->brightness);
	}

	if (ret != 0) {
		LOG_ERR("panel power up failed (%d)", ret);
	}

	return ret;
}

#define CORGI_LCD_INIT(n)                                                                          \
	BUILD_ASSERT(DT_INST_PROP(n, brightness) <= BRIGHTNESS_MAX, "brightness is 0 to 0x2f");    \
	static const struct corgi_lcd_config corgi_lcd_config_##n = {                              \
		.bus = SPI_DT_SPEC_INST_GET(n, SPI_OP_MODE_CONTROLLER | SPI_WORD_SET(8)),          \
		.backlight_on = GPIO_DT_SPEC_INST_GET(n, backlight_on_gpios),                      \
		.backlight_cont = GPIO_DT_SPEC_INST_GET(n, backlight_cont_gpios),                  \
		.brightness = DT_INST_PROP(n, brightness),                                         \
		.qvga = DT_INST_PROP(n, qvga),                                                     \
	};                                                                                         \
	DEVICE_DT_INST_DEFINE(n, corgi_lcd_init, NULL, NULL, &corgi_lcd_config_##n, POST_KERNEL,   \
			      CONFIG_CORGI_LCD_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(CORGI_LCD_INIT)
