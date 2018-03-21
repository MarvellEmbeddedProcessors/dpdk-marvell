/*  SPDX-License-Identifier: BSD-3-Clause
 *  Copyright(c) 2018 Marvell International Ltd.
 */

#include <rte_ethdev_driver.h>
#include <rte_kvargs.h>
#include <rte_log.h>
#include <rte_malloc.h>
#include <rte_bus_vdev.h>
#include <rte_net.h>

/* Unluckily, container_of is defined by both DPDK and MUSDK,
 * we'll declare only one version.
 *
 * Note that it is not used in this PMD anyway.
 */
#ifdef container_of
#undef container_of
#endif

#include <fcntl.h>
#include <linux/ethtool.h>
#include <linux/sockios.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <rte_mvep_common.h>
#include "mvgiu_ethdev.h"

/* prefetch shift */
#define MRVL_MUSDK_PREFETCH_SHIFT 2
#define MRVL_IFACE_NAME_ARG "iface"
#define MRVL_CFG_ARG "cfg"
#define MRVL_COOKIE_ADDR_INVALID ~0ULL
#define MRVL_COOKIE_HIGH_ADDR_MASK (0xffffff0000000000)

#define MRVL_BURST_SIZE 64

/** Port Rx offload capabilities */
#define MVGIU_RX_OFFLOADS (DEV_RX_OFFLOAD_CRC_STRIP)

/** Port Tx offloads capabilities */
#define MVGIU_TX_OFFLOADS (0)

#define MVGIU_SW_PARSE

static const char * const valid_args[] = {
	MRVL_IFACE_NAME_ARG,
	MRVL_CFG_ARG,
	NULL
};

static uint64_t cookie_addr_high = MRVL_COOKIE_ADDR_INVALID;
struct giu_bpool *mvgiu_port_to_bpool_lookup[RTE_MAX_ETHPORTS];
int mvgiu_port_bpool_size[GIU_BPOOL_NUM_POOLS][RTE_MAX_LCORE];

struct mvgiu_ifnames {
	const char *names[1];
	int idx;
};

/*
 * To use buffer harvesting based on loopback port shadow queue structure
 * was introduced for buffers information bookkeeping.
 *
 * Before sending the packet, related buffer information (pp2_buff_inf) is
 * stored in shadow queue. After packet is transmitted no longer used
 * packet buffer is released back to it's original hardware pool,
 * on condition it originated from interface.
 * In case it  was generated by application itself i.e: mbuf->port field is
 * 0xff then its released to software mempool.
 */
struct mvgiu_shadow_txq {
	int head;           /* write index - used when sending buffers */
	int tail;           /* read index - used when releasing buffers */
	u16 size;           /* queue occupied size */
	u16 num_to_release; /* number of buffers sent, that can be released */
	/* the queue-entries MUST be of type 'giu_buff_inf' as there is an
	 * assumption it is continuous when it is used in 'giu_bpool_put_buffs'
	 */
	struct giu_buff_inf	ent[MVGIU_TX_SHADOWQ_SIZE];
	struct giu_bpool	*bpool[MVGIU_TX_SHADOWQ_SIZE];
};

struct mvgiu_rxq {
	struct mvgiu_priv *priv;
	struct rte_mempool *mp;
	uint32_t size;
	int queue_id;
	int port_id;
	uint64_t bytes_recv;
	uint64_t packets_recv;
	u16 data_offset; /* Offset of the data within the buffer */
};

struct mvgiu_txq {
	struct mvgiu_priv *priv;
	int queue_id;
	int port_id;
	uint64_t bytes_sent;
	uint64_t packets_sent;
	struct mvgiu_shadow_txq shadow_txqs[RTE_MAX_LCORE];
	int tx_deferred_start;
	uint32_t size;
};

/**
 * Release already sent buffers to bpool (buffer-pool).
 *
 * @param sq
 *   Pointer to the shadow queue.
 */
static inline void
mvgiu_free_sent_buffers(struct mvgiu_shadow_txq *sq)
{
	struct giu_buff_inf *entry;
	struct giu_bpool *bpool;
	uint16_t nb_done = 0, num = 0, skip_bufs = 0;
	unsigned int core_id;
	int i;

	core_id = rte_lcore_id();
	if (core_id == LCORE_ID_ANY)
		core_id = 0;

	nb_done = sq->num_to_release;
	sq->num_to_release = 0;

	for (i = 0; i < nb_done; i++) {
		entry = &sq->ent[sq->tail + num];
		bpool = sq->bpool[sq->tail + num];
		if (unlikely(!entry->addr)) {
			RTE_LOG(ERR, PMD,
				"Shadow memory @%d: cookie(%lx), pa(%lx)!\n",
				sq->tail, (u64)entry->cookie,
				(u64)entry->addr);
			skip_bufs = 1;
			goto skip;
		}

		if (unlikely(!bpool)) {
			struct rte_mbuf *mbuf;

			mbuf = (struct rte_mbuf *)entry->cookie;
			rte_pktmbuf_free(mbuf);
			skip_bufs = 1;
			goto skip;
		}

		mvgiu_port_bpool_size[bpool->id][core_id]++;
		num++;
		if (unlikely(sq->tail + num == MVGIU_TX_SHADOWQ_SIZE))
			goto skip;
		continue;
skip:
		if (likely(num))
			giu_bpool_put_buffs(sq->bpool[sq->tail],
					    &sq->ent[sq->tail], &num);
		num += skip_bufs;
		sq->tail = (sq->tail + num) & MVGIU_TX_SHADOWQ_MASK;
		sq->size -= num;
		num = 0;
		skip_bufs = 0;
	}

	if (likely(num)) {
		giu_bpool_put_buffs(sq->bpool[sq->tail],
				    &sq->ent[sq->tail], &num);
		sq->tail = (sq->tail + num) & MVGIU_TX_SHADOWQ_MASK;
		sq->size -= num;
	}
}

/**
 * Release already sent buffers to bpool (buffer-pool).
 *
 * @param gpio
 *   Pointer to the port structure.
 * @param hif
 *   Pointer to the MUSDK hardware interface.
 * @param sq
 *   Pointer to the shadow queue.
 * @param tc
 *   Tc number.
 * @param qid
 *   Queue id number.
 */
