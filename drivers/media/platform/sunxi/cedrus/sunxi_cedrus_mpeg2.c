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

#include "sunxi_cedrus_common.h"
#include "sunxi_cedrus_regs.h"

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
			      dma_addr_t src_buf_addr,
			      dma_addr_t dst_luma_addr,
			      dma_addr_t dst_chroma_addr,
			      struct v4l2_ctrl_mpeg2_frame_hdr *frame_hdr)
{
	struct sunxi_cedrus_dev *dev = ctx->dev;

	u16 width = DIV_ROUND_UP(frame_hdr->width, 16);
	u16 height = DIV_ROUND_UP(frame_hdr->height, 16);

	u32 pic_header = 0;
	u32 vld_len = frame_hdr->slice_len - frame_hdr->slice_pos;
	int i;

	struct vb2_buffer *fwd_vb2_buf, *bwd_vb2_buf;
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
	sunxi_cedrus_write(dev, VE_CTRL_MPEG, VE_CTRL);

	/* Set quantization matrices. */
	for (i = 0; i < 64; i++) {
		sunxi_cedrus_write(dev, m_iq(i), VE_MPEG_IQ_MIN_INPUT);
		sunxi_cedrus_write(dev, m_niq(i), VE_MPEG_IQ_MIN_INPUT);
	}

	/* Set frame dimensions. */
	sunxi_cedrus_write(dev, width << 8 | height, VE_MPEG_SIZE);
	sunxi_cedrus_write(dev, width << 20 | height << 4, VE_MPEG_FRAME_SIZE);

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
	sunxi_cedrus_write(dev, pic_header, VE_MPEG_PIC_HDR);

	/* Enable interrupt and an unknown control flag. */
	sunxi_cedrus_write(dev, VE_MPEG_CTRL_MPEG2, VE_MPEG_CTRL);

	/* Macroblock address. */
	sunxi_cedrus_write(dev, 0, VE_MPEG_MBA);

	/* Clear previous errors. */
	sunxi_cedrus_write(dev, 0, VE_MPEG_ERROR);

	/* Clear correct macroblocks register. */
	sunxi_cedrus_write(dev, 0, VE_MPEG_CTR_MB);

	/* Forward and backward prediction reference buffers. */
	sunxi_cedrus_write(dev, fwd_luma, VE_MPEG_FWD_LUMA);
	sunxi_cedrus_write(dev, fwd_chroma, VE_MPEG_FWD_CHROMA);
	sunxi_cedrus_write(dev, bwd_luma, VE_MPEG_BACK_LUMA);
	sunxi_cedrus_write(dev, bwd_chroma, VE_MPEG_BACK_CHROMA);

	/* Desination luma and chroma buffers. */
	sunxi_cedrus_write(dev, dst_luma_addr, VE_MPEG_REC_LUMA);
	sunxi_cedrus_write(dev, dst_chroma_addr, VE_MPEG_REC_CHROMA);
	sunxi_cedrus_write(dev, dst_luma_addr, VE_MPEG_ROT_LUMA);
	sunxi_cedrus_write(dev, dst_chroma_addr, VE_MPEG_ROT_CHROMA);

	/* Source offset and length in bits. */
	sunxi_cedrus_write(dev, frame_hdr->slice_pos, VE_MPEG_VLD_OFFSET);
	sunxi_cedrus_write(dev, vld_len, VE_MPEG_VLD_LEN);

	/* Source beginning and end addresses. */
	sunxi_cedrus_write(dev, VE_MPEG_VLD_ADDR_VAL(src_buf_addr),
			   VE_MPEG_VLD_ADDR);
	sunxi_cedrus_write(dev, src_buf_addr + VBV_SIZE - 1, VE_MPEG_VLD_END);
}

void sunxi_cedrus_mpeg2_trigger(struct sunxi_cedrus_ctx *ctx, bool mpeg1)
{
	struct sunxi_cedrus_dev *dev = ctx->dev;

	/* Trigger MPEG engine. */
	if (mpeg1)
		sunxi_cedrus_write(dev, VE_TRIG_MPEG1, VE_MPEG_TRIGGER);
	else
		sunxi_cedrus_write(dev, VE_TRIG_MPEG2, VE_MPEG_TRIGGER);
}
