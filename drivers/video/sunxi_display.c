/*
 * (C) Copyright 2013-2014 Luc Verhaegen <libv@skynet.be>
 *
 * Display driver for Allwinner SoCs.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation's version 2 and any
 * later version the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

/*
 * This driver does nothing but HDMI at a fixed mode right now. At some
 * point in the near future, LCD and VGA will be added.
 *
 * The display driver infrastructure in uboot does not immediately allow for
 * modeline creation off of edid. The mode is therefor hardcoded to
 * 1024x768@60Hz 32bpp. This is acceptable for most HDMI monitors, but far
 * from ideal. If so desired, alter the modeline in video_hw_init()
 */

#include <common.h>

#include <asm/io.h>
#include <asm/global_data.h>
#include <video_fb.h>
#include <linux/fb.h>
#include <asm/arch-sunxi/sunxi_display.h>

/* for simplefb */
#ifdef CONFIG_OF_BOARD_SETUP
#include <libfdt.h>
#endif

DECLARE_GLOBAL_DATA_PTR;

struct sunxi_display {
	GraphicDevice graphic_device[1];
	int enabled;
} sunxi_display[1];

/*
 * Convenience functions to ease readability, and to provide an easy
 * comparison with the sunxi kms driver.
 */
static unsigned int
sunxi_io_read(void *base, int offset)
{
	return readl(base + offset);
}

static void
sunxi_io_write(void *base, int offset, unsigned int value)
{
	writel(value, base + offset);
}

static void
sunxi_io_mask(void *base, int offset, unsigned int value, unsigned int mask)
{
	unsigned int tmp = readl(base + offset);

	tmp &= ~mask;
	tmp |= value & mask;

	writel(tmp, base + offset);
}

/*
 * CCMU regs: clocks.
 */
#define SUNXI_CCMU_PLL3_CFG		0x010
#define SUNXI_CCMU_PLL5_CFG		0x020
#define SUNXI_CCMU_PLL7_CFG		0x030
#define SUNXI_CCMU_AHB_GATING1		0x064
#define SUNXI_CCMU_DRAM_CLK_GATING	0x100
#define SUNXI_DE_BE0_CLK		0x104
#define SUNXI_LCDC0_CH0_CLK		0x118
#define SUNXI_LCDC0_CH1_CLK		0x12C
#define SUNXI_CCMU_HDMI_CLK		0x150

/*
 * DEBE regs.
 *
 * This is the entity that mixes and matches the different layers and inputs.
 * Allwinner calls it the back-end, but i like composer better.
 */
#define SUNXI_COMP_MODE			0x800
#define SUNXI_COMP_DISP_SIZE		0x808
#define SUNXI_COMP_LAYER0_SIZE		0x810
#define SUNXI_COMP_LAYER0_POS		0x820
#define SUNXI_COMP_LAYER0_STRIDE	0x840
#define SUNXI_COMP_LAYER0_ADDR_LOW	0X850
#define SUNXI_COMP_LAYER_ADDR_HIGH	0X860
#define SUNXI_COMP_REG_CTL		0X870
#define SUNXI_COMP_LAYER0_ATTR0		0x890
#define SUNXI_COMP_LAYER0_ATTR1		0x8a0

/*
 * LCDC, what allwinner calls a CRTC, so timing controller and serializer.
 */
#define SUNXI_LCDC_ENABLE		0x000
#define SUNXI_LCDC_INT0			0x004
#define SUNXI_LCDC_INT1			0x008
#define SUNXI_LCDC_TCON0_DOTCLOCK	0x044
#define SUNXI_LCDC_TCON0_IO_TRI		0x08c
#define SUNXI_LCDC_TCON1_ENABLE		0x090
#define SUNXI_LCDC_TCON1_TIMING_SRC	0x094
#define SUNXI_LCDC_TCON1_TIMING_SCALE	0x098
#define SUNXI_LCDC_TCON1_TIMING_OUT	0x09c
#define SUNXI_LCDC_TCON1_TIMING_H	0x0a0
#define SUNXI_LCDC_TCON1_TIMING_V	0x0a4
#define SUNXI_LCDC_TCON1_TIMING_SYNC	0x0a8
#define SUNXI_LCDC_TCON1_IO_TRI		0x0f4

