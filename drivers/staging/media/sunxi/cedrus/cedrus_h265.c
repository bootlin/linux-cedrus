// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2013 Jens Kuske <jenskuske@gmail.com>
 * Copyright (C) 2018 Paul Kocialkowski <paul.kocialkowski@bootlin.com>
 */

#include <linux/types.h>

#include <media/videobuf2-dma-contig.h>

#include "cedrus.h"
#include "cedrus_hw.h"
#include "cedrus_regs.h"

/*
 * FIXME: Neighbor info buffer size is apparently doubled for H6, which may be
 * related to 10 bit H265 support or interlaced.
 */
#define CEDRUS_H265_NEIGHBOR_INFO_BUF_SIZE	(397 * SZ_1K)
#define CEDRUS_H265_ENTRY_POINTS_BUF_SIZE	(4 * SZ_1K)
#define CEDRUS_H265_MV_COL_BUF_SIZE		(1024 * SZ_1K) // FIXME

static enum cedrus_irq_status
cedrus_h265_irq_status(struct cedrus_ctx *ctx)
{
	struct cedrus_dev *dev = ctx->dev;
	u32 reg;

	reg = cedrus_read(dev, VE_DEC_H265_STATUS);
	reg &= VE_DEC_H265_STATUS_CHECK_MASK;

	if (reg & VE_DEC_H265_STATUS_CHECK_ERROR ||
	    !(reg & VE_DEC_H265_STATUS_SUCCESS))
		return CEDRUS_IRQ_ERROR;

	return CEDRUS_IRQ_OK;
}

static void cedrus_h265_irq_clear(struct cedrus_ctx *ctx)
{
	struct cedrus_dev *dev = ctx->dev;

	cedrus_write(dev, VE_DEC_H265_STATUS, VE_DEC_H265_STATUS_CHECK_MASK);
}

static void cedrus_h265_irq_disable(struct cedrus_ctx *ctx)
{
	struct cedrus_dev *dev = ctx->dev;
	u32 reg = cedrus_read(dev, VE_DEC_H265_CTRL);

	reg &= ~VE_DEC_H265_CTRL_IRQ_MASK;

	cedrus_write(dev, VE_DEC_H265_CTRL, reg);
}

static void cedrus_h265_sram_write(struct cedrus_dev *dev, u32 offset,
				   u32 *data, unsigned int count)
{
	cedrus_write(dev, VE_DEC_H265_SRAM_OFFSET, offset);

	while (count--)
		cedrus_write(dev, VE_DEC_H265_SRAM_DATA, *data++);
}

static void cedrus_h265_pic_list_write_single(struct cedrus_dev *dev,
					      unsigned int index,
					      u32 pic_order_cnt,
					      dma_addr_t mv_col_buf_addr,
					      dma_addr_t dst_luma_addr,
					      dma_addr_t dst_chroma_addr)
{
	u32 offset = VE_DEC_H265_SRAM_OFFSET_PIC_LIST +
		     VE_DEC_H265_SRAM_OFFSET_PIC_LIST_UNIT * index;
	u32 data[6] = {
		pic_order_cnt,
		pic_order_cnt,
		VE_DEC_H265_SRAM_DATA_ADDR_BASE(mv_col_buf_addr),
		VE_DEC_H265_SRAM_DATA_ADDR_BASE(mv_col_buf_addr),
		VE_DEC_H265_SRAM_DATA_ADDR_BASE(dst_luma_addr),
		VE_DEC_H265_SRAM_DATA_ADDR_BASE(dst_chroma_addr),
	};

	cedrus_h265_sram_write(dev, offset, data, ARRAY_SIZE(data));
}

static void cedrus_h265_setup(struct cedrus_ctx *ctx,
			      struct cedrus_run *run)
{
	struct cedrus_dev *dev = ctx->dev;
	const struct v4l2_ctrl_hevc_sps *sps;
	const struct v4l2_ctrl_hevc_pps *pps;
	const struct v4l2_ctrl_hevc_slice_params *slice_params;
	dma_addr_t src_buf_addr;
	dma_addr_t dst_luma_addr, dst_chroma_addr;
	dma_addr_t mv_col_buf_addr;
	u32 chroma_log2_weight_denom;
	u32 output_frame_index;
	u32 reg;

