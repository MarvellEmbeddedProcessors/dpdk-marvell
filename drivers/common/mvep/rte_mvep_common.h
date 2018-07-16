/*  SPDX-License-Identifier: BSD-3-Clause
 *  Copyright(c) 2018 Marvell International Ltd.
 */

#ifndef __RTE_MVEP_COMMON_H__
#define __RTE_MVEP_COMMON_H__

#include <rte_kvargs.h>

/* TODO - remove once giu will be serialized in the standard way */
#define REGFILE_VAR_DIR         "/var/"
#define REGFILE_NAME_PREFIX     "nic-pf-"
#define REGFILE_MAX_FILE_NAME   64

enum mvep_module_type {
	MVEP_MOD_T_NONE = 0,
	MVEP_MOD_T_PP2,
	MVEP_MOD_T_SAM,
	MVEP_MOD_T_GIU,
	MVEP_MOD_T_NETA,

	MVEP_MOD_T_LAST
};

int rte_mvep_init(enum mvep_module_type module, struct rte_kvargs *kvlist);
int rte_mvep_deinit(enum mvep_module_type module);

#endif /* __RTE_MVEP_COMMON_H__ */
