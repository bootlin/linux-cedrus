// SPDX-License-Identifier: GPL-2.0
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
 */

#include <linux/platform_device.h>
#include <linux/of_reserved_mem.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/clk.h>
#include <linux/regmap.h>
#include <linux/reset.h>
#include <linux/soc/sunxi/sunxi_sram.h>

#include <media/videobuf2-core.h>
#include <media/v4l2-mem2mem.h>

#include "cedrus.h"
#include "cedrus_hw.h"
#include "cedrus_regs.h"

int cedrus_engine_enable(struct cedrus_dev *dev, enum cedrus_codec codec)
{
	u32 reg = 0;

	/*
	 * FIXME: This is only valid on 32-bits DDR's, we should test
	 * it on the A13/A33.
	 */
	reg |= VE_CTRL_REC_WR_MODE_2MB;

	reg |= VE_CTRL_CACHE_BUS_BW_128;

	switch (codec) {
	case CEDRUS_CODEC_MPEG2:
		reg |= VE_CTRL_DEC_MODE_MPEG;
		break;

	case CEDRUS_CODEC_H264:
		reg |= VE_CTRL_DEC_MODE_H264;
		break;

	default:
		return -EINVAL;
	}

	cedrus_write(dev, VE_CTRL, reg);

	return 0;
}

void cedrus_engine_disable(struct cedrus_dev *dev)
{
	cedrus_write(dev, VE_CTRL, VE_CTRL_DEC_MODE_DISABLED);
}

static irqreturn_t cedrus_bh(int irq, void *data)
{
	struct cedrus_dev *dev = data;
	struct cedrus_ctx *ctx;

	ctx = v4l2_m2m_get_curr_priv(dev->m2m_dev);
	if (!ctx) {
		v4l2_err(&dev->v4l2_dev,
			 "Instance released before the end of transaction\n");
		return IRQ_HANDLED;
	}

	v4l2_m2m_job_finish(ctx->dev->m2m_dev, ctx->fh.m2m_ctx);

	return IRQ_HANDLED;
}

static irqreturn_t cedrus_irq(int irq, void *data)
{
	struct cedrus_dev *dev = data;
	struct cedrus_ctx *ctx;
	struct vb2_v4l2_buffer *src_vb, *dst_vb;
	enum cedrus_irq_status status;
	enum vb2_buffer_state state;
	unsigned long flags;

	spin_lock_irqsave(&dev->irq_lock, flags);

	ctx = v4l2_m2m_get_curr_priv(dev->m2m_dev);
	if (!ctx) {
		v4l2_err(&dev->v4l2_dev,
			 "Instance released before the end of transaction\n");
		spin_unlock_irqrestore(&dev->irq_lock, flags);

		return IRQ_NONE;
	}

	status = dev->dec_ops[ctx->current_codec]->irq_status(ctx);
	if (status == CEDRUS_IRQ_NONE) {
		spin_unlock_irqrestore(&dev->irq_lock, flags);
		return IRQ_NONE;
	}

	dev->dec_ops[ctx->current_codec]->irq_disable(ctx);
	dev->dec_ops[ctx->current_codec]->irq_clear(ctx);

	src_vb = v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx);
	dst_vb = v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);

	if (!src_vb || !dst_vb) {
		v4l2_err(&dev->v4l2_dev,
			 "Missing source and/or destination buffers\n");
		spin_unlock_irqrestore(&dev->irq_lock, flags);

		return IRQ_HANDLED;
	}

	if (ctx->job_abort || status == CEDRUS_IRQ_ERROR)
		state = VB2_BUF_STATE_ERROR;
	else
		state = VB2_BUF_STATE_DONE;

	v4l2_m2m_buf_done(src_vb, state);
	v4l2_m2m_buf_done(dst_vb, state);

	spin_unlock_irqrestore(&dev->irq_lock, flags);

	return IRQ_WAKE_THREAD;
}