	sps = run->h265.sps;
	pps = run->h265.pps;
	slice_params = run->h265.slice_params;

	u32 slice_len = slice_params->bit_size; //(145067 - 3) * 8;
	u32 slice_pos = slice_params->data_bit_offset; //(0x11 - 1) * 8;

	printk(KERN_ERR "%s()\n", __func__);

	/* Activate H265 engine. */
	cedrus_engine_enable(dev, CEDRUS_CODEC_H265);

	/* Source offset and length in bits. */

	cedrus_write(dev, VE_DEC_H265_BITS_OFFSET, slice_pos);

	reg = slice_len - slice_pos;
	cedrus_write(dev, VE_DEC_H265_BITS_LEN, reg);

	/* Source beginning and end addresses. */

	src_buf_addr = vb2_dma_contig_plane_dma_addr(&run->src->vb2_buf, 0);

	reg = VE_DEC_H265_BITS_ADDR_BASE(src_buf_addr);
	reg |= VE_DEC_H265_BITS_ADDR_VALID_SLICE_DATA;
	reg |= VE_DEC_H265_BITS_ADDR_LAST_SLICE_DATA;
	reg |= VE_DEC_H265_BITS_ADDR_FIRST_SLICE_DATA;

	cedrus_write(dev, VE_DEC_H265_BITS_ADDR, reg);

	reg = src_buf_addr + VBV_SIZE - 1;
	cedrus_write(dev, VE_DEC_H265_BITS_END_ADDR,
		     VE_DEC_H265_BITS_END_ADDR_BASE(reg));

	/* Coding tree block address: start at the beginning. */

	reg = VE_DEC_H265_DEC_CTB_ADDR_X(0) | VE_DEC_H265_DEC_CTB_ADDR_Y(0);
	cedrus_write(dev, VE_DEC_H265_DEC_CTB_ADDR, reg);

	cedrus_write(dev, VE_DEC_H265_TILE_START_CTB, 0);
	cedrus_write(dev, VE_DEC_H265_TILE_END_CTB, 0);

	/* Clear the number of correctly-decoded coding tree blocks. */
	cedrus_write(dev, VE_DEC_H265_DEC_CTB_NUM, 0);

	/* Initialize bitstream access. */

	cedrus_write(dev, VE_DEC_H265_TRIGGER, VE_DEC_H265_TRIGGER_INIT_SWDEC);

	/* Bitstream parameters. */

	reg = VE_DEC_H265_DEC_NAL_HDR_NAL_UNIT_TYPE(slice_params->nal_unit_type) |
	      VE_DEC_H265_DEC_NAL_HDR_NUH_TEMPORAL_ID_PLUS1(slice_params->nuh_temporal_id_plus1);
	cedrus_write(dev, VE_DEC_H265_DEC_NAL_HDR, reg);

