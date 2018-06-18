// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2013 Jens Kuske <jenskuske@gmail.com>
 * Copyright (c) 2018 Bootlin
 */

#include <linux/types.h>

#include <media/videobuf2-dma-contig.h>

#include "cedrus.h"
#include "cedrus_hw.h"
#include "cedrus_regs.h"

enum cedrus_h264_sram_off {
	CEDRUS_SRAM_H264_PRED_WEIGHT_TABLE	= 0x000,
	CEDRUS_SRAM_H264_FRAMEBUFFER_LIST	= 0x100,
	CEDRUS_SRAM_H264_REF_LIST_0		= 0x190,
	CEDRUS_SRAM_H264_REF_LIST_1		= 0x199,
	CEDRUS_SRAM_H264_SCALING_LIST_8x8	= 0x200,
	CEDRUS_SRAM_H264_SCALING_LIST_4x4	= 0x218,
};

struct cedrus_h264_sram_ref_pic {
	__le32	top_field_order_cnt;
	__le32	bottom_field_order_cnt;
	__le32	frame_info;
	__le32	luma_ptr;
	__le32	chroma_ptr;
	__le32	extra_data_ptr;
	__le32	extra_data_end;
	__le32	reserved;
} __packed;

/* One for the output, 16 for the reference images */
#define CEDRUS_H264_FRAME_NUM		17

#define CEDRUS_PIC_INFO_BUF_SIZE	(128 * SZ_1K)
#define CEDRUS_NEIGHBOR_INFO_BUF_SIZE	(16 * SZ_1K)

static void cedrus_h264_write_sram(struct cedrus_dev *dev,
				   enum cedrus_h264_sram_off off,
				   const void *data, size_t len)
{
	const u32 *buffer = data;
	size_t count = DIV_ROUND_UP(len, 4);

	cedrus_write(dev, VE_AVC_SRAM_PORT_OFFSET, off << 2);

	do {
		cedrus_write(dev, VE_AVC_SRAM_PORT_DATA, *buffer++);
	} while (--count);
}

static void cedrus_fill_ref_pic(struct cedrus_h264_sram_ref_pic *pic,
				struct vb2_buffer *buf,
				dma_addr_t extra_buf,
				size_t extra_buf_len,
				unsigned int top_field_order_cnt,
				unsigned int bottom_field_order_cnt,
				enum cedrus_h264_pic_type pic_type)
{
	pic->top_field_order_cnt = top_field_order_cnt;
	pic->bottom_field_order_cnt = bottom_field_order_cnt;
	pic->frame_info = pic_type << 8;
	pic->luma_ptr = vb2_dma_contig_plane_dma_addr(buf, 0) - PHYS_OFFSET;
	pic->chroma_ptr = vb2_dma_contig_plane_dma_addr(buf, 1) - PHYS_OFFSET;
	pic->extra_data_ptr = extra_buf - PHYS_OFFSET;
	pic->extra_data_end = (extra_buf - PHYS_OFFSET) + extra_buf_len;
}

static void cedrus_write_frame_list(struct cedrus_ctx *ctx,
				    struct cedrus_run *run)
{
	struct cedrus_h264_sram_ref_pic pic_list[CEDRUS_H264_FRAME_NUM];
	const struct v4l2_ctrl_h264_decode_param *dec_param = run->h264.decode_param;
	const struct v4l2_ctrl_h264_slice_param *slice = run->h264.slice_param;
	const struct v4l2_ctrl_h264_sps *sps = run->h264.sps;
	struct cedrus_buffer *output_buf;
	struct cedrus_dev *dev = ctx->dev;
	unsigned long used_dpbs = 0;
	unsigned int position;
	unsigned int output = 0;
	unsigned int i;

	memset(pic_list, 0, sizeof(pic_list));

	for (i = 0; i < ARRAY_SIZE(dec_param->dpb); i++) {
		const struct v4l2_h264_dpb_entry *dpb = &dec_param->dpb[i];
		const struct cedrus_buffer *cedrus_buf;
		struct vb2_buffer *ref_buf;

		if (!(dpb->flags & V4L2_H264_DPB_ENTRY_FLAG_ACTIVE))
			continue;

		ref_buf = ctx->dst_bufs[dpb->buf_index];
		cedrus_buf = vb2_to_cedrus_buffer(ref_buf);
		position = cedrus_buf->codec.h264.position;
		used_dpbs |= BIT(position);
		
		cedrus_fill_ref_pic(&pic_list[position], ref_buf,
				    ctx->codec.h264.mv_col_buf_dma,
				    ctx->codec.h264.mv_col_buf_size,
				    dpb->top_field_order_cnt,
				    dpb->bottom_field_order_cnt,
				    cedrus_buf->codec.h264.pic_type);

		output = max(position, output);
	}

