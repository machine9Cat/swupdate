/*
 * (C) Copyright 2014-2016
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 *
 * Hamming code from
 * https://github.com/martinezjavier/writeloader
 * Copyright (C) 2011 ISEE 2007, SL
 * Author: Javier Martinez Canillas <martinez.javier@gmail.com>
 * Author: Agusti Fontquerni Gorchs <afontquerni@iseebcn.com>
 * Overview:
 *   Writes a loader binary to a NAND flash memory device and calculates
 *   1-bit Hamming ECC codes to fill the MTD's out-of-band (oob) area
 *   independently of the ECC technique implemented on the NAND driver.
 *   This is a workaround required for TI ARM OMAP DM3730 ROM boot to load.
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdbool.h>
#include <errno.h>
#include <linux/version.h>
#include <sys/ioctl.h>

#include <mtd/mtd-user.h>
#include "swupdate.h"
#include "handler.h"
#include "util.h"
#include "flash.h"
#include "progress.h"

#define PROCMTD	"/proc/mtd"
#define LINESIZE	80

void flash_handler(void);

/* Check whether buffer is filled with character 'pattern' */
static inline int buffer_check_pattern(unsigned char *buffer, size_t size,
                                       unsigned char pattern)
{
        /* Invalid input */
        if (!buffer || (size == 0))
                return 0;

        /* No match on first byte */
        if (*buffer != pattern)
                return 0;

        /* First byte matched and buffer is 1 byte long, OK. */
        if (size == 1)
                return 1;

        /*
         * Check buffer longer than 1 byte. We already know that buffer[0]
         * matches the pattern, so the test below only checks whether the
         * buffer[0...size-2] == buffer[1...size-1] , which is a test for
         * whether the buffer is filled with constant value.
         */
        return !memcmp(buffer, buffer + 1, size - 1);
}

struct mtd_out {
	int fd;
	struct mtd_dev_info *mtd;
	struct flash_description *flash;
	void *buf;
	int buf_max;
	int buf_pos;
	int eb_pos;
};

static int nand_write_eb(struct mtd_out *mo)
{
	int ret = 0;
	struct mtd_dev_info *mtd = mo->mtd;
	struct flash_description *flash = mo->flash;
	int eb_pos = mo->eb_pos;
	int fill_sz;

	while (1) {
		/*find an good eb*/
		while (eb_pos <= mtd->size / mtd->eb_size) {
			ret = mtd_is_bad(mtd, mo->fd, eb_pos);
			if (ret == 0)
				break;
			if (ret < 0) {
				ERROR("mtd%d: MTD get bad block failed", mtd->mtd_num);
				goto exit;
			}
			/*ret=1 bad block*/
			eb_pos++;
		}
		/* if all 'ff' no need write*/
		if (buffer_check_pattern(mo->buf, mo->buf_pos, 0xff)) {
			ret = 0;
			break;
		}
		/* if write size uneq n*mtd->min_io_size need fill*/
		fill_sz = mo->buf_pos % mtd->min_io_size;
		if (fill_sz > 0) {
			fill_sz = mtd->min_io_size - fill_sz;
			memset(mo->buf + mo->buf_pos, 0xff, fill_sz);
			mo->buf_pos += fill_sz;
		}

		/* Write out data to flash */
		ret = mtd_write(flash->libmtd, mtd, mo->fd, eb_pos,
						0, mo->buf, mo->buf_pos, NULL, 0, MTD_OPS_PLACE_OOB);
		if (ret == 0) {
			break;
		}
		/* err do*/
		if (errno != EIO) {
			ERROR("mtd%d: MTD write failure", mtd->mtd_num);
			goto exit;
		}
		ret = mtd_erase(flash->libmtd, mtd, mo->fd, eb_pos);
		if (ret < 0) {
			int errno_tmp = errno;
			TRACE("mtd%d: MTD Erase failure", mtd->mtd_num);
			if (errno_tmp != EIO)
				goto exit;
		}

		TRACE("Marking block at %08x bad", eb_pos * mtd->eb_size);
		ret = mtd_mark_bad(mtd, mo->fd, eb_pos);
		if (ret < 0) {
			ERROR("mtd%d: MTD Mark bad block failure", mtd->mtd_num);
			goto exit;
		}

		eb_pos += 1;
	}

exit:
	mo->eb_pos = eb_pos;
	return ret;
}

