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

#include <media/videobuf2-dma-contig.h>

#include "cedrus_common.h"
#include "cedrus_hw.h"
#include "cedrus_regs.h"

static const u8 mpeg_default_intra_quant[64] = {
	 8, 16, 16, 19, 16, 19, 22, 22,
	22, 22, 22, 22, 26, 24, 26, 27,
	27, 27, 26, 26, 26, 26, 27, 27,
	27, 29, 29, 29, 34, 34, 34, 29,
	29, 29, 27, 27, 29, 29, 32, 32,
	34, 34, 37, 38, 37, 35, 35, 34,
	35, 38, 38, 40, 40, 40, 48, 48,
	46, 46, 56, 56, 58, 69, 69, 83
};

#define m_iq(i) (((64 + i) << 8) | mpeg_default_intra_quant[i])

static const u8 mpeg_default_non_intra_quant[64] = {
	16, 16, 16, 16, 16, 16, 16, 16,
	16, 16, 16, 16, 16, 16, 16, 16,
	16, 16, 16, 16, 16, 16, 16, 16,
	16, 16, 16, 16, 16, 16, 16, 16,
	16, 16, 16, 16, 16, 16, 16, 16,
	16, 16, 16, 16, 16, 16, 16, 16,
	16, 16, 16, 16, 16, 16, 16, 16,
	16, 16, 16, 16, 16, 16, 16, 16
};

#define m_niq(i) ((i << 8) | mpeg_default_non_intra_quant[i])

void sunxi_cedrus_mpeg2_setup(struct sunxi_cedrus_ctx *ctx,
			      struct sunxi_cedrus_run *run)
{
	struct sunxi_cedrus_dev *dev = ctx->dev;
	const struct v4l2_ctrl_mpeg2_frame_hdr *frame_hdr = run->mpeg2.hdr;

	u16 width = DIV_ROUND_UP(frame_hdr->width, 16);
	u16 height = DIV_ROUND_UP(frame_hdr->height, 16);

	u32 pic_header = 0;
	u32 vld_len = frame_hdr->slice_len - frame_hdr->slice_pos;
	int i;

	struct vb2_buffer *fwd_vb2_buf, *bwd_vb2_buf;
	dma_addr_t src_buf_addr, dst_luma_addr, dst_chroma_addr;
	dma_addr_t fwd_luma = 0, fwd_chroma = 0, bwd_luma = 0, bwd_chroma = 0;


	fwd_vb2_buf = ctx->dst_bufs[frame_hdr->forward_ref_index];
	if (fwd_vb2_buf) {
		fwd_luma = vb2_dma_contig_plane_dma_addr(fwd_vb2_buf, 0);
		fwd_chroma = vb2_dma_contig_plane_dma_addr(fwd_vb2_buf, 1);
	}

	bwd_vb2_buf = ctx->dst_bufs[frame_hdr->backward_ref_index];
	if (bwd_vb2_buf) {
		bwd_luma = vb2_dma_contig_plane_dma_addr(bwd_vb2_buf, 0);
		bwd_chroma = vb2_dma_contig_plane_dma_addr(bwd_vb2_buf, 1);
	}

	/* Activate MPEG engine. */
	sunxi_cedrus_engine_enable(dev, SUNXI_CEDRUS_ENGINE_MPEG);

	/* Set quantization matrices. */
	for (i = 0; i < 64; i++) {
		sunxi_cedrus_write(dev, VE_MPEG_IQ_MIN_INPUT, m_iq(i));
		sunxi_cedrus_write(dev, VE_MPEG_IQ_MIN_INPUT, m_niq(i));
	}

	/* Set frame dimensions. */
	sunxi_cedrus_write(dev, VE_MPEG_SIZE, width << 8 | height);
	sunxi_cedrus_write(dev, VE_MPEG_FRAME_SIZE, width << 20 | height << 4);