/*
 * HDMI regs.
 */
#define SUNXI_HDMI_CTRL			0x004
#define SUNXI_HDMI_INT_CTRL		0x008
#define SUNXI_HDMI_HPD			0x00c
#define SUNXI_HDMI_VIDEO_CTRL		0x010
#define SUNXI_HDMI_VIDEO_SIZE		0x014
#define SUNXI_HDMI_VIDEO_BP		0x018
#define SUNXI_HDMI_VIDEO_FP		0x01c
#define SUNXI_HDMI_VIDEO_SPW		0x020
#define SUNXI_HDMI_VIDEO_POLARITY	0x024
#define SUNXI_HDMI_TX_DRIVER0		0x200
#define SUNXI_HDMI_TX_DRIVER1		0x204
#define SUNXI_HDMI_TX_DRIVER2		0x208
#define SUNXI_HDMI_TX_DRIVER3		0x20C

static int
sunxi_hdmi_hpd_detect(void)
{
	void *ccmu = (void *) SUNXI_CCM_BASE;
	void *hdmi = (void *) SUNXI_HDMI_BASE;

	/* set video pll1 to 300MHz */
	sunxi_io_write(ccmu, SUNXI_CCMU_PLL7_CFG, 0x8010D064);

	/* Set hdmi parent to video pll1 */
	sunxi_io_mask(ccmu, SUNXI_CCMU_HDMI_CLK, 0x01000000, 0x03000000);

	/* set ahb gating to pass */
	sunxi_io_mask(ccmu, SUNXI_CCMU_AHB_GATING1, 0x800, 0x800);

	/* clk on */
	sunxi_io_mask(ccmu, SUNXI_CCMU_HDMI_CLK, 0x80000000, 0x80000000);

	sunxi_io_write(hdmi, SUNXI_HDMI_CTRL, 0x80000000);
	sunxi_io_write(hdmi, SUNXI_HDMI_TX_DRIVER0, 0xA0800000);

	udelay(100);

	if (sunxi_io_read(hdmi, SUNXI_HDMI_HPD) & 0x01)
		return 1;

	/* no need to keep these running. */
	sunxi_io_write(hdmi, SUNXI_HDMI_CTRL, 0);
	sunxi_io_mask(ccmu, SUNXI_CCMU_HDMI_CLK, 0, 0x80000000);
	sunxi_io_mask(ccmu, SUNXI_CCMU_AHB_GATING1, 0, 0x800);
	sunxi_io_mask(ccmu, SUNXI_CCMU_PLL7_CFG, 0, 0x80000000);

	return 0;
}

static int
sunxi_pll5_frequency(void)
{
	void *ccmu = (void *) SUNXI_CCM_BASE;
	unsigned int pll5 = sunxi_io_read(ccmu, SUNXI_CCMU_PLL5_CFG);
	int n, k, p;

	n = (pll5 >> 8) & 0x1F;
	k = ((pll5 >> 4) & 0x03) + 1;
	p = (pll5 >> 16) & 0x03;

	return (24000 * n * k) >> p;
}