static inline
void mvgiu_check_n_free_sent_buffers(struct giu_gpio *gpio,
				     struct mvgiu_shadow_txq *sq,
				     u8 tc,
				     u8 qid)
{
	u16 num_conf = 0;

	giu_gpio_get_num_outq_done(gpio, tc, qid, &num_conf);

	sq->num_to_release += num_conf;

	if (likely(sq->num_to_release < MVGIU_BUF_RELEASE_BURST_SIZE))
		return;

	mvgiu_free_sent_buffers(sq);
}

/**
 * Release buffers to hardware bpool (buffer-pool)
 *
 * @param rxq
 *   Receive queue pointer.
 * @param num
 *   Number of buffers to release to bpool.
 *
 * @return
 *   0 on success, negative error value otherwise.
 */
static int mvgiu_fill_bpool(struct mvgiu_rxq *rxq, int num)
{
	struct giu_buff_inf entries[MVGIU_TXD_MAX];
	struct giu_bpool *bpool;
	struct rte_mbuf *mbufs[MVGIU_TXD_MAX];
	unsigned int core_id;
	int i, ret = 0;

	core_id = rte_lcore_id();
	if (core_id == LCORE_ID_ANY)
		core_id = 0;

	bpool = rxq->priv->bpool;

	ret = rte_pktmbuf_alloc_bulk(rxq->mp, mbufs, num);
	if (ret)
		return ret;

	if (cookie_addr_high == MRVL_COOKIE_ADDR_INVALID)
		cookie_addr_high =
			(uint64_t)mbufs[0] & MRVL_COOKIE_HIGH_ADDR_MASK;

	for (i = 0; i < num; i++) {
		if (((uint64_t)mbufs[i] & MRVL_COOKIE_HIGH_ADDR_MASK)
			!= cookie_addr_high) {
			RTE_LOG(ERR, PMD,
				"mbuf virtual addr high is out of range "
				"0x%x instead of 0x%x\n",
				(uint32_t)((uint64_t)mbufs[i] >> 32),
				(uint32_t)(cookie_addr_high >> 32));
			ret = -1;
			goto out;
		}

		entries[i].addr =
			rte_mbuf_data_iova_default(mbufs[i]);
		entries[i].cookie = (uintptr_t)mbufs[i];
	}

	giu_bpool_put_buffs(bpool, entries, (uint16_t *)&i);
	mvgiu_port_bpool_size[bpool->id][core_id] += i;

out:
	for (; i < num; i++)
		rte_pktmbuf_free(mbufs[i]);

	return ret;
}


static inline uint32_t
mvgiu_get_bpool_size(int pool_id)
{
	uint32_t i, size = 0;

	for (i = 0; i < RTE_MAX_LCORE; i++)
		size += mvgiu_port_bpool_size[pool_id][i];

	return size;
}

/**
 * Configure RX Queues in a given port.
 *
 * Sets up RX queues, their Traffic Classes and DPDK rxq->(TC,inq) mapping.
 *
 * @param priv Port's private data
 * @param max_queues Maximum number of queues to configure.
 * @returns 0 in case of success, negative value otherwise.
 */
static int
mvgiu_configure_rxqs(struct mvgiu_priv *priv, uint16_t max_queues)
{
	size_t i;

	/* Direct mapping of queues i.e. 0->0, 1->1 etc. */
	for (i = 0; i < max_queues; ++i) {
		priv->rxq_map[i].tc = 0;
		priv->rxq_map[i].inq = i;
	}

	return 0;
}

/**
 * Ethernet device configuration.
 *
 * Prepare the driver for a given number of TX and RX queues and
 * configure RSS.
 *
 * @param dev
 *   Pointer to Ethernet device structure.
 *
 * @return
 *   0 on success, negative error value otherwise.
 */
static int
mvgiu_dev_configure(struct rte_eth_dev *dev)
{
	struct mvgiu_priv *priv = dev->data->dev_private;
	int ret;

	if (dev->data->dev_conf.rxmode.mq_mode != ETH_MQ_RX_NONE &&
	    dev->data->dev_conf.rxmode.mq_mode != ETH_MQ_RX_RSS) {
		RTE_LOG(INFO, PMD, "Unsupported rx multi queue mode %d\n",
			dev->data->dev_conf.rxmode.mq_mode);
		return -EINVAL;
	}

	if (!(dev->data->dev_conf.rxmode.offloads & DEV_RX_OFFLOAD_CRC_STRIP)) {
		RTE_LOG(INFO, PMD,
			"L2 CRC stripping is always enabled in hw\n");
		dev->data->dev_conf.rxmode.offloads |= DEV_RX_OFFLOAD_CRC_STRIP;
	}

	if (dev->data->dev_conf.rxmode.offloads & DEV_RX_OFFLOAD_VLAN_STRIP) {
		RTE_LOG(INFO, PMD, "VLAN stripping not supported\n");
		return -EINVAL;
	}

	if (dev->data->dev_conf.rxmode.split_hdr_size) {
		RTE_LOG(INFO, PMD, "Split headers not supported\n");
		return -EINVAL;
	}

	if (dev->data->dev_conf.rxmode.offloads & DEV_RX_OFFLOAD_SCATTER) {
		RTE_LOG(INFO, PMD, "RX Scatter/Gather not supported\n");
		return -EINVAL;
	}

	if (dev->data->dev_conf.rxmode.offloads & DEV_RX_OFFLOAD_TCP_LRO) {
		RTE_LOG(INFO, PMD, "LRO not supported\n");
		return -EINVAL;
	}

	if (dev->data->dev_conf.rxmode.offloads & DEV_RX_OFFLOAD_JUMBO_FRAME)
		dev->data->mtu = dev->data->dev_conf.rxmode.max_rx_pkt_len -
				 ETHER_HDR_LEN - ETHER_CRC_LEN;

	ret = mvgiu_configure_rxqs(priv, dev->data->nb_rx_queues);
	if (ret < 0)
		return ret;

	priv->nb_rx_queues = dev->data->nb_rx_queues;

	/*
	 * Calculate the minimum bpool size for refill feature as follows:
	 * 2 default burst sizes multiply by number of rx queues.
	 * If the bpool size will be below this value, new buffers will
	 * be added to the pool.
	 */
	priv->bpool_min_size = priv->nb_rx_queues * MRVL_BURST_SIZE * 2;

	/*
	 * Calculate the maximum bpool size for refill feature as follows:
	 * maximum number of descriptors in rx queue multiply by number
	 * of rx queues plus minimum bpool size.
	 * In case the bpool size will exceed this value, superfluous buffers
	 * will be removed
	 */
	priv->bpool_max_size = (priv->nb_rx_queues * MVGIU_RXD_MAX) +
				priv->bpool_min_size;

	return 0;
}


