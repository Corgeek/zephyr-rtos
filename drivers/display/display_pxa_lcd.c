/*
 * Copyright (c) 2026 Takayuki Goto
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief PXA27x LCD controller, single 16 bpp framebuffer
 *
 * One DMA descriptor that points to itself keeps channel 0 scanning the
 * framebuffer forever, so the CPU only ever writes pixels. The framebuffer is
 * in cached SDRAM, so every write is cleaned out to memory for the DMA.
 *
 * Whatever ran before Zephyr (Linux, ahead of a kexec) may have left the
 * controller running on its own framebuffer, which Zephyr's image now
 * overwrites. It is stopped cleanly before anything else.
 *
 * display_set_orientation() turns what write() and read() see: rotated by
 * 90 or 270 degrees, a portrait panel is presented as landscape, with the
 * resolution swapped in the capabilities. DISPLAY_ORIENTATION_ROTATED_90
 * puts the top of the picture on the panel's right edge. The framebuffer
 * returned by get_framebuffer() always stays in the panel's own layout.
 */

#define DT_DRV_COMPAT intel_pxa27x_lcd

#include <errno.h>
#include <string.h>
#include <zephyr/cache.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/sys_io.h>
#include <soc.h>

LOG_MODULE_REGISTER(display_pxa_lcd, CONFIG_DISPLAY_LOG_LEVEL);

#define LCCR0  0x000U
#define LCCR1  0x004U
#define LCCR2  0x008U
#define LCCR3  0x00cU
#define LCCR4  0x010U
#define LCSR0  0x038U
#define FDADR0 0x200U

#define LCCR0_ENB    BIT(0)
#define LCCR0_LDM    BIT(3)
#define LCCR0_SFM    BIT(4)
#define LCCR0_IUM    BIT(5)
#define LCCR0_EFM    BIT(6)
#define LCCR0_PAS    BIT(7)
#define LCCR0_DIS    BIT(10)
#define LCCR0_QDM    BIT(11)
#define LCCR0_BM     BIT(20)
#define LCCR0_OUM    BIT(21)
#define LCCR0_LDDALT BIT(26)

/* Every interrupt source masked: the driver polls the one it needs */
#define LCCR0_MASKS                                                                                \
	(LCCR0_LDM | LCCR0_SFM | LCCR0_IUM | LCCR0_EFM | LCCR0_QDM | LCCR0_BM | LCCR0_OUM)

#define LCCR3_VSP     BIT(20)
#define LCCR3_HSP     BIT(21)
#define LCCR3_PCP     BIT(22)
#define LCCR3_OEP     BIT(23)
#define LCCR3_BPP_16  (4U << 24)
#define LCCR4_PAL_FOR (3U << 15)
#define LCSR0_LDD     BIT(0)

#define LDCMD_LENGTH_MASK 0x001ffffcU

/* L_DD[15:0], L_FCLK, L_LCLK, L_PCLK, L_BIAS */
#define LCD_FIRST_PIN 58U
#define LCD_LAST_PIN  77U
#define LCD_PIN_AF    2U

#define BYTES_PER_PIXEL 2U

/* Disabling waits for the end of the frame: one frame at 60 Hz, with margin */
#define DISABLE_TIMEOUT_US 50000U

struct pxa_lcd_desc {
	uint32_t fdadr;
	uint32_t fsadr;
	uint32_t fidr;
	uint32_t ldcmd;
} __aligned(16);

struct pxa_lcd_timing {
	uint16_t hsync;
	uint16_t hbp;
	uint16_t hfp;
	uint16_t vsync;
	uint16_t vbp;
	uint16_t vfp;
	uint32_t pixel_clock;
	bool hsync_high;
	bool vsync_high;
	bool de_high;
	bool pclk_rising;
};

struct pxa_lcd_config {
	mem_addr_t base;
	unsigned int cken_bit;
	uint16_t width;
	uint16_t height;
	bool alternate_mapping;
	struct pxa_lcd_timing timing;
	uint16_t *fb;
	struct pxa_lcd_desc *desc;
};

struct pxa_lcd_data {
	enum display_orientation orientation;
};

static inline size_t fb_size(const struct pxa_lcd_config *cfg)
{
	return (size_t)cfg->width * cfg->height * BYTES_PER_PIXEL;
}

static inline bool swapped(const struct pxa_lcd_data *data)
{
	return data->orientation == DISPLAY_ORIENTATION_ROTATED_90 ||
	       data->orientation == DISPLAY_ORIENTATION_ROTATED_270;
}

