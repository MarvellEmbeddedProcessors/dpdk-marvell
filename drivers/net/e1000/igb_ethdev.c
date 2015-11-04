/*-
 *   BSD LICENSE
 *
 *   Copyright(c) 2010-2015 Intel Corporation. All rights reserved.
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
 *     * Neither the name of Intel Corporation nor the names of its
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

#include <sys/queue.h>
#include <stdio.h>
#include <errno.h>
#include <stdint.h>
#include <stdarg.h>

#include <rte_common.h>
#include <rte_interrupts.h>
#include <rte_byteorder.h>
#include <rte_log.h>
#include <rte_debug.h>
#include <rte_pci.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_memory.h>
#include <rte_memzone.h>
#include <rte_eal.h>
#include <rte_atomic.h>
#include <rte_malloc.h>
#include <rte_dev.h>

#include "e1000_logs.h"
#include "base/e1000_api.h"
#include "e1000_ethdev.h"
#include "igb_regs.h"

/*
 * Default values for port configuration
 */
#define IGB_DEFAULT_RX_FREE_THRESH  32
#define IGB_DEFAULT_RX_PTHRESH      8
#define IGB_DEFAULT_RX_HTHRESH      8
#define IGB_DEFAULT_RX_WTHRESH      0

#define IGB_DEFAULT_TX_PTHRESH      32
#define IGB_DEFAULT_TX_HTHRESH      0
#define IGB_DEFAULT_TX_WTHRESH      0

#define IGB_HKEY_MAX_INDEX 10

/* Bit shift and mask */
#define IGB_4_BIT_WIDTH  (CHAR_BIT / 2)
#define IGB_4_BIT_MASK   RTE_LEN2MASK(IGB_4_BIT_WIDTH, uint8_t)
#define IGB_8_BIT_WIDTH  CHAR_BIT
#define IGB_8_BIT_MASK   UINT8_MAX

/* Additional timesync values. */
#define E1000_ETQF_FILTER_1588 3
#define E1000_TIMINCA_INCVALUE 16000000
#define E1000_TIMINCA_INIT     ((0x02 << E1000_TIMINCA_16NS_SHIFT) \
				| E1000_TIMINCA_INCVALUE)
#define E1000_TSAUXC_DISABLE_SYSTIME 0x80000000

static int  eth_igb_configure(struct rte_eth_dev *dev);
static int  eth_igb_start(struct rte_eth_dev *dev);
static void eth_igb_stop(struct rte_eth_dev *dev);
static void eth_igb_close(struct rte_eth_dev *dev);
static void eth_igb_promiscuous_enable(struct rte_eth_dev *dev);
static void eth_igb_promiscuous_disable(struct rte_eth_dev *dev);
static void eth_igb_allmulticast_enable(struct rte_eth_dev *dev);
static void eth_igb_allmulticast_disable(struct rte_eth_dev *dev);
static int  eth_igb_link_update(struct rte_eth_dev *dev,
				int wait_to_complete);
static void eth_igb_stats_get(struct rte_eth_dev *dev,
				struct rte_eth_stats *rte_stats);
static int eth_igb_xstats_get(struct rte_eth_dev *dev,
			      struct rte_eth_xstats *xstats, unsigned n);
static void eth_igb_stats_reset(struct rte_eth_dev *dev);
static void eth_igb_xstats_reset(struct rte_eth_dev *dev);
static void eth_igb_infos_get(struct rte_eth_dev *dev,
			      struct rte_eth_dev_info *dev_info);
static void eth_igbvf_infos_get(struct rte_eth_dev *dev,
				struct rte_eth_dev_info *dev_info);
static int  eth_igb_flow_ctrl_get(struct rte_eth_dev *dev,
				struct rte_eth_fc_conf *fc_conf);
static int  eth_igb_flow_ctrl_set(struct rte_eth_dev *dev,
				struct rte_eth_fc_conf *fc_conf);
static int eth_igb_lsc_interrupt_setup(struct rte_eth_dev *dev);
static int eth_igb_rxq_interrupt_setup(struct rte_eth_dev *dev);
static int eth_igb_interrupt_get_status(struct rte_eth_dev *dev);
static int eth_igb_interrupt_action(struct rte_eth_dev *dev);
static void eth_igb_interrupt_handler(struct rte_intr_handle *handle,
							void *param);
static int  igb_hardware_init(struct e1000_hw *hw);
static void igb_hw_control_acquire(struct e1000_hw *hw);
static void igb_hw_control_release(struct e1000_hw *hw);
static void igb_init_manageability(struct e1000_hw *hw);
static void igb_release_manageability(struct e1000_hw *hw);

static int  eth_igb_mtu_set(struct rte_eth_dev *dev, uint16_t mtu);

static int eth_igb_vlan_filter_set(struct rte_eth_dev *dev,
		uint16_t vlan_id, int on);
static void eth_igb_vlan_tpid_set(struct rte_eth_dev *dev, uint16_t tpid_id);
static void eth_igb_vlan_offload_set(struct rte_eth_dev *dev, int mask);

static void igb_vlan_hw_filter_enable(struct rte_eth_dev *dev);
static void igb_vlan_hw_filter_disable(struct rte_eth_dev *dev);
static void igb_vlan_hw_strip_enable(struct rte_eth_dev *dev);
static void igb_vlan_hw_strip_disable(struct rte_eth_dev *dev);
static void igb_vlan_hw_extend_enable(struct rte_eth_dev *dev);
static void igb_vlan_hw_extend_disable(struct rte_eth_dev *dev);

static int eth_igb_led_on(struct rte_eth_dev *dev);
static int eth_igb_led_off(struct rte_eth_dev *dev);

static void igb_intr_disable(struct e1000_hw *hw);
static int  igb_get_rx_buffer_size(struct e1000_hw *hw);
static void eth_igb_rar_set(struct rte_eth_dev *dev,
		struct ether_addr *mac_addr,
		uint32_t index, uint32_t pool);
static void eth_igb_rar_clear(struct rte_eth_dev *dev, uint32_t index);
static void eth_igb_default_mac_addr_set(struct rte_eth_dev *dev,
		struct ether_addr *addr);

static void igbvf_intr_disable(struct e1000_hw *hw);
static int igbvf_dev_configure(struct rte_eth_dev *dev);
static int igbvf_dev_start(struct rte_eth_dev *dev);
static void igbvf_dev_stop(struct rte_eth_dev *dev);
static void igbvf_dev_close(struct rte_eth_dev *dev);
static int eth_igbvf_link_update(struct e1000_hw *hw);
static void eth_igbvf_stats_get(struct rte_eth_dev *dev,
				struct rte_eth_stats *rte_stats);
static int eth_igbvf_xstats_get(struct rte_eth_dev *dev,
				struct rte_eth_xstats *xstats, unsigned n);
static void eth_igbvf_stats_reset(struct rte_eth_dev *dev);
static int igbvf_vlan_filter_set(struct rte_eth_dev *dev,
		uint16_t vlan_id, int on);
static int igbvf_set_vfta(struct e1000_hw *hw, uint16_t vid, bool on);
static void igbvf_set_vfta_all(struct rte_eth_dev *dev, bool on);
static void igbvf_default_mac_addr_set(struct rte_eth_dev *dev,
		struct ether_addr *addr);
static int igbvf_get_reg_length(struct rte_eth_dev *dev);
static int igbvf_get_regs(struct rte_eth_dev *dev,
		struct rte_dev_reg_info *regs);

static int eth_igb_rss_reta_update(struct rte_eth_dev *dev,
				   struct rte_eth_rss_reta_entry64 *reta_conf,
				   uint16_t reta_size);
static int eth_igb_rss_reta_query(struct rte_eth_dev *dev,
				  struct rte_eth_rss_reta_entry64 *reta_conf,
				  uint16_t reta_size);

static int eth_igb_syn_filter_set(struct rte_eth_dev *dev,
			struct rte_eth_syn_filter *filter,
			bool add);
static int eth_igb_syn_filter_get(struct rte_eth_dev *dev,
			struct rte_eth_syn_filter *filter);
static int eth_igb_syn_filter_handle(struct rte_eth_dev *dev,
			enum rte_filter_op filter_op,
			void *arg);
static int igb_add_2tuple_filter(struct rte_eth_dev *dev,
			struct rte_eth_ntuple_filter *ntuple_filter);
static int igb_remove_2tuple_filter(struct rte_eth_dev *dev,
			struct rte_eth_ntuple_filter *ntuple_filter);
static int eth_igb_add_del_flex_filter(struct rte_eth_dev *dev,
			struct rte_eth_flex_filter *filter,
			bool add);
static int eth_igb_get_flex_filter(struct rte_eth_dev *dev,
			struct rte_eth_flex_filter *filter);
static int eth_igb_flex_filter_handle(struct rte_eth_dev *dev,
			enum rte_filter_op filter_op,
			void *arg);
static int igb_add_5tuple_filter_82576(struct rte_eth_dev *dev,
			struct rte_eth_ntuple_filter *ntuple_filter);
static int igb_remove_5tuple_filter_82576(struct rte_eth_dev *dev,
			struct rte_eth_ntuple_filter *ntuple_filter);
static int igb_add_del_ntuple_filter(struct rte_eth_dev *dev,
			struct rte_eth_ntuple_filter *filter,
			bool add);
static int igb_get_ntuple_filter(struct rte_eth_dev *dev,
			struct rte_eth_ntuple_filter *filter);
static int igb_ntuple_filter_handle(struct rte_eth_dev *dev,
				enum rte_filter_op filter_op,
				void *arg);
static int igb_add_del_ethertype_filter(struct rte_eth_dev *dev,
			struct rte_eth_ethertype_filter *filter,
			bool add);
static int igb_ethertype_filter_handle(struct rte_eth_dev *dev,
				enum rte_filter_op filter_op,
				void *arg);
static int igb_get_ethertype_filter(struct rte_eth_dev *dev,
			struct rte_eth_ethertype_filter *filter);
static int eth_igb_filter_ctrl(struct rte_eth_dev *dev,
		     enum rte_filter_type filter_type,
		     enum rte_filter_op filter_op,
		     void *arg);
static int eth_igb_get_reg_length(struct rte_eth_dev *dev);
static int eth_igb_get_regs(struct rte_eth_dev *dev,
		struct rte_dev_reg_info *regs);
static int eth_igb_get_eeprom_length(struct rte_eth_dev *dev);
static int eth_igb_get_eeprom(struct rte_eth_dev *dev,
		struct rte_dev_eeprom_info *eeprom);
static int eth_igb_set_eeprom(struct rte_eth_dev *dev,
		struct rte_dev_eeprom_info *eeprom);
static int eth_igb_set_mc_addr_list(struct rte_eth_dev *dev,
				    struct ether_addr *mc_addr_set,
				    uint32_t nb_mc_addr);
static int igb_timesync_enable(struct rte_eth_dev *dev);
static int igb_timesync_disable(struct rte_eth_dev *dev);
static int igb_timesync_read_rx_timestamp(struct rte_eth_dev *dev,
					  struct timespec *timestamp,
					  uint32_t flags);
static int igb_timesync_read_tx_timestamp(struct rte_eth_dev *dev,
					  struct timespec *timestamp);
static int eth_igb_rx_queue_intr_enable(struct rte_eth_dev *dev,
					uint16_t queue_id);
static int eth_igb_rx_queue_intr_disable(struct rte_eth_dev *dev,
					 uint16_t queue_id);
static void eth_igb_assign_msix_vector(struct e1000_hw *hw, int8_t direction,
				       uint8_t queue, uint8_t msix_vector);
static void eth_igb_write_ivar(struct e1000_hw *hw, uint8_t msix_vector,
			       uint8_t index, uint8_t offset);
static void eth_igb_configure_msix_intr(struct rte_eth_dev *dev);

/*
 * Define VF Stats MACRO for Non "cleared on read" register
 */
#define UPDATE_VF_STAT(reg, last, cur)            \
{                                                 \
	u32 latest = E1000_READ_REG(hw, reg);     \
	cur += (latest - last) & UINT_MAX;        \
	last = latest;                            \
}

#define IGB_FC_PAUSE_TIME 0x0680
#define IGB_LINK_UPDATE_CHECK_TIMEOUT  90  /* 9s */
#define IGB_LINK_UPDATE_CHECK_INTERVAL 100 /* ms */

#define IGBVF_PMD_NAME "rte_igbvf_pmd"     /* PMD name */

static enum e1000_fc_mode igb_fc_setting = e1000_fc_full;

/*
 * The set of PCI devices this driver supports
 */
static const struct rte_pci_id pci_id_igb_map[] = {

#define RTE_PCI_DEV_ID_DECL_IGB(vend, dev) {RTE_PCI_DEVICE(vend, dev)},
#include "rte_pci_dev_ids.h"

{0},
};

/*
 * The set of PCI devices this driver supports (for 82576&I350 VF)
 */
static const struct rte_pci_id pci_id_igbvf_map[] = {

#define RTE_PCI_DEV_ID_DECL_IGBVF(vend, dev) {RTE_PCI_DEVICE(vend, dev)},
#include "rte_pci_dev_ids.h"

{0},
};

static const struct rte_eth_desc_lim rx_desc_lim = {
	.nb_max = E1000_MAX_RING_DESC,
	.nb_min = E1000_MIN_RING_DESC,
	.nb_align = IGB_RXD_ALIGN,
};

static const struct rte_eth_desc_lim tx_desc_lim = {
	.nb_max = E1000_MAX_RING_DESC,
	.nb_min = E1000_MIN_RING_DESC,
	.nb_align = IGB_RXD_ALIGN,
};

static const struct eth_dev_ops eth_igb_ops = {
	.dev_configure        = eth_igb_configure,
	.dev_start            = eth_igb_start,
	.dev_stop             = eth_igb_stop,
	.dev_close            = eth_igb_close,
	.promiscuous_enable   = eth_igb_promiscuous_enable,
	.promiscuous_disable  = eth_igb_promiscuous_disable,
	.allmulticast_enable  = eth_igb_allmulticast_enable,
	.allmulticast_disable = eth_igb_allmulticast_disable,
	.link_update          = eth_igb_link_update,
	.stats_get            = eth_igb_stats_get,
	.xstats_get           = eth_igb_xstats_get,
	.stats_reset          = eth_igb_stats_reset,
	.xstats_reset         = eth_igb_xstats_reset,
	.dev_infos_get        = eth_igb_infos_get,
	.mtu_set              = eth_igb_mtu_set,
	.vlan_filter_set      = eth_igb_vlan_filter_set,
	.vlan_tpid_set        = eth_igb_vlan_tpid_set,
	.vlan_offload_set     = eth_igb_vlan_offload_set,
	.rx_queue_setup       = eth_igb_rx_queue_setup,
	.rx_queue_intr_enable = eth_igb_rx_queue_intr_enable,
	.rx_queue_intr_disable = eth_igb_rx_queue_intr_disable,
	.rx_queue_release     = eth_igb_rx_queue_release,
	.rx_queue_count       = eth_igb_rx_queue_count,
	.rx_descriptor_done   = eth_igb_rx_descriptor_done,
	.tx_queue_setup       = eth_igb_tx_queue_setup,
	.tx_queue_release     = eth_igb_tx_queue_release,
	.dev_led_on           = eth_igb_led_on,
	.dev_led_off          = eth_igb_led_off,
	.flow_ctrl_get        = eth_igb_flow_ctrl_get,
	.flow_ctrl_set        = eth_igb_flow_ctrl_set,
	.mac_addr_add         = eth_igb_rar_set,
	.mac_addr_remove      = eth_igb_rar_clear,
	.mac_addr_set         = eth_igb_default_mac_addr_set,
	.reta_update          = eth_igb_rss_reta_update,
	.reta_query           = eth_igb_rss_reta_query,
	.rss_hash_update      = eth_igb_rss_hash_update,
	.rss_hash_conf_get    = eth_igb_rss_hash_conf_get,
	.filter_ctrl          = eth_igb_filter_ctrl,
	.set_mc_addr_list     = eth_igb_set_mc_addr_list,
	.rxq_info_get         = igb_rxq_info_get,
	.txq_info_get         = igb_txq_info_get,
	.timesync_enable      = igb_timesync_enable,
	.timesync_disable     = igb_timesync_disable,
	.timesync_read_rx_timestamp = igb_timesync_read_rx_timestamp,
	.timesync_read_tx_timestamp = igb_timesync_read_tx_timestamp,
	.get_reg_length       = eth_igb_get_reg_length,
	.get_reg              = eth_igb_get_regs,
	.get_eeprom_length    = eth_igb_get_eeprom_length,
	.get_eeprom           = eth_igb_get_eeprom,
	.set_eeprom           = eth_igb_set_eeprom,
};

/*
 * dev_ops for virtual function, bare necessities for basic vf
 * operation have been implemented
 */
static const struct eth_dev_ops igbvf_eth_dev_ops = {
	.dev_configure        = igbvf_dev_configure,
	.dev_start            = igbvf_dev_start,
	.dev_stop             = igbvf_dev_stop,
	.dev_close            = igbvf_dev_close,
	.link_update          = eth_igb_link_update,
	.stats_get            = eth_igbvf_stats_get,
	.xstats_get           = eth_igbvf_xstats_get,
	.stats_reset          = eth_igbvf_stats_reset,
	.xstats_reset         = eth_igbvf_stats_reset,
	.vlan_filter_set      = igbvf_vlan_filter_set,
	.dev_infos_get        = eth_igbvf_infos_get,
	.rx_queue_setup       = eth_igb_rx_queue_setup,
	.rx_queue_release     = eth_igb_rx_queue_release,
	.tx_queue_setup       = eth_igb_tx_queue_setup,
	.tx_queue_release     = eth_igb_tx_queue_release,
	.set_mc_addr_list     = eth_igb_set_mc_addr_list,
	.rxq_info_get         = igb_rxq_info_get,
	.txq_info_get         = igb_txq_info_get,
	.mac_addr_set         = igbvf_default_mac_addr_set,
	.get_reg_length       = igbvf_get_reg_length,
	.get_reg              = igbvf_get_regs,
};

/* store statistics names and its offset in stats structure */
struct rte_igb_xstats_name_off {
	char name[RTE_ETH_XSTATS_NAME_SIZE];
	unsigned offset;
};

static const struct rte_igb_xstats_name_off rte_igb_stats_strings[] = {
	{"rx_crc_errors", offsetof(struct e1000_hw_stats, crcerrs)},
	{"rx_align_errors", offsetof(struct e1000_hw_stats, algnerrc)},
	{"rx_symbol_errors", offsetof(struct e1000_hw_stats, symerrs)},
	{"rx_missed_packets", offsetof(struct e1000_hw_stats, mpc)},
	{"tx_single_collision_packets", offsetof(struct e1000_hw_stats, scc)},
	{"tx_multiple_collision_packets", offsetof(struct e1000_hw_stats, mcc)},
	{"tx_excessive_collision_packets", offsetof(struct e1000_hw_stats,
		ecol)},
	{"tx_late_collisions", offsetof(struct e1000_hw_stats, latecol)},
	{"tx_total_collisions", offsetof(struct e1000_hw_stats, colc)},
	{"tx_deferred_packets", offsetof(struct e1000_hw_stats, dc)},
	{"tx_no_carrier_sense_packets", offsetof(struct e1000_hw_stats, tncrs)},
	{"rx_carrier_ext_errors", offsetof(struct e1000_hw_stats, cexterr)},
	{"rx_length_errors", offsetof(struct e1000_hw_stats, rlec)},
	{"rx_xon_packets", offsetof(struct e1000_hw_stats, xonrxc)},
	{"tx_xon_packets", offsetof(struct e1000_hw_stats, xontxc)},
	{"rx_xoff_packets", offsetof(struct e1000_hw_stats, xoffrxc)},
	{"tx_xoff_packets", offsetof(struct e1000_hw_stats, xofftxc)},
	{"rx_flow_control_unsupported_packets", offsetof(struct e1000_hw_stats,
		fcruc)},
	{"rx_size_64_packets", offsetof(struct e1000_hw_stats, prc64)},
	{"rx_size_65_to_127_packets", offsetof(struct e1000_hw_stats, prc127)},
	{"rx_size_128_to_255_packets", offsetof(struct e1000_hw_stats, prc255)},
	{"rx_size_256_to_511_packets", offsetof(struct e1000_hw_stats, prc511)},
	{"rx_size_512_to_1023_packets", offsetof(struct e1000_hw_stats,
		prc1023)},
	{"rx_size_1024_to_max_packets", offsetof(struct e1000_hw_stats,
		prc1522)},
	{"rx_broadcast_packets", offsetof(struct e1000_hw_stats, bprc)},
	{"rx_multicast_packets", offsetof(struct e1000_hw_stats, mprc)},
	{"rx_undersize_errors", offsetof(struct e1000_hw_stats, ruc)},
	{"rx_fragment_errors", offsetof(struct e1000_hw_stats, rfc)},
	{"rx_oversize_errors", offsetof(struct e1000_hw_stats, roc)},
	{"rx_jabber_errors", offsetof(struct e1000_hw_stats, rjc)},
	{"rx_management_packets", offsetof(struct e1000_hw_stats, mgprc)},
	{"rx_management_dropped", offsetof(struct e1000_hw_stats, mgpdc)},
	{"tx_management_packets", offsetof(struct e1000_hw_stats, mgptc)},
	{"rx_total_packets", offsetof(struct e1000_hw_stats, tpr)},
	{"tx_total_packets", offsetof(struct e1000_hw_stats, tpt)},
	{"rx_total_bytes", offsetof(struct e1000_hw_stats, tor)},
	{"tx_total_bytes", offsetof(struct e1000_hw_stats, tot)},
	{"tx_size_64_packets", offsetof(struct e1000_hw_stats, ptc64)},
	{"tx_size_65_to_127_packets", offsetof(struct e1000_hw_stats, ptc127)},
	{"tx_size_128_to_255_packets", offsetof(struct e1000_hw_stats, ptc255)},
	{"tx_size_256_to_511_packets", offsetof(struct e1000_hw_stats, ptc511)},
	{"tx_size_512_to_1023_packets", offsetof(struct e1000_hw_stats,
		ptc1023)},
	{"tx_size_1023_to_max_packets", offsetof(struct e1000_hw_stats,
		ptc1522)},
	{"tx_multicast_packets", offsetof(struct e1000_hw_stats, mptc)},
	{"tx_broadcast_packets", offsetof(struct e1000_hw_stats, bptc)},
	{"tx_tso_packets", offsetof(struct e1000_hw_stats, tsctc)},
	{"tx_tso_errors", offsetof(struct e1000_hw_stats, tsctfc)},
	{"rx_sent_to_host_packets", offsetof(struct e1000_hw_stats, rpthc)},
	{"tx_sent_by_host_packets", offsetof(struct e1000_hw_stats, hgptc)},
	{"rx_code_violation_packets", offsetof(struct e1000_hw_stats, scvpc)},

	{"interrupt_assert_count", offsetof(struct e1000_hw_stats, iac)},
};

