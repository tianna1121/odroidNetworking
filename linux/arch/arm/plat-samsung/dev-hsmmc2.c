/* linux/arch/arm/plat-s3c/dev-hsmmc2.c
 *
 * Copyright (c) 2009 Samsung Electronics
 * Copyright (c) 2009 Maurus Cuelenaere
 *
 * Based on arch/arm/plat-s3c/dev-hsmmc1.c
 * original file Copyright (c) 2008 Simtec Electronics
 *
 * S3C series device definition for hsmmc device 2
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
*/

#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/mmc/host.h>

#include <mach/map.h>
#include <plat/sdhci.h>
#include <plat/devs.h>
#include <plat/gpio-cfg.h>

#define S3C_SZ_HSMMC	(0x1000)

static struct resource s3c_hsmmc2_resource[] = {
	[0] = {
		.start = S3C_PA_HSMMC2,
		.end   = S3C_PA_HSMMC2 + S3C_SZ_HSMMC - 1,
		.flags = IORESOURCE_MEM,
	},
	[1] = {
		.start = IRQ_HSMMC2,
		.end   = IRQ_HSMMC2,
		.flags = IORESOURCE_IRQ,
	}
};

static u64 s3c_device_hsmmc2_dmamask = 0xffffffffUL;

struct s3c_sdhci_platdata s3c_hsmmc2_def_platdata = {
	.max_width	= 4,
	.host_caps	= (MMC_CAP_4_BIT_DATA |
			   MMC_CAP_MMC_HIGHSPEED | MMC_CAP_SD_HIGHSPEED),
	.clk_type	= S3C_SDHCI_CLK_DIV_INTERNAL,
};

struct platform_device s3c_device_hsmmc2 = {
	.name		= "s3c-sdhci",
	.id		= 2,
	.num_resources	= ARRAY_SIZE(s3c_hsmmc2_resource),
	.resource	= s3c_hsmmc2_resource,
	.dev		= {
		.dma_mask		= &s3c_device_hsmmc2_dmamask,
		.coherent_dma_mask	= 0xffffffffUL,
		.platform_data		= &s3c_hsmmc2_def_platdata,
	},
};
// Hardkernel / ODROID
EXPORT_SYMBOL(s3c_device_hsmmc2);

void s3c_sdhci2_set_platdata(struct s3c_sdhci_platdata *pd)
{
	struct s3c_sdhci_platdata *set = &s3c_hsmmc2_def_platdata;

	set->cd_type = pd->cd_type;
	set->ext_cd_init = pd->ext_cd_init;
	set->ext_cd_cleanup = pd->ext_cd_cleanup;
	set->ext_cd_gpio = pd->ext_cd_gpio;
	/* if it uses eint as cd pin, pull up/down value of eint port
	   should be NONE */
	if (pd->ext_cd_gpio)
		s3c_gpio_setpull(pd->ext_cd_gpio, S3C_GPIO_PULL_NONE);
	set->ext_cd_gpio_invert = pd->ext_cd_gpio_invert;
	set->pm_flags = pd->pm_flags;

	if (pd->vmmc_name)
		set->vmmc_name = pd->vmmc_name;
	if (pd->max_width)
		set->max_width = pd->max_width;
	if (pd->cfg_gpio)
		set->cfg_gpio = pd->cfg_gpio;
	if (pd->cfg_card)
		set->cfg_card = pd->cfg_card;
	if (pd->host_caps)
		set->host_caps |= pd->host_caps;
	if (pd->clk_type)
		set->clk_type = pd->clk_type;
}