/* Framebuffer index of the picture's pixel (px, py) */
static inline size_t fb_index(const struct pxa_lcd_config *cfg, const struct pxa_lcd_data *data,
			      uint16_t px, uint16_t py)
{
	uint16_t x;
	uint16_t y;

	switch (data->orientation) {
	case DISPLAY_ORIENTATION_ROTATED_90:
		x = cfg->width - 1U - py;
		y = px;
		break;
	case DISPLAY_ORIENTATION_ROTATED_180:
		x = cfg->width - 1U - px;
		y = cfg->height - 1U - py;
		break;
	case DISPLAY_ORIENTATION_ROTATED_270:
		x = py;
		y = cfg->height - 1U - px;
		break;
	default:
		x = px;
		y = py;
		break;
	}

	return ((size_t)y * cfg->width) + x;
}

static int check_area(const struct device *dev, uint16_t x, uint16_t y,
		      const struct display_buffer_descriptor *desc)
{
	const struct pxa_lcd_config *cfg = dev->config;
	const struct pxa_lcd_data *data = dev->data;
	uint16_t w = swapped(data) ? cfg->height : cfg->width;
	uint16_t h = swapped(data) ? cfg->width : cfg->height;

	if ((x + desc->width > w) || (y + desc->height > h) || (desc->pitch < desc->width) ||
	    desc->width == 0U || desc->height == 0U) {
		return -EINVAL;
	}

	return 0;
}

static int pxa_lcd_write(const struct device *dev, const uint16_t x, const uint16_t y,
			 const struct display_buffer_descriptor *desc, const void *buf)
{
	const struct pxa_lcd_config *cfg = dev->config;
	const struct pxa_lcd_data *data = dev->data;
	const uint16_t *src = buf;
	size_t a;
	size_t b;
	uint16_t x0;
	uint16_t x1;
	uint16_t y0;
	uint16_t y1;

	if (check_area(dev, x, y, desc) != 0) {
		return -EINVAL;
	}

	if (data->orientation == DISPLAY_ORIENTATION_NORMAL) {
		uint16_t *dst = cfg->fb + ((size_t)y * cfg->width) + x;

		for (uint16_t row = 0U; row < desc->height; row++) {
			memcpy(dst, src, (size_t)desc->width * BYTES_PER_PIXEL);
			(void)sys_cache_data_flush_range(dst,
							 (size_t)desc->width * BYTES_PER_PIXEL);
			dst += cfg->width;
			src += desc->pitch;
		}

		return 0;
	}

	for (uint16_t row = 0U; row < desc->height; row++) {
		for (uint16_t col = 0U; col < desc->width; col++) {
			cfg->fb[fb_index(cfg, data, x + col, y + row)] = src[col];
		}
		src += desc->pitch;
	}

	/* A right-angle rotation keeps the area a rectangle in the framebuffer */
	a = fb_index(cfg, data, x, y);
	b = fb_index(cfg, data, x + desc->width - 1U, y + desc->height - 1U);
	x0 = MIN(a % cfg->width, b % cfg->width);
	x1 = MAX(a % cfg->width, b % cfg->width);
	y0 = MIN(a / cfg->width, b / cfg->width);
	y1 = MAX(a / cfg->width, b / cfg->width);

	for (uint16_t fy = y0; fy <= y1; fy++) {
		(void)sys_cache_data_flush_range(&cfg->fb[((size_t)fy * cfg->width) + x0],
						 (size_t)(x1 - x0 + 1U) * BYTES_PER_PIXEL);
	}

	return 0;
}

static int pxa_lcd_read(const struct device *dev, const uint16_t x, const uint16_t y,
			const struct display_buffer_descriptor *desc, void *buf)
{
	const struct pxa_lcd_config *cfg = dev->config;
	const struct pxa_lcd_data *data = dev->data;
	uint16_t *dst = buf;

	if (check_area(dev, x, y, desc) != 0) {
		return -EINVAL;
	}

	for (uint16_t row = 0U; row < desc->height; row++) {
		for (uint16_t col = 0U; col < desc->width; col++) {
			dst[col] = cfg->fb[fb_index(cfg, data, x + col, y + row)];
		}
		dst += desc->pitch;
	}

	return 0;
}

static int pxa_lcd_clear(const struct device *dev)
{
	const struct pxa_lcd_config *cfg = dev->config;

	memset(cfg->fb, 0, fb_size(cfg));
	(void)sys_cache_data_flush_range(cfg->fb, fb_size(cfg));

	return 0;
}

static void *pxa_lcd_get_framebuffer(const struct device *dev)
{
	const struct pxa_lcd_config *cfg = dev->config;

	return cfg->fb;
}

static int pxa_lcd_blanking_on(const struct device *dev)
{
	ARG_UNUSED(dev);

	return 0;
}