static void
sunxi_composer_init(void)
{
	void *ccmu = (void *) SUNXI_CCM_BASE;
	void *composer = (void *) SUNXI_DE_BE0_BASE;
	int pll5 = sunxi_pll5_frequency();
	int halve;

	if (pll5 < 300000)
		halve = 0;
	else
		halve = 1;

	/* reset off */
	sunxi_io_mask(ccmu, SUNXI_DE_BE0_CLK, 0x40000000, 0x40000000);

	/* set to pll5 */
	sunxi_io_mask(ccmu, SUNXI_DE_BE0_CLK, 0x02000000, 0x03000000);

	if (halve)
		sunxi_io_mask(ccmu, SUNXI_DE_BE0_CLK, 0x01, 0x03);
	else
		sunxi_io_mask(ccmu, SUNXI_DE_BE0_CLK, 0, 0x03);

	sunxi_io_mask(ccmu, SUNXI_CCMU_AHB_GATING1, 0x1000, 0x1000);
	sunxi_io_mask(ccmu, SUNXI_CCMU_DRAM_CLK_GATING,
		      0x04000000, 0x04000000);

	/* enable */
	sunxi_io_mask(ccmu, SUNXI_DE_BE0_CLK, 0x80000000, 0x80000000);

	/* engine bug, clear registers after reset. */
	{
		/*
		 * Since uboot prototypes but never declares memset_io, we
		 * have to do this by hand.
		 */
		int i;

		for (i = 0x0800; i < 0x1000; i += 4)
			sunxi_io_write(composer, i, 0);
	}

	sunxi_io_mask(composer, SUNXI_COMP_MODE, 0x01, 0x01);
}

static void
sunxi_composer_mode_set(struct fb_videomode *mode, unsigned int address)
{
	void *composer = (void *) SUNXI_DE_BE0_BASE;
#define SUNXI_FORMAT_XRGB8888 0x09
	unsigned int format = SUNXI_FORMAT_XRGB8888;

	/* enable */
	sunxi_io_write(composer, SUNXI_COMP_DISP_SIZE,
		       ((mode->yres - 1) << 16) | (mode->xres - 1));

	sunxi_io_write(composer, SUNXI_COMP_LAYER0_SIZE,
		       ((mode->yres - 1) << 16) | (mode->xres - 1));
	sunxi_io_write(composer, SUNXI_COMP_LAYER0_STRIDE, mode->xres << 5);
	sunxi_io_write(composer, SUNXI_COMP_LAYER0_ADDR_LOW, address << 3);
	sunxi_io_mask(composer, SUNXI_COMP_LAYER_ADDR_HIGH,
		      address >> 29, 0xFF);

	sunxi_io_mask(composer, SUNXI_COMP_LAYER0_ATTR1, format << 8, 0x0F00);

	sunxi_io_mask(composer, SUNXI_COMP_LAYER0_ATTR1, 0, 0x04);
	sunxi_io_mask(composer, SUNXI_COMP_LAYER0_ATTR1, 0, 0x03);

	sunxi_io_mask(composer, SUNXI_COMP_MODE, 0x100, 0x100);
}