	reg = VE_DEC_H265_DEC_SPS_HDR_STRONG_INTRA_SMOOTHING_ENABLE_FLAG(sps->strong_intra_smoothing_enabled_flag) |
	      VE_DEC_H265_DEC_SPS_HDR_SPS_TEMPORAL_MVP_ENABLE_FLAG(sps->sps_temporal_mvp_enabled_flag) |
	      VE_DEC_H265_DEC_SPS_HDR_SAMPLE_ADAPTIVE_OFFSET_ENABLED_FLAG(sps->sample_adaptive_offset_enabled_flag) |
	      VE_DEC_H265_DEC_SPS_HDR_AMP_ENABLED_FLAG(sps->amp_enabled_flag) |
	      VE_DEC_H265_DEC_SPS_HDR_MAX_TRANSFORM_HIERARCHY_DEPTH_INTRA(sps->max_transform_hierarchy_depth_intra) |
	      VE_DEC_H265_DEC_SPS_HDR_MAX_TRANSFORM_HIERARCHY_DEPTH_INTER(sps->max_transform_hierarchy_depth_inter) |
	      VE_DEC_H265_DEC_SPS_HDR_LOG2_DIFF_MAX_MIN_TRANSFORM_BLOCK_SIZE(sps->log2_diff_max_min_luma_transform_block_size) |
	      VE_DEC_H265_DEC_SPS_HDR_LOG2_MIN_TRANSFORM_BLOCK_SIZE_MINUS2(sps->log2_min_luma_transform_block_size_minus2) |
	      VE_DEC_H265_DEC_SPS_HDR_LOG2_DIFF_MAX_MIN_LUMA_CODING_BLOCK_SIZE(sps->log2_diff_max_min_luma_coding_block_size) |
	      VE_DEC_H265_DEC_SPS_HDR_LOG2_MIN_LUMA_CODING_BLOCK_SIZE_MINUS3(sps->log2_min_luma_coding_block_size_minus3) |
	      VE_DEC_H265_DEC_SPS_HDR_BIT_DEPTH_CHROMA_MINUS8(sps->bit_depth_chroma_minus8) |
	      VE_DEC_H265_DEC_SPS_HDR_SEPARATE_COLOUR_PLANE_FLAG(sps->separate_colour_plane_flag) |
	      VE_DEC_H265_DEC_SPS_HDR_CHROMA_FORMAT_IDC(sps->chroma_format_idc);
	cedrus_write(dev, VE_DEC_H265_DEC_SPS_HDR, reg);

	// TODO: PCM
	cedrus_write(dev, 0x0000050c, 0x00000000); // PCM

	reg = VE_DEC_H265_DEC_PPS_CTRL0_PPS_CR_QP_OFFSET(pps->pps_cr_qp_offset) |
	      VE_DEC_H265_DEC_PPS_CTRL0_PPS_CB_QP_OFFSET(pps->pps_cb_qp_offset) |
	      VE_DEC_H265_DEC_PPS_CTRL0_INIT_QP_MINUS26(pps->init_qp_minus26) |
	      VE_DEC_H265_DEC_PPS_CTRL0_DIFF_CU_QP_DELTA_DEPTH(pps->diff_cu_qp_delta_depth) |
	      VE_DEC_H265_DEC_PPS_CTRL0_CU_QP_DELTA_ENABLED_FLAG(pps->cu_qp_delta_enabled_flag) |
	      VE_DEC_H265_DEC_PPS_CTRL0_TRANSFORM_SKIP_ENABLED_FLAG(pps->transform_skip_enabled_flag) |
	      VE_DEC_H265_DEC_PPS_CTRL0_CONSTRAINED_INTRA_PRED_FLAG(pps->constrained_intra_pred_flag) |
	      VE_DEC_H265_DEC_PPS_CTRL0_SIGN_DATA_HIDING_FLAG(pps->sign_data_hiding_enabled_flag);
	cedrus_write(dev, VE_DEC_H265_DEC_PPS_CTRL0, reg);

	/* TODO: Support for tile entry-points. */
	reg = VE_DEC_H265_DEC_PPS_CTRL1_LOG2_PARALLEL_MERGE_LEVEL_MINUS2(pps->log2_parallel_merge_level_minus2) |
	      VE_DEC_H265_DEC_PPS_CTRL1_LOOP_FILTER_ACROSS_SLICES_ENABLED_FLAG(pps->pps_loop_filter_across_slices_enabled_flag) |
	      VE_DEC_H265_DEC_PPS_CTRL1_LOOP_FILTER_ACROSS_TILES_ENABLED_FLAG(pps->loop_filter_across_tiles_enabled_flag) |
	      VE_DEC_H265_DEC_PPS_CTRL1_ENTROPY_CODING_SYNC_ENABLED_FLAG(pps->entropy_coding_sync_enabled_flag) |
	      VE_DEC_H265_DEC_PPS_CTRL1_TILES_ENABLED_FLAG(0) |
	      VE_DEC_H265_DEC_PPS_CTRL1_TRANSQUANT_BYPASS_ENABLE_FLAG(pps->transquant_bypass_enabled_flag) |
	      VE_DEC_H265_DEC_PPS_CTRL1_WEIGHTED_BIPRED_FLAG(pps->weighted_bipred_flag) |
	      VE_DEC_H265_DEC_PPS_CTRL1_WEIGHTED_PRED_FLAG(pps->weighted_pred_flag);
	cedrus_write(dev, VE_DEC_H265_DEC_PPS_CTRL1, reg);

