/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(C) 2021 Marvell.
 */

#ifndef _CNXK_BPHY_CGX_H_
#define _CNXK_BPHY_CGX_H_

#include <rte_log.h>

extern int bphy_cgx_rawdev_logtype;

#define BPHY_CGX_LOG(level, fmt, args...) \
	rte_log(RTE_LOG_ ## level, bphy_cgx_rawdev_logtype, "%s(): " fmt "\n", __func__, ##args)

int cnxk_bphy_cgx_dev_selftest(uint16_t dev_id);

#endif /* _CNXK_BPHY_CGX_H_ */