#define IGB_NB_XSTATS (sizeof(rte_igb_stats_strings) / \
		sizeof(rte_igb_stats_strings[0]))

static const struct rte_igb_xstats_name_off rte_igbvf_stats_strings[] = {
	{"rx_multicast_packets", offsetof(struct e1000_vf_stats, mprc)},
	{"rx_good_loopback_packets", offsetof(struct e1000_vf_stats, gprlbc)},
	{"tx_good_loopback_packets", offsetof(struct e1000_vf_stats, gptlbc)},
	{"rx_good_loopback_bytes", offsetof(struct e1000_vf_stats, gorlbc)},
	{"tx_good_loopback_bytes", offsetof(struct e1000_vf_stats, gotlbc)},
};

#define IGBVF_NB_XSTATS (sizeof(rte_igbvf_stats_strings) / \
		sizeof(rte_igbvf_stats_strings[0]))

/**
 * Atomically reads the link status information from global
 * structure rte_eth_dev.
 *
 * @param dev
 *   - Pointer to the structure rte_eth_dev to read from.
 *   - Pointer to the buffer to be saved with the link status.
 *
 * @return
 *   - On success, zero.
 *   - On failure, negative value.
 */
static inline int
rte_igb_dev_atomic_read_link_status(struct rte_eth_dev *dev,
				struct rte_eth_link *link)
{
	struct rte_eth_link *dst = link;
	struct rte_eth_link *src = &(dev->data->dev_link);

	if (rte_atomic64_cmpset((uint64_t *)dst, *(uint64_t *)dst,
					*(uint64_t *)src) == 0)
		return -1;

	return 0;
}

/**
 * Atomically writes the link status information into global
 * structure rte_eth_dev.
 *
 * @param dev
 *   - Pointer to the structure rte_eth_dev to read from.
 *   - Pointer to the buffer to be saved with the link status.
 *
 * @return
 *   - On success, zero.
 *   - On failure, negative value.
 */
static inline int
rte_igb_dev_atomic_write_link_status(struct rte_eth_dev *dev,
				struct rte_eth_link *link)
{
	struct rte_eth_link *dst = &(dev->data->dev_link);
	struct rte_eth_link *src = link;

	if (rte_atomic64_cmpset((uint64_t *)dst, *(uint64_t *)dst,
					*(uint64_t *)src) == 0)
		return -1;

	return 0;
}

static inline void
igb_intr_enable(struct rte_eth_dev *dev)
{
	struct e1000_interrupt *intr =
		E1000_DEV_PRIVATE_TO_INTR(dev->data->dev_private);
	struct e1000_hw *hw =
		E1000_DEV_PRIVATE_TO_HW(dev->data->dev_private);

	E1000_WRITE_REG(hw, E1000_IMS, intr->mask);
	E1000_WRITE_FLUSH(hw);
}

static void
igb_intr_disable(struct e1000_hw *hw)
{
	E1000_WRITE_REG(hw, E1000_IMC, ~0);
	E1000_WRITE_FLUSH(hw);
}

static inline int32_t
igb_pf_reset_hw(struct e1000_hw *hw)
{
	uint32_t ctrl_ext;
	int32_t status;

	status = e1000_reset_hw(hw);

	ctrl_ext = E1000_READ_REG(hw, E1000_CTRL_EXT);
	/* Set PF Reset Done bit so PF/VF Mail Ops can work */
	ctrl_ext |= E1000_CTRL_EXT_PFRSTD;
	E1000_WRITE_REG(hw, E1000_CTRL_EXT, ctrl_ext);
	E1000_WRITE_FLUSH(hw);

	return status;
}

static void
igb_identify_hardware(struct rte_eth_dev *dev)
{
	struct e1000_hw *hw =
		E1000_DEV_PRIVATE_TO_HW(dev->data->dev_private);

	hw->vendor_id = dev->pci_dev->id.vendor_id;
	hw->device_id = dev->pci_dev->id.device_id;
	hw->subsystem_vendor_id = dev->pci_dev->id.subsystem_vendor_id;
	hw->subsystem_device_id = dev->pci_dev->id.subsystem_device_id;

	e1000_set_mac_type(hw);

	/* need to check if it is a vf device below */
}

static int
igb_reset_swfw_lock(struct e1000_hw *hw)
{
	int ret_val;

	/*
	 * Do mac ops initialization manually here, since we will need
	 * some function pointers set by this call.
	 */
	ret_val = e1000_init_mac_params(hw);
	if (ret_val)
		return ret_val;

	/*
	 * SMBI lock should not fail in this early stage. If this is the case,
	 * it is due to an improper exit of the application.
	 * So force the release of the faulty lock.
	 */
	if (e1000_get_hw_semaphore_generic(hw) < 0) {
		PMD_DRV_LOG(DEBUG, "SMBI lock released");
	}
	e1000_put_hw_semaphore_generic(hw);

	if (hw->mac.ops.acquire_swfw_sync != NULL) {
		uint16_t mask;

		/*
		 * Phy lock should not fail in this early stage. If this is the case,
		 * it is due to an improper exit of the application.
		 * So force the release of the faulty lock.
		 */
		mask = E1000_SWFW_PHY0_SM << hw->bus.func;
		if (hw->bus.func > E1000_FUNC_1)
			mask <<= 2;
		if (hw->mac.ops.acquire_swfw_sync(hw, mask) < 0) {
			PMD_DRV_LOG(DEBUG, "SWFW phy%d lock released",
				    hw->bus.func);
		}
		hw->mac.ops.release_swfw_sync(hw, mask);

		/*
		 * This one is more tricky since it is common to all ports; but
		 * swfw_sync retries last long enough (1s) to be almost sure that if
		 * lock can not be taken it is due to an improper lock of the
		 * semaphore.
		 */
		mask = E1000_SWFW_EEP_SM;
		if (hw->mac.ops.acquire_swfw_sync(hw, mask) < 0) {
			PMD_DRV_LOG(DEBUG, "SWFW common locks released");
		}
		hw->mac.ops.release_swfw_sync(hw, mask);
	}

	return E1000_SUCCESS;
}

static int
eth_igb_dev_init(struct rte_eth_dev *eth_dev)
{
	int error = 0;
	struct rte_pci_device *pci_dev;
	struct e1000_hw *hw =
		E1000_DEV_PRIVATE_TO_HW(eth_dev->data->dev_private);
	struct e1000_vfta * shadow_vfta =
		E1000_DEV_PRIVATE_TO_VFTA(eth_dev->data->dev_private);
	struct e1000_filter_info *filter_info =
		E1000_DEV_PRIVATE_TO_FILTER_INFO(eth_dev->data->dev_private);
	struct e1000_adapter *adapter =
		E1000_DEV_PRIVATE(eth_dev->data->dev_private);

	uint32_t ctrl_ext;

	pci_dev = eth_dev->pci_dev;

	rte_eth_copy_pci_info(eth_dev, pci_dev);

	eth_dev->dev_ops = &eth_igb_ops;
	eth_dev->rx_pkt_burst = &eth_igb_recv_pkts;
	eth_dev->tx_pkt_burst = &eth_igb_xmit_pkts;

	/* for secondary processes, we don't initialise any further as primary
	 * has already done this work. Only check we don't need a different
	 * RX function */
	if (rte_eal_process_type() != RTE_PROC_PRIMARY){
		if (eth_dev->data->scattered_rx)
			eth_dev->rx_pkt_burst = &eth_igb_recv_scattered_pkts;
		return 0;
	}

	hw->hw_addr= (void *)pci_dev->mem_resource[0].addr;

	igb_identify_hardware(eth_dev);
	if (e1000_setup_init_funcs(hw, FALSE) != E1000_SUCCESS) {
		error = -EIO;
		goto err_late;
	}

	e1000_get_bus_info(hw);

	/* Reset any pending lock */
	if (igb_reset_swfw_lock(hw) != E1000_SUCCESS) {
		error = -EIO;
		goto err_late;
	}

	/* Finish initialization */
	if (e1000_setup_init_funcs(hw, TRUE) != E1000_SUCCESS) {
		error = -EIO;
		goto err_late;
	}

	hw->mac.autoneg = 1;
	hw->phy.autoneg_wait_to_complete = 0;
	hw->phy.autoneg_advertised = E1000_ALL_SPEED_DUPLEX;

	/* Copper options */
	if (hw->phy.media_type == e1000_media_type_copper) {
		hw->phy.mdix = 0; /* AUTO_ALL_MODES */
		hw->phy.disable_polarity_correction = 0;
		hw->phy.ms_type = e1000_ms_hw_default;
	}

	/*
	 * Start from a known state, this is important in reading the nvm
	 * and mac from that.
	 */
	igb_pf_reset_hw(hw);

	/* Make sure we have a good EEPROM before we read from it */
	if (e1000_validate_nvm_checksum(hw) < 0) {
		/*
		 * Some PCI-E parts fail the first check due to
		 * the link being in sleep state, call it again,
		 * if it fails a second time its a real issue.
		 */
		if (e1000_validate_nvm_checksum(hw) < 0) {
			PMD_INIT_LOG(ERR, "EEPROM checksum invalid");
			error = -EIO;
			goto err_late;
		}
	}

	/* Read the permanent MAC address out of the EEPROM */
	if (e1000_read_mac_addr(hw) != 0) {
		PMD_INIT_LOG(ERR, "EEPROM error while reading MAC address");
		error = -EIO;
		goto err_late;
	}

	/* Allocate memory for storing MAC addresses */
	eth_dev->data->mac_addrs = rte_zmalloc("e1000",
		ETHER_ADDR_LEN * hw->mac.rar_entry_count, 0);
	if (eth_dev->data->mac_addrs == NULL) {
		PMD_INIT_LOG(ERR, "Failed to allocate %d bytes needed to "
						"store MAC addresses",
				ETHER_ADDR_LEN * hw->mac.rar_entry_count);
		error = -ENOMEM;
		goto err_late;
	}

	/* Copy the permanent MAC address */
	ether_addr_copy((struct ether_addr *)hw->mac.addr, &eth_dev->data->mac_addrs[0]);

	/* initialize the vfta */
	memset(shadow_vfta, 0, sizeof(*shadow_vfta));

	/* Now initialize the hardware */
	if (igb_hardware_init(hw) != 0) {
		PMD_INIT_LOG(ERR, "Hardware initialization failed");
		rte_free(eth_dev->data->mac_addrs);
		eth_dev->data->mac_addrs = NULL;
		error = -ENODEV;
		goto err_late;
	}
	hw->mac.get_link_status = 1;
	adapter->stopped = 0;

	/* Indicate SOL/IDER usage */
	if (e1000_check_reset_block(hw) < 0) {
		PMD_INIT_LOG(ERR, "PHY reset is blocked due to"
					"SOL/IDER session");
	}

	/* initialize PF if max_vfs not zero */
	igb_pf_host_init(eth_dev);

	ctrl_ext = E1000_READ_REG(hw, E1000_CTRL_EXT);
	/* Set PF Reset Done bit so PF/VF Mail Ops can work */
	ctrl_ext |= E1000_CTRL_EXT_PFRSTD;
	E1000_WRITE_REG(hw, E1000_CTRL_EXT, ctrl_ext);
	E1000_WRITE_FLUSH(hw);

	PMD_INIT_LOG(DEBUG, "port_id %d vendorID=0x%x deviceID=0x%x",
		     eth_dev->data->port_id, pci_dev->id.vendor_id,
		     pci_dev->id.device_id);

	/* enable support intr */
	igb_intr_enable(eth_dev);

	TAILQ_INIT(&filter_info->flex_list);
	filter_info->flex_mask = 0;
	TAILQ_INIT(&filter_info->twotuple_list);
	filter_info->twotuple_mask = 0;
	TAILQ_INIT(&filter_info->fivetuple_list);
	filter_info->fivetuple_mask = 0;

	return 0;

err_late:
	igb_hw_control_release(hw);

	return (error);
}

static int
eth_igb_dev_uninit(struct rte_eth_dev *eth_dev)
{
	struct rte_pci_device *pci_dev;
	struct e1000_hw *hw;
	struct e1000_adapter *adapter =
		E1000_DEV_PRIVATE(eth_dev->data->dev_private);

	PMD_INIT_FUNC_TRACE();

	if (rte_eal_process_type() != RTE_PROC_PRIMARY)
		return -EPERM;

	hw = E1000_DEV_PRIVATE_TO_HW(eth_dev->data->dev_private);
	pci_dev = eth_dev->pci_dev;

	if (adapter->stopped == 0)
		eth_igb_close(eth_dev);

	eth_dev->dev_ops = NULL;
	eth_dev->rx_pkt_burst = NULL;
	eth_dev->tx_pkt_burst = NULL;

	/* Reset any pending lock */
	igb_reset_swfw_lock(hw);

	rte_free(eth_dev->data->mac_addrs);
	eth_dev->data->mac_addrs = NULL;

	/* uninitialize PF if max_vfs not zero */
	igb_pf_host_uninit(eth_dev);

	/* disable uio intr before callback unregister */
	rte_intr_disable(&(pci_dev->intr_handle));
	rte_intr_callback_unregister(&(pci_dev->intr_handle),
		eth_igb_interrupt_handler, (void *)eth_dev);

	return 0;
}

/*
 * Virtual Function device init
 */
static int
eth_igbvf_dev_init(struct rte_eth_dev *eth_dev)
{
	struct rte_pci_device *pci_dev;
	struct e1000_adapter *adapter =
		E1000_DEV_PRIVATE(eth_dev->data->dev_private);
	struct e1000_hw *hw =
		E1000_DEV_PRIVATE_TO_HW(eth_dev->data->dev_private);
	int diag;

	PMD_INIT_FUNC_TRACE();

	eth_dev->dev_ops = &igbvf_eth_dev_ops;
	eth_dev->rx_pkt_burst = &eth_igb_recv_pkts;
	eth_dev->tx_pkt_burst = &eth_igb_xmit_pkts;

	/* for secondary processes, we don't initialise any further as primary
	 * has already done this work. Only check we don't need a different
	 * RX function */
	if (rte_eal_process_type() != RTE_PROC_PRIMARY){
		if (eth_dev->data->scattered_rx)
			eth_dev->rx_pkt_burst = &eth_igb_recv_scattered_pkts;
		return 0;
	}

	pci_dev = eth_dev->pci_dev;

	rte_eth_copy_pci_info(eth_dev, pci_dev);

	hw->device_id = pci_dev->id.device_id;
	hw->vendor_id = pci_dev->id.vendor_id;
	hw->hw_addr = (void *)pci_dev->mem_resource[0].addr;
	adapter->stopped = 0;

	/* Initialize the shared code (base driver) */
	diag = e1000_setup_init_funcs(hw, TRUE);
	if (diag != 0) {
		PMD_INIT_LOG(ERR, "Shared code init failed for igbvf: %d",
			diag);
		return -EIO;
	}

	/* init_mailbox_params */
	hw->mbx.ops.init_params(hw);

	/* Disable the interrupts for VF */
	igbvf_intr_disable(hw);

	diag = hw->mac.ops.reset_hw(hw);

	/* Allocate memory for storing MAC addresses */
	eth_dev->data->mac_addrs = rte_zmalloc("igbvf", ETHER_ADDR_LEN *
		hw->mac.rar_entry_count, 0);
	if (eth_dev->data->mac_addrs == NULL) {
		PMD_INIT_LOG(ERR,
			"Failed to allocate %d bytes needed to store MAC "
			"addresses",
			ETHER_ADDR_LEN * hw->mac.rar_entry_count);
		return -ENOMEM;
	}

	/* Copy the permanent MAC address */
	ether_addr_copy((struct ether_addr *) hw->mac.perm_addr,
			&eth_dev->data->mac_addrs[0]);

	PMD_INIT_LOG(DEBUG, "port %d vendorID=0x%x deviceID=0x%x "
		     "mac.type=%s",
		     eth_dev->data->port_id, pci_dev->id.vendor_id,
		     pci_dev->id.device_id, "igb_mac_82576_vf");

	return 0;
}

static int
eth_igbvf_dev_uninit(struct rte_eth_dev *eth_dev)
{
	struct e1000_adapter *adapter =
		E1000_DEV_PRIVATE(eth_dev->data->dev_private);

	PMD_INIT_FUNC_TRACE();

	if (rte_eal_process_type() != RTE_PROC_PRIMARY)
		return -EPERM;

	if (adapter->stopped == 0)
		igbvf_dev_close(eth_dev);

	eth_dev->dev_ops = NULL;
	eth_dev->rx_pkt_burst = NULL;
	eth_dev->tx_pkt_burst = NULL;

	rte_free(eth_dev->data->mac_addrs);
	eth_dev->data->mac_addrs = NULL;

	return 0;
}

static struct eth_driver rte_igb_pmd = {
	.pci_drv = {
		.name = "rte_igb_pmd",
		.id_table = pci_id_igb_map,
		.drv_flags = RTE_PCI_DRV_NEED_MAPPING | RTE_PCI_DRV_INTR_LSC |
			RTE_PCI_DRV_DETACHABLE,
	},
	.eth_dev_init = eth_igb_dev_init,
	.eth_dev_uninit = eth_igb_dev_uninit,
	.dev_private_size = sizeof(struct e1000_adapter),
};

/*
 * virtual function driver struct
 */
static struct eth_driver rte_igbvf_pmd = {
	.pci_drv = {
		.name = "rte_igbvf_pmd",
		.id_table = pci_id_igbvf_map,
		.drv_flags = RTE_PCI_DRV_NEED_MAPPING | RTE_PCI_DRV_DETACHABLE,
	},
	.eth_dev_init = eth_igbvf_dev_init,
	.eth_dev_uninit = eth_igbvf_dev_uninit,
	.dev_private_size = sizeof(struct e1000_adapter),
};

static int
rte_igb_pmd_init(const char *name __rte_unused, const char *params __rte_unused)
{
	rte_eth_driver_register(&rte_igb_pmd);
	return 0;
}

static void
igb_vmdq_vlan_hw_filter_enable(struct rte_eth_dev *dev)
{
	struct e1000_hw *hw =
		E1000_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	/* RCTL: enable VLAN filter since VMDq always use VLAN filter */
	uint32_t rctl = E1000_READ_REG(hw, E1000_RCTL);
	rctl |= E1000_RCTL_VFE;
	E1000_WRITE_REG(hw, E1000_RCTL, rctl);
}

/*
 * VF Driver initialization routine.
 * Invoked one at EAL init time.
 * Register itself as the [Virtual Poll Mode] Driver of PCI IGB devices.
 */
static int
rte_igbvf_pmd_init(const char *name __rte_unused, const char *params __rte_unused)
{
	PMD_INIT_FUNC_TRACE();

	rte_eth_driver_register(&rte_igbvf_pmd);
	return (0);
}

static int
igb_check_mq_mode(struct rte_eth_dev *dev)
{
	enum rte_eth_rx_mq_mode rx_mq_mode = dev->data->dev_conf.rxmode.mq_mode;
	enum rte_eth_tx_mq_mode tx_mq_mode = dev->data->dev_conf.txmode.mq_mode;
	uint16_t nb_rx_q = dev->data->nb_rx_queues;
	uint16_t nb_tx_q = dev->data->nb_rx_queues;

	if ((rx_mq_mode & ETH_MQ_RX_DCB_FLAG) ||
	    tx_mq_mode == ETH_MQ_TX_DCB ||
	    tx_mq_mode == ETH_MQ_TX_VMDQ_DCB) {
		PMD_INIT_LOG(ERR, "DCB mode is not supported.");
		return -EINVAL;
	}
	if (RTE_ETH_DEV_SRIOV(dev).active != 0) {
		/* Check multi-queue mode.
		 * To no break software we accept ETH_MQ_RX_NONE as this might
		 * be used to turn off VLAN filter.
		 */

		if (rx_mq_mode == ETH_MQ_RX_NONE ||
		    rx_mq_mode == ETH_MQ_RX_VMDQ_ONLY) {
			dev->data->dev_conf.rxmode.mq_mode = ETH_MQ_RX_VMDQ_ONLY;
			RTE_ETH_DEV_SRIOV(dev).nb_q_per_pool = 1;
		} else {
			/* Only support one queue on VFs.
			 * RSS together with SRIOV is not supported.
			 */
			PMD_INIT_LOG(ERR, "SRIOV is active,"
					" wrong mq_mode rx %d.",
					rx_mq_mode);
			return -EINVAL;
		}
		/* TX mode is not used here, so mode might be ignored.*/
		if (tx_mq_mode != ETH_MQ_TX_VMDQ_ONLY) {
			/* SRIOV only works in VMDq enable mode */
			PMD_INIT_LOG(WARNING, "SRIOV is active,"
					" TX mode %d is not supported. "
					" Driver will behave as %d mode.",
					tx_mq_mode, ETH_MQ_TX_VMDQ_ONLY);
		}

		/* check valid queue number */
		if ((nb_rx_q > 1) || (nb_tx_q > 1)) {
			PMD_INIT_LOG(ERR, "SRIOV is active,"
					" only support one queue on VFs.");
			return -EINVAL;
		}
	} else {
		/* To no break software that set invalid mode, only display
		 * warning if invalid mode is used.
		 */
		if (rx_mq_mode != ETH_MQ_RX_NONE &&
		    rx_mq_mode != ETH_MQ_RX_VMDQ_ONLY &&
		    rx_mq_mode != ETH_MQ_RX_RSS) {
			/* RSS together with VMDq not supported*/
			PMD_INIT_LOG(ERR, "RX mode %d is not supported.",
				     rx_mq_mode);
			return -EINVAL;
		}

		if (tx_mq_mode != ETH_MQ_TX_NONE &&
		    tx_mq_mode != ETH_MQ_TX_VMDQ_ONLY) {
			PMD_INIT_LOG(WARNING, "TX mode %d is not supported."
					" Due to txmode is meaningless in this"
					" driver, just ignore.",
					tx_mq_mode);
		}
	}
	return 0;
}