static void
sunxi_lcdc_pll_set(int dotclock, int *clk_div, int *clk_double)
{
	void *ccmu = (void *) SUNXI_CCM_BASE;
	int value, n, m, diff;
	int best_n = 0, best_m = 0, best_diff = 0x0FFFFFFF;
	int best_double = 0;

	if ((dotclock < 20000) || (dotclock > 400000)) {
		printf("%s: Error: dotclock %d is out of range.\n",
		       __func__, dotclock);
		return;
	}

	for (m = 15; m > 0; m--) {
		n = (m * dotclock) / 3000;

		if ((n > 9) && (n < 128)) {
			value = (3000 * n) / m;
			diff = value - dotclock;
			if (diff < 0)
				diff = -diff;

			if (diff < best_diff) {
				best_diff = diff;
				best_m = m;
				best_n = n;
				best_double = 0;
			}
		}

		n++;
		if ((n > 9) && (n < 128)) {
			value = (3000 * n) / m;
			diff = abs(value - dotclock);
			if (diff < 0)
				diff = -diff;

			if (diff < best_diff) {
				best_diff = diff;
				best_m = m;
				best_n = n;
				best_double = 0;
			}
		}

		/* these are just duplicates. */
		if (!(m & 1))
			continue;

		n = (m * dotclock) / 6000;
		if ((n > 63) && (n < 128)) {
			value = (6000 * n) / m;
			diff = abs(value - dotclock);
			if (diff < 0)
				diff = -diff;

			if (diff < best_diff) {
				best_diff = diff;
				best_m = m;
				best_n = n;
				best_double = 1;
			}
		}

		n++;
		if ((n > 63) && (n < 128)) {
			value = (6000 * n) / m;
			diff = abs(value - dotclock);
			if (diff < 0)
				diff = -diff;

			if (diff < best_diff) {
				best_diff = diff;
				best_m = m;
				best_n = n;
				best_double = 1;
			}
		}
	}

#if 0
	if (best_double)
		printf("dotclock: %06dkHz = %06dkHz: (2 * 3MHz * %d) / %d\n",
		       dotclock, (6000 * best_n) / best_m, best_n, best_m);
	else
		printf("dotclock: %06dkHz = %06dkHz: (3MHz * %d) / %d\n",
		       dotclock, (3000 * best_n) / best_m, best_n, best_m);
#endif

	sunxi_io_mask(ccmu, SUNXI_CCMU_PLL3_CFG, 0x80000000, 0x80000000);
	sunxi_io_mask(ccmu, SUNXI_CCMU_PLL3_CFG, 0x8000, 0x8000);
	sunxi_io_mask(ccmu, SUNXI_CCMU_PLL3_CFG, best_n, 0x7F);

	if (best_double)
		sunxi_io_mask(ccmu, SUNXI_LCDC0_CH1_CLK,
			      0x02000000, 0x03000000);
	else
		sunxi_io_mask(ccmu, SUNXI_LCDC0_CH1_CLK,
			      0, 0x03000000);
	sunxi_io_mask(ccmu, SUNXI_LCDC0_CH1_CLK, best_m - 1, 0x0F);
	sunxi_io_mask(ccmu, SUNXI_LCDC0_CH1_CLK, 0, 0x0800);

	sunxi_io_mask(ccmu, SUNXI_LCDC0_CH1_CLK, 0x80008000, 0x80008000);

	*clk_div = best_m;
	*clk_double = best_double;
}

static void
sunxi_lcdc_init(void)
{
	void *ccmu = (void *) SUNXI_CCM_BASE;
	void *lcdc = (void *) SUNXI_LCD0_BASE;

	/* Pll1 was already enabled in hpd detect. */
	sunxi_io_mask(ccmu, SUNXI_LCDC0_CH0_CLK, 0x01000000, 0x03000000);

	sunxi_io_mask(ccmu, SUNXI_LCDC0_CH1_CLK, 0x01000000, 0x03000000);
	sunxi_io_mask(ccmu, SUNXI_LCDC0_CH1_CLK, 0, 0x0800);

	/* just randomly set it at 30MHz */
	sunxi_io_mask(ccmu, SUNXI_LCDC0_CH1_CLK, 0x09, 0x0F);

	sunxi_io_mask(ccmu, SUNXI_LCDC0_CH0_CLK, 0x40000000, 0x40000000);

	sunxi_io_mask(ccmu, SUNXI_CCMU_AHB_GATING1, 0x10, 0x10);

	/* toggle ch0 clock */
	sunxi_io_mask(ccmu, SUNXI_LCDC0_CH0_CLK, 0x80000000, 0x80000000);
	while (sunxi_io_read(ccmu, SUNXI_LCDC0_CH0_CLK) & 0x80000000)
		sunxi_io_mask(ccmu, SUNXI_LCDC0_CH0_CLK, 0, 0x80000000);

	/* toggle ch1 s1 & s2 clocks */
	sunxi_io_mask(ccmu, SUNXI_LCDC0_CH1_CLK, 0x80008000, 0x80008000);
	while (sunxi_io_read(ccmu, SUNXI_LCDC0_CH1_CLK) & 0x80008000)
		sunxi_io_mask(ccmu, SUNXI_LCDC0_CH1_CLK, 0, 0x80008000);

	sunxi_io_write(lcdc, SUNXI_LCDC_ENABLE, 0);

	sunxi_io_write(lcdc, SUNXI_LCDC_INT0, 0);
	sunxi_io_write(lcdc, SUNXI_LCDC_INT1, 0x20);

	/*
	 * disable tcon0 dot clock:
	 * This doesn't disable the dotclock, it just nulls the divider.
	 */
	sunxi_io_write(lcdc, SUNXI_LCDC_TCON0_DOTCLOCK, 0xF0000000);

	/* all io lines disabled. */
	sunxi_io_write(lcdc, SUNXI_LCDC_TCON0_IO_TRI, 0x0FFFFFFF);
	sunxi_io_write(lcdc, SUNXI_LCDC_TCON1_IO_TRI, 0x0FFFFFFF);
}