static int mtd_out_fun(void *out, const void *buf, unsigned int len)
{
	int ret;
	struct mtd_out *mo = (struct mtd_out *)out;
	int cp_len, cp_pos;
	char *dstbuf;
	char *srcbuf;

	/* if len=0 its the last package write to and end. */
	if (len == 0) {
		if (mo->buf_pos == 0)
			return 0;
		ret = nand_write_eb(mo);
		return ret;
	}
	/* copy and write */
	cp_pos = 0;
	while (len > 0) {
		/* copy to buf */
		if (mo->buf_pos + len > mo->buf_max)
			cp_len = mo->buf_max - mo->buf_pos;
		else
			cp_len = len;

		dstbuf = mo->buf;
		srcbuf = (char *)buf;
		memcpy(&dstbuf[mo->buf_pos], &srcbuf[cp_pos], cp_len);
		len -= cp_len;
		cp_pos += cp_len;
		mo->buf_pos += cp_len;
		/* need write to */
		if (mo->buf_pos == mo->buf_max) {
			ret = nand_write_eb(mo);
			if (ret != 0) {
				return -1;
			}
			mo->buf_pos = 0;
			mo->eb_pos += 1;
		}
	}

	return 0;
}

static int flash_write_nand(int mtdnum, struct img_type *img)
{
	int ret;
	char mtd_device[LINESIZE];
	struct mtd_out out;
	struct flash_description *flash = get_flash_info();
	struct mtd_dev_info *mtd = &flash->mtd_info[mtdnum].mtd;

	/*
	 * if nothing to do, returns without errors
	 */
	if (!img->size)
		return 0;

	if (img->size > mtd->size) {
		ERROR("Image %s does not fit into mtd%d", img->fname, mtdnum);
		return -EIO;
	}
    /* erase mtd must */
    if(flash_erase(mtdnum)) {
		ERROR("I cannot erasing %s",
			img->device);
		return -1;
	}

	memset(&out, 0, sizeof(out));

	out.buf_max = mtd->eb_size / mtd->min_io_size * mtd->min_io_size;
	out.buf = calloc(1, out.buf_max);
	if (out.buf == NULL) {
		ERROR("%s: calloc mem err: %s", __func__, strerror(errno));
		return -ENOMEM;
	}

	snprintf(mtd_device, sizeof(mtd_device), "/dev/mtd%d", mtdnum);
	if ((out.fd = open(mtd_device, O_RDWR)) < 0) {
		ERROR("%s: %s: %s", __func__, mtd_device, strerror(errno));
		free(out.buf);
		return -ENODEV;
	}

	out.mtd = mtd;
	out.flash = flash;
	ret = copyimage(&out, img, mtd_out_fun);
	if (ret < 0) {
		goto exit;
	}
	/* endof write call writeimage,buf=NULL len=0 */
	ret = mtd_out_fun(&out, NULL, 0);
exit:
	if (ret < 0) {
		ERROR("Installing image %s into mtd%d failed",
			  img->fname,
			  mtdnum);
		return -1;
	}

	free(out.buf);
	close(out.fd);
	return 0;
}

static int flash_write_nor(int mtdnum, struct img_type *img)
{
	int fdout;
	char mtd_device[LINESIZE];
	int ret;
	struct flash_description *flash = get_flash_info();

	if  (!mtd_dev_present(flash->libmtd, mtdnum)) {
		ERROR("MTD %d does not exist", mtdnum);
		return -ENODEV;
	}

	if(flash_erase_sector(mtdnum, img->offset, img->size)) {
		ERROR("I cannot erasing %s",
			img->device);
		return -1;
	}

	snprintf(mtd_device, sizeof(mtd_device), "/dev/mtd%d", mtdnum);
	if ((fdout = open(mtd_device, O_RDWR)) < 0) {
		ERROR( "%s: %s: %s", __func__, mtd_device, strerror(errno));
		return -1;
	}

	ret = copyimage(&fdout, img, NULL);

	/* tell 'nbytes == 0' (EOF) from 'nbytes < 0' (read error) */
	if (ret < 0) {
		ERROR("Failure installing into: %s", img->device);
		return -1;
	}
	close(fdout);
	return 0;
}

static int flash_write_image(int mtdnum, struct img_type *img)
{
	struct flash_description *flash = get_flash_info();

	if (!isNand(flash, mtdnum))
		return flash_write_nor(mtdnum, img);
	else
		return flash_write_nand(mtdnum, img);
}

static int install_flash_image(struct img_type *img,
	void __attribute__ ((__unused__)) *data)
{
	int mtdnum;

	if (strlen(img->mtdname))
		mtdnum = get_mtd_from_name(img->mtdname);
	else
		mtdnum = get_mtd_from_device(img->device);
	if (mtdnum < 0) {
		ERROR("Wrong MTD device in description: %s",
			strlen(img->mtdname) ? img->mtdname : img->device);
		return -1;
	}

	TRACE("Copying %s into /dev/mtd%d", img->fname, mtdnum);
	if (flash_write_image(mtdnum, img)) {
		ERROR("I cannot copy %s into %s partition",
			img->fname,
			img->device);
		return -1;
	}

	return 0;
}

__attribute__((constructor))
void flash_handler(void)
{
	register_handler("flash", install_flash_image,
				IMAGE_HANDLER | FILE_HANDLER, NULL);
}
