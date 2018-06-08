/*
 * Sunxi-Cedrus VPU driver
 *
 * Copyright (C) 2018 Paul Kocialkowski <paul.kocialkowski@bootlin.com>
 * Copyright (C) 2016 Florent Revest <florent.revest@free-electrons.com>
 *
 * Based on the vim2m driver, that is:
 *
 * Copyright (c) 2009-2010 Samsung Electronics Co., Ltd.
 * Pawel Osciak, <pawel@osciak.com>
 * Marek Szyprowski, <m.szyprowski@samsung.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/platform_device.h>
#include <linux/of_reserved_mem.h>
#include <linux/dma-mapping.h>
#include <linux/mfd/syscon.h>
#include <linux/interrupt.h>
#include <linux/clk.h>
#include <linux/regmap.h>
#include <linux/reset.h>
#include <linux/soc/sunxi/sunxi_sram.h>

#include <media/videobuf2-core.h>
#include <media/v4l2-mem2mem.h>

#include "cedrus_common.h"
#include "cedrus_hw.h"
#include "cedrus_regs.h"

#define SYSCON_SRAM_CTRL_REG0	0x0
#define SYSCON_SRAM_C1_MAP_VE	0x7fffffff

int sunxi_cedrus_engine_enable(struct sunxi_cedrus_dev *dev,
			       enum sunxi_cedrus_engine engine)
{
	u32 reg = 0;

	/*
	 * FIXME: This is only valid on 32-bits DDR's, we should test
	 * it on the A13/A33.
	 */
	reg |= VE_CTRL_REC_WR_MODE_2MB;

	reg |= VE_CTRL_CACHE_BUS_BW_128;

	switch (engine) {
	case SUNXI_CEDRUS_ENGINE_MPEG:
		reg |= VE_CTRL_DEC_MODE_MPEG;
		break;

	default:
		return -EINVAL;
	}

	sunxi_cedrus_write(dev, VE_CTRL, reg);
	return 0;
}

void sunxi_cedrus_engine_disable(struct sunxi_cedrus_dev *dev)
{
	sunxi_cedrus_write(dev, VE_CTRL, VE_CTRL_DEC_MODE_DISABLED);
}

static irqreturn_t sunxi_cedrus_ve_irq(int irq, void *dev_id)
{
	struct sunxi_cedrus_dev *dev = dev_id;
	struct sunxi_cedrus_ctx *ctx;
	struct sunxi_cedrus_buffer *src_buffer, *dst_buffer;
	struct vb2_v4l2_buffer *src_vb, *dst_vb;
	unsigned long flags;
	unsigned int value, status;

	spin_lock_irqsave(&dev->irq_lock, flags);

	/* Disable MPEG interrupts and stop the MPEG engine */
	value = sunxi_cedrus_read(dev, VE_MPEG_CTRL);
	sunxi_cedrus_write(dev, VE_MPEG_CTRL, value & (~0xf));

	status = sunxi_cedrus_read(dev, VE_MPEG_STATUS);
	sunxi_cedrus_write(dev, VE_MPEG_STATUS, 0x0000c00f);
	sunxi_cedrus_engine_disable(dev);

	ctx = v4l2_m2m_get_curr_priv(dev->m2m_dev);
	if (!ctx) {
		pr_err("Instance released before the end of transaction\n");
		spin_unlock_irqrestore(&dev->irq_lock, flags);

		return IRQ_HANDLED;
	}

	src_vb = v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx);
	dst_vb = v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);

	if (!src_vb || !dst_vb) {
		pr_err("Unable to get source and/or destination buffers\n");
		spin_unlock_irqrestore(&dev->irq_lock, flags);

		return IRQ_HANDLED;
	}

	src_buffer = container_of(src_vb, struct sunxi_cedrus_buffer, vb);
	dst_buffer = container_of(dst_vb, struct sunxi_cedrus_buffer, vb);

	/* First bit of MPEG_STATUS indicates success. */
	if (ctx->job_abort || !(status & 0x01))
		src_buffer->state = dst_buffer->state = VB2_BUF_STATE_ERROR;
	else
		src_buffer->state = dst_buffer->state = VB2_BUF_STATE_DONE;

	list_add_tail(&src_buffer->list, &ctx->src_list);
	list_add_tail(&dst_buffer->list, &ctx->dst_list);

	spin_unlock_irqrestore(&dev->irq_lock, flags);

	schedule_work(&ctx->run_work);

	return IRQ_HANDLED;
}