static int
eth_igb_configure(struct rte_eth_dev *dev)
{
	struct e1000_interrupt *intr =
		E1000_DEV_PRIVATE_TO_INTR(dev->data->dev_private);
	int ret;

	PMD_INIT_FUNC_TRACE();

	/* multipe queue mode checking */
	ret  = igb_check_mq_mode(dev);
	if (ret != 0) {
		PMD_DRV_LOG(ERR, "igb_check_mq_mode fails with %d.",
			    ret);
		return ret;
	}

	intr->flags |= E1000_FLAG_NEED_LINK_UPDATE;
	PMD_INIT_FUNC_TRACE();

	return 0;
}

static int
eth_igb_start(struct rte_eth_dev *dev)
{
	struct e1000_hw *hw =
		E1000_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	struct e1000_adapter *adapter =
		E1000_DEV_PRIVATE(dev->data->dev_private);
	struct rte_intr_handle *intr_handle = &dev->pci_dev->intr_handle;
	int ret, mask;
	uint32_t intr_vector = 0;
	uint32_t ctrl_ext;

	PMD_INIT_FUNC_TRACE();

	/* Power up the phy. Needed to make the link go Up */
	e1000_power_up_phy(hw);

	/*
	 * Packet Buffer Allocation (PBA)
	 * Writing PBA sets the receive portion of the buffer
	 * the remainder is used for the transmit buffer.
	 */
	if (hw->mac.type == e1000_82575) {
		uint32_t pba;

		pba = E1000_PBA_32K; /* 32K for Rx, 16K for Tx */
		E1000_WRITE_REG(hw, E1000_PBA, pba);
	}

	/* Put the address into the Receive Address Array */
	e1000_rar_set(hw, hw->mac.addr, 0);

	/* Initialize the hardware */
	if (igb_hardware_init(hw)) {
		PMD_INIT_LOG(ERR, "Unable to initialize the hardware");
		return (-EIO);
	}
	adapter->stopped = 0;

	E1000_WRITE_REG(hw, E1000_VET, ETHER_TYPE_VLAN << 16 | ETHER_TYPE_VLAN);

	ctrl_ext = E1000_READ_REG(hw, E1000_CTRL_EXT);
	/* Set PF Reset Done bit so PF/VF Mail Ops can work */
	ctrl_ext |= E1000_CTRL_EXT_PFRSTD;
	E1000_WRITE_REG(hw, E1000_CTRL_EXT, ctrl_ext);
	E1000_WRITE_FLUSH(hw);

	/* configure PF module if SRIOV enabled */
	igb_pf_host_configure(dev);

	/* check and configure queue intr-vector mapping */
	if (dev->data->dev_conf.intr_conf.rxq != 0) {
		intr_vector = dev->data->nb_rx_queues;
		if (rte_intr_efd_enable(intr_handle, intr_vector))
			return -1;
	}

	if (rte_intr_dp_is_en(intr_handle)) {
		intr_handle->intr_vec =
			rte_zmalloc("intr_vec",
				    dev->data->nb_rx_queues * sizeof(int), 0);
		if (intr_handle->intr_vec == NULL) {
			PMD_INIT_LOG(ERR, "Failed to allocate %d rx_queues"
				     " intr_vec\n", dev->data->nb_rx_queues);
			return -ENOMEM;
		}
	}

	/* confiugre msix for rx interrupt */
	eth_igb_configure_msix_intr(dev);

	/* Configure for OS presence */
	igb_init_manageability(hw);

	eth_igb_tx_init(dev);

	/* This can fail when allocating mbufs for descriptor rings */
	ret = eth_igb_rx_init(dev);
	if (ret) {
		PMD_INIT_LOG(ERR, "Unable to initialize RX hardware");
		igb_dev_clear_queues(dev);
		return ret;
	}

	e1000_clear_hw_cntrs_base_generic(hw);

	/*
	 * VLAN Offload Settings
	 */
	mask = ETH_VLAN_STRIP_MASK | ETH_VLAN_FILTER_MASK | \
			ETH_VLAN_EXTEND_MASK;
	eth_igb_vlan_offload_set(dev, mask);

	if (dev->data->dev_conf.rxmode.mq_mode == ETH_MQ_RX_VMDQ_ONLY) {
		/* Enable VLAN filter since VMDq always use VLAN filter */
		igb_vmdq_vlan_hw_filter_enable(dev);
	}

	if ((hw->mac.type == e1000_82576) || (hw->mac.type == e1000_82580) ||
		(hw->mac.type == e1000_i350) || (hw->mac.type == e1000_i210) ||
		(hw->mac.type == e1000_i211)) {
		/* Configure EITR with the maximum possible value (0xFFFF) */
		E1000_WRITE_REG(hw, E1000_EITR(0), 0xFFFF);
	}

	/* Setup link speed and duplex */
	switch (dev->data->dev_conf.link_speed) {
	case ETH_LINK_SPEED_AUTONEG:
		if (dev->data->dev_conf.link_duplex == ETH_LINK_AUTONEG_DUPLEX)
			hw->phy.autoneg_advertised = E1000_ALL_SPEED_DUPLEX;
		else if (dev->data->dev_conf.link_duplex == ETH_LINK_HALF_DUPLEX)
			hw->phy.autoneg_advertised = E1000_ALL_HALF_DUPLEX;
		else if (dev->data->dev_conf.link_duplex == ETH_LINK_FULL_DUPLEX)
			hw->phy.autoneg_advertised = E1000_ALL_FULL_DUPLEX;
		else
			goto error_invalid_config;
		break;
	case ETH_LINK_SPEED_10:
		if (dev->data->dev_conf.link_duplex == ETH_LINK_AUTONEG_DUPLEX)
			hw->phy.autoneg_advertised = E1000_ALL_10_SPEED;
		else if (dev->data->dev_conf.link_duplex == ETH_LINK_HALF_DUPLEX)
			hw->phy.autoneg_advertised = ADVERTISE_10_HALF;
		else if (dev->data->dev_conf.link_duplex == ETH_LINK_FULL_DUPLEX)
			hw->phy.autoneg_advertised = ADVERTISE_10_FULL;
		else
			goto error_invalid_config;
		break;
	case ETH_LINK_SPEED_100:
		if (dev->data->dev_conf.link_duplex == ETH_LINK_AUTONEG_DUPLEX)
			hw->phy.autoneg_advertised = E1000_ALL_100_SPEED;
		else if (dev->data->dev_conf.link_duplex == ETH_LINK_HALF_DUPLEX)
			hw->phy.autoneg_advertised = ADVERTISE_100_HALF;
		else if (dev->data->dev_conf.link_duplex == ETH_LINK_FULL_DUPLEX)
			hw->phy.autoneg_advertised = ADVERTISE_100_FULL;
		else
			goto error_invalid_config;
		break;
	case ETH_LINK_SPEED_1000:
		if ((dev->data->dev_conf.link_duplex == ETH_LINK_AUTONEG_DUPLEX) ||
				(dev->data->dev_conf.link_duplex == ETH_LINK_FULL_DUPLEX))
			hw->phy.autoneg_advertised = ADVERTISE_1000_FULL;
		else
			goto error_invalid_config;
		break;
	case ETH_LINK_SPEED_10000:
	default:
		goto error_invalid_config;
	}
	e1000_setup_link(hw);

	/* check if lsc interrupt feature is enabled */
	if (dev->data->dev_conf.intr_conf.lsc != 0) {
		if (rte_intr_allow_others(intr_handle)) {
			rte_intr_callback_register(intr_handle,
						   eth_igb_interrupt_handler,
						   (void *)dev);
			eth_igb_lsc_interrupt_setup(dev);
		} else
			PMD_INIT_LOG(INFO, "lsc won't enable because of"
				     " no intr multiplex\n");
	}

	/* check if rxq interrupt is enabled */
	if (dev->data->dev_conf.intr_conf.rxq != 0)
		eth_igb_rxq_interrupt_setup(dev);

	/* enable uio/vfio intr/eventfd mapping */
	rte_intr_enable(intr_handle);

	/* resume enabled intr since hw reset */
	igb_intr_enable(dev);

	PMD_INIT_LOG(DEBUG, "<<");

	return (0);

error_invalid_config:
	PMD_INIT_LOG(ERR, "Invalid link_speed/link_duplex (%u/%u) for port %u",
		     dev->data->dev_conf.link_speed,
		     dev->data->dev_conf.link_duplex, dev->data->port_id);
	igb_dev_clear_queues(dev);
	return (-EINVAL);
}

/*********************************************************************
 *
 *  This routine disables all traffic on the adapter by issuing a
 *  global reset on the MAC.
 *
 **********************************************************************/
static void
eth_igb_stop(struct rte_eth_dev *dev)
{
	struct e1000_hw *hw = E1000_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	struct e1000_filter_info *filter_info =
		E1000_DEV_PRIVATE_TO_FILTER_INFO(dev->data->dev_private);
	struct rte_eth_link link;
	struct e1000_flex_filter *p_flex;
	struct e1000_5tuple_filter *p_5tuple, *p_5tuple_next;
	struct e1000_2tuple_filter *p_2tuple, *p_2tuple_next;
	struct rte_intr_handle *intr_handle = &dev->pci_dev->intr_handle;

	igb_intr_disable(hw);

	/* disable intr eventfd mapping */
	rte_intr_disable(intr_handle);

	igb_pf_reset_hw(hw);
	E1000_WRITE_REG(hw, E1000_WUC, 0);

	/* Set bit for Go Link disconnect */
	if (hw->mac.type >= e1000_82580) {
		uint32_t phpm_reg;

		phpm_reg = E1000_READ_REG(hw, E1000_82580_PHY_POWER_MGMT);
		phpm_reg |= E1000_82580_PM_GO_LINKD;
		E1000_WRITE_REG(hw, E1000_82580_PHY_POWER_MGMT, phpm_reg);
	}

	/* Power down the phy. Needed to make the link go Down */
	if (hw->phy.media_type == e1000_media_type_copper)
		e1000_power_down_phy(hw);
	else
		e1000_shutdown_fiber_serdes_link(hw);

	igb_dev_clear_queues(dev);

	/* clear the recorded link status */
	memset(&link, 0, sizeof(link));
	rte_igb_dev_atomic_write_link_status(dev, &link);

	/* Remove all flex filters of the device */
	while ((p_flex = TAILQ_FIRST(&filter_info->flex_list))) {
		TAILQ_REMOVE(&filter_info->flex_list, p_flex, entries);
		rte_free(p_flex);
	}
	filter_info->flex_mask = 0;

	/* Remove all ntuple filters of the device */
	for (p_5tuple = TAILQ_FIRST(&filter_info->fivetuple_list);
	     p_5tuple != NULL; p_5tuple = p_5tuple_next) {
		p_5tuple_next = TAILQ_NEXT(p_5tuple, entries);
		TAILQ_REMOVE(&filter_info->fivetuple_list,
			     p_5tuple, entries);
		rte_free(p_5tuple);
	}
	filter_info->fivetuple_mask = 0;
	for (p_2tuple = TAILQ_FIRST(&filter_info->twotuple_list);
	     p_2tuple != NULL; p_2tuple = p_2tuple_next) {
		p_2tuple_next = TAILQ_NEXT(p_2tuple, entries);
		TAILQ_REMOVE(&filter_info->twotuple_list,
			     p_2tuple, entries);
		rte_free(p_2tuple);
	}
	filter_info->twotuple_mask = 0;

	/* Clean datapath event and queue/vec mapping */
	rte_intr_efd_disable(intr_handle);
	if (intr_handle->intr_vec != NULL) {
		rte_free(intr_handle->intr_vec);
		intr_handle->intr_vec = NULL;
	}
}

static void
eth_igb_close(struct rte_eth_dev *dev)
{
	struct e1000_hw *hw = E1000_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	struct e1000_adapter *adapter =
		E1000_DEV_PRIVATE(dev->data->dev_private);
	struct rte_eth_link link;
	struct rte_pci_device *pci_dev;

	eth_igb_stop(dev);
	adapter->stopped = 1;

	e1000_phy_hw_reset(hw);
	igb_release_manageability(hw);
	igb_hw_control_release(hw);

	/* Clear bit for Go Link disconnect */
	if (hw->mac.type >= e1000_82580) {
		uint32_t phpm_reg;

		phpm_reg = E1000_READ_REG(hw, E1000_82580_PHY_POWER_MGMT);
		phpm_reg &= ~E1000_82580_PM_GO_LINKD;
		E1000_WRITE_REG(hw, E1000_82580_PHY_POWER_MGMT, phpm_reg);
	}

	igb_dev_free_queues(dev);

	pci_dev = dev->pci_dev;
	if (pci_dev->intr_handle.intr_vec) {
		rte_free(pci_dev->intr_handle.intr_vec);
		pci_dev->intr_handle.intr_vec = NULL;
	}

	memset(&link, 0, sizeof(link));
	rte_igb_dev_atomic_write_link_status(dev, &link);
}

static int
igb_get_rx_buffer_size(struct e1000_hw *hw)
{
	uint32_t rx_buf_size;
	if (hw->mac.type == e1000_82576) {
		rx_buf_size = (E1000_READ_REG(hw, E1000_RXPBS) & 0xffff) << 10;
	} else if (hw->mac.type == e1000_82580 || hw->mac.type == e1000_i350) {
		/* PBS needs to be translated according to a lookup table */
		rx_buf_size = (E1000_READ_REG(hw, E1000_RXPBS) & 0xf);
		rx_buf_size = (uint32_t) e1000_rxpbs_adjust_82580(rx_buf_size);
		rx_buf_size = (rx_buf_size << 10);
	} else if (hw->mac.type == e1000_i210 || hw->mac.type == e1000_i211) {
		rx_buf_size = (E1000_READ_REG(hw, E1000_RXPBS) & 0x3f) << 10;
	} else {
		rx_buf_size = (E1000_READ_REG(hw, E1000_PBA) & 0xffff) << 10;
	}

	return rx_buf_size;
}

/*********************************************************************
 *
 *  Initialize the hardware
 *
 **********************************************************************/
static int
igb_hardware_init(struct e1000_hw *hw)
{
	uint32_t rx_buf_size;
	int diag;

	/* Let the firmware know the OS is in control */
	igb_hw_control_acquire(hw);

	/*
	 * These parameters control the automatic generation (Tx) and
	 * response (Rx) to Ethernet PAUSE frames.
	 * - High water mark should allow for at least two standard size (1518)
	 *   frames to be received after sending an XOFF.
	 * - Low water mark works best when it is very near the high water mark.
	 *   This allows the receiver to restart by sending XON when it has
	 *   drained a bit. Here we use an arbitrary value of 1500 which will
	 *   restart after one full frame is pulled from the buffer. There
	 *   could be several smaller frames in the buffer and if so they will
	 *   not trigger the XON until their total number reduces the buffer
	 *   by 1500.
	 * - The pause time is fairly large at 1000 x 512ns = 512 usec.
	 */
	rx_buf_size = igb_get_rx_buffer_size(hw);

	hw->fc.high_water = rx_buf_size - (ETHER_MAX_LEN * 2);
	hw->fc.low_water = hw->fc.high_water - 1500;
	hw->fc.pause_time = IGB_FC_PAUSE_TIME;
	hw->fc.send_xon = 1;

	/* Set Flow control, use the tunable location if sane */
	if ((igb_fc_setting != e1000_fc_none) && (igb_fc_setting < 4))
		hw->fc.requested_mode = igb_fc_setting;
	else
		hw->fc.requested_mode = e1000_fc_none;

	/* Issue a global reset */
	igb_pf_reset_hw(hw);
	E1000_WRITE_REG(hw, E1000_WUC, 0);

	diag = e1000_init_hw(hw);
	if (diag < 0)
		return (diag);

	E1000_WRITE_REG(hw, E1000_VET, ETHER_TYPE_VLAN << 16 | ETHER_TYPE_VLAN);
	e1000_get_phy_info(hw);
	e1000_check_for_link(hw);

	return (0);
}

/* This function is based on igb_update_stats_counters() in igb/if_igb.c */
static void
igb_read_stats_registers(struct e1000_hw *hw, struct e1000_hw_stats *stats)
{
	int pause_frames;

	if(hw->phy.media_type == e1000_media_type_copper ||
	    (E1000_READ_REG(hw, E1000_STATUS) & E1000_STATUS_LU)) {
		stats->symerrs +=
		    E1000_READ_REG(hw,E1000_SYMERRS);
		stats->sec += E1000_READ_REG(hw, E1000_SEC);
	}

	stats->crcerrs += E1000_READ_REG(hw, E1000_CRCERRS);
	stats->mpc += E1000_READ_REG(hw, E1000_MPC);
	stats->scc += E1000_READ_REG(hw, E1000_SCC);
	stats->ecol += E1000_READ_REG(hw, E1000_ECOL);

	stats->mcc += E1000_READ_REG(hw, E1000_MCC);
	stats->latecol += E1000_READ_REG(hw, E1000_LATECOL);
	stats->colc += E1000_READ_REG(hw, E1000_COLC);
	stats->dc += E1000_READ_REG(hw, E1000_DC);
	stats->rlec += E1000_READ_REG(hw, E1000_RLEC);
	stats->xonrxc += E1000_READ_REG(hw, E1000_XONRXC);
	stats->xontxc += E1000_READ_REG(hw, E1000_XONTXC);
	/*
	** For watchdog management we need to know if we have been
	** paused during the last interval, so capture that here.
	*/
	pause_frames = E1000_READ_REG(hw, E1000_XOFFRXC);
	stats->xoffrxc += pause_frames;
	stats->xofftxc += E1000_READ_REG(hw, E1000_XOFFTXC);
	stats->fcruc += E1000_READ_REG(hw, E1000_FCRUC);
	stats->prc64 += E1000_READ_REG(hw, E1000_PRC64);
	stats->prc127 += E1000_READ_REG(hw, E1000_PRC127);
	stats->prc255 += E1000_READ_REG(hw, E1000_PRC255);
	stats->prc511 += E1000_READ_REG(hw, E1000_PRC511);
	stats->prc1023 += E1000_READ_REG(hw, E1000_PRC1023);
	stats->prc1522 += E1000_READ_REG(hw, E1000_PRC1522);
	stats->gprc += E1000_READ_REG(hw, E1000_GPRC);
	stats->bprc += E1000_READ_REG(hw, E1000_BPRC);
	stats->mprc += E1000_READ_REG(hw, E1000_MPRC);
	stats->gptc += E1000_READ_REG(hw, E1000_GPTC);

	/* For the 64-bit byte counters the low dword must be read first. */
	/* Both registers clear on the read of the high dword */

	stats->gorc += E1000_READ_REG(hw, E1000_GORCL);
	stats->gorc += ((uint64_t)E1000_READ_REG(hw, E1000_GORCH) << 32);
	stats->gotc += E1000_READ_REG(hw, E1000_GOTCL);
	stats->gotc += ((uint64_t)E1000_READ_REG(hw, E1000_GOTCH) << 32);

	stats->rnbc += E1000_READ_REG(hw, E1000_RNBC);
	stats->ruc += E1000_READ_REG(hw, E1000_RUC);
	stats->rfc += E1000_READ_REG(hw, E1000_RFC);
	stats->roc += E1000_READ_REG(hw, E1000_ROC);
	stats->rjc += E1000_READ_REG(hw, E1000_RJC);

	stats->tor += E1000_READ_REG(hw, E1000_TORL);
	stats->tor += ((uint64_t)E1000_READ_REG(hw, E1000_TORH) << 32);
	stats->tot += E1000_READ_REG(hw, E1000_TOTL);
	stats->tot += ((uint64_t)E1000_READ_REG(hw, E1000_TOTH) << 32);

	stats->tpr += E1000_READ_REG(hw, E1000_TPR);
	stats->tpt += E1000_READ_REG(hw, E1000_TPT);
	stats->ptc64 += E1000_READ_REG(hw, E1000_PTC64);
	stats->ptc127 += E1000_READ_REG(hw, E1000_PTC127);
	stats->ptc255 += E1000_READ_REG(hw, E1000_PTC255);
	stats->ptc511 += E1000_READ_REG(hw, E1000_PTC511);
	stats->ptc1023 += E1000_READ_REG(hw, E1000_PTC1023);
	stats->ptc1522 += E1000_READ_REG(hw, E1000_PTC1522);
	stats->mptc += E1000_READ_REG(hw, E1000_MPTC);
	stats->bptc += E1000_READ_REG(hw, E1000_BPTC);

	/* Interrupt Counts */

	stats->iac += E1000_READ_REG(hw, E1000_IAC);
	stats->icrxptc += E1000_READ_REG(hw, E1000_ICRXPTC);
	stats->icrxatc += E1000_READ_REG(hw, E1000_ICRXATC);
	stats->ictxptc += E1000_READ_REG(hw, E1000_ICTXPTC);
	stats->ictxatc += E1000_READ_REG(hw, E1000_ICTXATC);
	stats->ictxqec += E1000_READ_REG(hw, E1000_ICTXQEC);
	stats->ictxqmtc += E1000_READ_REG(hw, E1000_ICTXQMTC);
	stats->icrxdmtc += E1000_READ_REG(hw, E1000_ICRXDMTC);
	stats->icrxoc += E1000_READ_REG(hw, E1000_ICRXOC);

	/* Host to Card Statistics */

	stats->cbtmpc += E1000_READ_REG(hw, E1000_CBTMPC);
	stats->htdpmc += E1000_READ_REG(hw, E1000_HTDPMC);
	stats->cbrdpc += E1000_READ_REG(hw, E1000_CBRDPC);
	stats->cbrmpc += E1000_READ_REG(hw, E1000_CBRMPC);
	stats->rpthc += E1000_READ_REG(hw, E1000_RPTHC);
	stats->hgptc += E1000_READ_REG(hw, E1000_HGPTC);
	stats->htcbdpc += E1000_READ_REG(hw, E1000_HTCBDPC);
	stats->hgorc += E1000_READ_REG(hw, E1000_HGORCL);
	stats->hgorc += ((uint64_t)E1000_READ_REG(hw, E1000_HGORCH) << 32);
	stats->hgotc += E1000_READ_REG(hw, E1000_HGOTCL);
	stats->hgotc += ((uint64_t)E1000_READ_REG(hw, E1000_HGOTCH) << 32);
	stats->lenerrs += E1000_READ_REG(hw, E1000_LENERRS);
	stats->scvpc += E1000_READ_REG(hw, E1000_SCVPC);
	stats->hrmpc += E1000_READ_REG(hw, E1000_HRMPC);

	stats->algnerrc += E1000_READ_REG(hw, E1000_ALGNERRC);
	stats->rxerrc += E1000_READ_REG(hw, E1000_RXERRC);
	stats->tncrs += E1000_READ_REG(hw, E1000_TNCRS);
	stats->cexterr += E1000_READ_REG(hw, E1000_CEXTERR);
	stats->tsctc += E1000_READ_REG(hw, E1000_TSCTC);
	stats->tsctfc += E1000_READ_REG(hw, E1000_TSCTFC);
}