static int pxa_lcd_blanking_off(const struct device *dev)
{
	ARG_UNUSED(dev);

	return 0;
}

static void pxa_lcd_get_capabilities(const struct device *dev,
				     struct display_capabilities *capabilities)
{
	const struct pxa_lcd_config *cfg = dev->config;
	const struct pxa_lcd_data *data = dev->data;

	memset(capabilities, 0, sizeof(*capabilities));
	capabilities->x_resolution = swapped(data) ? cfg->height : cfg->width;
	capabilities->y_resolution = swapped(data) ? cfg->width : cfg->height;
	capabilities->supported_pixel_formats = PIXEL_FORMAT_RGB_565;
	capabilities->current_pixel_format = PIXEL_FORMAT_RGB_565;
	capabilities->current_orientation = data->orientation;
}

static int pxa_lcd_set_pixel_format(const struct device *dev,
				    const enum display_pixel_format pixel_format)
{
	ARG_UNUSED(dev);

	return (pixel_format == PIXEL_FORMAT_RGB_565) ? 0 : -ENOTSUP;
}

static int pxa_lcd_set_orientation(const struct device *dev,
				   const enum display_orientation orientation)
{
	struct pxa_lcd_data *data = dev->data;

	switch (orientation) {
	case DISPLAY_ORIENTATION_NORMAL:
	case DISPLAY_ORIENTATION_ROTATED_90:
	case DISPLAY_ORIENTATION_ROTATED_180:
	case DISPLAY_ORIENTATION_ROTATED_270:
		data->orientation = orientation;
		return 0;
	default:
		return -ENOTSUP;
	}
}

static DEVICE_API(display, pxa_lcd_api) = {
	.blanking_on = pxa_lcd_blanking_on,
	.blanking_off = pxa_lcd_blanking_off,
	.write = pxa_lcd_write,
	.read = pxa_lcd_read,
	.clear = pxa_lcd_clear,
	.get_framebuffer = pxa_lcd_get_framebuffer,
	.get_capabilities = pxa_lcd_get_capabilities,
	.set_pixel_format = pxa_lcd_set_pixel_format,
	.set_orientation = pxa_lcd_set_orientation,
};

static void pxa_lcd_stop(const struct pxa_lcd_config *cfg)
{
	uint32_t lccr0 = sys_read32(cfg->base + LCCR0);

	if ((lccr0 & LCCR0_ENB) == 0U) {
		return;
	}

	/* A normal disable finishes the current frame, then flags LDD */
	sys_write32(0xffffffffU, cfg->base + LCSR0);
	sys_write32(lccr0 | LCCR0_DIS, cfg->base + LCCR0);

	for (uint32_t us = 0U; us < DISABLE_TIMEOUT_US; us += 10U) {
		if ((sys_read32(cfg->base + LCSR0) & LCSR0_LDD) != 0U) {
			break;
		}
		k_busy_wait(10U);
	}

	sys_write32(0U, cfg->base + LCCR0);
	sys_write32(0xffffffffU, cfg->base + LCSR0);
}

static uint32_t pxa_lcd_lccr3(const struct pxa_lcd_config *cfg)
{
	const struct pxa_lcd_timing *t = &cfg->timing;
	uint32_t lclk = pxa_lcd_clock_hz();
	uint32_t pcd = DIV_ROUND_UP(lclk, 2U * t->pixel_clock);
	uint32_t lccr3 = LCCR3_BPP_16;

	/* Pixel clock = LCLK / (2 x (PCD + 1)), never faster than asked */
	pcd = CLAMP(pcd, 1U, 256U) - 1U;
	LOG_INF("LCLK %u Hz, PCD %u, pixel clock %u Hz", lclk, pcd, lclk / (2U * (pcd + 1U)));

	lccr3 |= pcd;
	lccr3 |= t->hsync_high ? 0U : LCCR3_HSP;
	lccr3 |= t->vsync_high ? 0U : LCCR3_VSP;
	lccr3 |= t->de_high ? 0U : LCCR3_OEP;
	lccr3 |= t->pclk_rising ? LCCR3_PCP : 0U;

	return lccr3;
}

