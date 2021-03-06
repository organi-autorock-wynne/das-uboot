/*
 * (C) Copyright 2010
 * Texas Instruments, <www.ti.com>
 *
 * Aneesh V <aneesh@ti.com>
 *
 * SPDX-License-Identifier:	GPL-2.0+
 */
#include <common.h>
#include <spl.h>
#include <asm/u-boot.h>
#include <mmc.h>
#include <version.h>
#include <image.h>
#include <fdt_support.h>

#ifdef CONFIG_SPL_SMP_BOOT
#include <asm/smp.h>
#endif

DECLARE_GLOBAL_DATA_PTR;

static int mmc_load_image_raw(struct mmc *mmc, unsigned long sector)
{
	unsigned long err;
	u32 image_size_sectors;
	struct image_header *header;

	header = (struct image_header *)(CONFIG_SYS_TEXT_BASE -
						sizeof(struct image_header));

	/* read image header to find the image size & load address */
	err = mmc->block_dev.block_read(0, sector, 1, header);
	if (err == 0)
		goto end;

	if (image_get_magic(header) != IH_MAGIC)
		return -1;

	spl_parse_image_header(header);

	/* convert size to sectors - round up */
	image_size_sectors = (spl_image.size + mmc->read_bl_len - 1) /
				mmc->read_bl_len;

	/* Read the header too to avoid extra memcpy */
	err = mmc->block_dev.block_read(0, sector, image_size_sectors,
					(void *)spl_image.load_addr);

end:
#ifdef CONFIG_SPL_LIBCOMMON_SUPPORT
	if (err == 0)
		printf("spl: mmc blk read err - %lu\n", err);
#endif

	return (err == 0);
}

#ifdef CONFIG_SPL_OS_BOOT
#if defined(CONFIG_SPL_PACKIMG)
#include <packimg.h>

int mmc_load_image_initrd(struct mmc *mmc, void *fdt)
{
#ifdef CONFIG_SYS_MMCSD_RAW_MODE_INITRD_SECTOR
	int err, do_smp_boot = 0;
	struct pack_header *ph;
	struct pack_entry *pe;

	err = mmc_load_packimg_header(mmc, CONFIG_SYS_MMCSD_RAW_MODE_INITRD_SECTOR);
	if (err < 0) {
#ifdef CONFIG_SPL_SMP_BOOT
		if (imx_get_boot_arg() == SMP_BOOT_DONE_SIGNATURE) {
			puts("abort smp boot\n");
			imx_set_boot_arg(SMP_ABORT_BOOT_SIGNATURE);
			mdelay(100);
			imx_kill_secondary();
		}
#endif
		return err;
	}

	ph = mmc_get_packimg_header();
	pe = (struct pack_entry *)(ph + 1);

#ifdef CONFIG_SPL_SMP_BOOT
	if (imx_get_boot_arg() == SMP_BOOT_DONE_SIGNATURE) {
		imx_set_boot_arg(SMP_START_LOAD_INITRD_SIGNATURE);
		do_smp_boot = 1;
	}
	else
#endif
	{
		err = mmc_load_packimg_entry(mmc, CONFIG_SYS_MMCSD_RAW_MODE_INITRD_SECTOR, pe);
		if (err < 0)
			return err;
	}

	fdt_initrd(fdt, pe->ldaddr, pe->ldaddr + pe->size);

	if (do_smp_boot) {
		ulong spl_start = CONFIG_SPL_RANGE_BEGIN;
		ulong spl_end = CONFIG_SPL_RANGE_END;

		err = fdt_add_mem_rsv(fdt, CONFIG_SPL_RANGE_BEGIN, CONFIG_SPL_RANGE_END - CONFIG_SPL_RANGE_BEGIN);
		if (err < 0)
			printf("fdt reserve %x - %x fail\n", CONFIG_SPL_RANGE_BEGIN, CONFIG_SPL_RANGE_END);

		err = fdt_set_chosen(fdt, " maxcpus=1", &spl_start, &spl_end);
		if (err < 0)
			printf("fdt change boot cpu number fail\n");
	}	

#endif /* CONFIG_SYS_MMCSD_RAW_MODE_INITRD_SECTOR */

	return 0;
}