static void
sunxi_lcdc_mode_set(struct fb_videomode *mode, int *clk_div, int *clk_double)
{
	void *lcdc = (void *) SUNXI_LCD0_BASE;
	int total;

	/* use tcon1 */
	sunxi_io_mask(lcdc, SUNXI_LCDC_ENABLE, 0x01, 0x01);

	/* enabled, 0x1E start delay */
	sunxi_io_write(lcdc, SUNXI_LCDC_TCON1_ENABLE, 0x800001E0);

	sunxi_io_write(lcdc, SUNXI_LCDC_TCON1_TIMING_SRC,
		       ((mode->xres - 1) << 16) | (mode->yres - 1));
	sunxi_io_write(lcdc, SUNXI_LCDC_TCON1_TIMING_SCALE,
		       ((mode->xres - 1) << 16) | (mode->yres - 1));
	sunxi_io_write(lcdc, SUNXI_LCDC_TCON1_TIMING_OUT,
		       ((mode->xres - 1) << 16) | (mode->yres - 1));

	total = mode->left_margin + mode->xres + mode->right_margin +
		mode->hsync_len;
	sunxi_io_write(lcdc, SUNXI_LCDC_TCON1_TIMING_H,
		       ((total - 1) << 16) |
		       (mode->hsync_len + mode->left_margin - 1));

	total = mode->upper_margin + mode->yres + mode->lower_margin +
		mode->vsync_len;
	sunxi_io_write(lcdc, SUNXI_LCDC_TCON1_TIMING_V,
		       ((total *  2) << 16) |
		       (mode->vsync_len + mode->upper_margin - 1));

	sunxi_io_write(lcdc, SUNXI_LCDC_TCON1_TIMING_SYNC,
		       ((mode->hsync_len - 1) << 16) | (mode->vsync_len - 1));

	sunxi_lcdc_pll_set(mode->pixclock, clk_div, clk_double);
}