static void
eth_igb_stats_get(struct rte_eth_dev *dev, struct rte_eth_stats *rte_stats)
{
	struct e1000_hw *hw = E1000_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	struct e1000_hw_stats *stats =
			E1000_DEV_PRIVATE_TO_STATS(dev->data->dev_private);

	igb_read_stats_registers(hw, stats);

	if (rte_stats == NULL)
		return;

	/* Rx Errors */
	rte_stats->ibadcrc = stats->crcerrs;
	rte_stats->ibadlen = stats->rlec + stats->ruc + stats->roc;
	rte_stats->imissed = stats->mpc;
	rte_stats->ierrors = rte_stats->ibadcrc +
	                     rte_stats->ibadlen +
	                     rte_stats->imissed +
	                     stats->rxerrc + stats->algnerrc + stats->cexterr;

	/* Tx Errors */
	rte_stats->oerrors = stats->ecol + stats->latecol;

	/* XON/XOFF pause frames */
	rte_stats->tx_pause_xon  = stats->xontxc;
	rte_stats->rx_pause_xon  = stats->xonrxc;
	rte_stats->tx_pause_xoff = stats->xofftxc;
	rte_stats->rx_pause_xoff = stats->xoffrxc;

	rte_stats->ipackets = stats->gprc;
	rte_stats->opackets = stats->gptc;
	rte_stats->ibytes   = stats->gorc;
	rte_stats->obytes   = stats->gotc;
}

static void
eth_igb_stats_reset(struct rte_eth_dev *dev)
{
	struct e1000_hw_stats *hw_stats =
			E1000_DEV_PRIVATE_TO_STATS(dev->data->dev_private);

	/* HW registers are cleared on read */
	eth_igb_stats_get(dev, NULL);

	/* Reset software totals */
	memset(hw_stats, 0, sizeof(*hw_stats));
}

static void
eth_igb_xstats_reset(struct rte_eth_dev *dev)
{
	struct e1000_hw_stats *stats =
			E1000_DEV_PRIVATE_TO_STATS(dev->data->dev_private);

	/* HW registers are cleared on read */
	eth_igb_xstats_get(dev, NULL, IGB_NB_XSTATS);

	/* Reset software totals */
	memset(stats, 0, sizeof(*stats));
}

static int
eth_igb_xstats_get(struct rte_eth_dev *dev, struct rte_eth_xstats *xstats,
		   unsigned n)
{
	struct e1000_hw *hw = E1000_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	struct e1000_hw_stats *hw_stats =
			E1000_DEV_PRIVATE_TO_STATS(dev->data->dev_private);
	unsigned i;

	if (n < IGB_NB_XSTATS)
		return IGB_NB_XSTATS;

	igb_read_stats_registers(hw, hw_stats);

	/* If this is a reset xstats is NULL, and we have cleared the
	 * registers by reading them.
	 */
	if (!xstats)
		return 0;

	/* Extended stats */
	for (i = 0; i < IGB_NB_XSTATS; i++) {
		snprintf(xstats[i].name, sizeof(xstats[i].name),
			 "%s", rte_igb_stats_strings[i].name);
		xstats[i].value = *(uint64_t *)(((char *)hw_stats) +
			rte_igb_stats_strings[i].offset);
	}

	return IGB_NB_XSTATS;
}

static void
igbvf_read_stats_registers(struct e1000_hw *hw, struct e1000_vf_stats *hw_stats)
{
	/* Good Rx packets, include VF loopback */
	UPDATE_VF_STAT(E1000_VFGPRC,
	    hw_stats->last_gprc, hw_stats->gprc);

	/* Good Rx octets, include VF loopback */
	UPDATE_VF_STAT(E1000_VFGORC,
	    hw_stats->last_gorc, hw_stats->gorc);

	/* Good Tx packets, include VF loopback */
	UPDATE_VF_STAT(E1000_VFGPTC,
	    hw_stats->last_gptc, hw_stats->gptc);

	/* Good Tx octets, include VF loopback */
	UPDATE_VF_STAT(E1000_VFGOTC,
	    hw_stats->last_gotc, hw_stats->gotc);

	/* Rx Multicst packets */
	UPDATE_VF_STAT(E1000_VFMPRC,
	    hw_stats->last_mprc, hw_stats->mprc);

	/* Good Rx loopback packets */
	UPDATE_VF_STAT(E1000_VFGPRLBC,
	    hw_stats->last_gprlbc, hw_stats->gprlbc);

	/* Good Rx loopback octets */
	UPDATE_VF_STAT(E1000_VFGORLBC,
	    hw_stats->last_gorlbc, hw_stats->gorlbc);

	/* Good Tx loopback packets */
	UPDATE_VF_STAT(E1000_VFGPTLBC,
	    hw_stats->last_gptlbc, hw_stats->gptlbc);

	/* Good Tx loopback octets */
	UPDATE_VF_STAT(E1000_VFGOTLBC,
	    hw_stats->last_gotlbc, hw_stats->gotlbc);
}

static int
eth_igbvf_xstats_get(struct rte_eth_dev *dev, struct rte_eth_xstats *xstats,
		     unsigned n)
{
	struct e1000_hw *hw = E1000_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	struct e1000_vf_stats *hw_stats = (struct e1000_vf_stats *)
			E1000_DEV_PRIVATE_TO_STATS(dev->data->dev_private);
	unsigned i;

	if (n < IGBVF_NB_XSTATS)
		return IGBVF_NB_XSTATS;

	igbvf_read_stats_registers(hw, hw_stats);

	if (!xstats)
		return 0;

	for (i = 0; i < IGBVF_NB_XSTATS; i++) {
		snprintf(xstats[i].name, sizeof(xstats[i].name), "%s",
			 rte_igbvf_stats_strings[i].name);
		xstats[i].value = *(uint64_t *)(((char *)hw_stats) +
			rte_igbvf_stats_strings[i].offset);
	}

	return IGBVF_NB_XSTATS;
}

static void
eth_igbvf_stats_get(struct rte_eth_dev *dev, struct rte_eth_stats *rte_stats)
{
	struct e1000_hw *hw = E1000_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	struct e1000_vf_stats *hw_stats = (struct e1000_vf_stats *)
			  E1000_DEV_PRIVATE_TO_STATS(dev->data->dev_private);

	igbvf_read_stats_registers(hw, hw_stats);

	if (rte_stats == NULL)
		return;

	rte_stats->ipackets = hw_stats->gprc;
	rte_stats->ibytes = hw_stats->gorc;
	rte_stats->opackets = hw_stats->gptc;
	rte_stats->obytes = hw_stats->gotc;
	rte_stats->imcasts = hw_stats->mprc;
	rte_stats->ilbpackets = hw_stats->gprlbc;
	rte_stats->ilbbytes = hw_stats->gorlbc;
	rte_stats->olbpackets = hw_stats->gptlbc;
	rte_stats->olbbytes = hw_stats->gotlbc;
}

static void
eth_igbvf_stats_reset(struct rte_eth_dev *dev)
{
	struct e1000_vf_stats *hw_stats = (struct e1000_vf_stats*)
			E1000_DEV_PRIVATE_TO_STATS(dev->data->dev_private);

	/* Sync HW register to the last stats */
	eth_igbvf_stats_get(dev, NULL);

	/* reset HW current stats*/
	memset(&hw_stats->gprc, 0, sizeof(*hw_stats) -
	       offsetof(struct e1000_vf_stats, gprc));
}

static void
eth_igb_infos_get(struct rte_eth_dev *dev, struct rte_eth_dev_info *dev_info)
{
	struct e1000_hw *hw = E1000_DEV_PRIVATE_TO_HW(dev->data->dev_private);

	dev_info->min_rx_bufsize = 256; /* See BSIZE field of RCTL register. */
	dev_info->max_rx_pktlen  = 0x3FFF; /* See RLPML register. */
	dev_info->max_mac_addrs = hw->mac.rar_entry_count;
	dev_info->rx_offload_capa =
		DEV_RX_OFFLOAD_VLAN_STRIP |
		DEV_RX_OFFLOAD_IPV4_CKSUM |
		DEV_RX_OFFLOAD_UDP_CKSUM  |
		DEV_RX_OFFLOAD_TCP_CKSUM;
	dev_info->tx_offload_capa =
		DEV_TX_OFFLOAD_VLAN_INSERT |
		DEV_TX_OFFLOAD_IPV4_CKSUM  |
		DEV_TX_OFFLOAD_UDP_CKSUM   |
		DEV_TX_OFFLOAD_TCP_CKSUM   |
		DEV_TX_OFFLOAD_SCTP_CKSUM  |
		DEV_TX_OFFLOAD_TCP_TSO;

	switch (hw->mac.type) {
	case e1000_82575:
		dev_info->max_rx_queues = 4;
		dev_info->max_tx_queues = 4;
		dev_info->max_vmdq_pools = 0;
		break;

	case e1000_82576:
		dev_info->max_rx_queues = 16;
		dev_info->max_tx_queues = 16;
		dev_info->max_vmdq_pools = ETH_8_POOLS;
		dev_info->vmdq_queue_num = 16;
		break;

	case e1000_82580:
		dev_info->max_rx_queues = 8;
		dev_info->max_tx_queues = 8;
		dev_info->max_vmdq_pools = ETH_8_POOLS;
		dev_info->vmdq_queue_num = 8;
		break;

	case e1000_i350:
		dev_info->max_rx_queues = 8;
		dev_info->max_tx_queues = 8;
		dev_info->max_vmdq_pools = ETH_8_POOLS;
		dev_info->vmdq_queue_num = 8;
		break;

	case e1000_i354:
		dev_info->max_rx_queues = 8;
		dev_info->max_tx_queues = 8;
		break;

	case e1000_i210:
		dev_info->max_rx_queues = 4;
		dev_info->max_tx_queues = 4;
		dev_info->max_vmdq_pools = 0;
		break;

	case e1000_i211:
		dev_info->max_rx_queues = 2;
		dev_info->max_tx_queues = 2;
		dev_info->max_vmdq_pools = 0;
		break;

	default:
		/* Should not happen */
		break;
	}
	dev_info->hash_key_size = IGB_HKEY_MAX_INDEX * sizeof(uint32_t);
	dev_info->reta_size = ETH_RSS_RETA_SIZE_128;
	dev_info->flow_type_rss_offloads = IGB_RSS_OFFLOAD_ALL;

	dev_info->default_rxconf = (struct rte_eth_rxconf) {
		.rx_thresh = {
			.pthresh = IGB_DEFAULT_RX_PTHRESH,
			.hthresh = IGB_DEFAULT_RX_HTHRESH,
			.wthresh = IGB_DEFAULT_RX_WTHRESH,
		},
		.rx_free_thresh = IGB_DEFAULT_RX_FREE_THRESH,
		.rx_drop_en = 0,
	};

	dev_info->default_txconf = (struct rte_eth_txconf) {
		.tx_thresh = {
			.pthresh = IGB_DEFAULT_TX_PTHRESH,
			.hthresh = IGB_DEFAULT_TX_HTHRESH,
			.wthresh = IGB_DEFAULT_TX_WTHRESH,
		},
		.txq_flags = 0,
	};

	dev_info->rx_desc_lim = rx_desc_lim;
	dev_info->tx_desc_lim = tx_desc_lim;
}

static void
eth_igbvf_infos_get(struct rte_eth_dev *dev, struct rte_eth_dev_info *dev_info)
{
	struct e1000_hw *hw = E1000_DEV_PRIVATE_TO_HW(dev->data->dev_private);

	dev_info->min_rx_bufsize = 256; /* See BSIZE field of RCTL register. */
	dev_info->max_rx_pktlen  = 0x3FFF; /* See RLPML register. */
	dev_info->max_mac_addrs = hw->mac.rar_entry_count;
	dev_info->rx_offload_capa = DEV_RX_OFFLOAD_VLAN_STRIP |
				DEV_RX_OFFLOAD_IPV4_CKSUM |
				DEV_RX_OFFLOAD_UDP_CKSUM  |
				DEV_RX_OFFLOAD_TCP_CKSUM;
	dev_info->tx_offload_capa = DEV_TX_OFFLOAD_VLAN_INSERT |
				DEV_TX_OFFLOAD_IPV4_CKSUM  |
				DEV_TX_OFFLOAD_UDP_CKSUM   |
				DEV_TX_OFFLOAD_TCP_CKSUM   |
				DEV_TX_OFFLOAD_SCTP_CKSUM  |
				DEV_TX_OFFLOAD_TCP_TSO;
	switch (hw->mac.type) {
	case e1000_vfadapt:
		dev_info->max_rx_queues = 2;
		dev_info->max_tx_queues = 2;
		break;
	case e1000_vfadapt_i350:
		dev_info->max_rx_queues = 1;
		dev_info->max_tx_queues = 1;
		break;
	default:
		/* Should not happen */
		break;
	}

	dev_info->default_rxconf = (struct rte_eth_rxconf) {
		.rx_thresh = {
			.pthresh = IGB_DEFAULT_RX_PTHRESH,
			.hthresh = IGB_DEFAULT_RX_HTHRESH,
			.wthresh = IGB_DEFAULT_RX_WTHRESH,
		},
		.rx_free_thresh = IGB_DEFAULT_RX_FREE_THRESH,
		.rx_drop_en = 0,
	};

	dev_info->default_txconf = (struct rte_eth_txconf) {
		.tx_thresh = {
			.pthresh = IGB_DEFAULT_TX_PTHRESH,
			.hthresh = IGB_DEFAULT_TX_HTHRESH,
			.wthresh = IGB_DEFAULT_TX_WTHRESH,
		},
		.txq_flags = 0,
	};

	dev_info->rx_desc_lim = rx_desc_lim;
	dev_info->tx_desc_lim = tx_desc_lim;
}

/* return 0 means link status changed, -1 means not changed */
static int
eth_igb_link_update(struct rte_eth_dev *dev, int wait_to_complete)
{
	struct e1000_hw *hw =
		E1000_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	struct rte_eth_link link, old;
	int link_check, count;

	link_check = 0;
	hw->mac.get_link_status = 1;

	/* possible wait-to-complete in up to 9 seconds */
	for (count = 0; count < IGB_LINK_UPDATE_CHECK_TIMEOUT; count ++) {
		/* Read the real link status */
		switch (hw->phy.media_type) {
		case e1000_media_type_copper:
			/* Do the work to read phy */
			e1000_check_for_link(hw);
			link_check = !hw->mac.get_link_status;
			break;

		case e1000_media_type_fiber:
			e1000_check_for_link(hw);
			link_check = (E1000_READ_REG(hw, E1000_STATUS) &
				      E1000_STATUS_LU);
			break;

		case e1000_media_type_internal_serdes:
			e1000_check_for_link(hw);
			link_check = hw->mac.serdes_has_link;
			break;

		/* VF device is type_unknown */
		case e1000_media_type_unknown:
			eth_igbvf_link_update(hw);
			link_check = !hw->mac.get_link_status;
			break;

		default:
			break;
		}
		if (link_check || wait_to_complete == 0)
			break;
		rte_delay_ms(IGB_LINK_UPDATE_CHECK_INTERVAL);
	}
	memset(&link, 0, sizeof(link));
	rte_igb_dev_atomic_read_link_status(dev, &link);
	old = link;

	/* Now we check if a transition has happened */
	if (link_check) {
		hw->mac.ops.get_link_up_info(hw, &link.link_speed,
					  &link.link_duplex);
		link.link_status = 1;
	} else if (!link_check) {
		link.link_speed = 0;
		link.link_duplex = 0;
		link.link_status = 0;
	}
	rte_igb_dev_atomic_write_link_status(dev, &link);

	/* not changed */
	if (old.link_status == link.link_status)
		return -1;

	/* changed */
	return 0;
}

/*
 * igb_hw_control_acquire sets CTRL_EXT:DRV_LOAD bit.
 * For ASF and Pass Through versions of f/w this means
 * that the driver is loaded.
 */
static void
igb_hw_control_acquire(struct e1000_hw *hw)
{
	uint32_t ctrl_ext;

	/* Let firmware know the driver has taken over */
	ctrl_ext = E1000_READ_REG(hw, E1000_CTRL_EXT);
	E1000_WRITE_REG(hw, E1000_CTRL_EXT, ctrl_ext | E1000_CTRL_EXT_DRV_LOAD);
}

/*
 * igb_hw_control_release resets CTRL_EXT:DRV_LOAD bit.
 * For ASF and Pass Through versions of f/w this means that the
 * driver is no longer loaded.
 */
static void
igb_hw_control_release(struct e1000_hw *hw)
{
	uint32_t ctrl_ext;

	/* Let firmware taken over control of h/w */
	ctrl_ext = E1000_READ_REG(hw, E1000_CTRL_EXT);
	E1000_WRITE_REG(hw, E1000_CTRL_EXT,
			ctrl_ext & ~E1000_CTRL_EXT_DRV_LOAD);
}

/*
 * Bit of a misnomer, what this really means is
 * to enable OS management of the system... aka
 * to disable special hardware management features.
 */
static void
igb_init_manageability(struct e1000_hw *hw)
{
	if (e1000_enable_mng_pass_thru(hw)) {
		uint32_t manc2h = E1000_READ_REG(hw, E1000_MANC2H);
		uint32_t manc = E1000_READ_REG(hw, E1000_MANC);

		/* disable hardware interception of ARP */
		manc &= ~(E1000_MANC_ARP_EN);

		/* enable receiving management packets to the host */
		manc |= E1000_MANC_EN_MNG2HOST;
		manc2h |= 1 << 5;  /* Mng Port 623 */
		manc2h |= 1 << 6;  /* Mng Port 664 */
		E1000_WRITE_REG(hw, E1000_MANC2H, manc2h);
		E1000_WRITE_REG(hw, E1000_MANC, manc);
	}
}

static void
igb_release_manageability(struct e1000_hw *hw)
{
	if (e1000_enable_mng_pass_thru(hw)) {
		uint32_t manc = E1000_READ_REG(hw, E1000_MANC);

		manc |= E1000_MANC_ARP_EN;
		manc &= ~E1000_MANC_EN_MNG2HOST;

		E1000_WRITE_REG(hw, E1000_MANC, manc);
	}
}

static void
eth_igb_promiscuous_enable(struct rte_eth_dev *dev)
{
	struct e1000_hw *hw =
		E1000_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	uint32_t rctl;

	rctl = E1000_READ_REG(hw, E1000_RCTL);
	rctl |= (E1000_RCTL_UPE | E1000_RCTL_MPE);
	E1000_WRITE_REG(hw, E1000_RCTL, rctl);
}

static void
eth_igb_promiscuous_disable(struct rte_eth_dev *dev)
{
	struct e1000_hw *hw =
		E1000_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	uint32_t rctl;

	rctl = E1000_READ_REG(hw, E1000_RCTL);
	rctl &= (~E1000_RCTL_UPE);
	if (dev->data->all_multicast == 1)
		rctl |= E1000_RCTL_MPE;
	else
		rctl &= (~E1000_RCTL_MPE);
	E1000_WRITE_REG(hw, E1000_RCTL, rctl);
}

static void
eth_igb_allmulticast_enable(struct rte_eth_dev *dev)
{
	struct e1000_hw *hw =
		E1000_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	uint32_t rctl;

	rctl = E1000_READ_REG(hw, E1000_RCTL);
	rctl |= E1000_RCTL_MPE;
	E1000_WRITE_REG(hw, E1000_RCTL, rctl);
}

static void
eth_igb_allmulticast_disable(struct rte_eth_dev *dev)
{
	struct e1000_hw *hw =
		E1000_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	uint32_t rctl;

	if (dev->data->promiscuous == 1)
		return; /* must remain in all_multicast mode */
	rctl = E1000_READ_REG(hw, E1000_RCTL);
	rctl &= (~E1000_RCTL_MPE);
	E1000_WRITE_REG(hw, E1000_RCTL, rctl);
}