/**
 * DPDK callback to bring the link up.
 *
 * @param dev
 *   Pointer to Ethernet device structure.
 *
 * @return
 *   0 on success, negative error value otherwise.
 */
static int
mvgiu_dev_set_link_up(struct rte_eth_dev *dev)
{
	struct mvgiu_priv *priv = dev->data->dev_private;

	giu_gpio_enable(priv->gpio);

	dev->data->dev_link.link_status = ETH_LINK_UP;

	return 0;
}

/**
 * DPDK callback to bring the link down.
 *
 * @param dev
 *   Pointer to Ethernet device structure.
 *
 * @return
 *   0 on success, negative error value otherwise.
 */
static int
mvgiu_dev_set_link_down(struct rte_eth_dev *dev)
{
	struct mvgiu_priv *priv = dev->data->dev_private;

	giu_gpio_disable(priv->gpio);

	dev->data->dev_link.link_status = ETH_LINK_DOWN;
	return 0;
}

/**
 * DPDK callback to start the device.
 *
 * @param dev
 *   Pointer to Ethernet device structure.
 *
 * @return
 *   0 on success, negative errno value on failure.
 */
static int
mvgiu_dev_start(struct rte_eth_dev *dev)
{
	return mvgiu_dev_set_link_up(dev);
}

/**
 * DPDK callback to stop the device.
 *
 * @param dev
 *   Pointer to Ethernet device structure.
 */
static void
mvgiu_dev_stop(struct rte_eth_dev *dev)
{
	mvgiu_dev_set_link_down(dev);
}

/**
 * DPDK callback to retrieve physical link information.
 *
 * @param dev
 *   Pointer to Ethernet device structure.
 * @param wait_to_complete
 *   Wait for request completion (ignored).
 *
 * @return
 *   0 on success, negative error value otherwise.
 */
static int
mvgiu_link_update(struct rte_eth_dev *dev __rte_unused,
		  int wait_to_complete __rte_unused)
{
	dev->data->dev_link.link_speed = ETH_SPEED_NUM_10G;
	dev->data->dev_link.link_duplex = ETH_LINK_FULL_DUPLEX;
	dev->data->dev_link.link_autoneg = ETH_LINK_FIXED;

	return 0;
}

/**
 * Flush receive queues.
 *
 * @param dev
 *   Pointer to Ethernet device structure.
 */
static void
mvgiu_flush_rx_queues(struct rte_eth_dev *dev)
{
	int i, ret;

	RTE_LOG(INFO, PMD, "Flushing rx queues\n");
	for (i = 0; i < dev->data->nb_rx_queues; i++) {
		uint16_t num;

		do {
			struct mvgiu_rxq *q = dev->data->rx_queues[i];
			struct giu_gpio_desc descs[MVGIU_RXD_MAX];

			num = MVGIU_RXD_MAX;
			ret = giu_gpio_recv(q->priv->gpio,
					    q->priv->rxq_map[q->queue_id].tc,
					    q->priv->rxq_map[q->queue_id].inq,
					    descs, &num);
		} while (ret == 0 && num);
	}
}

/**
 * Flush transmit shadow queues.
 *
 * @param dev
 *   Pointer to Ethernet device structure.
 */
static void
mvgiu_flush_tx_shadow_queues(struct rte_eth_dev *dev)
{
	int i, j;
	struct mvgiu_txq *txq;

	RTE_LOG(INFO, PMD, "Flushing tx shadow queues\n");
	for (i = 0; i < dev->data->nb_tx_queues; i++) {
		txq = (struct mvgiu_txq *)dev->data->tx_queues[i];

		for (j = 0; j < RTE_MAX_LCORE; j++) {
			struct mvgiu_shadow_txq *sq;

			sq = &txq->shadow_txqs[j];
			sq->num_to_release = sq->size;
			mvgiu_free_sent_buffers(sq);
			while (sq->tail != sq->head) {
				uint64_t addr = cookie_addr_high |
					sq->ent[sq->tail].cookie;
				rte_pktmbuf_free(
					(struct rte_mbuf *)addr);
				sq->tail = (sq->tail + 1) &
					    MVGIU_TX_SHADOWQ_MASK;
			}
			memset(sq, 0, sizeof(*sq));
		}
	}
}

/**
 * Flush hardware bpool (buffer-pool).
 *
 * @param dev
 *   Pointer to Ethernet device structure.
 */
static void
mvgiu_drain_bpool(struct mvgiu_priv *priv __rte_unused,
		  uint32_t num __rte_unused)
{
	/* TODO - there is no API to get buffers from the pool, so we need to
	 * record all buffers in a local queue
	 */
}

/**
 * Flush hardware bpool (buffer-pool).
 *
 * @param dev
 *   Pointer to Ethernet device structure.
 */
static void
mvgiu_flush_bpool(struct rte_eth_dev *dev)
{
	struct mvgiu_priv *priv = dev->data->dev_private;
	uint32_t num;
	int ret;

	ret = giu_bpool_get_num_buffs(priv->bpool, &num);
	if (ret) {
		RTE_LOG(ERR, PMD, "Failed to get bpool buffers number\n");
		return;
	}

	mvgiu_drain_bpool(priv, num);
}

/**
 * DPDK callback to close the device.
 *
 * @param dev
 *   Pointer to Ethernet device structure.
 */
static void
mvgiu_dev_close(struct rte_eth_dev *dev)
{
	mvgiu_flush_rx_queues(dev);
	mvgiu_flush_tx_shadow_queues(dev);
	mvgiu_flush_bpool(dev);
}

/**
 * DPDK callback to get information about the device.
 *
 * @param dev
 *   Pointer to Ethernet device structure (unused).
 * @param info
 *   Info structure output buffer.
 */
static void
mvgiu_dev_infos_get(struct rte_eth_dev *dev __rte_unused,
		   struct rte_eth_dev_info *info)
{
	info->speed_capa = ETH_LINK_SPEED_10M |
			   ETH_LINK_SPEED_100M |
			   ETH_LINK_SPEED_1G |
			   ETH_LINK_SPEED_10G;

	/* TODO - should be taken from gpio's capabilities */
	info->max_rx_queues = MVGIU_RXQ_MAX;
	/* TODO - should be taken from gpio's capabilities */
	info->max_tx_queues = MVGIU_TXQ_MAX;
	info->max_mac_addrs = 0;