	/* TODO: Support for picture type for interlaced videos. */
      	reg = VE_DEC_H265_DEC_SLICE_HDR_INFO0_PICTURE_TYPE(0) |
	      VE_DEC_H265_DEC_SLICE_HDR_INFO0_FIVE_MINUS_MAX_NUM_MERGE_CAND(slice_params->five_minus_max_num_merge_cand) |
	      VE_DEC_H265_DEC_SLICE_HDR_INFO0_NUM_REF_IDX_L1_ACTIVE_MINUS1(slice_params->num_ref_idx_l1_active_minus1) |
	      VE_DEC_H265_DEC_SLICE_HDR_INFO0_NUM_REF_IDX_L0_ACTIVE_MINUS1(slice_params->num_ref_idx_l0_active_minus1) |
	      VE_DEC_H265_DEC_SLICE_HDR_INFO0_COLLOCATED_REF_IDX(slice_params->collocated_ref_idx) |
	      VE_DEC_H265_DEC_SLICE_HDR_INFO0_COLLOCATED_FROM_L0_FLAG(slice_params->collocated_from_l0_flag) |
	      VE_DEC_H265_DEC_SLICE_HDR_INFO0_CABAC_INIT_FLAG(slice_params->cabac_init_flag) |
	      VE_DEC_H265_DEC_SLICE_HDR_INFO0_MVD_L1_ZERO_FLAG(slice_params->mvd_l1_zero_flag) |
	      VE_DEC_H265_DEC_SLICE_HDR_INFO0_SLICE_SAO_CHROMA_FLAG(slice_params->slice_sao_chroma_flag) |
	      VE_DEC_H265_DEC_SLICE_HDR_INFO0_SLICE_SAO_LUMA_FLAG(slice_params->slice_sao_luma_flag) |
	      VE_DEC_H265_DEC_SLICE_HDR_INFO0_SLICE_TEMPORAL_MVP_ENABLE_FLAG(slice_params->slice_temporal_mvp_enabled_flag) |
	      VE_DEC_H265_DEC_SLICE_HDR_INFO0_COLOUR_PLANE_ID(slice_params->colour_plane_id) |
	      VE_DEC_H265_DEC_SLICE_HDR_INFO0_SLICE_TYPE(slice_params->slice_type) |
	      VE_DEC_H265_DEC_SLICE_HDR_INFO0_DEPENDENT_SLICE_SEGMENT_FLAG(pps->dependent_slice_segment_flag) |
	      VE_DEC_H265_DEC_SLICE_HDR_INFO0_FIRST_SLICE_SEGMENT_IN_PIC_FLAG(1);
	cedrus_write(dev, VE_DEC_H265_DEC_SLICE_HDR_INFO0, reg);