static int mmc_load_image_raw_os(struct mmc *mmc)
{
	int err;
	void *fdt;
	struct pack_entry *fdt_pe, *kernel_pe;

	if ((err = mmc_load_packimg(mmc, CONFIG_SYS_MMCSD_RAW_MODE_PACKIMG_SECTOR)) < 0)
		return err;

	fdt_pe = mmc_get_packimg_entry_by_name(CONFIG_DEFAULT_FDT_FILE);
	kernel_pe = mmc_get_packimg_entry_by_name(CONFIG_DEFAULT_KERNEL_FILE);
	if (!fdt_pe || !kernel_pe) {
		printf("Bad FDT&kernel packimg\n");
		return -1;
	}

	if (fdt_pe->ldaddr != CONFIG_SYS_SPL_ARGS_ADDR) {
		printf("FDT address(0x%x) must be 0x%x\n", fdt_pe->ldaddr, CONFIG_SYS_SPL_ARGS_ADDR);
		return -1;
	}

	spl_image.os = IH_OS_LINUX;
	spl_image.entry_point = kernel_pe->ldaddr;

	fdt = (void *)CONFIG_SYS_SPL_ARGS_ADDR;
	fdt_open_into(fdt, fdt, fdt_totalsize(fdt) + 0x10000);
	fdt_fixup_memory(fdt, CONFIG_SYS_SDRAM_BASE, PHYS_SDRAM_SIZE);

	if ((err = mmc_load_image_initrd(mmc, fdt)) < 0)
		printf("load initrd fail %d\n", err);

	fdt_pack(fdt);

	return 0;
}
#else
static int mmc_load_image_raw_os(struct mmc *mmc)
{
	if (!mmc->block_dev.block_read(0,
				       CONFIG_SYS_MMCSD_RAW_MODE_ARGS_SECTOR,
				       CONFIG_SYS_MMCSD_RAW_MODE_ARGS_SECTORS,
				       (void *)CONFIG_SYS_SPL_ARGS_ADDR)) {
#ifdef CONFIG_SPL_LIBCOMMON_SUPPORT
		printf("mmc args blk read error\n");
#endif
		return -1;
	}

	return mmc_load_image_raw(mmc, CONFIG_SYS_MMCSD_RAW_MODE_KERNEL_SECTOR);
}
#endif	//#if defined(CONFIG_SPL_PACKIMG)
#endif

void spl_mmc_load_image(void)
{
	struct mmc *mmc;
	int err;
	u32 boot_mode;

	mmc_initialize(gd->bd);
	/* We register only one device. So, the dev id is always 0 */
	mmc = find_mmc_device(0);
	if (!mmc) {
#ifdef CONFIG_SPL_LIBCOMMON_SUPPORT
		puts("spl: mmc device not found!!\n");
#endif
		hang();
	}

	boot_mode = spl_boot_mode();
	if (boot_mode == MMCSD_MODE_EMMCBOOT) {
		mmc->bus_width = 1;
		mmc->clock = 20000000;
		err = mmc_init(mmc);
		printf("1. mmc->bus_width = %d, mmc->clock = %d \n", mmc->bus_width, mmc->clock);
		if (err) {
#ifdef CONFIG_SPL_LIBCOMMON_SUPPORT
			printf("spl: mmc init failed: err - %d\n", err);
#endif
			hang();
		}

		if (spl_start_uboot()||mmc_load_image_raw_os(mmc)){
			/*
			 * We need to check what the partition is configured to.
			 * 1 and 2 match up to boot0 / boot1 and 7 is user data
			 * which is the first physical partition (0).
			 */
			int part = (mmc->part_config >> 3) & PART_ACCESS_MASK;

			if (part == 7)
				part = 0;

			if (mmc_switch_part(0, part)) {
#ifdef CONFIG_SPL_LIBCOMMON_SUPPORT
				puts("MMC partition switch failed\n");
#endif
				hang();
			}
			err = mmc_load_image_raw(mmc,
				CONFIG_SYS_MMCSD_RAW_MODE_U_BOOT_SECTOR);
			if (err)
				hang();
		}
		return;
	}

	err = mmc_init(mmc);
	printf("2. mmc->bus_width = %d, mmc->clock = %d \n", mmc->bus_width, mmc->clock);
	if (err) {
#ifdef CONFIG_SPL_LIBCOMMON_SUPPORT
		printf("spl: mmc init failed: err - %d\n", err);
#endif
		hang();
	}

	if (boot_mode == MMCSD_MODE_RAW) {
		debug("boot mode - RAW\n");
#ifdef CONFIG_SPL_OS_BOOT
		if (spl_start_uboot() || mmc_load_image_raw_os(mmc))
#endif
		err = mmc_load_image_raw(mmc,
			CONFIG_SYS_MMCSD_RAW_MODE_U_BOOT_SECTOR);
#ifdef CONFIG_SPL_FAT_SUPPORT
	} else if (boot_mode == MMCSD_MODE_FAT) {
		debug("boot mode - FAT\n");
#ifdef CONFIG_SPL_OS_BOOT
		if (spl_start_uboot() || spl_load_image_fat_os(&mmc->block_dev,
								CONFIG_SYS_MMC_SD_FAT_BOOT_PARTITION))
#endif
		err = spl_load_image_fat(&mmc->block_dev,
					CONFIG_SYS_MMC_SD_FAT_BOOT_PARTITION,
					CONFIG_SPL_FAT_LOAD_PAYLOAD_NAME);
#endif
	} else {
#ifdef CONFIG_SPL_LIBCOMMON_SUPPORT
		puts("spl: wrong MMC boot mode\n");
#endif
		hang();
	}

	if (err)
		hang();
}