	position = find_next_zero_bit(&used_dpbs, 17, output);
	if (position >= 17)
		position = find_first_zero_bit(&used_dpbs, 17);

	output_buf = vb2_to_cedrus_buffer(&run->dst->vb2_buf);
	output_buf->codec.h264.position = position;

	if (slice->flags & V4L2_SLICE_FLAG_FIELD_PIC)
		output_buf->codec.h264.pic_type = CEDRUS_H264_PIC_TYPE_FIELD;
	else if (sps->flags & V4L2_H264_SPS_FLAG_MB_ADAPTIVE_FRAME_FIELD)
		output_buf->codec.h264.pic_type = CEDRUS_H264_PIC_TYPE_MBAFF;
	else
		output_buf->codec.h264.pic_type = CEDRUS_H264_PIC_TYPE_FRAME;

	cedrus_fill_ref_pic(&pic_list[position], &run->dst->vb2_buf,
			    ctx->codec.h264.mv_col_buf_dma,
			    ctx->codec.h264.mv_col_buf_size,
			    dec_param->top_field_order_cnt,
			    dec_param->bottom_field_order_cnt,
			    output_buf->codec.h264.pic_type);

	cedrus_h264_write_sram(dev, CEDRUS_SRAM_H264_FRAMEBUFFER_LIST,
			       pic_list, sizeof(pic_list));

	cedrus_write(dev, VE_H264_OUTPUT_FRAME_IDX, position);
}

#define CEDRUS_MAX_REF_IDX	32

static void _cedrus_write_ref_list(struct cedrus_ctx *ctx,
				   struct cedrus_run *run,
				   const u8 *ref_list, u8 num_ref,
				   enum cedrus_h264_sram_off sram)
{
	const struct v4l2_ctrl_h264_decode_param *decode = run->h264.decode_param;
	struct cedrus_dev *dev = ctx->dev;
	u32 sram_array[CEDRUS_MAX_REF_IDX / sizeof(u32)];
	unsigned int size, i;

	memset(sram_array, 0, sizeof(sram_array));

	for (i = 0; i < num_ref; i += 4) {
		unsigned int j;

		for (j = 0; j < 4; j++) {
			const struct v4l2_h264_dpb_entry *dpb;
			const struct cedrus_buffer *cedrus_buf;
			const struct vb2_v4l2_buffer *ref_buf;
			unsigned int position;
			u8 ref_idx = i + j;
			u8 dpb_idx;

			if (ref_idx >= num_ref)
				break;

			dpb_idx = ref_list[ref_idx];
			dpb = &decode->dpb[dpb_idx];

			if (!(dpb->flags & V4L2_H264_DPB_ENTRY_FLAG_ACTIVE))
				continue;

			ref_buf = to_vb2_v4l2_buffer(ctx->dst_bufs[dpb->buf_index]);
			cedrus_buf = vb2_v4l2_to_cedrus_buffer(ref_buf);
			position = cedrus_buf->codec.h264.position;

			sram_array[i] |= position << (j * 8 + 1);
			if (ref_buf->field == V4L2_FIELD_BOTTOM)
				sram_array[i] |= BIT(j * 8);
		}
	}

	size = min((unsigned int)ALIGN(num_ref, 4), sizeof(sram_array));
	cedrus_h264_write_sram(dev, sram, &sram_array, size);
}

static void cedrus_write_ref_list0(struct cedrus_ctx *ctx,
				   struct cedrus_run *run)
{
	const struct v4l2_ctrl_h264_slice_param *slice = run->h264.slice_param;

	_cedrus_write_ref_list(ctx, run,
			       slice->ref_pic_list0,
			       slice->num_ref_idx_l0_active_minus1 + 1,
			       CEDRUS_SRAM_H264_REF_LIST_0);
}

static void cedrus_write_ref_list1(struct cedrus_ctx *ctx,
				   struct cedrus_run *run)
{
	const struct v4l2_ctrl_h264_slice_param *slice = run->h264.slice_param;

	_cedrus_write_ref_list(ctx, run,
			       slice->ref_pic_list1,
			       slice->num_ref_idx_l1_active_minus1 + 1,
			       CEDRUS_SRAM_H264_REF_LIST_1);
}