	reg = VE_DEC_H265_DEC_SLICE_HDR_INFO1_SLICE_TC_OFFSET_DIV2(slice_params->slice_tc_offset_div2) |
	      VE_DEC_H265_DEC_SLICE_HDR_INFO1_SLICE_BETA_OFFSET_DIV2(slice_params->slice_beta_offset_div2) |
	      VE_DEC_H265_DEC_SLICE_HDR_INFO1_SLICE_DEBLOCKING_FILTER_DISABLED_FLAG(slice_params->slice_deblocking_filter_disabled_flag) |
	      VE_DEC_H265_DEC_SLICE_HDR_INFO1_SLICE_LOOP_FILTER_ACROSS_SLICES_ENABLED_FLAG(slice_params->slice_loop_filter_across_slices_enabled_flag) |
	      VE_DEC_H265_DEC_SLICE_HDR_INFO1_SLICE_POC_BIGEST_IN_RPS_ST(slice_params->num_rps_poc_st_curr_after == 0) |
	      VE_DEC_H265_DEC_SLICE_HDR_INFO1_SLICE_CR_QP_OFFSET(slice_params->slice_cr_qp_offset) |
	      VE_DEC_H265_DEC_SLICE_HDR_INFO1_SLICE_CB_QP_OFFSET(slice_params->slice_cb_qp_offset) |
	      VE_DEC_H265_DEC_SLICE_HDR_INFO1_SLICE_QP_DELTA(slice_params->slice_qp_delta);
	cedrus_write(dev, VE_DEC_H265_DEC_SLICE_HDR_INFO1, reg);

	/* TODO: Support for tile entry-points. */
	chroma_log2_weight_denom = slice_params->pred_weight_table.luma_log2_weight_denom + slice_params->pred_weight_table.delta_chroma_log2_weight_denom;
	reg = VE_DEC_H265_DEC_SLICE_HDR_INFO2_NUM_ENTRY_POINT_OFFSETS(0) |
	      VE_DEC_H265_DEC_SLICE_HDR_INFO2_CHROMA_LOG2_WEIGHT_DENOM(slice_params->pred_weight_table.luma_log2_weight_denom) |
	      VE_DEC_H265_DEC_SLICE_HDR_INFO2_LUMA_LOG2_WEIGHT_DENOM(chroma_log2_weight_denom);
	cedrus_write(dev, VE_DEC_H265_DEC_SLICE_HDR_INFO2, reg);

	// picture header
//	cedrus_write(dev, 0x00000500, 0x00000054); // nal header
//	cedrus_write(dev, 0x00000504, 0x07019801); // SPS
//	cedrus_write(dev, 0x00000508, 0x01680280); // pic size
//	cedrus_write(dev, 0x0000050c, 0x00000000); // PCM
// 	cedrus_write(dev, 0x00000510, 0x00000019); // PPS 0
//	cedrus_write(dev, 0x00000514, 0x00000051); // PPS 1

	reg = VE_DEC_H265_DEC_PIC_SIZE_WIDTH(ctx->src_fmt.width) |
	      VE_DEC_H265_DEC_PIC_SIZE_HEIGHT(ctx->src_fmt.height);

	cedrus_write(dev, VE_DEC_H265_DEC_PIC_SIZE, reg);

	// slice header
//	cedrus_write(dev, 0x00000520, 0x00000989); // slice header 0
//	cedrus_write(dev, 0x00000524, 0x0060002b); // slice header 1
//	cedrus_write(dev, 0x00000528, 0x00000500); // slice header 2

	/* Scaling list */
	reg = VE_DEC_H265_SCALING_LIST_CTRL0_DEFAULT;
	cedrus_write(dev, VE_DEC_H265_SCALING_LIST_CTRL0, reg);

	// No tiles entry point list!
	// cedrus_write(dev, 0x00000580, 0x00000000); // entry point low addr

	// neightbor info addr

	reg = VE_DEC_H265_NEIGHBOR_INFO_ADDR_BASE(ctx->codec.h265.neighbor_info_buf_addr);
	cedrus_write(dev, VE_DEC_H265_NEIGHBOR_INFO_ADDR, reg);

	output_frame_index = 16;

	mv_col_buf_addr = ctx->codec.h265.mv_col_buf_addr; // FIXME: per-frame
	dst_luma_addr = cedrus_dst_buf_addr(ctx, run->dst->vb2_buf.index, 0); // FIXME - PHYS_OFFSET ?
	dst_chroma_addr = cedrus_dst_buf_addr(ctx, run->dst->vb2_buf.index, 1); // FIXME - PHYS_OFFSET ?

