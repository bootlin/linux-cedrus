/* SPDX-License-Identifier: GPL-2.0 */
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

#ifndef _CEDRUS_MPEG2_H_
#define _CEDRUS_MPEG2_H_

struct cedrus_ctx;
struct cedrus_run;

void cedrus_mpeg2_setup(struct cedrus_ctx *ctx, struct cedrus_run *run);
void cedrus_mpeg2_trigger(struct cedrus_ctx *ctx);

#endif