	/* TODO - should be taken from gpio's capabilities */
	info->rx_desc_lim.nb_max = MVGIU_RXD_MAX;
	/* TODO - should be taken from gpio's capabilities */
	info->rx_desc_lim.nb_min = MVGIU_RXD_MIN;
	info->rx_desc_lim.nb_align = MVGIU_RXD_ALIGN;

	/* TODO - should be taken from gpio's capabilities */
	info->tx_desc_lim.nb_max = MVGIU_TXD_MAX;
	/* TODO - should be taken from gpio's capabilities */
	info->tx_desc_lim.nb_min = MVGIU_TXD_MIN;
	info->tx_desc_lim.nb_align = MVGIU_TXD_ALIGN;

	info->rx_offload_capa = MVGIU_RX_OFFLOADS;
	info->rx_queue_offload_capa = MVGIU_RX_OFFLOADS;

	info->tx_offload_capa = MVGIU_TX_OFFLOADS;
	info->tx_queue_offload_capa = MVGIU_TX_OFFLOADS;

	info->flow_type_rss_offloads = 0;

	/* By default packets are dropped if no descriptors are available */
	info->default_rxconf.rx_drop_en = 1;
	info->default_rxconf.offloads = DEV_RX_OFFLOAD_CRC_STRIP;

	info->max_rx_pktlen = MVGIU_PKT_SIZE_MAX;
}

/**
 * Return supported packet types.
 *
 * @param dev
 *   Pointer to Ethernet device structure (unused).
 *
 * @return
 *   Const pointer to the table with supported packet types.
 */
static const uint32_t *
mvgiu_dev_supported_ptypes_get(struct rte_eth_dev *dev __rte_unused)
{
	static const uint32_t ptypes[] = {
		RTE_PTYPE_L2_ETHER,
		RTE_PTYPE_L3_IPV4,
		RTE_PTYPE_L3_IPV4_EXT,
		RTE_PTYPE_L3_IPV4_EXT_UNKNOWN,
		RTE_PTYPE_L3_IPV6,
		RTE_PTYPE_L3_IPV6_EXT,
		RTE_PTYPE_L2_ETHER_ARP,
		RTE_PTYPE_L4_TCP,
		RTE_PTYPE_L4_UDP
	};

	return ptypes;
}

/**
 * DPDK callback to get information about specific receive queue.
 *
 * @param dev
 *   Pointer to Ethernet device structure.
 * @param rx_queue_id
 *   Receive queue index.
 * @param qinfo
 *   Receive queue information structure.
 */
static void mvgiu_rxq_info_get(struct rte_eth_dev *dev, uint16_t rx_queue_id,
			      struct rte_eth_rxq_info *qinfo)
{
	struct mvgiu_rxq *rxq = dev->data->rx_queues[rx_queue_id];

	qinfo->mp = rxq->mp;
	qinfo->nb_desc = rxq->size;
}

/**
 * DPDK callback to get information about specific transmit queue.
 *
 * @param dev
 *   Pointer to Ethernet device structure.
 * @param tx_queue_id
 *   Transmit queue index.
 * @param qinfo
 *   Transmit queue information structure.
 */
static void mvgiu_txq_info_get(struct rte_eth_dev *dev, uint16_t tx_queue_id,
			      struct rte_eth_txq_info *qinfo)
{
	struct mvgiu_txq *txq = dev->data->tx_queues[tx_queue_id];

	qinfo->nb_desc = txq->size;
	qinfo->conf.tx_deferred_start = txq->tx_deferred_start;
}


/**
 * Check whether requested rx queue offloads match port offloads.
 *
 * @param
 *   dev Pointer to the device.
 * @param
 *   requested Bitmap of the requested offloads.
 *
 * @return
 *   1 if requested offloads are okay, 0 otherwise.
 */
static int
mvgiu_rx_queue_offloads_okay(struct rte_eth_dev *dev, uint64_t requested)
{
	uint64_t mandatory = dev->data->dev_conf.rxmode.offloads;
	uint64_t supported = MVGIU_RX_OFFLOADS;
	uint64_t unsupported = requested & ~supported;
	uint64_t missing = mandatory & ~requested;

	if (unsupported) {
		RTE_LOG(ERR, PMD, "Some Rx offloads are not supported. "
			"Requested 0x%" PRIx64 " supported 0x%" PRIx64 ".\n",
			requested, supported);
		return 0;
	}

	if (missing) {
		RTE_LOG(ERR, PMD, "Some Rx offloads are missing. "
			"Requested 0x%" PRIx64 " missing 0x%" PRIx64 ".\n",
			requested, missing);
		return 0;
	}

	return 1;
}

/**
 * DPDK callback to configure the receive queue.
 *
 * @param dev
 *   Pointer to Ethernet device structure.
 * @param idx
 *   RX queue index.
 * @param desc
 *   Number of descriptors to configure in queue.
 * @param socket
 *   NUMA socket on which memory must be allocated.
 * @param conf
 *   Thresholds parameters.
 * @param mp
 *   Memory pool for buffer allocations.
 *
 * @return
 *   0 on success, negative error value otherwise.
 */
static int
mvgiu_rx_queue_setup(struct rte_eth_dev *dev, uint16_t idx, uint16_t desc,
		    unsigned int socket,
		    const struct rte_eth_rxconf *conf,
		    struct rte_mempool *mp)
{
	struct mvgiu_priv *priv = dev->data->dev_private;
	struct mvgiu_rxq *rxq;
	uint32_t min_size,
		 max_rx_pkt_len = dev->data->dev_conf.rxmode.max_rx_pkt_len;
	int ret;

	if (!mvgiu_rx_queue_offloads_okay(dev, conf->offloads))
		return -ENOTSUP;

	min_size = rte_pktmbuf_data_room_size(mp) - RTE_PKTMBUF_HEADROOM -
		   MVGIU_PKT_EFFEC_OFFS;
	if (min_size < max_rx_pkt_len) {
		RTE_LOG(ERR, PMD,
			"Mbuf size must be increased to %u bytes to hold up to %u bytes of data.\n",
			max_rx_pkt_len + RTE_PKTMBUF_HEADROOM +
			MVGIU_PKT_EFFEC_OFFS,
			max_rx_pkt_len);
		return -EINVAL;
	}

	if (dev->data->rx_queues[idx]) {
		rte_free(dev->data->rx_queues[idx]);
		dev->data->rx_queues[idx] = NULL;
	}

