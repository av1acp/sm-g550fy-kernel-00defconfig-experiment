/*
 * Synopsys DesignWare Multimedia Card Interface driver
 *  (Based on NXP driver for lpc 31xx)
 *
 * Copyright (C) 2009 NXP Semiconductors
 * Copyright (C) 2009, 2010 Imagination Technologies Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/blkdev.h>
#include <linux/clk.h>
#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/mmc/card.h>
#include <linux/mmc/core.h>
#include <linux/mmc/host.h>
#include <linux/mmc/mmc.h>
#include <linux/mmc/sd.h>
#include <linux/mmc/sdio.h>
#include <linux/mmc/sdio_func.h>
#include <linux/mmc/sdio_ids.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/pinctrl/pinctrl.h>
#include <linux/pinctrl/consumer.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/stat.h>
#include <linux/string.h>
#include <linux/workqueue.h>

#include "dw_mmc.h"

#if defined(CONFIG_DEBUG_FS)
static int dw_mci_req_show(struct seq_file *s, void *v)
{
	struct dw_mci_slot *slot = s->private;
	struct mmc_request *mrq;
	struct mmc_command *cmd;
	struct mmc_data *data;

	/* Make sure we get a consistent snapshot of mmc_request */
	spin_lock_bh(&slot->host->lock);
	mrq = slot->mrq;

	if (mrq) {
		cmd = mrq->cmd;
		data = mrq->data;

		seq_printf(s,
			   "CMD%u(0x%x) flg %x rsp %x %x %x %x err %d\n",
			   cmd->opcode, cmd->arg, cmd->flags,
			   cmd->resp[0], cmd->resp[1], cmd->resp[2],
			   cmd->resp[2], cmd->error);
		if (data)
			seq_printf(s,
				   "DATA %u / %u * %u sg %d\n",
				   data->bytes_xfered, data->blocks,
				   data->blksz, data->sg_len);
	}

	spin_unlock_bh(&slot->host->lock);

	return 0;
}
MC_SEQ_FOPS(dw_mci_req_show);

static int dw_mci_regs_show(struct seq_file *s, void *v)
{
	seq_printf(s, "STATUS:\t0x%08x\n", SDMMC_STATUS);
	seq_printf(s, "RINTSTS:\t0x%08x\n", SDMMC_RINTSTS);
	seq_printf(s, "CMD:\t\t0x%08x\n", SDMMC_CMD);
	seq_printf(s, "CTRL:\t\t0x%08x\n", SDMMC_CTRL);
	seq_printf(s, "INTMASK:\t0x%08x\n", SDMMC_INTMASK);
	seq_printf(s, "CLKENA:\t\t0x%08x\n", SDMMC_CLKENA);

	return 0;
}
MC_SEQ_FOPS(dw_mci_regs_show);

static void dw_mci_init_debugfs(struct dw_mci_slot *slot)
{
	struct mmc_host *mmc = slot->mmc;
	struct dw_mci *host = slot->host;
	struct dentry *root;
	struct dentry *node;

	root = debugfs_create_dir(dev_name(&mmc->class_dev), NULL);
	if (IS_ERR(root))
		return;

	node = debugfs_create_file("regs", S_IFREG | S_IRUGO,
				       root, host, &dw_mci_regs_fops);
	if (!node)
		goto err;

	node = debugfs_create_file("req", S_IFREG | S_IRUGO,
				       root, slot, &dw_mci_req_fops);
	if (!node)
		goto err;

	slot->debugfs_root = root;

	return;

err:
	debugfs_remove_recursive(root);
}
#endif /* defined(CONFIG_DEBUG_FS) */

/* The supports_cmd23 is used to mark the difference between dw_mmc_v240a and 240a.
 * So that it is convenient to set the flag.
 */
static struct dw_mci_board *dw_mci_parse_dt(struct dw_mci *host)
{
	struct dw_mci_board *brd;
	struct device_node *np = host->dev->of_node;
	int ret;
	u32 clock_frequency;

	brd = devm_kzalloc(host->dev, sizeof(*brd), GFP_KERNEL);
	if (!brd)
		return ERR_PTR(-ENOMEM);

	ret = of_property_read_u32(np, "fifo-depth", &brd->fifo_depth);
	if (ret)
		brd->fifo_depth = 0;

	ret = of_property_read_u32(np, "card-detect-delay",
				 &brd->detect_delay_ms);
	if (ret)
		brd->detect_delay_ms = 0;

	if (of_find_property(np, "supports-highspeed", NULL))
		brd->caps |= MMC_CAP_SD_HIGHSPEED | MMC_CAP_MMC_HIGHSPEED;

	if (of_find_property(np, "caps2-do-retune", NULL))
		brd->caps2 |= MMC_CAP2_DO_RETUNE;

	if (of_find_property(np, "retry-delay", NULL))
		brd->retry = true;

	if (of_find_property(np, "dw-mshc-ctl-div", NULL))
		brd->ctl_div_16 = true;

