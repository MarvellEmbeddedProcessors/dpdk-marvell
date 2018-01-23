/*-
 *   BSD LICENSE
 *
 *   Copyright(c) 2017 Marvell International Ltd.
 *   Copyright(c) 2017 Semihalf.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of the copyright holder nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef _MRVL_ETHDEV_H_
#define _MRVL_ETHDEV_H_

#include <rte_spinlock.h>
#include <rte_flow_driver.h>
#include <rte_mtr_driver.h>
#include <rte_tm_driver.h>

/*
 * container_of is defined by both DPDK and MUSDK,
 * we'll declare only one version.
 *
 * Note that it is not used in this PMD anyway.
 */
#ifdef container_of
#undef container_of
#endif

#include <env/mv_autogen_comp_flags.h>
#include <drivers/mv_pp2.h>
#include <drivers/mv_pp2_bpool.h>
#include <drivers/mv_pp2_cls.h>
#include <drivers/mv_pp2_hif.h>
#include <drivers/mv_pp2_ppio.h>
#include "env/mv_common.h" /* for BIT() */

/** Maximum number of rx queues per port */
#define MRVL_PP2_RXQ_MAX 32

/** Maximum number of tx queues per port */
#define MRVL_PP2_TXQ_MAX 8

/** Minimum number of descriptors in tx queue */
#define MRVL_PP2_TXD_MIN 16

/** Maximum number of descriptors in tx queue */
#define MRVL_PP2_TXD_MAX 2048

/** Tx queue descriptors alignment */
#define MRVL_PP2_TXD_ALIGN 16

/** Minimum number of descriptors in rx queue */
#define MRVL_PP2_RXD_MIN 16

/** Maximum number of descriptors in rx queue */
#define MRVL_PP2_RXD_MAX 2048

/** Rx queue descriptors alignment */
#define MRVL_PP2_RXD_ALIGN 16

/** Maximum number of descriptors in tx aggregated queue */
#define MRVL_PP2_AGGR_TXQD_MAX 2048

/** Maximum number of Traffic Classes. */
#define MRVL_PP2_TC_MAX 8

/** Packet offset inside RX buffer. */
#define MRVL_PKT_OFFS 64

/** Maximum number of descriptors in shadow queue. Must be power of 2 */
#define MRVL_PP2_TX_SHADOWQ_SIZE MRVL_PP2_TXD_MAX

/** Shadow queue size mask (since shadow queue size is power of 2) */
#define MRVL_PP2_TX_SHADOWQ_MASK (MRVL_PP2_TX_SHADOWQ_SIZE - 1)

/** Minimum number of sent buffers to release from shadow queue to BM */
#define MRVL_PP2_BUF_RELEASE_BURST_SIZE	64

/* TCAM has 25 entries reserved for uc/mc filter entries */
#define MRVL_MAC_ADDRS_MAX 25

/** Maximum number of VLAN tags in initial configuration. */
/* There is a TCAM range reserved for VLAN filtering entries, range size is 33
 * 10 VLAN ID filter entries per port
 * 1 default VLAN filter entry per port
 * It is assumed that there are 3 ports for filter, not including loopback port
 */
#define MRVL_PRS_VLAN_FILT_MAX 10



struct mrvl_config {
	int is_set_mtu;
	uint16_t mtu;
	int is_link_down;
	int is_promisc;
	int is_mc_promisc;
	uint32_t mac_addr_to_add_idx[MRVL_MAC_ADDRS_MAX];
	struct ether_addr mac_addr_to_add[MRVL_MAC_ADDRS_MAX];
	int mac_addr_add_num;
	struct ether_addr mac_addr_to_set;
	int is_mac_addr_to_set;
	uint16_t vlan_fltrs_to_add[MRVL_PRS_VLAN_FILT_MAX];
	int vlan_fltrs_num;
};

/** Maximum length of a match string */
#define MRVL_MATCH_LEN 16

/** Parsed fields in processed rte_flow_item. */
enum mrvl_parsed_fields {
	/* eth flags */
	F_DMAC =         BIT(0),
	F_SMAC =         BIT(1),
	F_TYPE =         BIT(2),
	/* vlan flags */
	F_VLAN_PRI =     BIT(3),
	F_VLAN_ID =      BIT(4),
	F_VLAN_TCI =     BIT(5), /* not supported by MUSDK yet */
	/* ip4 flags */
	F_IP4_TOS =      BIT(6),
	F_IP4_SIP =      BIT(7),
	F_IP4_DIP =      BIT(8),
	F_IP4_PROTO =    BIT(9),
	/* ip6 flags */
	F_IP6_TC =       BIT(10), /* not supported by MUSDK yet */
	F_IP6_SIP =      BIT(11),
	F_IP6_DIP =      BIT(12),
	F_IP6_FLOW =     BIT(13),
	F_IP6_NEXT_HDR = BIT(14),
	/* tcp flags */
	F_TCP_SPORT =    BIT(15),
	F_TCP_DPORT =    BIT(16),
	/* udp flags */
	F_UDP_SPORT =    BIT(17),
	F_UDP_DPORT =    BIT(18),
};