	rxq = rte_zmalloc_socket("rxq", sizeof(*rxq), 0, socket);
	if (!rxq)
		return -ENOMEM;

	rxq->priv = priv;
	rxq->mp = mp;
	rxq->queue_id = idx;
	rxq->port_id = dev->data->port_id;
	rxq->size = desc;
	mvgiu_port_to_bpool_lookup[rxq->port_id] = priv->bpool;

	desc = RTE_MIN(desc, priv->bpool_capa.max_num_buffs);

	ret = mvgiu_fill_bpool(rxq, desc);
	if (ret) {
		rte_free(rxq);
		return ret;
	}

	priv->bpool_init_size += desc;

	dev->data->rx_queues[idx] = rxq;

	return 0;
}

/**
 * DPDK callback to release the receive queue.
 *
 * @param rxq
 *   Generic receive queue pointer.
 */
static void
mvgiu_rx_queue_release(void *rxq)
{
	struct mvgiu_rxq *q = rxq;

	mvgiu_drain_bpool(q->priv, q->size);
	rte_free(q);
}

/**
 * Check whether requested tx queue offloads match port offloads.
 *
 * @param
 *   dev Pointer to the device.
 * @param
 *   requested Bitmap of the requested offloads.
 *
 * @return
 *   1 if requested offloads are okay, 0 otherwise.
 */
static int
mvgiu_tx_queue_offloads_okay(struct rte_eth_dev *dev, uint64_t requested)
{
	uint64_t mandatory = dev->data->dev_conf.txmode.offloads;
	uint64_t supported = MVGIU_TX_OFFLOADS;
	uint64_t unsupported = requested & ~supported;
	uint64_t missing = mandatory & ~requested;

	if (unsupported) {
		RTE_LOG(ERR, PMD, "Some Rx offloads are not supported. "
			"Requested 0x%" PRIx64 " supported 0x%" PRIx64 ".\n",
			requested, supported);
		return 0;
	}

	if (missing) {
		RTE_LOG(ERR, PMD, "Some Rx offloads are missing. "
			"Requested 0x%" PRIx64 " missing 0x%" PRIx64 ".\n",
			requested, missing);
		return 0;
	}

	return 1;
}

/**
 * DPDK callback to configure the transmit queue.
 *
 * @param dev
 *   Pointer to Ethernet device structure.
 * @param idx
 *   Transmit queue index.
 * @param desc
 *   Number of descriptors to configure in the queue.
 * @param socket
 *   NUMA socket on which memory must be allocated.
 * @param conf
 *   Tx queue configuration parameters.
 *
 * @return
 *   0 on success, negative error value otherwise.
 */
static int
mvgiu_tx_queue_setup(struct rte_eth_dev *dev, uint16_t idx, uint16_t desc,
		    unsigned int socket,
		    const struct rte_eth_txconf *conf)
{
	struct mvgiu_priv *priv = dev->data->dev_private;
	struct mvgiu_txq *txq;

	if (!mvgiu_tx_queue_offloads_okay(dev, conf->offloads))
		return -ENOTSUP;

	if (dev->data->tx_queues[idx]) {
		rte_free(dev->data->tx_queues[idx]);
		dev->data->tx_queues[idx] = NULL;
	}

	txq = rte_zmalloc_socket("txq", sizeof(*txq), 0, socket);
	if (!txq)
		return -ENOMEM;

	txq->priv = priv;
	txq->queue_id = idx;
	txq->port_id = dev->data->port_id;
	txq->tx_deferred_start = conf->tx_deferred_start;
	txq->size = desc;

	dev->data->tx_queues[idx] = txq;

	return 0;
}

/**
 * DPDK callback to release the transmit queue.
 *
 * @param txq
 *   Generic transmit queue pointer.
 */
static void
mvgiu_tx_queue_release(void *txq)
{
	struct mvgiu_txq *q = txq;

	if (!q)
		return;

	rte_free(q);
}

/**
 * DPDK callback to get device statistics.
 *
 * @param dev
 *   Pointer to Ethernet device structure.
 * @param stats
 *   Stats structure output buffer.
 *
 * @return
 *   0 on success, negative error value otherwise.
 */
static int
mvgiu_stats_get(struct rte_eth_dev *dev, struct rte_eth_stats *stats)
{
	struct mvgiu_priv *priv = dev->data->dev_private;
	unsigned int i, idx;

	if (!priv->gpio)
		return -EPERM;

	for (i = 0; i < dev->data->nb_rx_queues; i++) {
		struct mvgiu_rxq *rxq = dev->data->rx_queues[i];

		if (!rxq)
			continue;

		idx = rxq->queue_id;
		if (unlikely(idx >= RTE_ETHDEV_QUEUE_STAT_CNTRS)) {
			RTE_LOG(ERR, PMD,
				"rx queue %d stats out of range (0 - %d)\n",
				idx, RTE_ETHDEV_QUEUE_STAT_CNTRS - 1);
			continue;
		}

		stats->q_ibytes[idx] = rxq->bytes_recv;
		stats->q_ipackets[idx] = rxq->packets_recv;
		stats->q_errors[idx] = 0;
		stats->ibytes += stats->q_ibytes[idx];
		stats->ipackets += stats->q_ipackets[idx];
	}

	for (i = 0; i < dev->data->nb_tx_queues; i++) {
		struct mvgiu_txq *txq = dev->data->tx_queues[i];

		if (!txq)
			continue;

		idx = txq->queue_id;
		if (unlikely(idx >= RTE_ETHDEV_QUEUE_STAT_CNTRS)) {
			RTE_LOG(ERR, PMD,
				"tx queue %d stats out of range (0 - %d)\n",
				idx, RTE_ETHDEV_QUEUE_STAT_CNTRS - 1);
		}

		stats->q_obytes[idx] = txq->bytes_sent;
		stats->q_opackets[idx] = txq->packets_sent;
		stats->obytes += stats->q_obytes[idx];
		stats->opackets += stats->q_opackets[idx];
	}

	stats->imissed += 0;
	stats->ierrors += 0;
	stats->rx_nombuf += 0;

	return 0;
}