	if (of_find_property(np, "caps2-poweroff-notify", NULL))
		brd->caps2 |= MMC_CAP2_POWEROFF_NOTIFY;

	if (of_find_property(np, "enable-sdio-wakeup", NULL))
		brd->pm_caps |= MMC_PM_KEEP_POWER | MMC_PM_WAKE_SDIO_IRQ;

	if (of_property_read_u32(np, "clock-frequency", &clock_frequency))
		clock_frequency = 0;
	brd->bus_hz = clock_frequency;

	return brd;
}

static int dw_mci_idmac_init(struct dw_mci *host)
{
	int i;

	/* Initialize IDMA controller */
	if (dw_mci_idmac_reset(host))
		return -ETIMEDOUT;

	/* Set the Descriptor Base Address Register */
	mci_writel(host, DBADDR, host->sg_dma);

	/* Mask out interrupts - they are enabled as needed */
	mci_writel(host, IDSTS, 0xffffffff);
	mci_writel(host, IDINTEN, 0);

	return 0;
}

int dw_mci_probe(struct dw_mci *host)
{
	int width, flags, ret = 0;
	u32 fifo_size;
	int init_slots = 0;

	if (!host->pdata) {
		host->pdata = dw_mci_parse_dt(host);
		if (IS_ERR(host->pdata))
			host->pdata = NULL;
	}

	if (host->pdata) {
		if (!host->pdata->select_slot && host->pdata->num_slots > 1) {
			dev_err(host->dev,
				"num_slots requested is more than one, "
				"but select_slot is not implemented\n");
			return -ENODEV;
		}
	}

	if (host->drv_data->init) {
		ret = host->drv_data->init(host);
		if (ret)
			return ret;
	}

	/*
	 * Attempt to detect and init the mmc card. If this fails, still boot
	 * up the host - the card might be inserted later.
	 */
	if (host->quirks & DW_MCI_QUIRK_DISABLE_SMU) {
		unsigned long irq_flags;

		spin_lock_irqsave(&host->irq_lock, irq_flags);
		/* disable all mmc interrupt first */
		mci_writel(host, INTMASK, 0);
		mci_writel(host, RTSR, 0);
		spin_unlock_irqrestore(&host->irq_lock, irq_flags);
	} else {
		unsigned long irq_flags;

		spin_lock_irqsave(&host->irq_lock, irq_flags);
		mci_writel(host, INTMASK, 0);
		mci_writel(host, INTMASK, SDMMC_INT_CMD_DONE);
		spin_unlock_irqrestore(&host->irq_lock, irq_flags);
	}

	/* We need at least one slot to succeed */
	for (i = 0; i < host->num_slots; i++)
		init_slots += dw_mci_init_slot(host, i);

	/*
	 * We now expect the mmc_host to be initialised.
	 * Start a new card detection after a 1s delay as a fallback - that way
	 * we don't have to rely on bus enumeration to add the new card to the
	 * host. Since the polling is boilerplate code that all Host Controllers
	 * could implement, let's move it into this common code and make it just
	 * configurable for those Host Controllers for which it's not working.
	 */
	if (!(host->quirks & DW_MCI_QUIRK_IDMAC_DTO))
		queue_delayed_work(host->card_workqueue, &host->card_work,
				   msecs_to_jiffies(host->pdata->detect_delay_ms));

	if (host->quirks & DW_MCI_QUIRK_NO_CARD_INIT)
		host->quirks &= ~DW_MCI_QUIRK_NO_DETECT;

	return 0;
}
EXPORT_SYMBOL(dw_mci_probe);

void dw_mci_remove(struct dw_mci *host)
{
	int i;

	mci_writel(host, RINTSTS, 0xffffffff);
	mci_writel(host, INTMASK, 0);

	for (i = 0; i < host->num_slots; i++)
		dw_mci_cleanup_slot(host->slot[i], i);

	/* unregister interrupt - just to be safe */
	free_irq(host->irq, host);

	destroy_workqueue(host->card_workqueue);
}
EXPORT_SYMBOL(dw_mci_remove);

#ifdef CONFIG_PM
int dw_mci_suspend(struct dw_mci *host)
{
	return 0;
}
EXPORT_SYMBOL(dw_mci_suspend);

int dw_mci_resume(struct dw_mci *host)
{
	int i, ret = 0;

	for (i = 0; i < host->num_slots; i++) {
		struct dw_mci_slot *slot = host->slot[i];
		if (!slot)
			continue;

		ret = mmc_resume_host(slot->mmc);
		if (ret < 0)
			return ret;
	}

	return ret;
}
EXPORT_SYMBOL(dw_mci_resume);
#endif /* CONFIG_PM */

static int __init dw_mci_init(void)
{
	return 0;
}

static void __exit dw_mci_exit(void)
{
}

module_init(dw_mci_init);
module_exit(dw_mci_exit);

MODULE_DESCRIPTION("DW Multimedia Card Interface driver");
MODULE_AUTHOR("NXP Semiconductor VietNam");
MODULE_AUTHOR("Imagination Technologies Ltd");
MODULE_LICENSE("GPL v2");