static void
sunxi_hdmi_mode_set(struct fb_videomode *mode, int clk_div, int clk_double)
{
	void *hdmi = (void *) SUNXI_HDMI_BASE;
	int h, v;

	sunxi_io_write(hdmi, SUNXI_HDMI_INT_CTRL, 0xFFFFFFFF);
	sunxi_io_write(hdmi, SUNXI_HDMI_VIDEO_POLARITY, 0x03e00000);
	sunxi_io_mask(hdmi, SUNXI_HDMI_TX_DRIVER0, 0xDE000000, 0xDE000000);
#ifdef CONFIG_SUN4I
	sunxi_io_write(hdmi, SUNXI_HDMI_TX_DRIVER1, 0x00D8C820);
#else
	sunxi_io_write(hdmi, SUNXI_HDMI_TX_DRIVER1, 0x00d8c830);
#endif
	sunxi_io_write(hdmi, SUNXI_HDMI_TX_DRIVER2, 0xfa4ef708);
	sunxi_io_write(hdmi, SUNXI_HDMI_TX_DRIVER3, 0);

	/* Use PLL3, setup clk div and doubler */
	sunxi_io_mask(hdmi, SUNXI_HDMI_TX_DRIVER3, 0, 0x00200000);
	sunxi_io_mask(hdmi, SUNXI_HDMI_TX_DRIVER2, clk_div << 4, 0xf0);
	if (clk_double)
		sunxi_io_mask(hdmi, SUNXI_HDMI_TX_DRIVER1, 0, 0x40);
	else
		sunxi_io_mask(hdmi, SUNXI_HDMI_TX_DRIVER1, 0x40, 0x40);

	sunxi_io_write(hdmi, SUNXI_HDMI_VIDEO_SIZE,
		       ((mode->yres - 1) << 16) | (mode->xres - 1));

	h = mode->hsync_len + mode->left_margin;
	v = mode->vsync_len + mode->upper_margin;
	sunxi_io_write(hdmi, SUNXI_HDMI_VIDEO_BP, ((v - 1) << 16) | (h - 1));

	h = mode->right_margin;
	v = mode->lower_margin;
	sunxi_io_write(hdmi, SUNXI_HDMI_VIDEO_FP, ((v - 1) << 16) | (h - 1));

	h = mode->hsync_len;
	v = mode->vsync_len;
	sunxi_io_write(hdmi, SUNXI_HDMI_VIDEO_SPW, ((v - 1) << 16) | (h - 1));

	if (mode->sync & FB_SYNC_HOR_HIGH_ACT)
		sunxi_io_mask(hdmi, SUNXI_HDMI_VIDEO_POLARITY, 0x01, 0x01);
	else
		sunxi_io_mask(hdmi, SUNXI_HDMI_VIDEO_POLARITY, 0, 0x01);

	if (mode->sync & FB_SYNC_VERT_HIGH_ACT)
		sunxi_io_mask(hdmi, SUNXI_HDMI_VIDEO_POLARITY, 0x02, 0x02);
	else
		sunxi_io_mask(hdmi, SUNXI_HDMI_VIDEO_POLARITY, 0, 0x02);
}

static void
sunxi_engines_init(void)
{
	sunxi_composer_init();
	sunxi_lcdc_init();
}

static void
sunxi_mode_set(struct fb_videomode *mode, unsigned int address)
{
	void *composer = (void *) SUNXI_DE_BE0_BASE;
	void *lcdc = (void *) SUNXI_LCD0_BASE;
	void *hdmi = (void *) SUNXI_HDMI_BASE;
	int clk_div, clk_double;

	sunxi_io_mask(hdmi, SUNXI_HDMI_VIDEO_CTRL, 0, 0x80000000);
	sunxi_io_mask(lcdc, SUNXI_LCDC_ENABLE, 0, 0x80000000);
	sunxi_io_mask(composer, SUNXI_COMP_MODE, 0, 0x02);

	sunxi_composer_mode_set(mode, address);
	sunxi_lcdc_mode_set(mode, &clk_div, &clk_double);
	sunxi_hdmi_mode_set(mode, clk_div, clk_double);

	sunxi_io_mask(composer, SUNXI_COMP_REG_CTL, 0x01, 0x01);
	sunxi_io_mask(composer, SUNXI_COMP_MODE, 0x02, 0x02);

	sunxi_io_mask(lcdc, SUNXI_LCDC_ENABLE, 0x80000000, 0x80000000);
	sunxi_io_mask(lcdc, SUNXI_LCDC_TCON1_IO_TRI, 0x00000000, 0x03000000);

	udelay(100);

	sunxi_io_mask(hdmi, SUNXI_HDMI_VIDEO_CTRL, 0x80000000, 0x80000000);
}