/** PMD-specific definition of a flow rule handle. */
struct mrvl_mtr;
struct rte_flow {
	LIST_ENTRY(rte_flow) next;
	struct mrvl_mtr *mtr;

	enum mrvl_parsed_fields pattern;

	struct pp2_cls_tbl_rule rule;
	struct pp2_cls_cos_desc cos;
	struct pp2_cls_tbl_action action;
};

struct mrvl_mtr_profile {
	LIST_ENTRY(mrvl_mtr_profile) next;
	uint32_t profile_id;
	int refcnt;
	struct rte_mtr_meter_profile profile;
};

struct mrvl_mtr {
	LIST_ENTRY(mrvl_mtr) next;
	uint32_t mtr_id;
	int refcnt;
	int shared;
	int enabled;
	int plcr_bit;
	struct mrvl_mtr_profile *profile;
	struct pp2_cls_plcr *plcr;
};

struct mrvl_tm_shaper_profile {
	LIST_ENTRY(mrvl_tm_shaper_profile) next;
	uint32_t id;
	int refcnt;
	struct rte_tm_shaper_params params;
};

enum {
	MRVL_NODE_PORT,
	MRVL_NODE_QUEUE,
};

struct mrvl_tm_node {
	LIST_ENTRY(mrvl_tm_node) next;
	uint32_t id;
	uint32_t type;
	int refcnt;
	struct mrvl_tm_node *parent;
	struct mrvl_tm_shaper_profile *profile;
	uint8_t weight;
	uint64_t stats_mask;
};

struct mrvl_priv {
	/* Hot fields, used in fast path. */
	struct pp2_bpool *bpool;  /**< BPool pointer */
	struct pp2_ppio	*ppio;    /**< Port handler pointer */
	rte_spinlock_t lock;	  /**< Spinlock for checking bpool status */
	uint16_t bpool_max_size;  /**< BPool maximum size */
	uint16_t bpool_min_size;  /**< BPool minimum size  */
	uint16_t bpool_init_size; /**< Configured BPool size  */

	/** Mapping for DPDK rx queue->(TC, MRVL relative inq) */
	struct {
		uint8_t tc;  /**< Traffic Class */
		uint8_t inq; /**< Relative in-queue number */
	} rxq_map[MRVL_PP2_RXQ_MAX] __rte_cache_aligned;

	/* Configuration data, used sporadically. */
	uint8_t pp_id;
	uint8_t ppio_id;
	uint8_t bpool_bit;
	uint8_t rss_hf_tcp;
	uint8_t uc_mc_flushed;
	uint8_t vlan_flushed;
	uint8_t isolated;
	struct mrvl_config init_cfg;

	struct pp2_ppio_params ppio_params;
	struct pp2_cls_qos_tbl_params qos_tbl_params;
	struct pp2_cls_tbl *qos_tbl;
	uint16_t nb_rx_queues;

	struct pp2_cls_tbl_params cls_tbl_params;
	struct pp2_cls_tbl *cls_tbl;
	uint32_t cls_tbl_pattern;
	LIST_HEAD(mrvl_flows, rte_flow) flows;

	struct pp2_cls_plcr *default_policer;

	LIST_HEAD(profiles, mrvl_mtr_profile) profiles;
	LIST_HEAD(mtrs, mrvl_mtr) mtrs;
	uint32_t used_plcrs;

	LIST_HEAD(shaper_profiles, mrvl_tm_shaper_profile) shaper_profiles;
	LIST_HEAD(nodes, mrvl_tm_node) nodes;
	uint64_t rate_max;
};

/** Flow operations forward declaration. */
extern const struct rte_flow_ops mrvl_flow_ops;

/** Meter operations forward declaration. */
extern const struct rte_mtr_ops mrvl_mtr_ops;

/** Traffic manager operations forward declaration. */
extern const struct rte_tm_ops mrvl_tm_ops;

#endif /* _MRVL_ETHDEV_H_ */
