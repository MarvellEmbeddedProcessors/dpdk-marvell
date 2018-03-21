/*  SPDX-License-Identifier: BSD-3-Clause
 *  Copyright(c) 2018 Marvell International Ltd.
 */

#include <rte_common.h>
#include <rte_log.h>
#include <fcntl.h>
#include <unistd.h>

#include <env/mv_autogen_comp_flags.h>
#include <env/mv_sys_dma.h>
#include <mng/mv_nmp_guest.h>

#include "mv_mvep_config.h"
#include "rte_mvep_common.h"

/* NMP Guest Timeout (ms)*/
#define NMP_GUEST_TIMEOUT	1000

/* TODO - remove once giu will be serialized in the standard way */
#define REGFILE_VAR_DIR         "/var/"
#define REGFILE_NAME_PREFIX     "nic-pf-"
#define REGFILE_MAX_FILE_NAME   64

int mvep_common_logtype;

#define MVEP_COMMON_LOG(level, fmt, args...) \
	rte_log(RTE_LOG_ ## level, mvep_common_logtype, "%s(): " fmt "\n", \
	__func__, ##args)

struct mvep {
	uint32_t ref_count;

	/* Guest Info */
	struct nmp_guest *nmp_guest;
	char *guest_prb_str;
	struct nmp_guest_info guest_info;
};

static struct mvep mvep;

static int wait_for_pf_init_done(void)
{
	char	file_name[REGFILE_MAX_FILE_NAME];
	int	timeout = 100000; /* 10s timeout */
	int	fd;

	/* Map GIU regfile */
	snprintf(file_name,
		 sizeof(file_name),
		 "%s%s%d", REGFILE_VAR_DIR, REGFILE_NAME_PREFIX, 0);

	/* wait for regfile to be opened by NMP */
	do {
		fd = open(file_name, O_RDWR);
		if (fd > 0)
			close(fd);
		usleep(100);
	} while (fd < 0 && --timeout);

	if (!timeout) {
		MVEP_COMMON_LOG(ERR,
			"failed to find regfile %s. timeout exceeded.\n",
			file_name);
		return -EFAULT;
	}

	return 0;
}

int rte_mvep_init(enum mvep_module_type module,
		  struct rte_kvargs *kvlist __rte_unused)
{
	int ret;

	if (!mvep.ref_count) {
		ret = mv_sys_dma_mem_init(MRVL_MUSDK_DMA_MEMSIZE);
		if (ret)
			return ret;
		mvep_common_logtype = rte_log_register("MVEP_COMMON");
	}

	mvep.ref_count++;

	if (module == MVEP_MOD_T_GIU) {
		struct nmp_guest_params nmp_guest_params;

		ret = wait_for_pf_init_done();
		if (ret)
			return ret;

		/* NMP Guest initializations */
		nmp_guest_params.id = NMP_GUEST_ID;
		nmp_guest_params.timeout = NMP_GUEST_TIMEOUT;
		ret = nmp_guest_init(&nmp_guest_params, &mvep.nmp_guest);
		if (ret)
			return ret;

		nmp_guest_get_probe_str(mvep.nmp_guest, &mvep.guest_prb_str);
		nmp_guest_get_relations_info(mvep.nmp_guest, &mvep.guest_info);
	}

	return 0;
}

int rte_mvep_deinit(enum mvep_module_type module __rte_unused)
{
	mvep.ref_count--;

	if (!mvep.ref_count)
		mv_sys_dma_mem_destroy();

	if (mvep.nmp_guest) {
		nmp_guest_deinit(mvep.nmp_guest);
		mvep.nmp_guest = NULL;
	}

	return 0;
}

