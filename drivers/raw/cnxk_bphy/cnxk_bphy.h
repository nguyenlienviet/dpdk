/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(C) 2023 Marvell.
 */

#ifndef _CNXK_BPHY_H_
#define _CNXK_BPHY_H_

#include <rte_log.h>

extern int bphy_rawdev_logtype;

#define BPHY_LOG(level, fmt, args...) \
	rte_log(RTE_LOG_ ## level, bphy_rawdev_logtype, "%s(): " fmt "\n", __func__, ##args)

#endif /* _CNXK_BPHY_H_ */
