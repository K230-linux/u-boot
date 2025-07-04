// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2011 OMICRON electronics GmbH
 *
 * based on drivers/mtd/nand/raw/nand_spl_load.c
 *
 * Copyright (C) 2011
 * Heiko Schocher, DENX Software Engineering, hs@denx.de.
 */

#include <common.h>
#include <image.h>
#include <log.h>
#include <spi.h>
#include <spi_flash.h>
#include <errno.h>
#include <spl.h>
#include <asm/global_data.h>
#include <dm/ofnode.h>
#include <nand.h>

#if CONFIG_IS_ENABLED(OS_BOOT)
/*
 * Load the kernel, check for a valid header we can parse, and if found load
 * the kernel and then device tree.
 */
static int spi_load_image_os(struct spl_image_info *spl_image,
			     struct spl_boot_device *bootdev,
			     struct spi_flash *flash,
			     struct image_header *header)
{
	int err;
	size_t lenth;

	lenth = sizeof(*header);
	/* Read for a header, parse or error out. */
	nand_read(flash, CONFIG_SYS_SPI_KERNEL_OFFS, &lenth,
		       (void *)header);

	if (image_get_magic(header) != IH_MAGIC)
		return -1;

	err = spl_parse_image_header(spl_image, bootdev, header);
	if (err)
		return err;

	lenth = spl_image->size;
	nand_read(flash, CONFIG_SYS_SPI_KERNEL_OFFS,
		       &lenth, (void *)spl_image->load_addr);

	lenth = CONFIG_SYS_SPI_ARGS_SIZE;
	/* Read device tree. */
	nand_read(flash, CONFIG_SYS_SPI_ARGS_OFFS,
		       &lenth,
		       (void *)CONFIG_SYS_SPL_ARGS_ADDR);

	return 0;
}
#endif

static ulong spl_spi_fit_read(struct spl_load_info *load, ulong sector,
			      ulong count, void *buf)
{
	struct spi_flash *flash = load->dev;
	ulong ret;

	ret = nand_read(flash, sector, &count, buf);
	if (!ret)
		return count;
	else
		return 0;
}

unsigned int __weak spl_spi_get_uboot_offs(struct spi_flash *flash)
{
#ifndef CONFIG_SYS_SPI_U_BOOT_OFFS
	#define CONFIG_SYS_SPI_U_BOOT_OFFS 0
#endif

	return CONFIG_SYS_SPI_U_BOOT_OFFS;
}

u32 __weak spl_spi_boot_bus(void)
{
	return CONFIG_SF_DEFAULT_BUS;
}

u32 __weak spl_spi_boot_cs(void)
{
	return CONFIG_SF_DEFAULT_CS;
}

/*
 * The main entry for SPI booting. It's necessary that SDRAM is already
 * configured and available since this code loads the main U-Boot image
 * from SPI into SDRAM and starts it from there.
 */
static int spl_spinand_load_image(struct spl_image_info *spl_image,
			      struct spl_boot_device *bootdev)
{
	int err = 0;
	unsigned int payload_offs;
	struct spi_flash *flash;
	struct image_header *header;
	unsigned int sf_bus = spl_spi_boot_bus();
	unsigned int sf_cs = spl_spi_boot_cs();
	size_t lenth;

	/*
	 * Load U-Boot image from SPI flash into RAM
	 * In DM mode: defaults speed and mode will be
	 * taken from DT when available
	 */
	flash = spi_flash_probe(sf_bus, sf_cs,
				CONFIG_SF_DEFAULT_SPEED,
				CONFIG_SF_DEFAULT_MODE);
	if (!flash) {
		puts("SPI probe failed.\n");
		return -ENODEV;
	}

	payload_offs = spl_spi_get_uboot_offs(flash);

	header = spl_get_load_buffer(-sizeof(*header), sizeof(*header));

	if (CONFIG_IS_ENABLED(OF_REAL)) {
		payload_offs = ofnode_conf_read_int("u-boot,spl-payload-offset",
						    payload_offs);
	}

#if CONFIG_IS_ENABLED(OS_BOOT)
	if (spl_start_uboot() || spi_load_image_os(spl_image, bootdev, flash, header))
#endif
	{
		lenth = sizeof(*header);
		/* Load u-boot, mkimage header is 64 bytes. */
		err = nand_read(flash, payload_offs, &lenth,
				     (void *)header);
		if (err) {
			debug("%s: Failed to read from SPI flash (err=%d)\n",
			      __func__, err);
			return err;
		}

		lenth = roundup(fdt_totalsize(header), 4);
		if (IS_ENABLED(CONFIG_SPL_LOAD_FIT_FULL) &&
		    image_get_magic(header) == FDT_MAGIC) {
			err = nand_read(flash, payload_offs,
					     &lenth,
					     (void *)CONFIG_SYS_LOAD_ADDR);
			if (err)
				return err;
			err = spl_parse_image_header(spl_image, bootdev,
					(struct image_header *)CONFIG_SYS_LOAD_ADDR);
		} else if (IS_ENABLED(CONFIG_SPL_LOAD_FIT) &&
			   image_get_magic(header) == FDT_MAGIC) {
			struct spl_load_info load;

			debug("Found FIT\n");
			load.dev = flash;
			load.priv = NULL;
			load.filename = NULL;
			load.bl_len = 1;
			load.read = spl_spi_fit_read;
			err = spl_load_simple_fit(spl_image, &load,
						  payload_offs,
						  header);
		} else if (IS_ENABLED(CONFIG_SPL_LOAD_IMX_CONTAINER)) {
			struct spl_load_info load;

			load.dev = flash;
			load.priv = NULL;
			load.filename = NULL;
			load.bl_len = 1;
			load.read = spl_spi_fit_read;

			err = spl_load_imx_container(spl_image, &load,
						     payload_offs);
		} else {
			err = spl_parse_image_header(spl_image, bootdev, header);
			if (err)
				return err;
			lenth = spl_image->size;
			err = nand_read(flash, payload_offs + spl_image->offset,
					     &lenth,
					     (void *)spl_image->load_addr);
		}
		if (IS_ENABLED(CONFIG_SPI_FLASH_SOFT_RESET)) {
			err = spi_nor_remove(flash);
			if (err)
				return err;
		}
	}

	return err;
}
/* Use priorty 1 so that boards can override this */
SPL_LOAD_IMAGE_METHOD("SPINAND", 1, BOOT_DEVICE_NAND, spl_spinand_load_image);