static int pxa_lcd_init(const struct device *dev)
{
	const struct pxa_lcd_config *cfg = dev->config;
	const struct pxa_lcd_timing *t = &cfg->timing;
	struct pxa_lcd_desc *desc = cfg->desc;
	uint32_t lccr0 = LCCR0_PAS | LCCR0_MASKS;

	pxa_clock_enable(cfg->cken_bit);
	pxa_lcd_stop(cfg);

	for (unsigned int pin = LCD_FIRST_PIN; pin <= LCD_LAST_PIN; pin++) {
		pxa_gpio_set_af(pin, LCD_PIN_AF, true);
	}

	(void)pxa_lcd_clear(dev);

	/* The descriptor loops on itself: the same frame, every frame */
	desc->fdadr = (uint32_t)(uintptr_t)desc;
	desc->fsadr = (uint32_t)(uintptr_t)cfg->fb;
	desc->fidr = 0U;
	desc->ldcmd = fb_size(cfg) & LDCMD_LENGTH_MASK;
	(void)sys_cache_data_flush_range(desc, sizeof(*desc));

	if (cfg->alternate_mapping) {
		lccr0 |= LCCR0_LDDALT;
	}

	/* Order from the PXA27x manual, section 11.7.10 */
	sys_write32(sys_read32(cfg->base + LCCR4) & ~LCCR4_PAL_FOR, cfg->base + LCCR4);
	sys_write32(pxa_lcd_lccr3(cfg), cfg->base + LCCR3);
	sys_write32(((uint32_t)(cfg->height - 1U)) | ((uint32_t)(t->vsync - 1U) << 10) |
			    ((uint32_t)t->vfp << 16) | ((uint32_t)t->vbp << 24),
		    cfg->base + LCCR2);
	sys_write32(((uint32_t)(cfg->width - 1U)) | ((uint32_t)(t->hsync - 1U) << 10) |
			    ((uint32_t)(t->hfp - 1U) << 16) | ((uint32_t)(t->hbp - 1U) << 24),
		    cfg->base + LCCR1);
	sys_write32(lccr0, cfg->base + LCCR0);
	sys_write32((uint32_t)(uintptr_t)desc, cfg->base + FDADR0);
	sys_write32(lccr0 | LCCR0_ENB, cfg->base + LCCR0);

	LOG_INF("%ux%u RGB565 at %p", cfg->width, cfg->height, (void *)cfg->fb);

	return 0;
}

#define PXA_LCD_TIMING(n) DT_INST_CHILD(n, display_timings)

#define PXA_LCD_INIT(n)                                                                            \
	BUILD_ASSERT(DT_INST_PROP(n, width) <= 1024 && DT_INST_PROP(n, height) <= 1024,            \
		     "LCCR1/LCCR2 hold at most 1024 pixels and lines");                            \
	static uint16_t pxa_lcd_fb_##n[DT_INST_PROP(n, width) * DT_INST_PROP(n, height)]           \
		__aligned(32) __noinit;                                                            \
	static struct pxa_lcd_desc pxa_lcd_desc_##n;                                               \
	static struct pxa_lcd_data pxa_lcd_data_##n;                                               \
	static const struct pxa_lcd_config pxa_lcd_config_##n = {                                  \
		.base = DT_INST_REG_ADDR(n),                                                       \
		.cken_bit = DT_INST_PROP(n, cken_bit),                                             \
		.width = DT_INST_PROP(n, width),                                                   \
		.height = DT_INST_PROP(n, height),                                                 \
		.alternate_mapping = DT_INST_PROP(n, alternate_mapping),                           \
		.timing =                                                                          \
			{                                                                          \
				.hsync = DT_PROP(PXA_LCD_TIMING(n), hsync_len),                    \
				.hbp = DT_PROP(PXA_LCD_TIMING(n), hback_porch),                    \
				.hfp = DT_PROP(PXA_LCD_TIMING(n), hfront_porch),                   \
				.vsync = DT_PROP(PXA_LCD_TIMING(n), vsync_len),                    \
				.vbp = DT_PROP(PXA_LCD_TIMING(n), vback_porch),                    \
				.vfp = DT_PROP(PXA_LCD_TIMING(n), vfront_porch),                   \
				.pixel_clock = DT_PROP(PXA_LCD_TIMING(n), clock_frequency),        \
				.hsync_high = DT_PROP(PXA_LCD_TIMING(n), hsync_active),            \
				.vsync_high = DT_PROP(PXA_LCD_TIMING(n), vsync_active),            \
				.de_high = DT_PROP(PXA_LCD_TIMING(n), de_active),                  \
				.pclk_rising = DT_PROP(PXA_LCD_TIMING(n), pixelclk_active),        \
			},                                                                         \
		.fb = pxa_lcd_fb_##n,                                                              \
		.desc = &pxa_lcd_desc_##n,                                                         \
	};                                                                                         \
	DEVICE_DT_INST_DEFINE(n, pxa_lcd_init, NULL, &pxa_lcd_data_##n, &pxa_lcd_config_##n,       \
			      POST_KERNEL, CONFIG_DISPLAY_INIT_PRIORITY, &pxa_lcd_api);

DT_INST_FOREACH_STATUS_OKAY(PXA_LCD_INIT)