static void cedrus_write_pred_weight_table(struct cedrus_ctx *ctx,
					   struct cedrus_run *run)
{
	const struct v4l2_ctrl_h264_slice_param *slice =
		run->h264.slice_param;	
	const struct v4l2_h264_pred_weight_table *pred_weight =
		&slice->pred_weight_table;
	struct cedrus_dev *dev = ctx->dev;
	int i, j, k;

#warning FIXME
	return;

	cedrus_write(dev, VE_H264_PRED_WEIGHT,
		     ((pred_weight->chroma_log2_weight_denom & 0xf) << 4) |
		     ((pred_weight->luma_log2_weight_denom & 0xf) << 0));

	cedrus_write(dev, VE_AVC_SRAM_PORT_OFFSET,
		     CEDRUS_SRAM_H264_PRED_WEIGHT_TABLE << 2);

	for (i = 0; i < ARRAY_SIZE(pred_weight->weight_factors); i++) {
		const struct v4l2_h264_weight_factors *factors =
			&pred_weight->weight_factors[i];

		for (j = 0; j < ARRAY_SIZE(factors->luma_weight); j++) {
			u32 val;

			val = ((factors->luma_offset[j] & 0x1ff) << 16) |
				(factors->luma_weight[j] & 0x1ff);
			cedrus_write(dev, VE_AVC_SRAM_PORT_DATA, val);
		}

		for (j = 0; j < ARRAY_SIZE(factors->chroma_weight); j++) {
			for (k = 0; k < ARRAY_SIZE(factors->chroma_weight[0]); k++) {
				u32 val;

				val = ((factors->chroma_offset[j][k] & 0x1ff) << 16) |
					(factors->chroma_weight[j][k] & 0x1ff);
				cedrus_write(dev, VE_AVC_SRAM_PORT_DATA, val);
			}
		}
	}
}

static void cedrus_write_scaling_lists(struct cedrus_ctx *ctx,
				       struct cedrus_run *run)
{
	const struct v4l2_ctrl_h264_scaling_matrix *scaling =
		run->h264.scaling_matrix;
	struct cedrus_dev *dev = ctx->dev;

	return;

	cedrus_h264_write_sram(dev, CEDRUS_SRAM_H264_SCALING_LIST_8x8,
			       scaling->scaling_list_8x8,
			       sizeof(scaling->scaling_list_8x8));

	cedrus_h264_write_sram(dev, CEDRUS_SRAM_H264_SCALING_LIST_4x4,
			       scaling->scaling_list_4x4,
			       sizeof(scaling->scaling_list_4x4));
}

static void cedrus_set_params(struct cedrus_ctx *ctx,
			      struct cedrus_run *run)
{
	const struct v4l2_ctrl_h264_slice_param *slice = run->h264.slice_param;
	const struct v4l2_ctrl_h264_pps *pps = run->h264.pps;
	const struct v4l2_ctrl_h264_sps *sps = run->h264.sps;
	struct cedrus_dev *dev = ctx->dev;
	dma_addr_t src_buf_addr;
	u32 offset = slice->header_bit_size;
	u32 len = (slice->size * 8) - offset;
	u32 reg;

	cedrus_write(dev, 0x250,
		     ctx->codec.h264.pic_info_buf_dma - PHYS_OFFSET);
	cedrus_write(dev, 0x254,
		     (ctx->codec.h264.pic_info_buf_dma - PHYS_OFFSET) + 0x48000);

	cedrus_write(dev, VE_H264_VLD_LEN, len);
	cedrus_write(dev, VE_H264_VLD_OFFSET, offset);

	src_buf_addr = vb2_dma_contig_plane_dma_addr(&run->src->vb2_buf, 0);
	src_buf_addr -= PHYS_OFFSET;
	cedrus_write(dev, VE_H264_VLD_ADDR,
		     VE_H264_VLD_ADDR_VAL(src_buf_addr) | VE_H264_VLD_ADDR_FIRST | VE_H264_VLD_ADDR_VALID | VE_H264_VLD_ADDR_LAST);
	cedrus_write(dev, VE_H264_VLD_END, src_buf_addr + VBV_SIZE - 1);

	cedrus_write(dev, VE_H264_TRIGGER_TYPE,
		     VE_H264_TRIGGER_TYPE_INIT_SWDEC);

	if ((slice->slice_type == V4L2_H264_SLICE_TYPE_P) ||
	    (slice->slice_type == V4L2_H264_SLICE_TYPE_SP) ||
	    (slice->slice_type == V4L2_H264_SLICE_TYPE_B))
		cedrus_write_ref_list0(ctx, run);

	if (slice->slice_type == V4L2_H264_SLICE_TYPE_B)
		cedrus_write_ref_list1(ctx, run);

