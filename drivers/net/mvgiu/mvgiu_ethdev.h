/*  SPDX-License-Identifier: BSD-3-Clause
 *  Copyright(c) 2018 Marvell International Ltd.
 */

#ifndef _MVGIU_ETHDEV_H_
#define _MVGIU_ETHDEV_H_

#include <rte_spinlock.h>
#include <rte_flow_driver.h>
#include <rte_ethdev.h>
#include <env/mv_autogen_comp_flags.h>
#include <drivers/mv_giu_bpool.h>
#include <drivers/mv_giu_gpio.h>

#define MVGIU_MAX_NUM_TCS_PER_PORT	1
#define MVGIU_MAX_NUM_QS_PER_TC		1

#define MVGIU_MAX_RX_BURST_SIZE		32
#define MVGIU_MAX_TX_BURST_SIZE		32

/** Maximum number of rx queues per port */
#define MVGIU_RXQ_MAX (GIU_GPIO_MAX_NUM_TCS * GIU_GPIO_TC_MAX_NUM_QS)

/** Maximum number of tx queues per port */
#define MVGIU_TXQ_MAX (GIU_GPIO_MAX_NUM_TCS)

/** Maximum number of descriptors in tx queue */
#define MVGIU_TXD_MAX 2048

/** Minimum number of descriptors in tx queue */
#define MVGIU_TXD_MIN MVGIU_TXD_MAX

/** Tx queue descriptors alignment */
#define MVGIU_TXD_ALIGN 16

/** Maximum number of descriptors in rx queue */
#define MVGIU_RXD_MAX 2048

/** Minimum number of descriptors in rx queue */
#define MVGIU_RXD_MIN MVGIU_RXD_MAX

/** Rx queue descriptors alignment */
#define MVGIU_RXD_ALIGN 16

/** Maximum number of descriptors in shadow queue. Must be power of 2 */
#define MVGIU_TX_SHADOWQ_SIZE MVGIU_TXD_MAX

/** Shadow queue size mask (since shadow queue size is power of 2) */
#define MVGIU_TX_SHADOWQ_MASK (MVGIU_TX_SHADOWQ_SIZE - 1)

/** Minimum number of sent buffers to release from shadow queue to BM */
#define MVGIU_BUF_RELEASE_BURST_SIZE	64

#define MVGIU_MAC_ADDRS_MAX 1

#define MVGIU_PKT_EFFEC_OFFS (0)
#define MVGIU_PKT_SIZE_MAX (10240)


struct mvgiu_priv {
	/* Hot fields, used in fast path. */
	struct giu_bpool *bpool;  /**< BPool pointer */
	struct giu_gpio	*gpio;    /**< Port handler pointer */
	rte_spinlock_t lock;	  /**< Spinlock for checking bpool status */
	uint16_t bpool_max_size;  /**< BPool maximum size */
	uint16_t bpool_min_size;  /**< BPool minimum size  */
	uint16_t bpool_init_size; /**< Configured BPool size  */

	/** Mapping for DPDK rx queue->(TC, MRVL relative inq) */
	struct {
		uint8_t tc;  /**< Traffic Class */
		uint8_t inq; /**< Relative in-queue number */
	} rxq_map[MVGIU_RXQ_MAX] __rte_cache_aligned;

	uint16_t nb_rx_queues;

	struct giu_bpool_capabilities bpool_capa;
	struct giu_gpio_capabilities gpio_capa;
};

#endif /* _MVGIU_ETHDEV_H_ */
