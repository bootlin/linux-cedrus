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

#ifndef _CEDRUS_HW_H_
#define _CEDRUS_HW_H_

enum cedrus_engine {
	CEDRUS_ENGINE_MPEG,
};

int cedrus_engine_enable(struct cedrus_dev *dev,
			       enum cedrus_engine engine);
void cedrus_engine_disable(struct cedrus_dev *dev);

int cedrus_hw_probe(struct cedrus_dev *dev);
void cedrus_hw_remove(struct cedrus_dev *dev);

#endif