	// picture parameters
	reg = 0;
	/*
	 * FIXME: the kernel headers are allowing the default value to
	 * be passed, but the libva doesn't give us that.
	 */
	reg |= (slice->num_ref_idx_l0_active_minus1 & 0x1f) << 10;
	reg |= (slice->num_ref_idx_l1_active_minus1 & 0x1f) << 5;
	reg |= (pps->weighted_bipred_idc & 0x3) << 2;
	if (pps->flags & V4L2_H264_PPS_FLAG_ENTROPY_CODING_MODE)
		reg |= BIT(15);
	if (pps->flags & V4L2_H264_PPS_FLAG_WEIGHTED_PRED)
		reg |= BIT(4);
	if (pps->flags & V4L2_H264_PPS_FLAG_CONSTRAINED_INTRA_PRED)
		reg |= BIT(1);
	if (pps->flags & V4L2_H264_PPS_FLAG_TRANSFORM_8X8_MODE)
		reg |= BIT(0);
	cedrus_write(dev, VE_H264_PIC_HDR, reg);

	// sequence parameters
	reg = BIT(19);
	reg |= (sps->pic_width_in_mbs_minus1 & 0xff) << 8;
	reg |= sps->pic_height_in_map_units_minus1 & 0xff;
	if (sps->flags & V4L2_H264_SPS_FLAG_FRAME_MBS_ONLY)
		reg |= BIT(18);
	if (sps->flags & V4L2_H264_SPS_FLAG_MB_ADAPTIVE_FRAME_FIELD)
		reg |= BIT(17);
	if (sps->flags & V4L2_H264_SPS_FLAG_DIRECT_8X8_INFERENCE)
		reg |= BIT(16);
	cedrus_write(dev, VE_H264_FRAME_SIZE, reg);

	// slice parameters
	reg = 0;
	/*
	 * FIXME: This bit marks all the frames as references. This
	 * should probably be set based on nal_ref_idc, but the libva
	 * doesn't pass that information along, so this is not always
	 * available. We should find something else, maybe change the
	 * kernel UAPI somehow?
	 */
	reg |= BIT(12);
	reg |= (slice->slice_type & 0xf) << 8;
	reg |= slice->cabac_init_idc & 0x3;
	reg |= BIT(5);
	if (slice->flags & V4L2_SLICE_FLAG_FIELD_PIC)
		reg |= BIT(4);
	if (slice->flags & V4L2_SLICE_FLAG_BOTTOM_FIELD)
		reg |= BIT(3);
	if (slice->flags & V4L2_SLICE_FLAG_DIRECT_SPATIAL_MV_PRED)
		reg |= BIT(2);
	cedrus_write(dev, VE_H264_SLICE_HDR, reg);

	reg = 0;
	reg |= (slice->num_ref_idx_l0_active_minus1 & 0x1f) << 24;
	reg |= (slice->num_ref_idx_l1_active_minus1 & 0x1f) << 16;
	reg |= (slice->disable_deblocking_filter_idc & 0x3) << 8;
	reg |= (slice->slice_alpha_c0_offset_div2 & 0xf) << 4;
	reg |= slice->slice_beta_offset_div2 & 0xf;
	cedrus_write(dev, VE_H264_SLICE_HDR2, reg);

	reg = 0;
	/*
	 * FIXME: This bit tells the video engine to use the default
	 * quantization matrices. This will obviously need to be
	 * changed to support the profiles supporting custom
	 * quantization matrices.
	 */
	reg |= BIT(24);
	reg |= (pps->second_chroma_qp_index_offset & 0x3f) << 16;
	reg |= (pps->chroma_qp_index_offset & 0x3f) << 8;
	reg |= (pps->pic_init_qp_minus26 + 26 + slice->slice_qp_delta) & 0x3f;
	cedrus_write(dev, VE_H264_QP_PARAM, reg);

	// clear status flags
	cedrus_write(dev, VE_H264_STATUS, cedrus_read(dev, VE_H264_STATUS));

	// enable int
	reg = cedrus_read(dev, VE_H264_CTRL) | 0x7;
	cedrus_write(dev, VE_H264_CTRL, reg);
}

static enum cedrus_irq_status
cedrus_h264_irq_status(struct cedrus_ctx *ctx)
{
	struct cedrus_dev *dev = ctx->dev;
	u32 reg = cedrus_read(dev, VE_H264_STATUS) & 0x7;

	if (!reg)
		return CEDRUS_IRQ_NONE;

	if (reg & (BIT(1) | BIT(2)))
		return CEDRUS_IRQ_ERROR;

	return CEDRUS_IRQ_OK;
}