static int
eth_igb_vlan_filter_set(struct rte_eth_dev *dev, uint16_t vlan_id, int on)
{
	struct e1000_hw *hw =
		E1000_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	struct e1000_vfta * shadow_vfta =
		E1000_DEV_PRIVATE_TO_VFTA(dev->data->dev_private);
	uint32_t vfta;
	uint32_t vid_idx;
	uint32_t vid_bit;

	vid_idx = (uint32_t) ((vlan_id >> E1000_VFTA_ENTRY_SHIFT) &
			      E1000_VFTA_ENTRY_MASK);
	vid_bit = (uint32_t) (1 << (vlan_id & E1000_VFTA_ENTRY_BIT_SHIFT_MASK));
	vfta = E1000_READ_REG_ARRAY(hw, E1000_VFTA, vid_idx);
	if (on)
		vfta |= vid_bit;
	else
		vfta &= ~vid_bit;
	E1000_WRITE_REG_ARRAY(hw, E1000_VFTA, vid_idx, vfta);

	/* update local VFTA copy */
	shadow_vfta->vfta[vid_idx] = vfta;

	return 0;
}

static void
eth_igb_vlan_tpid_set(struct rte_eth_dev *dev, uint16_t tpid)
{
	struct e1000_hw *hw =
		E1000_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	uint32_t reg = ETHER_TYPE_VLAN ;

	reg |= (tpid << 16);
	E1000_WRITE_REG(hw, E1000_VET, reg);
}

static void
igb_vlan_hw_filter_disable(struct rte_eth_dev *dev)
{
	struct e1000_hw *hw =
		E1000_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	uint32_t reg;

	/* Filter Table Disable */
	reg = E1000_READ_REG(hw, E1000_RCTL);
	reg &= ~E1000_RCTL_CFIEN;
	reg &= ~E1000_RCTL_VFE;
	E1000_WRITE_REG(hw, E1000_RCTL, reg);
}

static void
igb_vlan_hw_filter_enable(struct rte_eth_dev *dev)
{
	struct e1000_hw *hw =
		E1000_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	struct e1000_vfta * shadow_vfta =
		E1000_DEV_PRIVATE_TO_VFTA(dev->data->dev_private);
	uint32_t reg;
	int i;

	/* Filter Table Enable, CFI not used for packet acceptance */
	reg = E1000_READ_REG(hw, E1000_RCTL);
	reg &= ~E1000_RCTL_CFIEN;
	reg |= E1000_RCTL_VFE;
	E1000_WRITE_REG(hw, E1000_RCTL, reg);

	/* restore VFTA table */
	for (i = 0; i < IGB_VFTA_SIZE; i++)
		E1000_WRITE_REG_ARRAY(hw, E1000_VFTA, i, shadow_vfta->vfta[i]);
}

static void
igb_vlan_hw_strip_disable(struct rte_eth_dev *dev)
{
	struct e1000_hw *hw =
		E1000_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	uint32_t reg;

	/* VLAN Mode Disable */
	reg = E1000_READ_REG(hw, E1000_CTRL);
	reg &= ~E1000_CTRL_VME;
	E1000_WRITE_REG(hw, E1000_CTRL, reg);
}

static void
igb_vlan_hw_strip_enable(struct rte_eth_dev *dev)
{
	struct e1000_hw *hw =
		E1000_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	uint32_t reg;

	/* VLAN Mode Enable */
	reg = E1000_READ_REG(hw, E1000_CTRL);
	reg |= E1000_CTRL_VME;
	E1000_WRITE_REG(hw, E1000_CTRL, reg);
}

static void
igb_vlan_hw_extend_disable(struct rte_eth_dev *dev)
{
	struct e1000_hw *hw =
		E1000_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	uint32_t reg;

	/* CTRL_EXT: Extended VLAN */
	reg = E1000_READ_REG(hw, E1000_CTRL_EXT);
	reg &= ~E1000_CTRL_EXT_EXTEND_VLAN;
	E1000_WRITE_REG(hw, E1000_CTRL_EXT, reg);

	/* Update maximum packet length */
	if (dev->data->dev_conf.rxmode.jumbo_frame == 1)
		E1000_WRITE_REG(hw, E1000_RLPML,
			dev->data->dev_conf.rxmode.max_rx_pkt_len +
						VLAN_TAG_SIZE);
}

static void
igb_vlan_hw_extend_enable(struct rte_eth_dev *dev)
{
	struct e1000_hw *hw =
		E1000_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	uint32_t reg;

	/* CTRL_EXT: Extended VLAN */
	reg = E1000_READ_REG(hw, E1000_CTRL_EXT);
	reg |= E1000_CTRL_EXT_EXTEND_VLAN;
	E1000_WRITE_REG(hw, E1000_CTRL_EXT, reg);

	/* Update maximum packet length */
	if (dev->data->dev_conf.rxmode.jumbo_frame == 1)
		E1000_WRITE_REG(hw, E1000_RLPML,
			dev->data->dev_conf.rxmode.max_rx_pkt_len +
						2 * VLAN_TAG_SIZE);
}

static void
eth_igb_vlan_offload_set(struct rte_eth_dev *dev, int mask)
{
	if(mask & ETH_VLAN_STRIP_MASK){
		if (dev->data->dev_conf.rxmode.hw_vlan_strip)
			igb_vlan_hw_strip_enable(dev);
		else
			igb_vlan_hw_strip_disable(dev);
	}

	if(mask & ETH_VLAN_FILTER_MASK){
		if (dev->data->dev_conf.rxmode.hw_vlan_filter)
			igb_vlan_hw_filter_enable(dev);
		else
			igb_vlan_hw_filter_disable(dev);
	}

	if(mask & ETH_VLAN_EXTEND_MASK){
		if (dev->data->dev_conf.rxmode.hw_vlan_extend)
			igb_vlan_hw_extend_enable(dev);
		else
			igb_vlan_hw_extend_disable(dev);
	}
}


/**
 * It enables the interrupt mask and then enable the interrupt.
 *
 * @param dev
 *  Pointer to struct rte_eth_dev.
 *
 * @return
 *  - On success, zero.
 *  - On failure, a negative value.
 */
static int
eth_igb_lsc_interrupt_setup(struct rte_eth_dev *dev)
{
	struct e1000_interrupt *intr =
		E1000_DEV_PRIVATE_TO_INTR(dev->data->dev_private);

	intr->mask |= E1000_ICR_LSC;

	return 0;
}

/* It clears the interrupt causes and enables the interrupt.
 * It will be called once only during nic initialized.
 *
 * @param dev
 *  Pointer to struct rte_eth_dev.
 *
 * @return
 *  - On success, zero.
 *  - On failure, a negative value.
 */
static int eth_igb_rxq_interrupt_setup(struct rte_eth_dev *dev)
{
	uint32_t mask, regval;
	struct e1000_hw *hw =
		E1000_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	struct rte_eth_dev_info dev_info;

	memset(&dev_info, 0, sizeof(dev_info));
	eth_igb_infos_get(dev, &dev_info);

	mask = 0xFFFFFFFF >> (32 - dev_info.max_rx_queues);
	regval = E1000_READ_REG(hw, E1000_EIMS);
	E1000_WRITE_REG(hw, E1000_EIMS, regval | mask);

	return 0;
}

/*
 * It reads ICR and gets interrupt causes, check it and set a bit flag
 * to update link status.
 *
 * @param dev
 *  Pointer to struct rte_eth_dev.
 *
 * @return
 *  - On success, zero.
 *  - On failure, a negative value.
 */
static int
eth_igb_interrupt_get_status(struct rte_eth_dev *dev)
{
	uint32_t icr;
	struct e1000_hw *hw =
		E1000_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	struct e1000_interrupt *intr =
		E1000_DEV_PRIVATE_TO_INTR(dev->data->dev_private);

	igb_intr_disable(hw);

	/* read-on-clear nic registers here */
	icr = E1000_READ_REG(hw, E1000_ICR);

	intr->flags = 0;
	if (icr & E1000_ICR_LSC) {
		intr->flags |= E1000_FLAG_NEED_LINK_UPDATE;
	}

	if (icr & E1000_ICR_VMMB)
		intr->flags |= E1000_FLAG_MAILBOX;

	return 0;
}

/*
 * It executes link_update after knowing an interrupt is prsent.
 *
 * @param dev
 *  Pointer to struct rte_eth_dev.
 *
 * @return
 *  - On success, zero.
 *  - On failure, a negative value.
 */
static int
eth_igb_interrupt_action(struct rte_eth_dev *dev)
{
	struct e1000_hw *hw =
		E1000_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	struct e1000_interrupt *intr =
		E1000_DEV_PRIVATE_TO_INTR(dev->data->dev_private);
	uint32_t tctl, rctl;
	struct rte_eth_link link;
	int ret;

	if (intr->flags & E1000_FLAG_MAILBOX) {
		igb_pf_mbx_process(dev);
		intr->flags &= ~E1000_FLAG_MAILBOX;
	}

	igb_intr_enable(dev);
	rte_intr_enable(&(dev->pci_dev->intr_handle));

	if (intr->flags & E1000_FLAG_NEED_LINK_UPDATE) {
		intr->flags &= ~E1000_FLAG_NEED_LINK_UPDATE;

		/* set get_link_status to check register later */
		hw->mac.get_link_status = 1;
		ret = eth_igb_link_update(dev, 0);

		/* check if link has changed */
		if (ret < 0)
			return 0;

		memset(&link, 0, sizeof(link));
		rte_igb_dev_atomic_read_link_status(dev, &link);
		if (link.link_status) {
			PMD_INIT_LOG(INFO,
				     " Port %d: Link Up - speed %u Mbps - %s",
				     dev->data->port_id,
				     (unsigned)link.link_speed,
				     link.link_duplex == ETH_LINK_FULL_DUPLEX ?
				     "full-duplex" : "half-duplex");
		} else {
			PMD_INIT_LOG(INFO, " Port %d: Link Down",
				     dev->data->port_id);
		}

		PMD_INIT_LOG(DEBUG, "PCI Address: %04d:%02d:%02d:%d",
			     dev->pci_dev->addr.domain,
			     dev->pci_dev->addr.bus,
			     dev->pci_dev->addr.devid,
			     dev->pci_dev->addr.function);
		tctl = E1000_READ_REG(hw, E1000_TCTL);
		rctl = E1000_READ_REG(hw, E1000_RCTL);
		if (link.link_status) {
			/* enable Tx/Rx */
			tctl |= E1000_TCTL_EN;
			rctl |= E1000_RCTL_EN;
		} else {
			/* disable Tx/Rx */
			tctl &= ~E1000_TCTL_EN;
			rctl &= ~E1000_RCTL_EN;
		}
		E1000_WRITE_REG(hw, E1000_TCTL, tctl);
		E1000_WRITE_REG(hw, E1000_RCTL, rctl);
		E1000_WRITE_FLUSH(hw);
		_rte_eth_dev_callback_process(dev, RTE_ETH_EVENT_INTR_LSC);
	}

	return 0;
}

/**
 * Interrupt handler which shall be registered at first.
 *
 * @param handle
 *  Pointer to interrupt handle.
 * @param param
 *  The address of parameter (struct rte_eth_dev *) regsitered before.
 *
 * @return
 *  void
 */
static void
eth_igb_interrupt_handler(__rte_unused struct rte_intr_handle *handle,
							void *param)
{
	struct rte_eth_dev *dev = (struct rte_eth_dev *)param;

	eth_igb_interrupt_get_status(dev);
	eth_igb_interrupt_action(dev);
}

static int
eth_igb_led_on(struct rte_eth_dev *dev)
{
	struct e1000_hw *hw;

	hw = E1000_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	return (e1000_led_on(hw) == E1000_SUCCESS ? 0 : -ENOTSUP);
}

static int
eth_igb_led_off(struct rte_eth_dev *dev)
{
	struct e1000_hw *hw;

	hw = E1000_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	return (e1000_led_off(hw) == E1000_SUCCESS ? 0 : -ENOTSUP);
}

static int
eth_igb_flow_ctrl_get(struct rte_eth_dev *dev, struct rte_eth_fc_conf *fc_conf)
{
	struct e1000_hw *hw;
	uint32_t ctrl;
	int tx_pause;
	int rx_pause;

	hw = E1000_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	fc_conf->pause_time = hw->fc.pause_time;
	fc_conf->high_water = hw->fc.high_water;
	fc_conf->low_water = hw->fc.low_water;
	fc_conf->send_xon = hw->fc.send_xon;
	fc_conf->autoneg = hw->mac.autoneg;

	/*
	 * Return rx_pause and tx_pause status according to actual setting of
	 * the TFCE and RFCE bits in the CTRL register.
	 */
	ctrl = E1000_READ_REG(hw, E1000_CTRL);
	if (ctrl & E1000_CTRL_TFCE)
		tx_pause = 1;
	else
		tx_pause = 0;

	if (ctrl & E1000_CTRL_RFCE)
		rx_pause = 1;
	else
		rx_pause = 0;

	if (rx_pause && tx_pause)
		fc_conf->mode = RTE_FC_FULL;
	else if (rx_pause)
		fc_conf->mode = RTE_FC_RX_PAUSE;
	else if (tx_pause)
		fc_conf->mode = RTE_FC_TX_PAUSE;
	else
		fc_conf->mode = RTE_FC_NONE;

	return 0;
}

static int
eth_igb_flow_ctrl_set(struct rte_eth_dev *dev, struct rte_eth_fc_conf *fc_conf)
{
	struct e1000_hw *hw;
	int err;
	enum e1000_fc_mode rte_fcmode_2_e1000_fcmode[] = {
		e1000_fc_none,
		e1000_fc_rx_pause,
		e1000_fc_tx_pause,
		e1000_fc_full
	};
	uint32_t rx_buf_size;
	uint32_t max_high_water;
	uint32_t rctl;

	hw = E1000_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	if (fc_conf->autoneg != hw->mac.autoneg)
		return -ENOTSUP;
	rx_buf_size = igb_get_rx_buffer_size(hw);
	PMD_INIT_LOG(DEBUG, "Rx packet buffer size = 0x%x", rx_buf_size);

	/* At least reserve one Ethernet frame for watermark */
	max_high_water = rx_buf_size - ETHER_MAX_LEN;
	if ((fc_conf->high_water > max_high_water) ||
	    (fc_conf->high_water < fc_conf->low_water)) {
		PMD_INIT_LOG(ERR, "e1000 incorrect high/low water value");
		PMD_INIT_LOG(ERR, "high water must <=  0x%x", max_high_water);
		return (-EINVAL);
	}

	hw->fc.requested_mode = rte_fcmode_2_e1000_fcmode[fc_conf->mode];
	hw->fc.pause_time     = fc_conf->pause_time;
	hw->fc.high_water     = fc_conf->high_water;
	hw->fc.low_water      = fc_conf->low_water;
	hw->fc.send_xon	      = fc_conf->send_xon;

	err = e1000_setup_link_generic(hw);
	if (err == E1000_SUCCESS) {

		/* check if we want to forward MAC frames - driver doesn't have native
		 * capability to do that, so we'll write the registers ourselves */

		rctl = E1000_READ_REG(hw, E1000_RCTL);

		/* set or clear MFLCN.PMCF bit depending on configuration */
		if (fc_conf->mac_ctrl_frame_fwd != 0)
			rctl |= E1000_RCTL_PMCF;
		else
			rctl &= ~E1000_RCTL_PMCF;

		E1000_WRITE_REG(hw, E1000_RCTL, rctl);
		E1000_WRITE_FLUSH(hw);

		return 0;
	}

	PMD_INIT_LOG(ERR, "e1000_setup_link_generic = 0x%x", err);
	return (-EIO);
}

#define E1000_RAH_POOLSEL_SHIFT      (18)
static void
eth_igb_rar_set(struct rte_eth_dev *dev, struct ether_addr *mac_addr,
	        uint32_t index, __rte_unused uint32_t pool)
{
	struct e1000_hw *hw = E1000_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	uint32_t rah;

	e1000_rar_set(hw, mac_addr->addr_bytes, index);
	rah = E1000_READ_REG(hw, E1000_RAH(index));
	rah |= (0x1 << (E1000_RAH_POOLSEL_SHIFT + pool));
	E1000_WRITE_REG(hw, E1000_RAH(index), rah);
}

static void
eth_igb_rar_clear(struct rte_eth_dev *dev, uint32_t index)
{
	uint8_t addr[ETHER_ADDR_LEN];
	struct e1000_hw *hw = E1000_DEV_PRIVATE_TO_HW(dev->data->dev_private);

	memset(addr, 0, sizeof(addr));

	e1000_rar_set(hw, addr, index);
}

static void
eth_igb_default_mac_addr_set(struct rte_eth_dev *dev,
				struct ether_addr *addr)
{
	eth_igb_rar_clear(dev, 0);

	eth_igb_rar_set(dev, (void *)addr, 0, 0);
}
/*
 * Virtual Function operations
 */
static void
igbvf_intr_disable(struct e1000_hw *hw)
{
	PMD_INIT_FUNC_TRACE();

	/* Clear interrupt mask to stop from interrupts being generated */
	E1000_WRITE_REG(hw, E1000_EIMC, 0xFFFF);

	E1000_WRITE_FLUSH(hw);
}

static void
igbvf_stop_adapter(struct rte_eth_dev *dev)
{
	u32 reg_val;
	u16 i;
	struct rte_eth_dev_info dev_info;
	struct e1000_hw *hw = E1000_DEV_PRIVATE_TO_HW(dev->data->dev_private);

	memset(&dev_info, 0, sizeof(dev_info));
	eth_igbvf_infos_get(dev, &dev_info);

	/* Clear interrupt mask to stop from interrupts being generated */
	igbvf_intr_disable(hw);

	/* Clear any pending interrupts, flush previous writes */
	E1000_READ_REG(hw, E1000_EICR);

	/* Disable the transmit unit.  Each queue must be disabled. */
	for (i = 0; i < dev_info.max_tx_queues; i++)
		E1000_WRITE_REG(hw, E1000_TXDCTL(i), E1000_TXDCTL_SWFLSH);

	/* Disable the receive unit by stopping each queue */
	for (i = 0; i < dev_info.max_rx_queues; i++) {
		reg_val = E1000_READ_REG(hw, E1000_RXDCTL(i));
		reg_val &= ~E1000_RXDCTL_QUEUE_ENABLE;
		E1000_WRITE_REG(hw, E1000_RXDCTL(i), reg_val);
		while (E1000_READ_REG(hw, E1000_RXDCTL(i)) & E1000_RXDCTL_QUEUE_ENABLE)
			;
	}

	/* flush all queues disables */
	E1000_WRITE_FLUSH(hw);
	msec_delay(2);
}

static int eth_igbvf_link_update(struct e1000_hw *hw)
{
	struct e1000_mbx_info *mbx = &hw->mbx;
	struct e1000_mac_info *mac = &hw->mac;
	int ret_val = E1000_SUCCESS;

	PMD_INIT_LOG(DEBUG, "e1000_check_for_link_vf");

	/*
	 * We only want to run this if there has been a rst asserted.
	 * in this case that could mean a link change, device reset,
	 * or a virtual function reset
	 */

	/* If we were hit with a reset or timeout drop the link */
	if (!e1000_check_for_rst(hw, 0) || !mbx->timeout)
		mac->get_link_status = TRUE;

	if (!mac->get_link_status)
		goto out;

	/* if link status is down no point in checking to see if pf is up */
	if (!(E1000_READ_REG(hw, E1000_STATUS) & E1000_STATUS_LU))
		goto out;

	/* if we passed all the tests above then the link is up and we no
	 * longer need to check for link */
	mac->get_link_status = FALSE;

out:
	return ret_val;
}


static int
igbvf_dev_configure(struct rte_eth_dev *dev)
{
	struct rte_eth_conf* conf = &dev->data->dev_conf;

	PMD_INIT_LOG(DEBUG, "Configured Virtual Function port id: %d",
		     dev->data->port_id);

	/*
	 * VF has no ability to enable/disable HW CRC
	 * Keep the persistent behavior the same as Host PF
	 */
#ifndef RTE_LIBRTE_E1000_PF_DISABLE_STRIP_CRC
	if (!conf->rxmode.hw_strip_crc) {
		PMD_INIT_LOG(NOTICE, "VF can't disable HW CRC Strip");
		conf->rxmode.hw_strip_crc = 1;
	}
#else
	if (conf->rxmode.hw_strip_crc) {
		PMD_INIT_LOG(NOTICE, "VF can't enable HW CRC Strip");
		conf->rxmode.hw_strip_crc = 0;
	}
#endif

	return 0;
}

static int
igbvf_dev_start(struct rte_eth_dev *dev)
{
	struct e1000_hw *hw =
		E1000_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	struct e1000_adapter *adapter =
		E1000_DEV_PRIVATE(dev->data->dev_private);
	int ret;

	PMD_INIT_FUNC_TRACE();

	hw->mac.ops.reset_hw(hw);
	adapter->stopped = 0;

	/* Set all vfta */
	igbvf_set_vfta_all(dev,1);

	eth_igbvf_tx_init(dev);

	/* This can fail when allocating mbufs for descriptor rings */
	ret = eth_igbvf_rx_init(dev);
	if (ret) {
		PMD_INIT_LOG(ERR, "Unable to initialize RX hardware");
		igb_dev_clear_queues(dev);
		return ret;
	}

	return 0;
}

static void
igbvf_dev_stop(struct rte_eth_dev *dev)
{
	PMD_INIT_FUNC_TRACE();

	igbvf_stop_adapter(dev);

	/*
	  * Clear what we set, but we still keep shadow_vfta to
	  * restore after device starts
	  */
	igbvf_set_vfta_all(dev,0);

	igb_dev_clear_queues(dev);
}

static void
igbvf_dev_close(struct rte_eth_dev *dev)
{
	struct e1000_hw *hw = E1000_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	struct e1000_adapter *adapter =
		E1000_DEV_PRIVATE(dev->data->dev_private);

	PMD_INIT_FUNC_TRACE();

	e1000_reset_hw(hw);

	igbvf_dev_stop(dev);
	adapter->stopped = 1;
	igb_dev_free_queues(dev);
}

static int igbvf_set_vfta(struct e1000_hw *hw, uint16_t vid, bool on)
{
	struct e1000_mbx_info *mbx = &hw->mbx;
	uint32_t msgbuf[2];

	/* After set vlan, vlan strip will also be enabled in igb driver*/
	msgbuf[0] = E1000_VF_SET_VLAN;
	msgbuf[1] = vid;
	/* Setting the 8 bit field MSG INFO to TRUE indicates "add" */
	if (on)
		msgbuf[0] |= E1000_VF_SET_VLAN_ADD;

	return (mbx->ops.write_posted(hw, msgbuf, 2, 0));
}