void *
video_hw_init(void)
{
	static GraphicDevice *graphic_device = sunxi_display->graphic_device;
#if 1
	/*
	 * Vesa standard 1024x768@60
	 * 65.0  1024 1032 1176 1344  768 771 777 806  -hsync -vsync
	 */
	struct fb_videomode mode = {
		.name = "1024x768",
		.refresh = 60,
		.xres = 1024,
		.yres = 768,
		.pixclock = 65000,
		.left_margin = 160,
		.right_margin = 24,
		.upper_margin = 29,
		.lower_margin = 3,
		.hsync_len = 136,
		.vsync_len = 6,
		.sync = 0,
		.vmode = 0,
		.flag = 0,
	};
#else
	/* 1920x1080@60 */
	struct fb_videomode mode = {
		.name = "1920x1080",
		.refresh = 60,
		.xres = 1920,
		.yres = 1080,
		.pixclock = 148500,
		.left_margin = 88,
		.right_margin = 148,
		.upper_margin = 36,
		.lower_margin = 4,
		.hsync_len = 44,
		.vsync_len = 5,
		.sync = FB_SYNC_HOR_HIGH_ACT | FB_SYNC_VERT_HIGH_ACT,
		.vmode = FB_VMODE_NONINTERLACED,
		.flag = 0,
	};
#endif
	int ret;

	memset(sunxi_display, 0, sizeof(struct sunxi_display));

	printf("Reserved %dkB of RAM for Framebuffer.\n",
	       CONFIG_SUNXI_FB_SIZE >> 10);
	gd->fb_base = gd->ram_top;

	ret = sunxi_hdmi_hpd_detect();
	if (!ret)
		return NULL;

	printf("HDMI connected.\n");
	sunxi_display->enabled = 1;

	printf("Setting up a %s console.\n", mode.name);
	sunxi_engines_init();
	sunxi_mode_set(&mode, gd->fb_base - CONFIG_SYS_SDRAM_BASE);

	/*
	 * These are the only members of this structure that are used. All the
	 * others are driver specific. There is nothing to decribe pitch or
	 * stride, but we are lucky with our hw.
	 */
	graphic_device->frameAdrs = gd->fb_base;
	graphic_device->gdfIndex = GDF_32BIT_X888RGB;
	graphic_device->gdfBytesPP = 4;
	graphic_device->winSizeX = mode.xres;
	graphic_device->winSizeY = mode.yres;

	return graphic_device;
}

/*
 * Simplefb support.
 */
