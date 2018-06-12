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

#include <media/videobuf2-dma-contig.h>

#include "cedrus.h"
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

static void cedrus_mpeg2_setup(struct cedrus_ctx *ctx, struct cedrus_run *run)
{
	struct cedrus_dev *dev = ctx->dev;
	const struct v4l2_ctrl_mpeg2_slice_header *frame_hdr = run->mpeg2.hdr;

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
	cedrus_engine_enable(dev, CEDRUS_CODEC_MPEG2);

	/* Set quantization matrices. */
	for (i = 0; i < 64; i++) {
		cedrus_write(dev, VE_MPEG_IQ_MIN_INPUT, m_iq(i));
		cedrus_write(dev, VE_MPEG_IQ_MIN_INPUT, m_niq(i));
	}

	/* Set frame dimensions. */
	cedrus_write(dev, VE_MPEG_SIZE, width << 8 | height);
	cedrus_write(dev, VE_MPEG_FRAME_SIZE, width << 20 | height << 4);

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
	cedrus_write(dev, VE_MPEG_PIC_HDR, pic_header);

	/* Enable interrupt and an unknown control flag. */
	cedrus_write(dev, VE_MPEG_CTRL, VE_MPEG_CTRL_MPEG2);

	/* Macroblock address. */
	cedrus_write(dev, VE_MPEG_MBA, 0);

	/* Clear previous errors. */
	cedrus_write(dev, VE_MPEG_ERROR, 0);

	/* Clear correct macroblocks register. */
	cedrus_write(dev, VE_MPEG_CTR_MB, 0);

	/* Forward and backward prediction reference buffers. */
	cedrus_write(dev, VE_MPEG_FWD_LUMA, fwd_luma);
	cedrus_write(dev, VE_MPEG_FWD_CHROMA, fwd_chroma);
	cedrus_write(dev, VE_MPEG_BACK_LUMA, bwd_luma);
	cedrus_write(dev, VE_MPEG_BACK_CHROMA, bwd_chroma);

	/* Destination luma and chroma buffers. */
	dst_luma_addr = vb2_dma_contig_plane_dma_addr(&run->dst->vb2_buf, 0);
	dst_chroma_addr = vb2_dma_contig_plane_dma_addr(&run->dst->vb2_buf, 1);
	cedrus_write(dev, VE_MPEG_REC_LUMA, dst_luma_addr);
	cedrus_write(dev, VE_MPEG_REC_CHROMA, dst_chroma_addr);
	cedrus_write(dev, VE_MPEG_ROT_LUMA, dst_luma_addr);
	cedrus_write(dev, VE_MPEG_ROT_CHROMA, dst_chroma_addr);

	/* Source offset and length in bits. */
	cedrus_write(dev, VE_MPEG_VLD_OFFSET, frame_hdr->slice_pos);
	cedrus_write(dev, VE_MPEG_VLD_LEN, vld_len);

	/* Source beginning and end addresses. */
	src_buf_addr = vb2_dma_contig_plane_dma_addr(&run->src->vb2_buf, 0);
	cedrus_write(dev, VE_MPEG_VLD_ADDR, VE_MPEG_VLD_ADDR_VAL(src_buf_addr));
	cedrus_write(dev, VE_MPEG_VLD_END, src_buf_addr + VBV_SIZE - 1);
}

static void cedrus_mpeg2_trigger(struct cedrus_ctx *ctx)
{
	struct cedrus_dev *dev = ctx->dev;

	/* Trigger MPEG engine. */
	cedrus_write(dev, VE_MPEG_TRIGGER, VE_TRIG_MPEG2);
}

struct cedrus_dec_ops cedrus_dec_ops_mpeg2 = {
	.setup		= cedrus_mpeg2_setup,
	.trigger	= cedrus_mpeg2_trigger,
};