	cedrus_h265_pic_list_write_single(dev, output_frame_index, 0, mv_col_buf_addr, dst_luma_addr, dst_chroma_addr);

	cedrus_write(dev, VE_DEC_H265_OUTPUT_FRAME_IDX, output_frame_index);

	/* Enable appropriate interruptions. */
	cedrus_write(dev, VE_DEC_H265_CTRL, VE_DEC_H265_CTRL_IRQ_MASK);
}

static int cedrus_h265_start(struct cedrus_ctx *ctx)
{
	struct cedrus_dev *dev = ctx->dev;
	int ret;

	printk(KERN_ERR "%s()\n", __func__);

	ctx->codec.h265.mv_col_buf =
		dma_alloc_coherent(dev->dev, CEDRUS_H265_MV_COL_BUF_SIZE,
				   &ctx->codec.h265.mv_col_buf_addr,
				   GFP_KERNEL);
	if (!ctx->codec.h265.mv_col_buf)
		return -ENOMEM;

	ctx->codec.h265.neighbor_info_buf =
		dma_alloc_coherent(dev->dev, CEDRUS_H265_NEIGHBOR_INFO_BUF_SIZE,
				   &ctx->codec.h265.neighbor_info_buf_addr,
				   GFP_KERNEL);
	if (!ctx->codec.h265.neighbor_info_buf) {
		ret = -ENOMEM;
		goto err_mv_col_buf;
	}

	ctx->codec.h265.entry_points_buf =
		dma_alloc_coherent(dev->dev, CEDRUS_H265_ENTRY_POINTS_BUF_SIZE,
				   &ctx->codec.h265.entry_points_buf_addr,
				   GFP_KERNEL);
	if (!ctx->codec.h265.entry_points_buf) {
		ret = -ENOMEM;
		goto err_neighbor_info_buf;
	}

	return 0;

err_mv_col_buf:
	dma_free_coherent(dev->dev, CEDRUS_H265_MV_COL_BUF_SIZE,
			  ctx->codec.h265.mv_col_buf,
			  ctx->codec.h265.mv_col_buf_addr);

err_neighbor_info_buf:
	dma_free_coherent(dev->dev, CEDRUS_H265_NEIGHBOR_INFO_BUF_SIZE,
			  ctx->codec.h265.neighbor_info_buf,
			  ctx->codec.h265.neighbor_info_buf_addr);

	return ret;
}

static void cedrus_h265_stop(struct cedrus_ctx *ctx)
{
	struct cedrus_dev *dev = ctx->dev;

	printk(KERN_ERR "%s()\n", __func__);

	dma_free_coherent(dev->dev, CEDRUS_H265_MV_COL_BUF_SIZE,
			  ctx->codec.h265.mv_col_buf,
			  ctx->codec.h265.mv_col_buf_addr);

	dma_free_coherent(dev->dev, CEDRUS_H265_NEIGHBOR_INFO_BUF_SIZE,
			  ctx->codec.h265.neighbor_info_buf,
			  ctx->codec.h265.neighbor_info_buf_addr);

	dma_free_coherent(dev->dev, CEDRUS_H265_ENTRY_POINTS_BUF_SIZE,
			  ctx->codec.h265.entry_points_buf,
			  ctx->codec.h265.entry_points_buf_addr);
}

static void cedrus_h265_trigger(struct cedrus_ctx *ctx)
{
	struct cedrus_dev *dev = ctx->dev;

	printk(KERN_ERR "%s()\n", __func__);

	cedrus_write(dev, VE_DEC_H265_TRIGGER, VE_DEC_H265_TRIGGER_DEC_SLICE);
}

struct cedrus_dec_ops cedrus_dec_ops_h265 = {
	.irq_clear	= cedrus_h265_irq_clear,
	.irq_disable	= cedrus_h265_irq_disable,
	.irq_status	= cedrus_h265_irq_status,
	.setup		= cedrus_h265_setup,
	.start		= cedrus_h265_start,
	.stop		= cedrus_h265_stop,
	.trigger	= cedrus_h265_trigger,
};