#if defined(CONFIG_OF_BOARD_SETUP) && defined(CONFIG_VIDEO_DT_SIMPLEFB)
static void
sunxi_simplefb_clocks(void *blob, int node_simplefb)
{
	const char *compatible[] = {
		"allwinner,sun4i-a10-ahb-gates-clk",
		"allwinner,sun5i-a10s-ahb-gates-clk",
		"allwinner,sun5i-a13-ahb-gates-clk",
		"allwinner,sun7i-a20-ahb-gates-clk",
		NULL,
	};
	/*
	 * This currently ignores standalone clocks, like pll3/7, as these
	 * are still ignored in the dts files.
	 */
#define PLACEHOLDER_AHB_GATES 0xFFFFFFFF
	fdt32_t cells[] = {
		PLACEHOLDER_AHB_GATES, fdt32_to_cpu(0x24), /* ahb_lcd0 */
		PLACEHOLDER_AHB_GATES, fdt32_to_cpu(0x2B), /* ahb_hdmi */
		PLACEHOLDER_AHB_GATES, fdt32_to_cpu(0x2C), /* ahb_de_be0 */
	};
	const char *stringlist;
	int node_clock, i, phandle, stringlength, ret;

	/* Find the ahb_gates node. */
	for (i = 0; compatible[i]; i++) {
		node_clock =
			fdt_node_offset_by_compatible(blob, 0, compatible[i]);
		if (node_clock >= 0)
			break;
	}

	if (!compatible[i]) {
		eprintf("%s: unable to find ahb_gates device-tree node.\n",
			__func__);
		return;
	}

	/*
	 * sanity check clock-output-names.
	 *
	 * Not that this really matters as one is supposed to reference
	 * clock gating by actual register bit offsets.
	 */
	stringlist = fdt_getprop(blob, node_clock, "clock-output-names",
				 &stringlength);
	if (!stringlist) {
		eprintf("%s: unable to find clock-output-names property.\n",
			__func__);
		return;
	}

	if (!fdt_stringlist_contains(stringlist, stringlength, "ahb_de_be") &&
	    !fdt_stringlist_contains(stringlist, stringlength, "ahb_de_be0")) {
		printf("%s: unable to find ahb gating bit %s\n", __func__,
		       "ahb_de_be0");
		return;
	}

	if (!fdt_stringlist_contains(stringlist, stringlength, "ahb_lcd") &&
	    !fdt_stringlist_contains(stringlist, stringlength, "ahb_lcd0")) {
		printf("%s: unable to find ahb gating bit %s\n", __func__,
		       "ahb_lcd0");
		return;
	}

	if (!fdt_stringlist_contains(stringlist, stringlength, "ahb_hdmi") &&
	    !fdt_stringlist_contains(stringlist, stringlength, "ahb_hdmi0")) {
		printf("%s: unable to find ahb gating bit %s\n", __func__,
		       "ahb_hdmi/ahb_hdmi0");
		return;
	}

	/* Now add our actual clocks tuples. */
	phandle = fdt_get_phandle(blob, node_clock);

	for (i = 0; i < (sizeof(cells) / sizeof(*cells)); i++)
		if (cells[i] == PLACEHOLDER_AHB_GATES)
			cells[i] = fdt32_to_cpu(phandle);

	ret = fdt_setprop(blob, node_simplefb, "clocks", cells, sizeof(cells));
	if (ret)
		eprintf("%s: fdt_setprop \"clocks\" failed: %d\n",
			__func__, ret);
}

void
sunxi_simplefb_setup(void *blob)
{
	static GraphicDevice *graphic_device = sunxi_display->graphic_device;
	const char *name = "simple-framebuffer";
	const char *format = "x8r8g8b8";
	fdt32_t cells[2];
	int offset, stride, ret;

	if (!sunxi_display->enabled)
		return;

	offset = fdt_add_subnode(blob, 0, "framebuffer");
	if (offset < 0) {
		printf("%s: add subnode failed", __func__);
		return;
	}

	ret = fdt_setprop(blob, offset, "compatible", name, strlen(name) + 1);
	if (ret < 0)
		return;

	stride = graphic_device->winSizeX * graphic_device->gdfBytesPP;

	cells[0] = cpu_to_fdt32(gd->fb_base);
	cells[1] = cpu_to_fdt32(CONFIG_SUNXI_FB_SIZE);
	ret = fdt_setprop(blob, offset, "reg", cells, sizeof(cells[0]) * 2);
	if (ret < 0)
		return;

	cells[0] = cpu_to_fdt32(graphic_device->winSizeX);
	ret = fdt_setprop(blob, offset, "width", cells, sizeof(cells[0]));
	if (ret < 0)
		return;

	cells[0] = cpu_to_fdt32(graphic_device->winSizeY);
	ret = fdt_setprop(blob, offset, "height", cells, sizeof(cells[0]));
	if (ret < 0)
		return;

	cells[0] = cpu_to_fdt32(stride);
	ret = fdt_setprop(blob, offset, "stride", cells, sizeof(cells[0]));
	if (ret < 0)
		return;

	ret = fdt_setprop(blob, offset, "format", format, strlen(format) + 1);
	if (ret < 0)
		return;

	sunxi_simplefb_clocks(blob, offset);
}
#endif /* CONFIG_OF_BOARD_SETUP && CONFIG_VIDEO_DT_SIMPLEFB */