static const struct eth_dev_ops mvgiu_ops = {
	.dev_configure = mvgiu_dev_configure,
	.dev_start = mvgiu_dev_start,
	.dev_stop = mvgiu_dev_stop,
	.dev_set_link_up = mvgiu_dev_set_link_up,
	.dev_set_link_down = mvgiu_dev_set_link_down,
	.dev_close = mvgiu_dev_close,
	.link_update = mvgiu_link_update,
	.promiscuous_enable = NULL,
	.allmulticast_enable = NULL,
	.promiscuous_disable = NULL,
	.allmulticast_disable = NULL,
	.mac_addr_remove = NULL,
	.mac_addr_add = NULL,
	.mac_addr_set = NULL,
	.mtu_set = NULL,
	.stats_get = mvgiu_stats_get,
	.stats_reset = NULL,
	.xstats_get = NULL,
	.xstats_reset = NULL,
	.xstats_get_names = NULL,
	.dev_infos_get = mvgiu_dev_infos_get,
	.dev_supported_ptypes_get = mvgiu_dev_supported_ptypes_get,
	.rxq_info_get = mvgiu_rxq_info_get,
	.txq_info_get = mvgiu_txq_info_get,
	.vlan_filter_set = NULL,
	.tx_queue_start = NULL,
	.tx_queue_stop = NULL,
	.rx_queue_setup = mvgiu_rx_queue_setup,
	.rx_queue_release = mvgiu_rx_queue_release,
	.tx_queue_setup = mvgiu_tx_queue_setup,
	.tx_queue_release = mvgiu_tx_queue_release,
	.flow_ctrl_get = NULL,
	.flow_ctrl_set = NULL,
	.rss_hash_update = NULL,
	.rss_hash_conf_get = NULL,
	.filter_ctrl = NULL,
	.xstats_get_by_id = NULL,
	.xstats_get_names_by_id = NULL
};

static inline void parse(struct rte_mbuf *mbuf)
{
#ifdef MVGIU_SW_PARSE
	mbuf->packet_type = rte_net_get_ptype(mbuf, NULL, RTE_PTYPE_ALL_MASK);
#endif
}

/**
 * DPDK callback for receive.
 *
 * @param rxq
 *   Generic pointer to the receive queue.
 * @param rx_pkts
 *   Array to store received packets.
 * @param nb_pkts
 *   Maximum number of packets in array.
 *
 * @return
 *   Number of packets successfully received.
 */
static uint16_t
mvgiu_rx_pkt_burst(void *rxq, struct rte_mbuf **rx_pkts, uint16_t nb_pkts)
{
	struct mvgiu_rxq *q = rxq;
	struct giu_gpio_desc descs[nb_pkts];
	struct giu_bpool *bpool;
	int i, ret, rx_done = 0;
	unsigned int core_id = rte_lcore_id();

	bpool = q->priv->bpool;

	ret = giu_gpio_recv(q->priv->gpio, q->priv->rxq_map[q->queue_id].tc,
			    q->priv->rxq_map[q->queue_id].inq, descs, &nb_pkts);
	if (unlikely(ret < 0)) {
		RTE_LOG(ERR, PMD, "Failed to receive packets\n");
		return 0;
	}
	mvgiu_port_bpool_size[bpool->id][core_id] -= nb_pkts;

	for (i = 0; i < nb_pkts; i++) {
		struct rte_mbuf *mbuf;
		uint64_t addr;

		if (likely(nb_pkts - i > MRVL_MUSDK_PREFETCH_SHIFT)) {
			struct giu_gpio_desc *pref_desc;
			u64 pref_addr;

			pref_desc = &descs[i + MRVL_MUSDK_PREFETCH_SHIFT];
			pref_addr = cookie_addr_high |
				    giu_gpio_inq_desc_get_cookie(pref_desc);
			rte_mbuf_prefetch_part1((struct rte_mbuf *)(pref_addr));
			rte_mbuf_prefetch_part2((struct rte_mbuf *)(pref_addr));
		}

		addr = giu_gpio_inq_desc_get_cookie(&descs[i]);
		mbuf = (struct rte_mbuf *)(cookie_addr_high | addr);
		rte_pktmbuf_reset(mbuf);

		mbuf->data_off += q->data_offset;
		mbuf->pkt_len = giu_gpio_inq_desc_get_pkt_len(&descs[i]);
		mbuf->data_len = mbuf->pkt_len;
		mbuf->port = q->port_id;
		parse(mbuf);

		rx_pkts[rx_done++] = mbuf;
		q->bytes_recv += mbuf->pkt_len;
	}

	if (rte_spinlock_trylock(&q->priv->lock) == 1) {
		uint32_t num = mvgiu_get_bpool_size(bpool->id);

		if (unlikely(num <= q->priv->bpool_min_size ||
			     (!rx_done && num < q->priv->bpool_init_size))) {
			ret = mvgiu_fill_bpool(q, MRVL_BURST_SIZE);
			if (ret)
				RTE_LOG(ERR, PMD,
					"Failed to fill bpool, num %d\n", num);
		}
		rte_spinlock_unlock(&q->priv->lock);
	}

	q->packets_recv += rx_done;

	return rx_done;
}

/**
 * Prepare offload information.
 *
 * @param ol_flags
 *   Offload flags.
 * @param packet_type
 *   Packet type bitfield.
 * @param l3_type
 *   Pointer to the pp2_ouq_l3_type structure.
 * @param l4_type
 *   Pointer to the pp2_outq_l4_type structure.
 * @param gen_l3_cksum
 *   Will be set to 1 in case l3 checksum is computed.
 * @param l4_cksum
 *   Will be set to 1 in case l4 checksum is computed.
 *
 * @return
 *   0 on success, negative error value otherwise.
 */
static inline int
mvgiu_prepare_proto_info(uint64_t ol_flags, uint32_t packet_type,
			enum giu_outq_l3_type *l3_type,
			enum giu_outq_l4_type *l4_type)
{
	/*
	 * Based on ol_flags prepare information
	 * for giu_gpio_outq_desc_set_proto_info() which setups descriptor
	 * for offloading.
	 */
	if (ol_flags & PKT_TX_IPV4)
		*l3_type = GIU_OUTQ_L3_TYPE_IPV4_NO_OPTS;
	else if (ol_flags & PKT_TX_IPV6)
		*l3_type = GIU_OUTQ_L3_TYPE_IPV6_NO_EXT;
	else
		/* if something different then stop processing */
		return -1;

	ol_flags &= PKT_TX_L4_MASK;
	if (packet_type & RTE_PTYPE_L4_TCP)
		*l4_type = GIU_OUTQ_L4_TYPE_TCP;
	else if (packet_type & RTE_PTYPE_L4_UDP)
		*l4_type = GIU_OUTQ_L4_TYPE_UDP;
	else
		*l4_type = GIU_OUTQ_L4_TYPE_OTHER;

