/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2018 Marvell International Ltd.
 * Copyright(c) 2018 Semihalf.
 * All rights reserved.
 */

#ifndef _MVNETA_ETHDEV_H_
#define _MVNETA_ETHDEV_H_

/*
 * container_of is defined by both DPDK and MUSDK,
 * we'll declare only one version.
 *
 * Note that it is not used in this PMD anyway.
 */
#ifdef container_of
#undef container_of
#endif

#include <drivers/mv_neta.h>
#include <drivers/mv_neta_ppio.h>

/** Packet offset inside RX buffer. */
#define MRVL_NETA_PKT_OFFS 64

/** Maximum number of rx/tx queues per port */
#define MRVL_NETA_RXQ_MAX 8
#define MRVL_NETA_TXQ_MAX 8

/** Minimum/maximum number of descriptors in tx queue TODO is it? */
#define MRVL_NETA_TXD_MIN 16
#define MRVL_NETA_TXD_MAX 2048

/** Tx queue descriptors alignment in B */
#define MRVL_NETA_TXD_ALIGN 32

/** Minimum/maximum number of descriptors in rx queue TODO is it? */
#define MRVL_NETA_RXD_MIN 16
#define MRVL_NETA_RXD_MAX 2048

/** Rx queue descriptors alignment in B */
#define MRVL_NETA_RXD_ALIGN 32

#define MRVL_NETA_DEFAULT_TC 0

/** Maximum number of descriptors in shadow queue. Must be power of 2 */
#define MRVL_NETA_TX_SHADOWQ_SIZE MRVL_NETA_TXD_MAX

/** Shadow queue size mask (since shadow queue size is power of 2) */
#define MRVL_NETA_TX_SHADOWQ_MASK (MRVL_NETA_TX_SHADOWQ_SIZE - 1)

/** Minimum number of sent buffers to release from shadow queue to BM */
#define MRVL_NETA_BUF_RELEASE_BURST_SIZE	16

#define MVRL_NETA_RX_FREE_THRESH (MRVL_NETA_BUF_RELEASE_BURST_SIZE * 2)

#define MRVL_NETA_MTU_TO_MRU(mtu) \
	((mtu) + MV_MH_SIZE + ETHER_HDR_LEN + ETHER_CRC_LEN)
#define MRVL_NETA_MRU_TO_MTU(mru) \
	((mru) - MV_MH_SIZE - ETHER_HDR_LEN + ETHER_CRC_LEN)

struct mvneta_priv {
	/* Hot fields, used in fast path. */
	struct neta_ppio	*ppio;    /**< Port handler pointer */

	uint8_t pp_id;
	uint8_t ppio_id;	/* ppio port id */

	struct neta_ppio_params ppio_params;
	uint16_t nb_rx_queues;

	uint64_t rate_max;
};

#endif /* _MVNETA_ETHDEV_H_ */