static void igbvf_set_vfta_all(struct rte_eth_dev *dev, bool on)
{
	struct e1000_hw *hw =
		E1000_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	struct e1000_vfta * shadow_vfta =
		E1000_DEV_PRIVATE_TO_VFTA(dev->data->dev_private);
	int i = 0, j = 0, vfta = 0, mask = 1;

	for (i = 0; i < IGB_VFTA_SIZE; i++){
		vfta = shadow_vfta->vfta[i];
		if(vfta){
			mask = 1;
			for (j = 0; j < 32; j++){
				if(vfta & mask)
					igbvf_set_vfta(hw,
						(uint16_t)((i<<5)+j), on);
				mask<<=1;
			}
		}
	}

}

static int
igbvf_vlan_filter_set(struct rte_eth_dev *dev, uint16_t vlan_id, int on)
{
	struct e1000_hw *hw =
		E1000_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	struct e1000_vfta * shadow_vfta =
		E1000_DEV_PRIVATE_TO_VFTA(dev->data->dev_private);
	uint32_t vid_idx = 0;
	uint32_t vid_bit = 0;
	int ret = 0;

	PMD_INIT_FUNC_TRACE();

	/*vind is not used in VF driver, set to 0, check ixgbe_set_vfta_vf*/
	ret = igbvf_set_vfta(hw, vlan_id, !!on);
	if(ret){
		PMD_INIT_LOG(ERR, "Unable to set VF vlan");
		return ret;
	}
	vid_idx = (uint32_t) ((vlan_id >> 5) & 0x7F);
	vid_bit = (uint32_t) (1 << (vlan_id & 0x1F));

	/*Save what we set and retore it after device reset*/
	if (on)
		shadow_vfta->vfta[vid_idx] |= vid_bit;
	else
		shadow_vfta->vfta[vid_idx] &= ~vid_bit;

	return 0;
}

static void
igbvf_default_mac_addr_set(struct rte_eth_dev *dev, struct ether_addr *addr)
{
	struct e1000_hw *hw =
		E1000_DEV_PRIVATE_TO_HW(dev->data->dev_private);

	/* index is not used by rar_set() */
	hw->mac.ops.rar_set(hw, (void *)addr, 0);
}


static int
eth_igb_rss_reta_update(struct rte_eth_dev *dev,
			struct rte_eth_rss_reta_entry64 *reta_conf,
			uint16_t reta_size)
{
	uint8_t i, j, mask;
	uint32_t reta, r;
	uint16_t idx, shift;
	struct e1000_hw *hw = E1000_DEV_PRIVATE_TO_HW(dev->data->dev_private);

	if (reta_size != ETH_RSS_RETA_SIZE_128) {
		PMD_DRV_LOG(ERR, "The size of hash lookup table configured "
			"(%d) doesn't match the number hardware can supported "
			"(%d)\n", reta_size, ETH_RSS_RETA_SIZE_128);
		return -EINVAL;
	}

	for (i = 0; i < reta_size; i += IGB_4_BIT_WIDTH) {
		idx = i / RTE_RETA_GROUP_SIZE;
		shift = i % RTE_RETA_GROUP_SIZE;
		mask = (uint8_t)((reta_conf[idx].mask >> shift) &
						IGB_4_BIT_MASK);
		if (!mask)
			continue;
		if (mask == IGB_4_BIT_MASK)
			r = 0;
		else
			r = E1000_READ_REG(hw, E1000_RETA(i >> 2));
		for (j = 0, reta = 0; j < IGB_4_BIT_WIDTH; j++) {
			if (mask & (0x1 << j))
				reta |= reta_conf[idx].reta[shift + j] <<
							(CHAR_BIT * j);
			else
				reta |= r & (IGB_8_BIT_MASK << (CHAR_BIT * j));
		}
		E1000_WRITE_REG(hw, E1000_RETA(i >> 2), reta);
	}

	return 0;
}

static int
eth_igb_rss_reta_query(struct rte_eth_dev *dev,
		       struct rte_eth_rss_reta_entry64 *reta_conf,
		       uint16_t reta_size)
{
	uint8_t i, j, mask;
	uint32_t reta;
	uint16_t idx, shift;
	struct e1000_hw *hw = E1000_DEV_PRIVATE_TO_HW(dev->data->dev_private);

	if (reta_size != ETH_RSS_RETA_SIZE_128) {
		PMD_DRV_LOG(ERR, "The size of hash lookup table configured "
			"(%d) doesn't match the number hardware can supported "
			"(%d)\n", reta_size, ETH_RSS_RETA_SIZE_128);
		return -EINVAL;
	}

	for (i = 0; i < reta_size; i += IGB_4_BIT_WIDTH) {
		idx = i / RTE_RETA_GROUP_SIZE;
		shift = i % RTE_RETA_GROUP_SIZE;
		mask = (uint8_t)((reta_conf[idx].mask >> shift) &
						IGB_4_BIT_MASK);
		if (!mask)
			continue;
		reta = E1000_READ_REG(hw, E1000_RETA(i >> 2));
		for (j = 0; j < IGB_4_BIT_WIDTH; j++) {
			if (mask & (0x1 << j))
				reta_conf[idx].reta[shift + j] =
					((reta >> (CHAR_BIT * j)) &
						IGB_8_BIT_MASK);
		}
	}

	return 0;
}

#define MAC_TYPE_FILTER_SUP(type)    do {\
	if ((type) != e1000_82580 && (type) != e1000_i350 &&\
		(type) != e1000_82576)\
		return -ENOTSUP;\
} while (0)

static int
eth_igb_syn_filter_set(struct rte_eth_dev *dev,
			struct rte_eth_syn_filter *filter,
			bool add)
{
	struct e1000_hw *hw = E1000_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	uint32_t synqf, rfctl;

	if (filter->queue >= IGB_MAX_RX_QUEUE_NUM)
		return -EINVAL;

	synqf = E1000_READ_REG(hw, E1000_SYNQF(0));

	if (add) {
		if (synqf & E1000_SYN_FILTER_ENABLE)
			return -EINVAL;

		synqf = (uint32_t)(((filter->queue << E1000_SYN_FILTER_QUEUE_SHIFT) &
			E1000_SYN_FILTER_QUEUE) | E1000_SYN_FILTER_ENABLE);

		rfctl = E1000_READ_REG(hw, E1000_RFCTL);
		if (filter->hig_pri)
			rfctl |= E1000_RFCTL_SYNQFP;
		else
			rfctl &= ~E1000_RFCTL_SYNQFP;

		E1000_WRITE_REG(hw, E1000_RFCTL, rfctl);
	} else {
		if (!(synqf & E1000_SYN_FILTER_ENABLE))
			return -ENOENT;
		synqf = 0;
	}

	E1000_WRITE_REG(hw, E1000_SYNQF(0), synqf);
	E1000_WRITE_FLUSH(hw);
	return 0;
}

static int
eth_igb_syn_filter_get(struct rte_eth_dev *dev,
			struct rte_eth_syn_filter *filter)
{
	struct e1000_hw *hw = E1000_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	uint32_t synqf, rfctl;

	synqf = E1000_READ_REG(hw, E1000_SYNQF(0));
	if (synqf & E1000_SYN_FILTER_ENABLE) {
		rfctl = E1000_READ_REG(hw, E1000_RFCTL);
		filter->hig_pri = (rfctl & E1000_RFCTL_SYNQFP) ? 1 : 0;
		filter->queue = (uint8_t)((synqf & E1000_SYN_FILTER_QUEUE) >>
				E1000_SYN_FILTER_QUEUE_SHIFT);
		return 0;
	}

	return -ENOENT;
}

static int
eth_igb_syn_filter_handle(struct rte_eth_dev *dev,
			enum rte_filter_op filter_op,
			void *arg)
{
	struct e1000_hw *hw = E1000_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	int ret;

	MAC_TYPE_FILTER_SUP(hw->mac.type);

	if (filter_op == RTE_ETH_FILTER_NOP)
		return 0;

	if (arg == NULL) {
		PMD_DRV_LOG(ERR, "arg shouldn't be NULL for operation %u",
			    filter_op);
		return -EINVAL;
	}

	switch (filter_op) {
	case RTE_ETH_FILTER_ADD:
		ret = eth_igb_syn_filter_set(dev,
				(struct rte_eth_syn_filter *)arg,
				TRUE);
		break;
	case RTE_ETH_FILTER_DELETE:
		ret = eth_igb_syn_filter_set(dev,
				(struct rte_eth_syn_filter *)arg,
				FALSE);
		break;
	case RTE_ETH_FILTER_GET:
		ret = eth_igb_syn_filter_get(dev,
				(struct rte_eth_syn_filter *)arg);
		break;
	default:
		PMD_DRV_LOG(ERR, "unsupported operation %u\n", filter_op);
		ret = -EINVAL;
		break;
	}

	return ret;
}

#define MAC_TYPE_FILTER_SUP_EXT(type)    do {\
	if ((type) != e1000_82580 && (type) != e1000_i350)\
		return -ENOSYS; \
} while (0)

/* translate elements in struct rte_eth_ntuple_filter to struct e1000_2tuple_filter_info*/
static inline int
ntuple_filter_to_2tuple(struct rte_eth_ntuple_filter *filter,
			struct e1000_2tuple_filter_info *filter_info)
{
	if (filter->queue >= IGB_MAX_RX_QUEUE_NUM)
		return -EINVAL;
	if (filter->priority > E1000_2TUPLE_MAX_PRI)
		return -EINVAL;  /* filter index is out of range. */
	if (filter->tcp_flags > TCP_FLAG_ALL)
		return -EINVAL;  /* flags is invalid. */

	switch (filter->dst_port_mask) {
	case UINT16_MAX:
		filter_info->dst_port_mask = 0;
		filter_info->dst_port = filter->dst_port;
		break;
	case 0:
		filter_info->dst_port_mask = 1;
		break;
	default:
		PMD_DRV_LOG(ERR, "invalid dst_port mask.");
		return -EINVAL;
	}

	switch (filter->proto_mask) {
	case UINT8_MAX:
		filter_info->proto_mask = 0;
		filter_info->proto = filter->proto;
		break;
	case 0:
		filter_info->proto_mask = 1;
		break;
	default:
		PMD_DRV_LOG(ERR, "invalid protocol mask.");
		return -EINVAL;
	}

	filter_info->priority = (uint8_t)filter->priority;
	if (filter->flags & RTE_NTUPLE_FLAGS_TCP_FLAG)
		filter_info->tcp_flags = filter->tcp_flags;
	else
		filter_info->tcp_flags = 0;

	return 0;
}

static inline struct e1000_2tuple_filter *
igb_2tuple_filter_lookup(struct e1000_2tuple_filter_list *filter_list,
			struct e1000_2tuple_filter_info *key)
{
	struct e1000_2tuple_filter *it;

	TAILQ_FOREACH(it, filter_list, entries) {
		if (memcmp(key, &it->filter_info,
			sizeof(struct e1000_2tuple_filter_info)) == 0) {
			return it;
		}
	}
	return NULL;
}

/*
 * igb_add_2tuple_filter - add a 2tuple filter
 *
 * @param
 * dev: Pointer to struct rte_eth_dev.
 * ntuple_filter: ponter to the filter that will be added.
 *
 * @return
 *    - On success, zero.
 *    - On failure, a negative value.
 */
static int
igb_add_2tuple_filter(struct rte_eth_dev *dev,
			struct rte_eth_ntuple_filter *ntuple_filter)
{
	struct e1000_hw *hw = E1000_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	struct e1000_filter_info *filter_info =
		E1000_DEV_PRIVATE_TO_FILTER_INFO(dev->data->dev_private);
	struct e1000_2tuple_filter *filter;
	uint32_t ttqf = E1000_TTQF_DISABLE_MASK;
	uint32_t imir, imir_ext = E1000_IMIREXT_SIZE_BP;
	int i, ret;

	filter = rte_zmalloc("e1000_2tuple_filter",
			sizeof(struct e1000_2tuple_filter), 0);
	if (filter == NULL)
		return -ENOMEM;

	ret = ntuple_filter_to_2tuple(ntuple_filter,
				      &filter->filter_info);
	if (ret < 0) {
		rte_free(filter);
		return ret;
	}
	if (igb_2tuple_filter_lookup(&filter_info->twotuple_list,
					 &filter->filter_info) != NULL) {
		PMD_DRV_LOG(ERR, "filter exists.");
		rte_free(filter);
		return -EEXIST;
	}
	filter->queue = ntuple_filter->queue;

	/*
	 * look for an unused 2tuple filter index,
	 * and insert the filter to list.
	 */
	for (i = 0; i < E1000_MAX_TTQF_FILTERS; i++) {
		if (!(filter_info->twotuple_mask & (1 << i))) {
			filter_info->twotuple_mask |= 1 << i;
			filter->index = i;
			TAILQ_INSERT_TAIL(&filter_info->twotuple_list,
					  filter,
					  entries);
			break;
		}
	}
	if (i >= E1000_MAX_TTQF_FILTERS) {
		PMD_DRV_LOG(ERR, "2tuple filters are full.");
		rte_free(filter);
		return -ENOSYS;
	}

	imir = (uint32_t)(filter->filter_info.dst_port & E1000_IMIR_DSTPORT);
	if (filter->filter_info.dst_port_mask == 1) /* 1b means not compare. */
		imir |= E1000_IMIR_PORT_BP;
	else
		imir &= ~E1000_IMIR_PORT_BP;

	imir |= filter->filter_info.priority << E1000_IMIR_PRIORITY_SHIFT;

	ttqf |= E1000_TTQF_QUEUE_ENABLE;
	ttqf |= (uint32_t)(filter->queue << E1000_TTQF_QUEUE_SHIFT);
	ttqf |= (uint32_t)(filter->filter_info.proto & E1000_TTQF_PROTOCOL_MASK);
	if (filter->filter_info.proto_mask == 0)
		ttqf &= ~E1000_TTQF_MASK_ENABLE;

	/* tcp flags bits setting. */
	if (filter->filter_info.tcp_flags & TCP_FLAG_ALL) {
		if (filter->filter_info.tcp_flags & TCP_URG_FLAG)
			imir_ext |= E1000_IMIREXT_CTRL_URG;
		if (filter->filter_info.tcp_flags & TCP_ACK_FLAG)
			imir_ext |= E1000_IMIREXT_CTRL_ACK;
		if (filter->filter_info.tcp_flags & TCP_PSH_FLAG)
			imir_ext |= E1000_IMIREXT_CTRL_PSH;
		if (filter->filter_info.tcp_flags & TCP_RST_FLAG)
			imir_ext |= E1000_IMIREXT_CTRL_RST;
		if (filter->filter_info.tcp_flags & TCP_SYN_FLAG)
			imir_ext |= E1000_IMIREXT_CTRL_SYN;
		if (filter->filter_info.tcp_flags & TCP_FIN_FLAG)
			imir_ext |= E1000_IMIREXT_CTRL_FIN;
	} else
		imir_ext |= E1000_IMIREXT_CTRL_BP;
	E1000_WRITE_REG(hw, E1000_IMIR(i), imir);
	E1000_WRITE_REG(hw, E1000_TTQF(i), ttqf);
	E1000_WRITE_REG(hw, E1000_IMIREXT(i), imir_ext);
	return 0;
}

/*
 * igb_remove_2tuple_filter - remove a 2tuple filter
 *
 * @param
 * dev: Pointer to struct rte_eth_dev.
 * ntuple_filter: ponter to the filter that will be removed.
 *
 * @return
 *    - On success, zero.
 *    - On failure, a negative value.
 */
static int
igb_remove_2tuple_filter(struct rte_eth_dev *dev,
			struct rte_eth_ntuple_filter *ntuple_filter)
{
	struct e1000_hw *hw = E1000_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	struct e1000_filter_info *filter_info =
		E1000_DEV_PRIVATE_TO_FILTER_INFO(dev->data->dev_private);
	struct e1000_2tuple_filter_info filter_2tuple;
	struct e1000_2tuple_filter *filter;
	int ret;

	memset(&filter_2tuple, 0, sizeof(struct e1000_2tuple_filter_info));
	ret = ntuple_filter_to_2tuple(ntuple_filter,
				      &filter_2tuple);
	if (ret < 0)
		return ret;

	filter = igb_2tuple_filter_lookup(&filter_info->twotuple_list,
					 &filter_2tuple);
	if (filter == NULL) {
		PMD_DRV_LOG(ERR, "filter doesn't exist.");
		return -ENOENT;
	}

	filter_info->twotuple_mask &= ~(1 << filter->index);
	TAILQ_REMOVE(&filter_info->twotuple_list, filter, entries);
	rte_free(filter);

	E1000_WRITE_REG(hw, E1000_TTQF(filter->index), E1000_TTQF_DISABLE_MASK);
	E1000_WRITE_REG(hw, E1000_IMIR(filter->index), 0);
	E1000_WRITE_REG(hw, E1000_IMIREXT(filter->index), 0);
	return 0;
}

static inline struct e1000_flex_filter *
eth_igb_flex_filter_lookup(struct e1000_flex_filter_list *filter_list,
			struct e1000_flex_filter_info *key)
{
	struct e1000_flex_filter *it;

	TAILQ_FOREACH(it, filter_list, entries) {
		if (memcmp(key, &it->filter_info,
			sizeof(struct e1000_flex_filter_info)) == 0)
			return it;
	}

	return NULL;
}

static int
eth_igb_add_del_flex_filter(struct rte_eth_dev *dev,
			struct rte_eth_flex_filter *filter,
			bool add)
{
	struct e1000_hw *hw = E1000_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	struct e1000_filter_info *filter_info =
		E1000_DEV_PRIVATE_TO_FILTER_INFO(dev->data->dev_private);
	struct e1000_flex_filter *flex_filter, *it;
	uint32_t wufc, queueing, mask;
	uint32_t reg_off;
	uint8_t shift, i, j = 0;

	flex_filter = rte_zmalloc("e1000_flex_filter",
			sizeof(struct e1000_flex_filter), 0);
	if (flex_filter == NULL)
		return -ENOMEM;

	flex_filter->filter_info.len = filter->len;
	flex_filter->filter_info.priority = filter->priority;
	memcpy(flex_filter->filter_info.dwords, filter->bytes, filter->len);
	for (i = 0; i < RTE_ALIGN(filter->len, CHAR_BIT) / CHAR_BIT; i++) {
		mask = 0;
		/* reverse bits in flex filter's mask*/
		for (shift = 0; shift < CHAR_BIT; shift++) {
			if (filter->mask[i] & (0x01 << shift))
				mask |= (0x80 >> shift);
		}
		flex_filter->filter_info.mask[i] = mask;
	}

	wufc = E1000_READ_REG(hw, E1000_WUFC);
	if (flex_filter->index < E1000_MAX_FHFT)
		reg_off = E1000_FHFT(flex_filter->index);
	else
		reg_off = E1000_FHFT_EXT(flex_filter->index - E1000_MAX_FHFT);

	if (add) {
		if (eth_igb_flex_filter_lookup(&filter_info->flex_list,
				&flex_filter->filter_info) != NULL) {
			PMD_DRV_LOG(ERR, "filter exists.");
			rte_free(flex_filter);
			return -EEXIST;
		}
		flex_filter->queue = filter->queue;
		/*
		 * look for an unused flex filter index
		 * and insert the filter into the list.
		 */
		for (i = 0; i < E1000_MAX_FLEX_FILTERS; i++) {
			if (!(filter_info->flex_mask & (1 << i))) {
				filter_info->flex_mask |= 1 << i;
				flex_filter->index = i;
				TAILQ_INSERT_TAIL(&filter_info->flex_list,
					flex_filter,
					entries);
				break;
			}
		}
		if (i >= E1000_MAX_FLEX_FILTERS) {
			PMD_DRV_LOG(ERR, "flex filters are full.");
			rte_free(flex_filter);
			return -ENOSYS;
		}

		E1000_WRITE_REG(hw, E1000_WUFC, wufc | E1000_WUFC_FLEX_HQ |
				(E1000_WUFC_FLX0 << flex_filter->index));
		queueing = filter->len |
			(filter->queue << E1000_FHFT_QUEUEING_QUEUE_SHIFT) |
			(filter->priority << E1000_FHFT_QUEUEING_PRIO_SHIFT);
		E1000_WRITE_REG(hw, reg_off + E1000_FHFT_QUEUEING_OFFSET,
				queueing);
		for (i = 0; i < E1000_FLEX_FILTERS_MASK_SIZE; i++) {
			E1000_WRITE_REG(hw, reg_off,
					flex_filter->filter_info.dwords[j]);
			reg_off += sizeof(uint32_t);
			E1000_WRITE_REG(hw, reg_off,
					flex_filter->filter_info.dwords[++j]);
			reg_off += sizeof(uint32_t);
			E1000_WRITE_REG(hw, reg_off,
				(uint32_t)flex_filter->filter_info.mask[i]);
			reg_off += sizeof(uint32_t) * 2;
			++j;
		}
	} else {
		it = eth_igb_flex_filter_lookup(&filter_info->flex_list,
				&flex_filter->filter_info);
		if (it == NULL) {
			PMD_DRV_LOG(ERR, "filter doesn't exist.");
			rte_free(flex_filter);
			return -ENOENT;
		}

		for (i = 0; i < E1000_FHFT_SIZE_IN_DWD; i++)
			E1000_WRITE_REG(hw, reg_off + i * sizeof(uint32_t), 0);
		E1000_WRITE_REG(hw, E1000_WUFC, wufc &
			(~(E1000_WUFC_FLX0 << it->index)));

		filter_info->flex_mask &= ~(1 << it->index);
		TAILQ_REMOVE(&filter_info->flex_list, it, entries);
		rte_free(it);
		rte_free(flex_filter);
	}

	return 0;
}