	return 0;
}

/**
 * DPDK callback for transmit.
 *
 * @param txq
 *   Generic pointer transmit queue.
 * @param tx_pkts
 *   Packets to transmit.
 * @param nb_pkts
 *   Number of packets in array.
 *
 * @return
 *   Number of packets successfully transmitted.
 */
static uint16_t
mvgiu_tx_pkt_burst(void *txq, struct rte_mbuf **tx_pkts, uint16_t nb_pkts)
{
	struct mvgiu_txq *q = txq;
	struct mvgiu_shadow_txq *sq;
	struct giu_gpio_desc descs[nb_pkts];
	unsigned int core_id = rte_lcore_id();
	int i, ret, bytes_sent = 0;
	uint16_t num, sq_free_size;
	uint64_t addr;
	uint8_t tc = 0;

	sq = &q->shadow_txqs[core_id];

	if (unlikely(!q->priv->gpio))
		return 0;

	if (sq->size)
		mvgiu_check_n_free_sent_buffers(q->priv->gpio,
						sq,
						q->queue_id,
						0);

	sq_free_size = MVGIU_TX_SHADOWQ_SIZE - sq->size - 1;
	if (unlikely(nb_pkts > sq_free_size)) {
		RTE_LOG(DEBUG, PMD,
			"No room in shadow queue for %d packets! %d packets will be sent.\n",
			nb_pkts, sq_free_size);
		nb_pkts = sq_free_size;
	}

	for (i = 0; i < nb_pkts; i++) {
		struct rte_mbuf *mbuf = tx_pkts[i];
		enum giu_outq_l3_type l3_type;
		enum giu_outq_l4_type l4_type;

		if (likely(nb_pkts - i > MRVL_MUSDK_PREFETCH_SHIFT)) {
			struct rte_mbuf *pref_pkt_hdr;

			pref_pkt_hdr = tx_pkts[i + MRVL_MUSDK_PREFETCH_SHIFT];
			rte_mbuf_prefetch_part1(pref_pkt_hdr);
			rte_mbuf_prefetch_part2(pref_pkt_hdr);
		}

		sq->ent[sq->head].cookie = (uint64_t)mbuf;
		sq->ent[sq->head].addr =
			rte_mbuf_data_iova_default(mbuf);
		sq->bpool[sq->head] =
			(unlikely(mbuf->port >= RTE_MAX_ETHPORTS ||
			 mbuf->refcnt > 1)) ? NULL :
			 mvgiu_port_to_bpool_lookup[mbuf->port];
		sq->head = (sq->head + 1) & MVGIU_TX_SHADOWQ_MASK;
		sq->size++;

		giu_gpio_outq_desc_reset(&descs[i]);
		giu_gpio_outq_desc_set_phys_addr(&descs[i],
						 rte_pktmbuf_iova(mbuf));
		giu_gpio_outq_desc_set_pkt_offset(&descs[i], 0);
		giu_gpio_outq_desc_set_pkt_len(&descs[i],
					       rte_pktmbuf_pkt_len(mbuf));

		bytes_sent += rte_pktmbuf_pkt_len(mbuf);
		/*
		 * in case unsupported ol_flags were passed
		 * do not update descriptor offload information
		 */
		ret = mvgiu_prepare_proto_info(mbuf->ol_flags,
					       mbuf->packet_type,
					       &l3_type, &l4_type);
		if (unlikely(ret))
			continue;

		giu_gpio_outq_desc_set_proto_info(&descs[i], l3_type, l4_type,
						  mbuf->l2_len,
						  mbuf->l2_len + mbuf->l3_len);
	}

	num = nb_pkts;
	giu_gpio_send(q->priv->gpio, tc, q->queue_id, descs, &nb_pkts);
	/* number of packets that were not sent */
	if (unlikely(num > nb_pkts)) {
		for (i = nb_pkts; i < num; i++) {
			sq->head = (MVGIU_TX_SHADOWQ_SIZE + sq->head - 1) &
				MVGIU_TX_SHADOWQ_MASK;
			addr = sq->ent[sq->head].cookie;
			bytes_sent -=
				rte_pktmbuf_pkt_len((struct rte_mbuf *)addr);
		}
		sq->size -= num - nb_pkts;
	}

	q->bytes_sent += bytes_sent;
	q->packets_sent += nb_pkts;

	return nb_pkts;
}

/**
 * Create private device structure.
 *
 * @param dev_name
 *   Pointer to the port name passed in the initialization parameters.
 *
 * @return
 *   Pointer to the newly allocated private device structure.
 */
static struct mvgiu_priv *
mvgiu_priv_create(const char *dev_name)
{
	struct mvgiu_priv *priv;
	char	file_name[REGFILE_MAX_FILE_NAME];
	char	name[20];
	int	giu_id = 0; /* TODO: get this value from higher levels */
	int	bpool_id = 0; /* TODO: get this value from higher levels */
	int	gpio_id = 0; /* TODO: get this value from higher levels */
	int	ret;

	priv = rte_zmalloc_socket(dev_name, sizeof(*priv), 0, rte_socket_id());
	if (!priv)
		return NULL;

	/* Map GIU regfile */
	snprintf(file_name,
		 sizeof(file_name),
		 "%s%s%d", REGFILE_VAR_DIR, REGFILE_NAME_PREFIX, 0);

	/* Probe the GIU BPOOL */
	snprintf(name, sizeof(name), "giu_pool-%d:%d", giu_id, bpool_id);
	ret = giu_bpool_probe(name, file_name, &priv->bpool);
	if (ret) {
		RTE_LOG(ERR, PMD, "giu_bpool_probe failed!\n");
		goto out_free_priv;
	}

	ret = giu_bpool_get_capabilities(priv->bpool, &priv->bpool_capa);
	if (ret) {
		RTE_LOG(ERR, PMD, "giu_bpool_get_capabilities failed!\n");
		goto out_free_priv;
	}

	/* Probe the GIU GPIO */
	snprintf(name, sizeof(name), "gpio-%d:%d", giu_id, gpio_id);
	ret = giu_gpio_probe(name, file_name, &priv->gpio);
	if (ret) {
		RTE_LOG(ERR, PMD, "giu_gpio_probe failed!\n");
		goto out_free_priv;
	}

	ret = giu_gpio_get_capabilities(priv->gpio, &priv->gpio_capa);
	if (ret) {
		RTE_LOG(ERR, PMD, "giu_gpio_get_capabilities failed!\n");
		goto out_free_priv;
	}

	rte_spinlock_init(&priv->lock);

	return priv;
out_free_priv:
	rte_free(priv);
	return NULL;
}