int sunxi_cedrus_hw_probe(struct sunxi_cedrus_dev *dev)
{
	struct resource *res;
	int irq_dec;
	int ret;

	irq_dec = platform_get_irq(dev->pdev, 0);
	if (irq_dec <= 0) {
		dev_err(dev->dev, "could not get ve IRQ\n");
		return -ENXIO;
	}
	ret = devm_request_irq(dev->dev, irq_dec, sunxi_cedrus_ve_irq, 0,
			       dev_name(dev->dev), dev);
	if (ret) {
		dev_err(dev->dev, "could not request ve IRQ\n");
		return -ENXIO;
	}

	/*
	 * The VPU is only able to handle bus addresses so we have to subtract
	 * the RAM offset to the physcal addresses.
	 */
	dev->dev->dma_pfn_offset = PHYS_PFN_OFFSET;

	ret = of_reserved_mem_device_init(dev->dev);
	if (ret && ret != -ENODEV) {
		dev_err(dev->dev, "could not reserve memory\n");
		return -ENODEV;
	}

	ret = sunxi_sram_claim(dev->dev);
	if (ret) {
		dev_err(dev->dev, "couldn't map SRAM to device\n");
		return ret;
	}

	dev->ahb_clk = devm_clk_get(dev->dev, "ahb");
	if (IS_ERR(dev->ahb_clk)) {
		dev_err(dev->dev, "failed to get ahb clock\n");
		return PTR_ERR(dev->ahb_clk);
	}
	dev->mod_clk = devm_clk_get(dev->dev, "mod");
	if (IS_ERR(dev->mod_clk)) {
		dev_err(dev->dev, "failed to get mod clock\n");
		return PTR_ERR(dev->mod_clk);
	}
	dev->ram_clk = devm_clk_get(dev->dev, "ram");
	if (IS_ERR(dev->ram_clk)) {
		dev_err(dev->dev, "failed to get ram clock\n");
		return PTR_ERR(dev->ram_clk);
	}

	dev->rstc = devm_reset_control_get(dev->dev, NULL);

	res = platform_get_resource(dev->pdev, IORESOURCE_MEM, 0);
	dev->base = devm_ioremap_resource(dev->dev, res);
	if (!dev->base)
		dev_err(dev->dev, "could not maps MACC registers\n");

	dev->syscon = syscon_regmap_lookup_by_phandle(dev->dev->of_node,
						      "syscon");
	if (IS_ERR(dev->syscon)) {
		dev->syscon = NULL;
	} else {
		regmap_write_bits(dev->syscon, SYSCON_SRAM_CTRL_REG0,
				  SYSCON_SRAM_C1_MAP_VE,
				  SYSCON_SRAM_C1_MAP_VE);
	}

	ret = clk_prepare_enable(dev->ahb_clk);
	if (ret) {
		dev_err(dev->dev, "could not enable ahb clock\n");
		return -EFAULT;
	}
	ret = clk_prepare_enable(dev->mod_clk);
	if (ret) {
		clk_disable_unprepare(dev->ahb_clk);
		dev_err(dev->dev, "could not enable mod clock\n");
		return -EFAULT;
	}
	ret = clk_prepare_enable(dev->ram_clk);
	if (ret) {
		clk_disable_unprepare(dev->mod_clk);
		clk_disable_unprepare(dev->ahb_clk);
		dev_err(dev->dev, "could not enable ram clock\n");
		return -EFAULT;
	}

	ret = reset_control_reset(dev->rstc);
	if (ret) {
		clk_disable_unprepare(dev->ram_clk);
		clk_disable_unprepare(dev->mod_clk);
		clk_disable_unprepare(dev->ahb_clk);
		dev_err(dev->dev, "could not reset device\n");
		return -EFAULT;
	}

	return 0;
}

void sunxi_cedrus_hw_remove(struct sunxi_cedrus_dev *dev)
{
	reset_control_assert(dev->rstc);

	clk_disable_unprepare(dev->ram_clk);
	clk_disable_unprepare(dev->mod_clk);
	clk_disable_unprepare(dev->ahb_clk);

	sunxi_sram_release(dev->dev);
	of_reserved_mem_device_release(dev->dev);
}