static int
eth_igb_get_flex_filter(struct rte_eth_dev *dev,
			struct rte_eth_flex_filter *filter)
{
	struct e1000_hw *hw = E1000_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	struct e1000_filter_info *filter_info =
		E1000_DEV_PRIVATE_TO_FILTER_INFO(dev->data->dev_private);
	struct e1000_flex_filter flex_filter, *it;
	uint32_t wufc, queueing, wufc_en = 0;

	memset(&flex_filter, 0, sizeof(struct e1000_flex_filter));
	flex_filter.filter_info.len = filter->len;
	flex_filter.filter_info.priority = filter->priority;
	memcpy(flex_filter.filter_info.dwords, filter->bytes, filter->len);
	memcpy(flex_filter.filter_info.mask, filter->mask,
			RTE_ALIGN(filter->len, sizeof(char)) / sizeof(char));

	it = eth_igb_flex_filter_lookup(&filter_info->flex_list,
				&flex_filter.filter_info);
	if (it == NULL) {
		PMD_DRV_LOG(ERR, "filter doesn't exist.");
		return -ENOENT;
	}

	wufc = E1000_READ_REG(hw, E1000_WUFC);
	wufc_en = E1000_WUFC_FLEX_HQ | (E1000_WUFC_FLX0 << it->index);

	if ((wufc & wufc_en) == wufc_en) {
		uint32_t reg_off = 0;
		if (it->index < E1000_MAX_FHFT)
			reg_off = E1000_FHFT(it->index);
		else
			reg_off = E1000_FHFT_EXT(it->index - E1000_MAX_FHFT);

		queueing = E1000_READ_REG(hw,
				reg_off + E1000_FHFT_QUEUEING_OFFSET);
		filter->len = queueing & E1000_FHFT_QUEUEING_LEN;
		filter->priority = (queueing & E1000_FHFT_QUEUEING_PRIO) >>
			E1000_FHFT_QUEUEING_PRIO_SHIFT;
		filter->queue = (queueing & E1000_FHFT_QUEUEING_QUEUE) >>
			E1000_FHFT_QUEUEING_QUEUE_SHIFT;
		return 0;
	}
	return -ENOENT;
}

static int
eth_igb_flex_filter_handle(struct rte_eth_dev *dev,
			enum rte_filter_op filter_op,
			void *arg)
{
	struct e1000_hw *hw = E1000_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	struct rte_eth_flex_filter *filter;
	int ret = 0;

	MAC_TYPE_FILTER_SUP_EXT(hw->mac.type);

	if (filter_op == RTE_ETH_FILTER_NOP)
		return ret;

	if (arg == NULL) {
		PMD_DRV_LOG(ERR, "arg shouldn't be NULL for operation %u",
			    filter_op);
		return -EINVAL;
	}

	filter = (struct rte_eth_flex_filter *)arg;
	if (filter->len == 0 || filter->len > E1000_MAX_FLEX_FILTER_LEN
	    || filter->len % sizeof(uint64_t) != 0) {
		PMD_DRV_LOG(ERR, "filter's length is out of range");
		return -EINVAL;
	}
	if (filter->priority > E1000_MAX_FLEX_FILTER_PRI) {
		PMD_DRV_LOG(ERR, "filter's priority is out of range");
		return -EINVAL;
	}

	switch (filter_op) {
	case RTE_ETH_FILTER_ADD:
		ret = eth_igb_add_del_flex_filter(dev, filter, TRUE);
		break;
	case RTE_ETH_FILTER_DELETE:
		ret = eth_igb_add_del_flex_filter(dev, filter, FALSE);
		break;
	case RTE_ETH_FILTER_GET:
		ret = eth_igb_get_flex_filter(dev, filter);
		break;
	default:
		PMD_DRV_LOG(ERR, "unsupported operation %u", filter_op);
		ret = -EINVAL;
		break;
	}

	return ret;
}

/* translate elements in struct rte_eth_ntuple_filter to struct e1000_5tuple_filter_info*/
static inline int
ntuple_filter_to_5tuple_82576(struct rte_eth_ntuple_filter *filter,
			struct e1000_5tuple_filter_info *filter_info)
{
	if (filter->queue >= IGB_MAX_RX_QUEUE_NUM_82576)
		return -EINVAL;
	if (filter->priority > E1000_2TUPLE_MAX_PRI)
		return -EINVAL;  /* filter index is out of range. */
	if (filter->tcp_flags > TCP_FLAG_ALL)
		return -EINVAL;  /* flags is invalid. */

	switch (filter->dst_ip_mask) {
	case UINT32_MAX:
		filter_info->dst_ip_mask = 0;
		filter_info->dst_ip = filter->dst_ip;
		break;
	case 0:
		filter_info->dst_ip_mask = 1;
		break;
	default:
		PMD_DRV_LOG(ERR, "invalid dst_ip mask.");
		return -EINVAL;
	}

	switch (filter->src_ip_mask) {
	case UINT32_MAX:
		filter_info->src_ip_mask = 0;
		filter_info->src_ip = filter->src_ip;
		break;
	case 0:
		filter_info->src_ip_mask = 1;
		break;
	default:
		PMD_DRV_LOG(ERR, "invalid src_ip mask.");
		return -EINVAL;
	}

	switch (filter->dst_port_mask) {
	case UINT16_MAX:
		filter_info->dst_port_mask = 0;
		filter_info->dst_port = filter->dst_port;
		break;
	case 0:
		filter_info->dst_port_mask = 1;
		break;
	default:
		PMD_DRV_LOG(ERR, "invalid dst_port mask.");
		return -EINVAL;
	}

	switch (filter->src_port_mask) {
	case UINT16_MAX:
		filter_info->src_port_mask = 0;
		filter_info->src_port = filter->src_port;
		break;
	case 0:
		filter_info->src_port_mask = 1;
		break;
	default:
		PMD_DRV_LOG(ERR, "invalid src_port mask.");
		return -EINVAL;
	}

	switch (filter->proto_mask) {
	case UINT8_MAX:
		filter_info->proto_mask = 0;
		filter_info->proto = filter->proto;
		break;
	case 0:
		filter_info->proto_mask = 1;
		break;
	default:
		PMD_DRV_LOG(ERR, "invalid protocol mask.");
		return -EINVAL;
	}

	filter_info->priority = (uint8_t)filter->priority;
	if (filter->flags & RTE_NTUPLE_FLAGS_TCP_FLAG)
		filter_info->tcp_flags = filter->tcp_flags;
	else
		filter_info->tcp_flags = 0;

	return 0;
}

static inline struct e1000_5tuple_filter *
igb_5tuple_filter_lookup_82576(struct e1000_5tuple_filter_list *filter_list,
			struct e1000_5tuple_filter_info *key)
{
	struct e1000_5tuple_filter *it;

	TAILQ_FOREACH(it, filter_list, entries) {
		if (memcmp(key, &it->filter_info,
			sizeof(struct e1000_5tuple_filter_info)) == 0) {
			return it;
		}
	}
	return NULL;
}

/*
 * igb_add_5tuple_filter_82576 - add a 5tuple filter
 *
 * @param
 * dev: Pointer to struct rte_eth_dev.
 * ntuple_filter: ponter to the filter that will be added.
 *
 * @return
 *    - On success, zero.
 *    - On failure, a negative value.
 */
static int
igb_add_5tuple_filter_82576(struct rte_eth_dev *dev,
			struct rte_eth_ntuple_filter *ntuple_filter)
{
	struct e1000_hw *hw = E1000_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	struct e1000_filter_info *filter_info =
		E1000_DEV_PRIVATE_TO_FILTER_INFO(dev->data->dev_private);
	struct e1000_5tuple_filter *filter;
	uint32_t ftqf = E1000_FTQF_VF_BP | E1000_FTQF_MASK;
	uint32_t spqf, imir, imir_ext = E1000_IMIREXT_SIZE_BP;
	uint8_t i;
	int ret;

	filter = rte_zmalloc("e1000_5tuple_filter",
			sizeof(struct e1000_5tuple_filter), 0);
	if (filter == NULL)
		return -ENOMEM;

	ret = ntuple_filter_to_5tuple_82576(ntuple_filter,
					    &filter->filter_info);
	if (ret < 0) {
		rte_free(filter);
		return ret;
	}

	if (igb_5tuple_filter_lookup_82576(&filter_info->fivetuple_list,
					 &filter->filter_info) != NULL) {
		PMD_DRV_LOG(ERR, "filter exists.");
		rte_free(filter);
		return -EEXIST;
	}
	filter->queue = ntuple_filter->queue;

	/*
	 * look for an unused 5tuple filter index,
	 * and insert the filter to list.
	 */
	for (i = 0; i < E1000_MAX_FTQF_FILTERS; i++) {
		if (!(filter_info->fivetuple_mask & (1 << i))) {
			filter_info->fivetuple_mask |= 1 << i;
			filter->index = i;
			TAILQ_INSERT_TAIL(&filter_info->fivetuple_list,
					  filter,
					  entries);
			break;
		}
	}
	if (i >= E1000_MAX_FTQF_FILTERS) {
		PMD_DRV_LOG(ERR, "5tuple filters are full.");
		rte_free(filter);
		return -ENOSYS;
	}

	ftqf |= filter->filter_info.proto & E1000_FTQF_PROTOCOL_MASK;
	if (filter->filter_info.src_ip_mask == 0) /* 0b means compare. */
		ftqf &= ~E1000_FTQF_MASK_SOURCE_ADDR_BP;
	if (filter->filter_info.dst_ip_mask == 0)
		ftqf &= ~E1000_FTQF_MASK_DEST_ADDR_BP;
	if (filter->filter_info.src_port_mask == 0)
		ftqf &= ~E1000_FTQF_MASK_SOURCE_PORT_BP;
	if (filter->filter_info.proto_mask == 0)
		ftqf &= ~E1000_FTQF_MASK_PROTO_BP;
	ftqf |= (filter->queue << E1000_FTQF_QUEUE_SHIFT) &
		E1000_FTQF_QUEUE_MASK;
	ftqf |= E1000_FTQF_QUEUE_ENABLE;
	E1000_WRITE_REG(hw, E1000_FTQF(i), ftqf);
	E1000_WRITE_REG(hw, E1000_DAQF(i), filter->filter_info.dst_ip);
	E1000_WRITE_REG(hw, E1000_SAQF(i), filter->filter_info.src_ip);

	spqf = filter->filter_info.src_port & E1000_SPQF_SRCPORT;
	E1000_WRITE_REG(hw, E1000_SPQF(i), spqf);

	imir = (uint32_t)(filter->filter_info.dst_port & E1000_IMIR_DSTPORT);
	if (filter->filter_info.dst_port_mask == 1) /* 1b means not compare. */
		imir |= E1000_IMIR_PORT_BP;
	else
		imir &= ~E1000_IMIR_PORT_BP;
	imir |= filter->filter_info.priority << E1000_IMIR_PRIORITY_SHIFT;

	/* tcp flags bits setting. */
	if (filter->filter_info.tcp_flags & TCP_FLAG_ALL) {
		if (filter->filter_info.tcp_flags & TCP_URG_FLAG)
			imir_ext |= E1000_IMIREXT_CTRL_URG;
		if (filter->filter_info.tcp_flags & TCP_ACK_FLAG)
			imir_ext |= E1000_IMIREXT_CTRL_ACK;
		if (filter->filter_info.tcp_flags & TCP_PSH_FLAG)
			imir_ext |= E1000_IMIREXT_CTRL_PSH;
		if (filter->filter_info.tcp_flags & TCP_RST_FLAG)
			imir_ext |= E1000_IMIREXT_CTRL_RST;
		if (filter->filter_info.tcp_flags & TCP_SYN_FLAG)
			imir_ext |= E1000_IMIREXT_CTRL_SYN;
		if (filter->filter_info.tcp_flags & TCP_FIN_FLAG)
			imir_ext |= E1000_IMIREXT_CTRL_FIN;
	} else
		imir_ext |= E1000_IMIREXT_CTRL_BP;
	E1000_WRITE_REG(hw, E1000_IMIR(i), imir);
	E1000_WRITE_REG(hw, E1000_IMIREXT(i), imir_ext);
	return 0;
}

/*
 * igb_remove_5tuple_filter_82576 - remove a 5tuple filter
 *
 * @param
 * dev: Pointer to struct rte_eth_dev.
 * ntuple_filter: ponter to the filter that will be removed.
 *
 * @return
 *    - On success, zero.
 *    - On failure, a negative value.
 */
static int
igb_remove_5tuple_filter_82576(struct rte_eth_dev *dev,
				struct rte_eth_ntuple_filter *ntuple_filter)
{
	struct e1000_hw *hw = E1000_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	struct e1000_filter_info *filter_info =
		E1000_DEV_PRIVATE_TO_FILTER_INFO(dev->data->dev_private);
	struct e1000_5tuple_filter_info filter_5tuple;
	struct e1000_5tuple_filter *filter;
	int ret;

	memset(&filter_5tuple, 0, sizeof(struct e1000_5tuple_filter_info));
	ret = ntuple_filter_to_5tuple_82576(ntuple_filter,
					    &filter_5tuple);
	if (ret < 0)
		return ret;

	filter = igb_5tuple_filter_lookup_82576(&filter_info->fivetuple_list,
					 &filter_5tuple);
	if (filter == NULL) {
		PMD_DRV_LOG(ERR, "filter doesn't exist.");
		return -ENOENT;
	}

	filter_info->fivetuple_mask &= ~(1 << filter->index);
	TAILQ_REMOVE(&filter_info->fivetuple_list, filter, entries);
	rte_free(filter);

	E1000_WRITE_REG(hw, E1000_FTQF(filter->index),
			E1000_FTQF_VF_BP | E1000_FTQF_MASK);
	E1000_WRITE_REG(hw, E1000_DAQF(filter->index), 0);
	E1000_WRITE_REG(hw, E1000_SAQF(filter->index), 0);
	E1000_WRITE_REG(hw, E1000_SPQF(filter->index), 0);
	E1000_WRITE_REG(hw, E1000_IMIR(filter->index), 0);
	E1000_WRITE_REG(hw, E1000_IMIREXT(filter->index), 0);
	return 0;
}

static int
eth_igb_mtu_set(struct rte_eth_dev *dev, uint16_t mtu)
{
	uint32_t rctl;
	struct e1000_hw *hw;
	struct rte_eth_dev_info dev_info;
	uint32_t frame_size = mtu + (ETHER_HDR_LEN + ETHER_CRC_LEN +
				     VLAN_TAG_SIZE);

	hw = E1000_DEV_PRIVATE_TO_HW(dev->data->dev_private);

#ifdef RTE_LIBRTE_82571_SUPPORT
	/* XXX: not bigger than max_rx_pktlen */
	if (hw->mac.type == e1000_82571)
		return -ENOTSUP;
#endif
	eth_igb_infos_get(dev, &dev_info);

	/* check that mtu is within the allowed range */
	if ((mtu < ETHER_MIN_MTU) ||
	    (frame_size > dev_info.max_rx_pktlen))
		return -EINVAL;

	/* refuse mtu that requires the support of scattered packets when this
	 * feature has not been enabled before. */
	if (!dev->data->scattered_rx &&
	    frame_size > dev->data->min_rx_buf_size - RTE_PKTMBUF_HEADROOM)
		return -EINVAL;

	rctl = E1000_READ_REG(hw, E1000_RCTL);

	/* switch to jumbo mode if needed */
	if (frame_size > ETHER_MAX_LEN) {
		dev->data->dev_conf.rxmode.jumbo_frame = 1;
		rctl |= E1000_RCTL_LPE;
	} else {
		dev->data->dev_conf.rxmode.jumbo_frame = 0;
		rctl &= ~E1000_RCTL_LPE;
	}
	E1000_WRITE_REG(hw, E1000_RCTL, rctl);

	/* update max frame size */
	dev->data->dev_conf.rxmode.max_rx_pkt_len = frame_size;

	E1000_WRITE_REG(hw, E1000_RLPML,
			dev->data->dev_conf.rxmode.max_rx_pkt_len);

	return 0;
}

/*
 * igb_add_del_ntuple_filter - add or delete a ntuple filter
 *
 * @param
 * dev: Pointer to struct rte_eth_dev.
 * ntuple_filter: Pointer to struct rte_eth_ntuple_filter
 * add: if true, add filter, if false, remove filter
 *
 * @return
 *    - On success, zero.
 *    - On failure, a negative value.
 */