	/* Set MPEG picture header. */
	pic_header |= (frame_hdr->picture_coding_type & 0xf) << 28;
	pic_header |= (frame_hdr->f_code[0][0] & 0xf) << 24;
	pic_header |= (frame_hdr->f_code[0][1] & 0xf) << 20;
	pic_header |= (frame_hdr->f_code[1][0] & 0xf) << 16;
	pic_header |= (frame_hdr->f_code[1][1] & 0xf) << 12;
	pic_header |= (frame_hdr->intra_dc_precision & 0x3) << 10;
	pic_header |= (frame_hdr->picture_structure & 0x3) << 8;
	pic_header |= (frame_hdr->top_field_first & 0x1) << 7;
	pic_header |= (frame_hdr->frame_pred_frame_dct & 0x1) << 6;
	pic_header |= (frame_hdr->concealment_motion_vectors & 0x1) << 5;
	pic_header |= (frame_hdr->q_scale_type & 0x1) << 4;
	pic_header |= (frame_hdr->intra_vlc_format & 0x1) << 3;
	pic_header |= (frame_hdr->alternate_scan & 0x1) << 2;
	sunxi_cedrus_write(dev, VE_MPEG_PIC_HDR, pic_header);

	/* Enable interrupt and an unknown control flag. */
	sunxi_cedrus_write(dev, VE_MPEG_CTRL, VE_MPEG_CTRL_MPEG2);

	/* Macroblock address. */
	sunxi_cedrus_write(dev, VE_MPEG_MBA, 0);

	/* Clear previous errors. */
	sunxi_cedrus_write(dev, VE_MPEG_ERROR, 0);

	/* Clear correct macroblocks register. */
	sunxi_cedrus_write(dev, VE_MPEG_CTR_MB, 0);

	/* Forward and backward prediction reference buffers. */
	sunxi_cedrus_write(dev, VE_MPEG_FWD_LUMA, fwd_luma);
	sunxi_cedrus_write(dev, VE_MPEG_FWD_CHROMA, fwd_chroma);
	sunxi_cedrus_write(dev, VE_MPEG_BACK_LUMA, bwd_luma);
	sunxi_cedrus_write(dev, VE_MPEG_BACK_CHROMA, bwd_chroma);

	/* Destination luma and chroma buffers. */
	dst_luma_addr = vb2_dma_contig_plane_dma_addr(&run->dst->vb2_buf, 0);
	dst_chroma_addr = vb2_dma_contig_plane_dma_addr(&run->dst->vb2_buf, 1);
	sunxi_cedrus_write(dev, VE_MPEG_REC_LUMA, dst_luma_addr);
	sunxi_cedrus_write(dev, VE_MPEG_REC_CHROMA, dst_chroma_addr);
	sunxi_cedrus_write(dev, VE_MPEG_ROT_LUMA, dst_luma_addr);
	sunxi_cedrus_write(dev, VE_MPEG_ROT_CHROMA, dst_chroma_addr);

	/* Source offset and length in bits. */
	sunxi_cedrus_write(dev, VE_MPEG_VLD_OFFSET, frame_hdr->slice_pos);
	sunxi_cedrus_write(dev, VE_MPEG_VLD_LEN, vld_len);

	/* Source beginning and end addresses. */
	src_buf_addr = vb2_dma_contig_plane_dma_addr(&run->src->vb2_buf, 0);
	sunxi_cedrus_write(dev, VE_MPEG_VLD_ADDR,
			   VE_MPEG_VLD_ADDR_VAL(src_buf_addr));
	sunxi_cedrus_write(dev, VE_MPEG_VLD_END, src_buf_addr + VBV_SIZE - 1);
}

void sunxi_cedrus_mpeg2_trigger(struct sunxi_cedrus_ctx *ctx, bool mpeg1)
{
	struct sunxi_cedrus_dev *dev = ctx->dev;

	/* Trigger MPEG engine. */
	if (mpeg1)
		sunxi_cedrus_write(dev, VE_MPEG_TRIGGER, VE_TRIG_MPEG1);
	else
		sunxi_cedrus_write(dev, VE_MPEG_TRIGGER, VE_TRIG_MPEG2);
}
