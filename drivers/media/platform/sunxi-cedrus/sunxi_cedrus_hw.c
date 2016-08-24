/*
 * Sunxi Cedrus codec driver
 *
 * Copyright (C) 2016 Florent Revest
 * Florent Revest <florent.revest@free-electrons.com>
 *
 * Based on vim2m
 *
 * Copyright (c) 2009-2010 Samsung Electronics Co., Ltd.
 * Pawel Osciak, <pawel@osciak.com>
 * Marek Szyprowski, <m.szyprowski@samsung.com>
 *
 * And reverse engineering efforts of the 'Cedrus' project
 * Copyright (c) 2013-2014 Jens Kuske <jenskuske@gmail.com>
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

#include "sunxi_cedrus_common.h"

#include <linux/clk.h>
#include <linux/interrupt.h>
#include <linux/dma-mapping.h>
#include <linux/platform_device.h>
#include <linux/of_reserved_mem.h>
#include <linux/regmap.h>
#include <linux/reset.h>

#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-core.h>

#define SYSCON_SRAM_CTRL_REG0	0x0
#define SYSCON_SRAM_C1_MAP_VE	0x7fffffff

/*
 * Interrupt handlers.
 */

static irqreturn_t sunxi_cedrus_ve_irq(int irq, void *dev_id)
{
	struct sunxi_cedrus_dev *vpu = dev_id;
	struct sunxi_cedrus_ctx *curr_ctx;
	struct vb2_v4l2_buffer *src_vb, *dst_vb;
	struct media_request *req;
	int val;
	unsigned long flags;

	/* Disable MPEG interrupts and stop the MPEG engine */
	val = sunxi_cedrus_read(vpu, VE_MPEG_CTRL);
	sunxi_cedrus_write(vpu, val & (~0xf), VE_MPEG_CTRL);
	val = sunxi_cedrus_read(vpu, VE_MPEG_STATUS);
	sunxi_cedrus_write(vpu, 0x0000c00f, VE_MPEG_STATUS);
	sunxi_cedrus_write(vpu, VE_CTRL_REINIT, VE_CTRL);

	curr_ctx = v4l2_m2m_get_curr_priv(vpu->m2m_dev);

	if (!curr_ctx) {
		pr_err("Instance released before the end of transaction\n");
		return IRQ_HANDLED;
	}

	src_vb = v4l2_m2m_src_buf_remove(curr_ctx->fh.m2m_ctx);
	dst_vb = v4l2_m2m_dst_buf_remove(curr_ctx->fh.m2m_ctx);
	req = src_vb->vb2_buf.request;

	/* First bit of MPEG_STATUS means success */
	spin_lock_irqsave(&vpu->irqlock, flags);
	if (val & 0x1) {
		v4l2_m2m_buf_done(src_vb, VB2_BUF_STATE_DONE);
		v4l2_m2m_buf_done(dst_vb, VB2_BUF_STATE_DONE);
	} else {
		v4l2_m2m_buf_done(src_vb, VB2_BUF_STATE_ERROR);
		v4l2_m2m_buf_done(dst_vb, VB2_BUF_STATE_ERROR);
	}
	spin_unlock_irqrestore(&vpu->irqlock, flags);

	if (req)
		media_request_entity_complete(req, &curr_ctx->req_entity.base);

	v4l2_m2m_job_finish(vpu->m2m_dev, curr_ctx->fh.m2m_ctx);

	return IRQ_HANDLED;
}

/*
 * Initialization/clean-up.
 */

int sunxi_cedrus_hw_probe(struct sunxi_cedrus_dev *vpu)
{
	struct resource *res;
	int irq_dec;
	int ret;

	irq_dec = platform_get_irq(vpu->pdev, 0);
	if (irq_dec <= 0) {
		dev_err(vpu->dev, "could not get ve IRQ\n");
		return -ENXIO;
	}
	ret = devm_request_irq(vpu->dev, irq_dec, sunxi_cedrus_ve_irq, 0,
			       dev_name(vpu->dev), vpu);
	if (ret) {
		dev_err(vpu->dev, "could not request ve IRQ\n");
		return -ENXIO;
	}

	ret = of_reserved_mem_device_init(vpu->dev);
	if (ret) {
		dev_err(vpu->dev, "could not reserve memory\n");
		return -ENODEV;
	}

	vpu->ahb_clk = devm_clk_get(vpu->dev, "ahb");
	if (IS_ERR(vpu->ahb_clk)) {
		dev_err(vpu->dev, "failed to get ahb clock\n");
		return PTR_ERR(vpu->ahb_clk);
	}
	vpu->mod_clk = devm_clk_get(vpu->dev, "mod");
	if (IS_ERR(vpu->mod_clk)) {
		dev_err(vpu->dev, "failed to get mod clock\n");
		return PTR_ERR(vpu->mod_clk);
	}
	vpu->ram_clk = devm_clk_get(vpu->dev, "ram");
	if (IS_ERR(vpu->ram_clk)) {
		dev_err(vpu->dev, "failed to get ram clock\n");
		return PTR_ERR(vpu->ram_clk);
	}

	vpu->rstc = devm_reset_control_get(vpu->dev, NULL);

	res = platform_get_resource(vpu->pdev, IORESOURCE_MEM, 0);
	vpu->base = devm_ioremap_resource(vpu->dev, res);
	if (!vpu->base)
		dev_err(vpu->dev, "could not maps MACC registers\n");

	vpu->syscon = syscon_regmap_lookup_by_phandle(vpu->dev->of_node,
						      "syscon");
	if (IS_ERR(vpu->syscon)) {
		vpu->syscon = NULL;
	} else {
		regmap_write_bits(vpu->syscon, SYSCON_SRAM_CTRL_REG0,
				  SYSCON_SRAM_C1_MAP_VE,
				  SYSCON_SRAM_C1_MAP_VE);
	}

	ret = clk_prepare_enable(vpu->ahb_clk);
	if (ret) {
		dev_err(vpu->dev, "could not enable ahb clock\n");
		return -EFAULT;
	}
	ret = clk_prepare_enable(vpu->mod_clk);
	if (ret) {
		clk_disable_unprepare(vpu->ahb_clk);
		dev_err(vpu->dev, "could not enable mod clock\n");
		return -EFAULT;
	}
	ret = clk_prepare_enable(vpu->ram_clk);
	if (ret) {
		clk_disable_unprepare(vpu->mod_clk);
		clk_disable_unprepare(vpu->ahb_clk);
		dev_err(vpu->dev, "could not enable ram clock\n");
		return -EFAULT;
	}

	reset_control_assert(vpu->rstc);
	reset_control_deassert(vpu->rstc);

	return 0;
}

void sunxi_cedrus_hw_remove(struct sunxi_cedrus_dev *vpu)
{
	clk_disable_unprepare(vpu->ram_clk);
	clk_disable_unprepare(vpu->mod_clk);
	clk_disable_unprepare(vpu->ahb_clk);

	of_reserved_mem_device_release(vpu->dev);
}