/**
 * Create device representing Ethernet port.
 *
 * @param name
 *   Pointer to the port's name.
 *
 * @return
 *   0 on success, negative error value otherwise.
 */
static int
mvgiu_eth_dev_create(struct rte_vdev_device *vdev, const char *name)
{
	struct rte_eth_dev *eth_dev;
	struct mvgiu_priv *priv;
	int ret;

	eth_dev = rte_eth_dev_allocate(name);
	if (!eth_dev)
		return -ENOMEM;

	priv = mvgiu_priv_create(name);
	if (!priv) {
		ret = -ENOMEM;
		goto out_free_dev;
	}

	eth_dev->data->mac_addrs =
		rte_zmalloc("mac_addrs",
			    ETHER_ADDR_LEN * MVGIU_MAC_ADDRS_MAX, 0);
	if (!eth_dev->data->mac_addrs) {
		RTE_LOG(ERR, PMD, "Failed to allocate space for eth addrs\n");
		ret = -ENOMEM;
		goto out_free_priv;
	}

	eth_dev->rx_pkt_burst = mvgiu_rx_pkt_burst;
	eth_dev->tx_pkt_burst = mvgiu_tx_pkt_burst;
	eth_dev->data->kdrv = RTE_KDRV_NONE;
	eth_dev->data->dev_private = priv;
	eth_dev->device = &vdev->device;
	eth_dev->dev_ops = &mvgiu_ops;

	return 0;

out_free_priv:
	rte_free(priv);
out_free_dev:
	rte_eth_dev_release_port(eth_dev);

	return ret;
}

/**
 * Cleanup previously created device representing Ethernet port.
 *
 * @param name
 *   Pointer to the port name.
 */
static void
mvgiu_eth_dev_destroy(const char *name)
{
	struct rte_eth_dev *eth_dev;
	struct mvgiu_priv *priv;

	eth_dev = rte_eth_dev_allocated(name);
	if (!eth_dev)
		return;

	priv = eth_dev->data->dev_private;
	if (priv->gpio)
		giu_gpio_remove(priv->gpio);
	if (priv->bpool)
		giu_bpool_remove(priv->bpool);

	rte_free(priv);
	rte_eth_dev_release_port(eth_dev);
}

/**
 * Callback used by rte_kvargs_process() during argument parsing.
 *
 * @param key
 *   Pointer to the parsed key (unused).
 * @param value
 *   Pointer to the parsed value.
 * @param extra_args
 *   Pointer to the extra arguments which contains address of the
 *   table of pointers to parsed interface names.
 *
 * @return
 *   Always 0.
 */
static int
mvgiu_get_ifnames(const char *key __rte_unused, const char *value,
		 void *extra_args)
{
	struct mvgiu_ifnames *ifnames = extra_args;

	ifnames->names[ifnames->idx++] = value;

	return 0;
}

/**
 * DPDK callback to register the virtual device.
 *
 * @param vdev
 *   Pointer to the virtual device.
 *
 * @return
 *   0 on success, negative error value otherwise.
 */
static int
rte_pmd_mvgiu_probe(struct rte_vdev_device *vdev)
{
	struct rte_kvargs *kvlist;
	struct mvgiu_ifnames ifnames;
	int ret = -EINVAL;
	uint32_t i, ifnum;
	const char *params;

	params = rte_vdev_device_args(vdev);
	if (!params)
		return -EINVAL;

	kvlist = rte_kvargs_parse(params, valid_args);
	if (!kvlist)
		return -EINVAL;

	ifnum = rte_kvargs_count(kvlist, MRVL_IFACE_NAME_ARG);
	if (ifnum > RTE_DIM(ifnames.names))
		goto out_free_kvlist;

	ifnames.idx = 0;
	rte_kvargs_process(kvlist, MRVL_IFACE_NAME_ARG,
			   mvgiu_get_ifnames, &ifnames);

	ret = rte_mvep_init(MVEP_MOD_T_GIU, kvlist);
	if (ret)
		goto out_free_kvlist;

	memset(mvgiu_port_bpool_size, 0, sizeof(mvgiu_port_bpool_size));
	memset(mvgiu_port_to_bpool_lookup,
	       0,
	       sizeof(mvgiu_port_to_bpool_lookup));

	for (i = 0; i < ifnum; i++) {
		RTE_LOG(INFO, PMD, "Creating %s\n", ifnames.names[i]);
		ret = mvgiu_eth_dev_create(vdev, ifnames.names[i]);
		if (ret)
			goto out_cleanup;
	}

	rte_kvargs_free(kvlist);

	return 0;
out_cleanup:
	for (; i > 0; i--)
		mvgiu_eth_dev_destroy(ifnames.names[i]);

	rte_mvep_deinit(MVEP_MOD_T_GIU);
out_free_kvlist:
	rte_kvargs_free(kvlist);

	return ret;
}

/**
 * DPDK callback to remove virtual device.
 *
 * @param vdev
 *   Pointer to the removed virtual device.
 *
 * @return
 *   0 on success, negative error value otherwise.
 */
static int
rte_pmd_mvgiu_remove(struct rte_vdev_device *vdev)
{
	int i;
	const char *name;

	name = rte_vdev_device_name(vdev);
	if (!name)
		return -EINVAL;

	RTE_LOG(INFO, PMD, "Removing %s\n", name);

	for (i = 0; i < rte_eth_dev_count(); i++) {
		char ifname[RTE_ETH_NAME_MAX_LEN];

		rte_eth_dev_get_name_by_port(i, ifname);
		mvgiu_eth_dev_destroy(ifname);
	}

	rte_mvep_deinit(MVEP_MOD_T_GIU);

	return 0;
}

static struct rte_vdev_driver pmd_mvgiu_drv = {
	.probe = rte_pmd_mvgiu_probe,
	.remove = rte_pmd_mvgiu_remove,
};

RTE_PMD_REGISTER_VDEV(net_mvgiu, pmd_mvgiu_drv);
RTE_PMD_REGISTER_ALIAS(net_mvgiu, eth_mvgiu);