static void cedrus_h264_irq_clear(struct cedrus_ctx *ctx)
{
	struct cedrus_dev *dev = ctx->dev;

	cedrus_write(dev, VE_H264_STATUS, GENMASK(2, 0));
}

static void cedrus_h264_irq_disable(struct cedrus_ctx *ctx)
{
	struct cedrus_dev *dev = ctx->dev;
	u32 reg = cedrus_read(dev, VE_H264_CTRL) & ~GENMASK(2, 0);

	cedrus_write(dev, VE_H264_CTRL, reg);
}

static void cedrus_h264_setup(struct cedrus_ctx *ctx,
			      struct cedrus_run *run)
{
	struct cedrus_dev *dev = ctx->dev;

	cedrus_engine_enable(dev, CEDRUS_CODEC_H264);

	cedrus_write_scaling_lists(ctx, run);
	cedrus_write_frame_list(ctx, run);
	cedrus_write_pred_weight_table(ctx, run);
	cedrus_set_params(ctx, run);
}

static int cedrus_h264_start(struct cedrus_ctx *ctx)
{
	struct cedrus_dev *dev = ctx->dev;
	int ret;

	ctx->codec.h264.pic_info_buf =
		dma_alloc_coherent(dev->dev, CEDRUS_PIC_INFO_BUF_SIZE,
				   &ctx->codec.h264.pic_info_buf_dma,
				   GFP_KERNEL);
	if (!ctx->codec.h264.pic_info_buf)
		return -ENOMEM;

	ctx->codec.h264.neighbor_info_buf =
		dma_alloc_coherent(dev->dev, CEDRUS_NEIGHBOR_INFO_BUF_SIZE,
				   &ctx->codec.h264.neighbor_info_buf_dma,
				   GFP_KERNEL);
	if (!ctx->codec.h264.neighbor_info_buf) {
		ret = -ENOMEM;
		goto err_pic_buf;
	}

	ctx->codec.h264.mv_col_buf_size = DIV_ROUND_UP(ctx->src_fmt.width, 16) *
		DIV_ROUND_UP(ctx->src_fmt.height, 16) * 32;
	ctx->codec.h264.mv_col_buf = dma_alloc_coherent(dev->dev,
							ctx->codec.h264.mv_col_buf_size,
							&ctx->codec.h264.mv_col_buf_dma,
							GFP_KERNEL);
	if (!ctx->codec.h264.mv_col_buf) {
		ret = -ENOMEM;
		goto err_neighbor_buf;
	}

	return 0;

err_neighbor_buf:
	dma_free_coherent(dev->dev, CEDRUS_NEIGHBOR_INFO_BUF_SIZE,
			  ctx->codec.h264.neighbor_info_buf,
			  ctx->codec.h264.neighbor_info_buf_dma);
err_pic_buf:
	dma_free_coherent(dev->dev, CEDRUS_PIC_INFO_BUF_SIZE,
			  ctx->codec.h264.pic_info_buf,
			  ctx->codec.h264.pic_info_buf_dma);
	return ret;
}

static void cedrus_h264_stop(struct cedrus_ctx *ctx)
{
	struct cedrus_dev *dev = ctx->dev;

	dma_free_coherent(dev->dev, ctx->codec.h264.mv_col_buf_size,
			  ctx->codec.h264.mv_col_buf,
			  ctx->codec.h264.mv_col_buf_dma);
	dma_free_coherent(dev->dev, CEDRUS_NEIGHBOR_INFO_BUF_SIZE,
			  ctx->codec.h264.neighbor_info_buf,
			  ctx->codec.h264.neighbor_info_buf_dma);
	dma_free_coherent(dev->dev, CEDRUS_PIC_INFO_BUF_SIZE,
			  ctx->codec.h264.pic_info_buf,
			  ctx->codec.h264.pic_info_buf_dma);
}

static void cedrus_h264_trigger(struct cedrus_ctx *ctx)
{
	struct cedrus_dev *dev = ctx->dev;

	cedrus_write(dev, VE_H264_TRIGGER_TYPE,
		     VE_H264_TRIGGER_TYPE_AVC_SLICE_DECODE);
}

struct cedrus_dec_ops cedrus_dec_ops_h264 = {
	.irq_clear	= cedrus_h264_irq_clear,
	.irq_disable	= cedrus_h264_irq_disable,
	.irq_status	= cedrus_h264_irq_status,
	.setup		= cedrus_h264_setup,
	.start		= cedrus_h264_start,
	.stop		= cedrus_h264_stop,
	.trigger	= cedrus_h264_trigger,
};