int cedrus_hw_probe(struct cedrus_dev *dev)
{
	struct resource *res;
	int irq_dec;
	int ret;

	irq_dec = platform_get_irq(dev->pdev, 0);
	if (irq_dec <= 0) {
		v4l2_err(&dev->v4l2_dev, "Failed to get IRQ\n");
		return -ENXIO;
	}
	ret = devm_request_threaded_irq(dev->dev, irq_dec,
					cedrus_irq, cedrus_bh,
					0, dev_name(dev->dev), dev);
	if (ret) {
		v4l2_err(&dev->v4l2_dev, "Failed to request IRQ\n");
		return -ENXIO;
	}

	/*
	 * The VPU is only able to handle bus addresses so we have to subtract
	 * the RAM offset to the physcal addresses.
	 */
	dev->dev->dma_pfn_offset = PHYS_PFN_OFFSET;

	ret = of_reserved_mem_device_init(dev->dev);
	if (ret && ret != -ENODEV) {
		v4l2_err(&dev->v4l2_dev, "Failed to reserved memory\n");
		return -ENODEV;
	}

	ret = sunxi_sram_claim(dev->dev);
	if (ret) {
		v4l2_err(&dev->v4l2_dev, "Failed to claim SRAM\n");
		goto err_mem;
	}

	dev->ahb_clk = devm_clk_get(dev->dev, "ahb");
	if (IS_ERR(dev->ahb_clk)) {
		v4l2_err(&dev->v4l2_dev, "Failed to get AHB clock\n");

		ret = PTR_ERR(dev->ahb_clk);
		goto err_sram;
	}

	dev->mod_clk = devm_clk_get(dev->dev, "mod");
	if (IS_ERR(dev->mod_clk)) {
		v4l2_err(&dev->v4l2_dev, "Failed to get MOD clock\n");

		ret = PTR_ERR(dev->mod_clk);
		goto err_sram;
	}

	dev->ram_clk = devm_clk_get(dev->dev, "ram");
	if (IS_ERR(dev->ram_clk)) {
		v4l2_err(&dev->v4l2_dev, "Failed to get RAM clock\n");

		ret = PTR_ERR(dev->ram_clk);
		goto err_sram;
	}

	dev->rstc = devm_reset_control_get(dev->dev, NULL);
	if (IS_ERR(dev->rstc)) {
		v4l2_err(&dev->v4l2_dev, "Failed to get reset control\n");

		ret = PTR_ERR(dev->rstc);
		goto err_sram;
	}

	res = platform_get_resource(dev->pdev, IORESOURCE_MEM, 0);
	dev->base = devm_ioremap_resource(dev->dev, res);
	if (!dev->base) {
		v4l2_err(&dev->v4l2_dev, "Failed to map registers\n");

		ret = -EFAULT;
		goto err_sram;
	}

	ret = clk_set_rate(dev->mod_clk, CEDRUS_CLOCK_RATE_DEFAULT);
	if (ret) {
		v4l2_err(&dev->v4l2_dev, "Failed to set clock rate\n");
		goto err_sram;
	}

	ret = clk_prepare_enable(dev->ahb_clk);
	if (ret) {
		v4l2_err(&dev->v4l2_dev, "Failed to enable AHB clock\n");

		ret = -EFAULT;
		goto err_sram;
	}

	ret = clk_prepare_enable(dev->mod_clk);
	if (ret) {
		v4l2_err(&dev->v4l2_dev, "Failed to enable MOD clock\n");

		ret = -EFAULT;
		goto err_ahb_clk;
	}

	ret = clk_prepare_enable(dev->ram_clk);
	if (ret) {
		v4l2_err(&dev->v4l2_dev, "Failed to enable RAM clock\n");

		ret = -EFAULT;
		goto err_mod_clk;
	}

	ret = reset_control_reset(dev->rstc);
	if (ret) {
		v4l2_err(&dev->v4l2_dev, "Failed to apply reset\n");

		ret = -EFAULT;
		goto err_ram_clk;
	}

	return 0;

err_ram_clk:
	clk_disable_unprepare(dev->ram_clk);
err_mod_clk:
	clk_disable_unprepare(dev->mod_clk);
err_ahb_clk:
	clk_disable_unprepare(dev->ahb_clk);
err_sram:
	sunxi_sram_release(dev->dev);
err_mem:
	of_reserved_mem_device_release(dev->dev);

	return ret;
}

void cedrus_hw_remove(struct cedrus_dev *dev)
{
	reset_control_assert(dev->rstc);

	clk_disable_unprepare(dev->ram_clk);
	clk_disable_unprepare(dev->mod_clk);
	clk_disable_unprepare(dev->ahb_clk);

	sunxi_sram_release(dev->dev);

	of_reserved_mem_device_release(dev->dev);
}