static int
igb_add_del_ntuple_filter(struct rte_eth_dev *dev,
			struct rte_eth_ntuple_filter *ntuple_filter,
			bool add)
{
	struct e1000_hw *hw = E1000_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	int ret;

	switch (ntuple_filter->flags) {
	case RTE_5TUPLE_FLAGS:
	case (RTE_5TUPLE_FLAGS | RTE_NTUPLE_FLAGS_TCP_FLAG):
		if (hw->mac.type != e1000_82576)
			return -ENOTSUP;
		if (add)
			ret = igb_add_5tuple_filter_82576(dev,
							  ntuple_filter);
		else
			ret = igb_remove_5tuple_filter_82576(dev,
							     ntuple_filter);
		break;
	case RTE_2TUPLE_FLAGS:
	case (RTE_2TUPLE_FLAGS | RTE_NTUPLE_FLAGS_TCP_FLAG):
		if (hw->mac.type != e1000_82580 && hw->mac.type != e1000_i350)
			return -ENOTSUP;
		if (add)
			ret = igb_add_2tuple_filter(dev, ntuple_filter);
		else
			ret = igb_remove_2tuple_filter(dev, ntuple_filter);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

/*
 * igb_get_ntuple_filter - get a ntuple filter
 *
 * @param
 * dev: Pointer to struct rte_eth_dev.
 * ntuple_filter: Pointer to struct rte_eth_ntuple_filter
 *
 * @return
 *    - On success, zero.
 *    - On failure, a negative value.
 */
static int
igb_get_ntuple_filter(struct rte_eth_dev *dev,
			struct rte_eth_ntuple_filter *ntuple_filter)
{
	struct e1000_hw *hw = E1000_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	struct e1000_filter_info *filter_info =
		E1000_DEV_PRIVATE_TO_FILTER_INFO(dev->data->dev_private);
	struct e1000_5tuple_filter_info filter_5tuple;
	struct e1000_2tuple_filter_info filter_2tuple;
	struct e1000_5tuple_filter *p_5tuple_filter;
	struct e1000_2tuple_filter *p_2tuple_filter;
	int ret;

	switch (ntuple_filter->flags) {
	case RTE_5TUPLE_FLAGS:
	case (RTE_5TUPLE_FLAGS | RTE_NTUPLE_FLAGS_TCP_FLAG):
		if (hw->mac.type != e1000_82576)
			return -ENOTSUP;
		memset(&filter_5tuple,
			0,
			sizeof(struct e1000_5tuple_filter_info));
		ret = ntuple_filter_to_5tuple_82576(ntuple_filter,
						    &filter_5tuple);
		if (ret < 0)
			return ret;
		p_5tuple_filter = igb_5tuple_filter_lookup_82576(
					&filter_info->fivetuple_list,
					&filter_5tuple);
		if (p_5tuple_filter == NULL) {
			PMD_DRV_LOG(ERR, "filter doesn't exist.");
			return -ENOENT;
		}
		ntuple_filter->queue = p_5tuple_filter->queue;
		break;
	case RTE_2TUPLE_FLAGS:
	case (RTE_2TUPLE_FLAGS | RTE_NTUPLE_FLAGS_TCP_FLAG):
		if (hw->mac.type != e1000_82580 && hw->mac.type != e1000_i350)
			return -ENOTSUP;
		memset(&filter_2tuple,
			0,
			sizeof(struct e1000_2tuple_filter_info));
		ret = ntuple_filter_to_2tuple(ntuple_filter, &filter_2tuple);
		if (ret < 0)
			return ret;
		p_2tuple_filter = igb_2tuple_filter_lookup(
					&filter_info->twotuple_list,
					&filter_2tuple);
		if (p_2tuple_filter == NULL) {
			PMD_DRV_LOG(ERR, "filter doesn't exist.");
			return -ENOENT;
		}
		ntuple_filter->queue = p_2tuple_filter->queue;
		break;
	default:
		ret = -EINVAL;
		break;
	}

	return 0;
}

/*
 * igb_ntuple_filter_handle - Handle operations for ntuple filter.
 * @dev: pointer to rte_eth_dev structure
 * @filter_op:operation will be taken.
 * @arg: a pointer to specific structure corresponding to the filter_op
 */
static int
igb_ntuple_filter_handle(struct rte_eth_dev *dev,
				enum rte_filter_op filter_op,
				void *arg)
{
	struct e1000_hw *hw = E1000_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	int ret;

	MAC_TYPE_FILTER_SUP(hw->mac.type);

	if (filter_op == RTE_ETH_FILTER_NOP)
		return 0;

	if (arg == NULL) {
		PMD_DRV_LOG(ERR, "arg shouldn't be NULL for operation %u.",
			    filter_op);
		return -EINVAL;
	}

	switch (filter_op) {
	case RTE_ETH_FILTER_ADD:
		ret = igb_add_del_ntuple_filter(dev,
			(struct rte_eth_ntuple_filter *)arg,
			TRUE);
		break;
	case RTE_ETH_FILTER_DELETE:
		ret = igb_add_del_ntuple_filter(dev,
			(struct rte_eth_ntuple_filter *)arg,
			FALSE);
		break;
	case RTE_ETH_FILTER_GET:
		ret = igb_get_ntuple_filter(dev,
			(struct rte_eth_ntuple_filter *)arg);
		break;
	default:
		PMD_DRV_LOG(ERR, "unsupported operation %u.", filter_op);
		ret = -EINVAL;
		break;
	}
	return ret;
}

static inline int
igb_ethertype_filter_lookup(struct e1000_filter_info *filter_info,
			uint16_t ethertype)
{
	int i;

	for (i = 0; i < E1000_MAX_ETQF_FILTERS; i++) {
		if (filter_info->ethertype_filters[i] == ethertype &&
		    (filter_info->ethertype_mask & (1 << i)))
			return i;
	}
	return -1;
}

static inline int
igb_ethertype_filter_insert(struct e1000_filter_info *filter_info,
			uint16_t ethertype)
{
	int i;

	for (i = 0; i < E1000_MAX_ETQF_FILTERS; i++) {
		if (!(filter_info->ethertype_mask & (1 << i))) {
			filter_info->ethertype_mask |= 1 << i;
			filter_info->ethertype_filters[i] = ethertype;
			return i;
		}
	}
	return -1;
}

static inline int
igb_ethertype_filter_remove(struct e1000_filter_info *filter_info,
			uint8_t idx)
{
	if (idx >= E1000_MAX_ETQF_FILTERS)
		return -1;
	filter_info->ethertype_mask &= ~(1 << idx);
	filter_info->ethertype_filters[idx] = 0;
	return idx;
}


static int
igb_add_del_ethertype_filter(struct rte_eth_dev *dev,
			struct rte_eth_ethertype_filter *filter,
			bool add)
{
	struct e1000_hw *hw = E1000_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	struct e1000_filter_info *filter_info =
		E1000_DEV_PRIVATE_TO_FILTER_INFO(dev->data->dev_private);
	uint32_t etqf = 0;
	int ret;

	if (filter->ether_type == ETHER_TYPE_IPv4 ||
		filter->ether_type == ETHER_TYPE_IPv6) {
		PMD_DRV_LOG(ERR, "unsupported ether_type(0x%04x) in"
			" ethertype filter.", filter->ether_type);
		return -EINVAL;
	}

	if (filter->flags & RTE_ETHTYPE_FLAGS_MAC) {
		PMD_DRV_LOG(ERR, "mac compare is unsupported.");
		return -EINVAL;
	}
	if (filter->flags & RTE_ETHTYPE_FLAGS_DROP) {
		PMD_DRV_LOG(ERR, "drop option is unsupported.");
		return -EINVAL;
	}

	ret = igb_ethertype_filter_lookup(filter_info, filter->ether_type);
	if (ret >= 0 && add) {
		PMD_DRV_LOG(ERR, "ethertype (0x%04x) filter exists.",
			    filter->ether_type);
		return -EEXIST;
	}
	if (ret < 0 && !add) {
		PMD_DRV_LOG(ERR, "ethertype (0x%04x) filter doesn't exist.",
			    filter->ether_type);
		return -ENOENT;
	}

	if (add) {
		ret = igb_ethertype_filter_insert(filter_info,
			filter->ether_type);
		if (ret < 0) {
			PMD_DRV_LOG(ERR, "ethertype filters are full.");
			return -ENOSYS;
		}

		etqf |= E1000_ETQF_FILTER_ENABLE | E1000_ETQF_QUEUE_ENABLE;
		etqf |= (uint32_t)(filter->ether_type & E1000_ETQF_ETHERTYPE);
		etqf |= filter->queue << E1000_ETQF_QUEUE_SHIFT;
	} else {
		ret = igb_ethertype_filter_remove(filter_info, (uint8_t)ret);
		if (ret < 0)
			return -ENOSYS;
	}
	E1000_WRITE_REG(hw, E1000_ETQF(ret), etqf);
	E1000_WRITE_FLUSH(hw);

	return 0;
}

static int
igb_get_ethertype_filter(struct rte_eth_dev *dev,
			struct rte_eth_ethertype_filter *filter)
{
	struct e1000_hw *hw = E1000_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	struct e1000_filter_info *filter_info =
		E1000_DEV_PRIVATE_TO_FILTER_INFO(dev->data->dev_private);
	uint32_t etqf;
	int ret;

	ret = igb_ethertype_filter_lookup(filter_info, filter->ether_type);
	if (ret < 0) {
		PMD_DRV_LOG(ERR, "ethertype (0x%04x) filter doesn't exist.",
			    filter->ether_type);
		return -ENOENT;
	}

	etqf = E1000_READ_REG(hw, E1000_ETQF(ret));
	if (etqf & E1000_ETQF_FILTER_ENABLE) {
		filter->ether_type = etqf & E1000_ETQF_ETHERTYPE;
		filter->flags = 0;
		filter->queue = (etqf & E1000_ETQF_QUEUE) >>
				E1000_ETQF_QUEUE_SHIFT;
		return 0;
	}

	return -ENOENT;
}

/*
 * igb_ethertype_filter_handle - Handle operations for ethertype filter.
 * @dev: pointer to rte_eth_dev structure
 * @filter_op:operation will be taken.
 * @arg: a pointer to specific structure corresponding to the filter_op
 */
static int
igb_ethertype_filter_handle(struct rte_eth_dev *dev,
				enum rte_filter_op filter_op,
				void *arg)
{
	struct e1000_hw *hw = E1000_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	int ret;

	MAC_TYPE_FILTER_SUP(hw->mac.type);

	if (filter_op == RTE_ETH_FILTER_NOP)
		return 0;

	if (arg == NULL) {
		PMD_DRV_LOG(ERR, "arg shouldn't be NULL for operation %u.",
			    filter_op);
		return -EINVAL;
	}

	switch (filter_op) {
	case RTE_ETH_FILTER_ADD:
		ret = igb_add_del_ethertype_filter(dev,
			(struct rte_eth_ethertype_filter *)arg,
			TRUE);
		break;
	case RTE_ETH_FILTER_DELETE:
		ret = igb_add_del_ethertype_filter(dev,
			(struct rte_eth_ethertype_filter *)arg,
			FALSE);
		break;
	case RTE_ETH_FILTER_GET:
		ret = igb_get_ethertype_filter(dev,
			(struct rte_eth_ethertype_filter *)arg);
		break;
	default:
		PMD_DRV_LOG(ERR, "unsupported operation %u.", filter_op);
		ret = -EINVAL;
		break;
	}
	return ret;
}

static int
eth_igb_filter_ctrl(struct rte_eth_dev *dev,
		     enum rte_filter_type filter_type,
		     enum rte_filter_op filter_op,
		     void *arg)
{
	int ret = -EINVAL;

	switch (filter_type) {
	case RTE_ETH_FILTER_NTUPLE:
		ret = igb_ntuple_filter_handle(dev, filter_op, arg);
		break;
	case RTE_ETH_FILTER_ETHERTYPE:
		ret = igb_ethertype_filter_handle(dev, filter_op, arg);
		break;
	case RTE_ETH_FILTER_SYN:
		ret = eth_igb_syn_filter_handle(dev, filter_op, arg);
		break;
	case RTE_ETH_FILTER_FLEXIBLE:
		ret = eth_igb_flex_filter_handle(dev, filter_op, arg);
		break;
	default:
		PMD_DRV_LOG(WARNING, "Filter type (%d) not supported",
							filter_type);
		break;
	}

	return ret;
}

static int
eth_igb_set_mc_addr_list(struct rte_eth_dev *dev,
			 struct ether_addr *mc_addr_set,
			 uint32_t nb_mc_addr)
{
	struct e1000_hw *hw;

	hw = E1000_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	e1000_update_mc_addr_list(hw, (u8 *)mc_addr_set, nb_mc_addr);
	return 0;
}

static int
igb_timesync_enable(struct rte_eth_dev *dev)
{
	struct e1000_hw *hw = E1000_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	uint32_t tsync_ctl;
	uint32_t tsauxc;

	/* Enable system time for it isn't on by default. */
	tsauxc = E1000_READ_REG(hw, E1000_TSAUXC);
	tsauxc &= ~E1000_TSAUXC_DISABLE_SYSTIME;
	E1000_WRITE_REG(hw, E1000_TSAUXC, tsauxc);

	/* Start incrementing the register used to timestamp PTP packets. */
	E1000_WRITE_REG(hw, E1000_TIMINCA, E1000_TIMINCA_INIT);

	/* Enable L2 filtering of IEEE1588/802.1AS Ethernet frame types. */
	E1000_WRITE_REG(hw, E1000_ETQF(E1000_ETQF_FILTER_1588),
			(ETHER_TYPE_1588 |
			 E1000_ETQF_FILTER_ENABLE |
			 E1000_ETQF_1588));

	/* Enable timestamping of received PTP packets. */
	tsync_ctl = E1000_READ_REG(hw, E1000_TSYNCRXCTL);
	tsync_ctl |= E1000_TSYNCRXCTL_ENABLED;
	E1000_WRITE_REG(hw, E1000_TSYNCRXCTL, tsync_ctl);

	/* Enable Timestamping of transmitted PTP packets. */
	tsync_ctl = E1000_READ_REG(hw, E1000_TSYNCTXCTL);
	tsync_ctl |= E1000_TSYNCTXCTL_ENABLED;
	E1000_WRITE_REG(hw, E1000_TSYNCTXCTL, tsync_ctl);

	return 0;
}

static int
igb_timesync_disable(struct rte_eth_dev *dev)
{
	struct e1000_hw *hw = E1000_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	uint32_t tsync_ctl;

	/* Disable timestamping of transmitted PTP packets. */
	tsync_ctl = E1000_READ_REG(hw, E1000_TSYNCTXCTL);
	tsync_ctl &= ~E1000_TSYNCTXCTL_ENABLED;
	E1000_WRITE_REG(hw, E1000_TSYNCTXCTL, tsync_ctl);

	/* Disable timestamping of received PTP packets. */
	tsync_ctl = E1000_READ_REG(hw, E1000_TSYNCRXCTL);
	tsync_ctl &= ~E1000_TSYNCRXCTL_ENABLED;
	E1000_WRITE_REG(hw, E1000_TSYNCRXCTL, tsync_ctl);

	/* Disable L2 filtering of IEEE1588/802.1AS Ethernet frame types. */
	E1000_WRITE_REG(hw, E1000_ETQF(E1000_ETQF_FILTER_1588), 0);

	/* Stop incrementating the System Time registers. */
	E1000_WRITE_REG(hw, E1000_TIMINCA, 0);

	return 0;
}

static int
igb_timesync_read_rx_timestamp(struct rte_eth_dev *dev,
			       struct timespec *timestamp,
			       uint32_t flags __rte_unused)
{
	struct e1000_hw *hw = E1000_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	uint32_t tsync_rxctl;
	uint32_t rx_stmpl;
	uint32_t rx_stmph;

	tsync_rxctl = E1000_READ_REG(hw, E1000_TSYNCRXCTL);
	if ((tsync_rxctl & E1000_TSYNCRXCTL_VALID) == 0)
		return -EINVAL;

	rx_stmpl = E1000_READ_REG(hw, E1000_RXSTMPL);
	rx_stmph = E1000_READ_REG(hw, E1000_RXSTMPH);

	timestamp->tv_sec = (uint64_t)(((uint64_t)rx_stmph << 32) | rx_stmpl);
	timestamp->tv_nsec = 0;

	return  0;
}

static int
igb_timesync_read_tx_timestamp(struct rte_eth_dev *dev,
			       struct timespec *timestamp)
{
	struct e1000_hw *hw = E1000_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	uint32_t tsync_txctl;
	uint32_t tx_stmpl;
	uint32_t tx_stmph;

	tsync_txctl = E1000_READ_REG(hw, E1000_TSYNCTXCTL);
	if ((tsync_txctl & E1000_TSYNCTXCTL_VALID) == 0)
		return -EINVAL;

	tx_stmpl = E1000_READ_REG(hw, E1000_TXSTMPL);
	tx_stmph = E1000_READ_REG(hw, E1000_TXSTMPH);

	timestamp->tv_sec = (uint64_t)(((uint64_t)tx_stmph << 32) | tx_stmpl);
	timestamp->tv_nsec = 0;

	return  0;
}

static int
eth_igb_get_reg_length(struct rte_eth_dev *dev __rte_unused)
{
	int count = 0;
	int g_ind = 0;
	const struct reg_info *reg_group;

	while ((reg_group = igb_regs[g_ind++]))
		count += igb_reg_group_count(reg_group);

	return count;
}

static int
igbvf_get_reg_length(struct rte_eth_dev *dev __rte_unused)
{
	int count = 0;
	int g_ind = 0;
	const struct reg_info *reg_group;

	while ((reg_group = igbvf_regs[g_ind++]))
		count += igb_reg_group_count(reg_group);

	return count;
}

static int
eth_igb_get_regs(struct rte_eth_dev *dev,
	struct rte_dev_reg_info *regs)
{
	struct e1000_hw *hw = E1000_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	uint32_t *data = regs->data;
	int g_ind = 0;
	int count = 0;
	const struct reg_info *reg_group;

	/* Support only full register dump */
	if ((regs->length == 0) ||
	    (regs->length == (uint32_t)eth_igb_get_reg_length(dev))) {
		regs->version = hw->mac.type << 24 | hw->revision_id << 16 |
			hw->device_id;
		while ((reg_group = igb_regs[g_ind++]))
			count += igb_read_regs_group(dev, &data[count],
							reg_group);
		return 0;
	}

	return -ENOTSUP;
}

static int
igbvf_get_regs(struct rte_eth_dev *dev,
	struct rte_dev_reg_info *regs)
{
	struct e1000_hw *hw = E1000_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	uint32_t *data = regs->data;
	int g_ind = 0;
	int count = 0;
	const struct reg_info *reg_group;

	/* Support only full register dump */
	if ((regs->length == 0) ||
	    (regs->length == (uint32_t)igbvf_get_reg_length(dev))) {
		regs->version = hw->mac.type << 24 | hw->revision_id << 16 |
			hw->device_id;
		while ((reg_group = igbvf_regs[g_ind++]))
			count += igb_read_regs_group(dev, &data[count],
							reg_group);
		return 0;
	}

	return -ENOTSUP;
}

static int
eth_igb_get_eeprom_length(struct rte_eth_dev *dev)
{
	struct e1000_hw *hw = E1000_DEV_PRIVATE_TO_HW(dev->data->dev_private);

	/* Return unit is byte count */
	return hw->nvm.word_size * 2;
}

static int
eth_igb_get_eeprom(struct rte_eth_dev *dev,
	struct rte_dev_eeprom_info *in_eeprom)
{
	struct e1000_hw *hw = E1000_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	struct e1000_nvm_info *nvm = &hw->nvm;
	uint16_t *data = in_eeprom->data;
	int first, length;

	first = in_eeprom->offset >> 1;
	length = in_eeprom->length >> 1;
	if ((first >= hw->nvm.word_size) ||
	    ((first + length) >= hw->nvm.word_size))
		return -EINVAL;

	in_eeprom->magic = hw->vendor_id |
		((uint32_t)hw->device_id << 16);

	if ((nvm->ops.read) == NULL)
		return -ENOTSUP;

	return nvm->ops.read(hw, first, length, data);
}

static int
eth_igb_set_eeprom(struct rte_eth_dev *dev,
	struct rte_dev_eeprom_info *in_eeprom)
{
	struct e1000_hw *hw = E1000_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	struct e1000_nvm_info *nvm = &hw->nvm;
	uint16_t *data = in_eeprom->data;
	int first, length;

	first = in_eeprom->offset >> 1;
	length = in_eeprom->length >> 1;
	if ((first >= hw->nvm.word_size) ||
	    ((first + length) >= hw->nvm.word_size))
		return -EINVAL;

	in_eeprom->magic = (uint32_t)hw->vendor_id |
		((uint32_t)hw->device_id << 16);

	if ((nvm->ops.write) == NULL)
		return -ENOTSUP;
	return nvm->ops.write(hw,  first, length, data);
}

static struct rte_driver pmd_igb_drv = {
	.type = PMD_PDEV,
	.init = rte_igb_pmd_init,
};

static struct rte_driver pmd_igbvf_drv = {
	.type = PMD_PDEV,
	.init = rte_igbvf_pmd_init,
};

static int
eth_igb_rx_queue_intr_disable(struct rte_eth_dev *dev, uint16_t queue_id)
{
	struct e1000_hw *hw =
		E1000_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	uint32_t mask = 1 << queue_id;

	E1000_WRITE_REG(hw, E1000_EIMC, mask);
	E1000_WRITE_FLUSH(hw);

	return 0;
}

static int
eth_igb_rx_queue_intr_enable(struct rte_eth_dev *dev, uint16_t queue_id)
{
	struct e1000_hw *hw =
		E1000_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	uint32_t mask = 1 << queue_id;
	uint32_t regval;

	regval = E1000_READ_REG(hw, E1000_EIMS);
	E1000_WRITE_REG(hw, E1000_EIMS, regval | mask);
	E1000_WRITE_FLUSH(hw);

	rte_intr_enable(&dev->pci_dev->intr_handle);

	return 0;
}

static void
eth_igb_write_ivar(struct e1000_hw *hw, uint8_t  msix_vector,
		   uint8_t index, uint8_t offset)
{
	uint32_t val = E1000_READ_REG_ARRAY(hw, E1000_IVAR0, index);

	/* clear bits */
	val &= ~((uint32_t)0xFF << offset);

	/* write vector and valid bit */
	val |= (msix_vector | E1000_IVAR_VALID) << offset;

	E1000_WRITE_REG_ARRAY(hw, E1000_IVAR0, index, val);
}

static void
eth_igb_assign_msix_vector(struct e1000_hw *hw, int8_t direction,
			   uint8_t queue, uint8_t msix_vector)
{
	uint32_t tmp = 0;

	if (hw->mac.type == e1000_82575) {
		if (direction == 0)
			tmp = E1000_EICR_RX_QUEUE0 << queue;
		else if (direction == 1)
			tmp = E1000_EICR_TX_QUEUE0 << queue;
		E1000_WRITE_REG(hw, E1000_MSIXBM(msix_vector), tmp);
	} else if (hw->mac.type == e1000_82576) {
		if ((direction == 0) || (direction == 1))
			eth_igb_write_ivar(hw, msix_vector, queue & 0x7,
					   ((queue & 0x8) << 1) +
					   8 * direction);
	} else if ((hw->mac.type == e1000_82580) ||
			(hw->mac.type == e1000_i350) ||
			(hw->mac.type == e1000_i354) ||
			(hw->mac.type == e1000_i210) ||
			(hw->mac.type == e1000_i211)) {
		if ((direction == 0) || (direction == 1))
			eth_igb_write_ivar(hw, msix_vector,
					   queue >> 1,
					   ((queue & 0x1) << 4) +
					   8 * direction);
	}
}

/* Sets up the hardware to generate MSI-X interrupts properly
 * @hw
 *  board private structure
 */
static void
eth_igb_configure_msix_intr(struct rte_eth_dev *dev)
{
	int queue_id;
	uint32_t tmpval, regval, intr_mask;
	struct e1000_hw *hw =
		E1000_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	uint32_t vec = E1000_MISC_VEC_ID;
	uint32_t base = E1000_MISC_VEC_ID;
	uint32_t misc_shift = 0;

	struct rte_intr_handle *intr_handle = &dev->pci_dev->intr_handle;

	/* won't configure msix register if no mapping is done
	 * between intr vector and event fd
	 */
	if (!rte_intr_dp_is_en(intr_handle))
		return;

	if (rte_intr_allow_others(intr_handle)) {
		vec = base = E1000_RX_VEC_START;
		misc_shift = 1;
	}

	/* set interrupt vector for other causes */
	if (hw->mac.type == e1000_82575) {
		tmpval = E1000_READ_REG(hw, E1000_CTRL_EXT);
		/* enable MSI-X PBA support */
		tmpval |= E1000_CTRL_EXT_PBA_CLR;

		/* Auto-Mask interrupts upon ICR read */
		tmpval |= E1000_CTRL_EXT_EIAME;
		tmpval |= E1000_CTRL_EXT_IRCA;

		E1000_WRITE_REG(hw, E1000_CTRL_EXT, tmpval);

		/* enable msix_other interrupt */
		E1000_WRITE_REG_ARRAY(hw, E1000_MSIXBM(0), 0, E1000_EIMS_OTHER);
		regval = E1000_READ_REG(hw, E1000_EIAC);
		E1000_WRITE_REG(hw, E1000_EIAC, regval | E1000_EIMS_OTHER);
		regval = E1000_READ_REG(hw, E1000_EIAM);
		E1000_WRITE_REG(hw, E1000_EIMS, regval | E1000_EIMS_OTHER);
	} else if ((hw->mac.type == e1000_82576) ||
			(hw->mac.type == e1000_82580) ||
			(hw->mac.type == e1000_i350) ||
			(hw->mac.type == e1000_i354) ||
			(hw->mac.type == e1000_i210) ||
			(hw->mac.type == e1000_i211)) {
		/* turn on MSI-X capability first */
		E1000_WRITE_REG(hw, E1000_GPIE, E1000_GPIE_MSIX_MODE |
					E1000_GPIE_PBA | E1000_GPIE_EIAME |
					E1000_GPIE_NSICR);
		intr_mask = RTE_LEN2MASK(intr_handle->nb_efd, uint32_t) <<
			misc_shift;
		regval = E1000_READ_REG(hw, E1000_EIAC);
		E1000_WRITE_REG(hw, E1000_EIAC, regval | intr_mask);

		/* enable msix_other interrupt */
		regval = E1000_READ_REG(hw, E1000_EIMS);
		E1000_WRITE_REG(hw, E1000_EIMS, regval | intr_mask);
		tmpval = (dev->data->nb_rx_queues | E1000_IVAR_VALID) << 8;
		E1000_WRITE_REG(hw, E1000_IVAR_MISC, tmpval);
	}

	/* use EIAM to auto-mask when MSI-X interrupt
	 * is asserted, this saves a register write for every interrupt
	 */
	intr_mask = RTE_LEN2MASK(intr_handle->nb_efd, uint32_t) <<
		misc_shift;
	regval = E1000_READ_REG(hw, E1000_EIAM);
	E1000_WRITE_REG(hw, E1000_EIAM, regval | intr_mask);

	for (queue_id = 0; queue_id < dev->data->nb_rx_queues; queue_id++) {
		eth_igb_assign_msix_vector(hw, 0, queue_id, vec);
		intr_handle->intr_vec[queue_id] = vec;
		if (vec < base + intr_handle->nb_efd - 1)
			vec++;
	}

	E1000_WRITE_FLUSH(hw);
}

PMD_REGISTER_DRIVER(pmd_igb_drv);
PMD_REGISTER_DRIVER(pmd_igbvf_drv);
