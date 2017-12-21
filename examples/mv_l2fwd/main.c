/*-
 *   BSD LICENSE
 *
 *   Copyright(c) 2010-2016 Intel Corporation. All rights reserved.
 *   Copyright(c) 2017 Marvell International Ltd.
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

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <sys/types.h>
#include <string.h>
#include <sys/queue.h>
#include <stdarg.h>
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdbool.h>

#include <rte_common.h>
#include <rte_vect.h>
#include <rte_byteorder.h>
#include <rte_log.h>
#include <rte_memory.h>
#include <rte_memcpy.h>
#include <rte_memzone.h>
#include <rte_eal.h>
#include <rte_per_lcore.h>
#include <rte_launch.h>
#include <rte_atomic.h>
#include <rte_cycles.h>
#include <rte_prefetch.h>
#include <rte_lcore.h>
#include <rte_per_lcore.h>
#include <rte_branch_prediction.h>
#include <rte_interrupts.h>
#include <rte_pci.h>
#include <rte_random.h>
#include <rte_debug.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_mempool.h>
#include <rte_mbuf.h>
#include <rte_string_fns.h>

/* #define CHECK_CYCLES */
#ifdef CHECK_CYCLES
#include <sys/time.h>
#define CLK_MHZ 2000
static signed int long long usecs1, cnt1;
static struct timeval t1, t2;

#define START_COUNT_CYCLES \
	gettimeofday(&t1, NULL)

#define STOP_N_REPORT_COUNT_CYCLES(_num, _max) \
	do { \
		gettimeofday(&t2, NULL); \
		/* compute and print the elapsed time in millisec */ \
		if (_num) { \
			usecs1 += (t2.tv_sec - t1.tv_sec) * 1000000.0; \
			usecs1 += (t2.tv_usec - t1.tv_usec); \
			cnt1 += _num; \
		} \
		if (cnt1 >= _max) { \
			printf("Cycles count: %lld\n", \
			usecs1*CLK_MHZ/cnt1); \
			usecs1 = cnt1 = 0; \
		} \
	} while (0)

#else
#define START_COUNT_CYCLES
#define STOP_N_REPORT_COUNT_CYCLES(_num, _max)
#endif /* CHECK_CYCLES */


#define RTE_LOGTYPE_L2FWD RTE_LOGTYPE_USER1

/*
 * Configurable number of RX/TX ring descriptors
 */
#define RTE_TEST_RX_DESC_DEFAULT 1024
#define RTE_TEST_TX_DESC_DEFAULT 2048

#define MAX_PKT_BURST     256

#define MAX_TX_QUEUE_PER_PORT RTE_MAX_ETHPORTS
#define MAX_RX_QUEUE_PER_PORT 128

#define MAX_LCORE_PARAMS 1024

#define MAX_RX_QUEUE_PER_LCORE 16

#define NB_SOCKETS        8

/* Configure how many packets ahead to prefetch, when reading packets */
#define PREFETCH_OFFSET	  3

/* Used to mark destination port as 'invalid'. */
#define	BAD_PORT ((uint16_t)-1)


#define ETHER_FRAME_LEN_TO_MTU(length)	\
	(length - ETHER_HDR_LEN -	\
	ETHER_CRC_LEN)	/**< Packet size to ethernet MTU. */

/* Static global variables used within this file. */
static uint16_t nb_rxd = RTE_TEST_RX_DESC_DEFAULT;
static uint16_t nb_txd = RTE_TEST_TX_DESC_DEFAULT;

struct lcore_rx_queue {
	uint8_t port_id;
	uint8_t queue_id;
	uint8_t dest_port_id;
} __rte_cache_aligned;

struct lcore_conf {
	uint16_t n_rx_queue;
	struct lcore_rx_queue rx_queue_list[MAX_RX_QUEUE_PER_LCORE];
} __rte_cache_aligned;


/**< Ports set in promiscuous mode off by default. */
static int promiscuous_on;

static int numa_on = 1; /**< NUMA is enabled by default. */

/* Global variables. */

volatile bool force_quit;

/* MAC updating enabled by default */
static int mac_updating;
static int burst_size;

/* ethernet addresses of ports */
struct ether_addr ports_eth_addr[RTE_MAX_ETHPORTS];

xmm_t val_eth[RTE_MAX_ETHPORTS];

/* mask of enabled ports */
uint32_t enabled_port_mask;

/* list of enabled ports */
static uint32_t dst_ports[RTE_MAX_ETHPORTS];

struct lcore_conf lcore_conf[RTE_MAX_LCORE];

struct lcore_params {
	uint8_t port_id;
	uint8_t queue_id;
	uint8_t lcore_id;
} __rte_cache_aligned;

static struct lcore_params lcore_params_array[MAX_LCORE_PARAMS];

static struct lcore_params *lcore_params = lcore_params_array;
static uint16_t nb_lcore_params;

static struct rte_eth_conf port_conf = {
	.rxmode = {
		.mq_mode = ETH_MQ_RX_RSS,
		.max_rx_pkt_len = ETHER_MAX_LEN,
		.split_hdr_size = 0,
		.header_split   = 0, /**< Header Split disabled */
		.hw_ip_checksum = 0, /**< IP checksum offload enabled */
		.hw_vlan_filter = 0, /**< VLAN filtering disabled */
		.jumbo_frame    = 0, /**< Jumbo Frame Support disabled */
		.hw_strip_crc   = 0, /**< CRC stripped by hardware */
	},
	.rx_adv_conf = {
		.rss_conf = {
			.rss_key = NULL,
			.rss_hf = ETH_RSS_IP,
		},
	},
	.txmode = {
		.mq_mode = ETH_MQ_TX_NONE,
	},
};

static struct rte_mempool *pktmbuf_pool[NB_SOCKETS];

static int
check_lcore_params(void)
{
	uint8_t queue, lcore;
	uint16_t i;
	int socketid;

	for (i = 0; i < nb_lcore_params; ++i) {
		queue = lcore_params[i].queue_id;
		if (queue >= MAX_RX_QUEUE_PER_PORT) {
			printf("invalid queue number: %hhu\n", queue);
			return -1;
		}
		lcore = lcore_params[i].lcore_id;
		if (!rte_lcore_is_enabled(lcore)) {
			printf("error %d (%d): lcore %hhu is not enabled in lcore mask\n",
				i, nb_lcore_params, lcore);
			return -1;
		}
		socketid = rte_lcore_to_socket_id(lcore);
		if ((socketid != 0) &&
			(numa_on == 0)) {
			printf("warning: lcore %hhu is on socket %d with numa off\n",
				lcore, socketid);
		}
	}
	return 0;
}

static int
check_port_config(const unsigned nb_ports)
{
	unsigned portid;
	unsigned last_port = BAD_PORT, nb_ports_in_mask = 0;
	uint16_t i;

	/* reset dst_ports */
	for (portid = 0; portid < RTE_MAX_ETHPORTS; portid++)
		dst_ports[portid] = BAD_PORT;

	for (i = 0; i < nb_lcore_params; ++i) {
		portid = lcore_params[i].port_id;
		printf("check_port_config: lcore_params:%d, port_id:%d\n", i,
			portid);
		if ((enabled_port_mask & (1 << portid)) == 0) {
			printf("port %u is not enabled in port mask\n",
				portid);
			return -1;
		}

		if (dst_ports[portid] == BAD_PORT && last_port != portid) {
			if (nb_ports_in_mask % 2 && last_port != BAD_PORT) {
				dst_ports[portid] = last_port;
				dst_ports[last_port] = portid;
				printf("Dest Port Config: Port%d <-> Port%d\n",
					portid, last_port);
			} else
				last_port = portid;
			nb_ports_in_mask++;
		}
		if (portid >= nb_ports) {
			printf("port %u is not present on the board\n", portid);
			return -1;
		}
	}

	for (portid = 0; portid < RTE_MAX_ETHPORTS; portid++)
		if (dst_ports[portid] == BAD_PORT)
			dst_ports[portid] = portid;
	return 0;
}

static uint8_t
get_port_n_rx_queues(const uint8_t port)
{
	int queue = -1;
	uint16_t i;

	for (i = 0; i < nb_lcore_params; ++i) {
		if (lcore_params[i].port_id == port) {
			if (lcore_params[i].queue_id == queue+1)
				queue = lcore_params[i].queue_id;
			else
				rte_exit(EXIT_FAILURE, "queue ids of the port %d must be"
						" in sequence and must start with 0\n",
						lcore_params[i].port_id);
		}
	}
	return (uint8_t)(++queue);
}

static int
set_default_lcore_rx_queues(void)
{
	uint16_t i, j, nb_rx_queue = 0;

	for (i = 0; i < MAX_LCORE_PARAMS; ++i) {
		if (!rte_lcore_is_enabled(i))
			continue;

		for (j = 0; j < RTE_MAX_ETHPORTS; j++) {
			if ((enabled_port_mask & (1 << j)) == 0)
				continue;

			lcore_params_array[nb_lcore_params].port_id = j;
			lcore_params_array[nb_lcore_params].queue_id =
								nb_rx_queue;
			lcore_params_array[nb_lcore_params].lcore_id = i;
			printf("%d: lcore %d: port: %d, queue: %d\n",
				nb_lcore_params, i, j, nb_rx_queue);
			++nb_lcore_params;
		}
		nb_rx_queue++;
	}
	return 0;
}


static int
init_lcore_rx_queues(void)
{
	uint16_t i, nb_rx_queue;
	uint8_t lcore;

	for (i = 0; i < nb_lcore_params; ++i) {
		lcore = lcore_params[i].lcore_id;
		nb_rx_queue = lcore_conf[lcore].n_rx_queue;
		if (nb_rx_queue >= MAX_RX_QUEUE_PER_LCORE) {
			printf("error: too many queues (%u) for lcore: %u\n",
				(unsigned)nb_rx_queue + 1, (unsigned)lcore);
			return -1;
		}
		lcore_conf[lcore].rx_queue_list[nb_rx_queue].port_id =
			lcore_params[i].port_id;
		lcore_conf[lcore].rx_queue_list[nb_rx_queue].queue_id =
			lcore_params[i].queue_id;
		lcore_conf[lcore].n_rx_queue++;
	}
	return 0;
}

/* display usage */
static void
print_usage(const char *prgname)
{
	printf("%s [EAL options] --"
		" -p PORTMASK"
		" [-P]"
		" --config (port,queue,lcore)[,(port,queue,lcore)]"
		" [--enable-jumbo [--max-pkt-len PKTLEN]]"
		"  -p PORTMASK: Hexadecimal bitmask of ports to configure\n"
		"  -P : Enable promiscuous mode\n"
		"  -b BURST: Burst size\n"
		"  --config (port,queue,lcore): Rx queue configuration\n"
		"  --enable-jumbo: Enable jumbo frames\n"
		"  --max-pkt-len: Under the premise of enabling jumbo,\n"
		"                 maximum packet length in decimal (64-9600)\n"
		"  --[no-]mac-updating: Enable or disable MAC addresses updating (disabled by default)\n"
		"      When enabled:\n"
		"       - The source MAC address is replaced by the TX port MAC address\n"
		"       - The destination MAC address is replaced by 02:00:00:00:00:TX_PORT_ID\n",
		prgname);
}

static int
parse_max_pkt_len(const char *pktlen)
{
	char *end = NULL;
	unsigned long len;

	/* parse decimal string */
	len = strtoul(pktlen, &end, 10);
	if ((pktlen[0] == '\0') || (end == NULL) || (*end != '\0'))
		return -1;

	if (len == 0)
		return -1;

	return len;
}

static int
parse_portmask(const char *portmask)
{
	char *end = NULL;
	unsigned long pm;

	/* parse hexadecimal string */
	pm = strtoul(portmask, &end, 16);
	if ((portmask[0] == '\0') || (end == NULL) || (*end != '\0'))
		return -1;

	if (pm == 0)
		return -1;

	return pm;
}

static int
parse_config(const char *q_arg)
{
	char s[256];
	const char *p, *p0 = q_arg;
	char *end;
	enum fieldnames {
		FLD_PORT = 0,
		FLD_QUEUE,
		FLD_LCORE,
		_NUM_FLD
	};
	unsigned long int_fld[_NUM_FLD];
	char *str_fld[_NUM_FLD];
	int i;
	unsigned size;

	nb_lcore_params = 0;

	while ((p = strchr(p0, '(')) != NULL) {
		++p;
		p0 = strchr(p, ')');
		if (p0 == NULL)
			return -1;

		size = p0 - p;
		if (size >= sizeof(s))
			return -1;

		snprintf(s, sizeof(s), "%.*s", size, p);
		if (rte_strsplit(s, sizeof(s), str_fld, _NUM_FLD, ',')
		    != _NUM_FLD)
			return -1;

		for (i = 0; i < _NUM_FLD; i++) {
			errno = 0;
			int_fld[i] = strtoul(str_fld[i], &end, 0);
			if (errno != 0 || end == str_fld[i] || int_fld[i] > 255)
				return -1;
		}
		if (nb_lcore_params >= MAX_LCORE_PARAMS) {
			printf("exceeded max number of lcore params: %hu\n",
				nb_lcore_params);
			return -1;
		}
		lcore_params_array[nb_lcore_params].port_id =
			(uint8_t)int_fld[FLD_PORT];
		lcore_params_array[nb_lcore_params].queue_id =
			(uint8_t)int_fld[FLD_QUEUE];
		lcore_params_array[nb_lcore_params].lcore_id =
			(uint8_t)int_fld[FLD_LCORE];
		++nb_lcore_params;
	}
	return 0;
}

#define MAX_JUMBO_PKT_LEN  9600
#define MEMPOOL_CACHE_SIZE 256

static const char short_options[] =
	"p:"  /* portmask */
	"P"   /* promiscuous */
	"b:"  /* burst size */
	;

#define CMD_LINE_OPT_CONFIG "config"
 #define CMD_LINE_OPT_ENABLE_JUMBO "enable-jumbo"
#define CMD_LINE_OPT_MAC_UPDATING "mac-updating"
#define CMD_LINE_OPT_NO_MAC_UPDATING "no-mac-updating"

enum {
	/* long options mapped to a short option */

	/* first long only option value must be >= 256, so that we won't
	 * conflict with short options
	 */
	CMD_LINE_OPT_MIN_NUM = 256,
	CMD_LINE_OPT_CONFIG_NUM,
	CMD_LINE_OPT_ENABLE_JUMBO_NUM,
	CMD_LINE_OPT_MAC_UPDATING_NUM,
	CMD_LINE_OPT_NO_MAC_UPDATING_NUM,
};

static const struct option lgopts[] = {
	{CMD_LINE_OPT_CONFIG, 1, 0, CMD_LINE_OPT_CONFIG_NUM},
	{CMD_LINE_OPT_ENABLE_JUMBO, 0, 0, CMD_LINE_OPT_ENABLE_JUMBO_NUM},
	{CMD_LINE_OPT_MAC_UPDATING, 0, 0, CMD_LINE_OPT_MAC_UPDATING_NUM},
	{CMD_LINE_OPT_NO_MAC_UPDATING, 0, 0, CMD_LINE_OPT_NO_MAC_UPDATING_NUM},
	{NULL, 0, 0, 0}
};

/*
 * This expression is used to calculate the number of mbufs needed
 * depending on user input, taking  into account memory for rx and
 * tx hardware rings, cache per lcore and mtable per port per lcore.
 * RTE_MAX is used to ensure that NB_MBUF never goes below a minimum
 * value of 8192
 */
#define NB_MBUF RTE_MAX(	\
	(nb_ports*nb_rx_queue*RTE_TEST_RX_DESC_DEFAULT +	\
	nb_ports*nb_lcores*MAX_PKT_BURST +			\
	nb_ports*n_tx_queue*RTE_TEST_TX_DESC_DEFAULT +		\
	nb_lcores*MEMPOOL_CACHE_SIZE),				\
	(unsigned)8192)

/* Parse the argument given in the command line of the application */
static int
parse_args(int argc, char **argv)
{
	int opt, ret;
	char **argvopt;
	int option_index;
	char *prgname = argv[0];

	argvopt = argv;
	burst_size = 0;

	/* Error or normal output strings. */
	const char *str1 = "L2FWD: Invalid portmask";
	const char *str2 = "L2FWD: Promiscuous mode selected";
	const char *str5 = "L2FWD: Invalid config";
	const char *str8 =
		"L2FWD: Jumbo frame is enabled - disabling simple TX path";
	const char *str9 = "L2FWD: Invalid packet length";
	const char *str10 = "L2FWD: Set jumbo frame max packet len to ";

	while ((opt = getopt_long(argc, argvopt, short_options,
				lgopts, &option_index)) != EOF) {

		switch (opt) {
		/* portmask */
		case 'p':
			enabled_port_mask = parse_portmask(optarg);
			if (enabled_port_mask == 0) {
				printf("%s\n", str1);
				print_usage(prgname);
				return -1;
			}
			break;

		case 'P':
			printf("%s\n", str2);
			promiscuous_on = 1;
			break;
		case 'b':
			burst_size = atoi(optarg);
			if (burst_size > MAX_PKT_BURST)
				burst_size = MAX_PKT_BURST;
			printf("Burst Size: %d\n", burst_size);
			break;


		/* long options */
		case CMD_LINE_OPT_CONFIG_NUM:
			ret = parse_config(optarg);
			if (ret) {
				printf("%s\n", str5);
				print_usage(prgname);
				return -1;
			}
			break;

		case CMD_LINE_OPT_ENABLE_JUMBO_NUM: {
			struct option lenopts = {
				"max-pkt-len", required_argument, 0, 0
			};

			printf("%s\n", str8);
			port_conf.rxmode.jumbo_frame = 1;

			/*
			 * if no max-pkt-len set, use the default
			 * value ETHER_MAX_LEN.
			 */
			if (getopt_long(argc, argvopt, "",
					&lenopts, &option_index) == 0) {
				ret = parse_max_pkt_len(optarg);
				if ((ret < 64) ||
					(ret > MAX_JUMBO_PKT_LEN)) {
					printf("%s\n", str9);
					print_usage(prgname);
					return -1;
				}
				port_conf.rxmode.max_rx_pkt_len = ret;
			}
			printf("%s %u\n", str10,
				(unsigned int)port_conf.rxmode.max_rx_pkt_len);
			break;
		}

		case CMD_LINE_OPT_MAC_UPDATING_NUM:
			mac_updating = 1;
			break;

		case CMD_LINE_OPT_NO_MAC_UPDATING_NUM:
			mac_updating = 0;
			break;

		default:
			print_usage(prgname);
			return -1;
		}
	}

	printf("mac address updating is %s\n", mac_updating ? "enabled" : "disabled");

	if (!nb_lcore_params)
		set_default_lcore_rx_queues();

	if (optind >= 0)
		argv[optind-1] = prgname;

	ret = optind-1;
	optind = 1; /* reset getopt lib */
	return ret;
}

static void
print_ethaddr(const char *name, const struct ether_addr *eth_addr)
{
	char buf[ETHER_ADDR_FMT_SIZE];
	ether_format_addr(buf, ETHER_ADDR_FMT_SIZE, eth_addr);
	printf("%s%s", name, buf);
}

static void
mac_update(struct rte_mbuf *m, unsigned dest_portid)
{
	struct ether_hdr *eth;
	void *tmp;

	eth = rte_pktmbuf_mtod(m, struct ether_hdr *);

	/* 02:00:00:00:00:xx */
	tmp = &eth->d_addr.addr_bytes[0];
	*((uint64_t *)tmp) = 0x000000000002 + ((uint64_t)dest_portid << 40);

	/* src addr */
	ether_addr_copy(&ports_eth_addr[dest_portid], &eth->s_addr);
}

/* main processing loop */
static int
main_loop(__attribute__((unused)) void *dummy)
{
	struct rte_mbuf *pkts_burst[MAX_PKT_BURST];
	struct rte_mbuf *m;
	unsigned lcore_id, drop_pkts = 0, rx_pkts = 0, tx_pkts = 0;
	uint64_t prev_tsc, diff_tsc, cur_tsc;
	int i, j, nb_rx;
	int sent, nbq = 0;
	uint8_t portid, queueid, dst_port;
	struct lcore_conf *qconf;
	const uint64_t drain_tsc = (rte_get_tsc_hz() + US_PER_S - 1) * 5 +
				   US_PER_S*rte_lcore_id();

	prev_tsc = 0;

	lcore_id = rte_lcore_id();
	qconf = &lcore_conf[lcore_id];

	if (qconf->n_rx_queue == 0) {
		RTE_LOG(INFO, L2FWD, "lcore %u has nothing to do\n", lcore_id);
		return 0;
	}

	for (i = 0; i < qconf->n_rx_queue; i++) {

		portid = qconf->rx_queue_list[i].port_id;
		queueid = qconf->rx_queue_list[i].queue_id;
		nbq += get_port_n_rx_queues(portid);
		RTE_LOG(INFO, L2FWD,
			" -- lcoreid=%u portid=%hhu rxqueueid=%hhu, nbq=%d\n",
			lcore_id, portid, queueid, nbq);
	}

	const int burst = burst_size;
	RTE_LOG(INFO, L2FWD, "entering main loop on lcore %u, burst: %d\n",
		lcore_id, burst);

	while (!force_quit) {
		cur_tsc = rte_rdtsc() + US_PER_S*lcore_id;

		/*
		 * TX burst queue drain
		 */
		diff_tsc = cur_tsc - prev_tsc;
		if (unlikely(diff_tsc > drain_tsc)) {

			prev_tsc = cur_tsc;
			if (unlikely(drop_pkts))
				printf("Core%d: Receved: %u packets, Sent: %u packets, Dropped: %u packets\n",
					lcore_id, rx_pkts, tx_pkts, drop_pkts);
		}

		/*
		 * Read packet from RX queues
		 */
		for (i = 0; i < qconf->n_rx_queue; ++i) {
			portid = qconf->rx_queue_list[i].port_id;
			queueid = qconf->rx_queue_list[i].queue_id;
			dst_port = qconf->rx_queue_list[i].dest_port_id;

START_COUNT_CYCLES
			nb_rx = rte_eth_rx_burst(portid, queueid, pkts_burst,
						 burst);
STOP_N_REPORT_COUNT_CYCLES((unsigned int)nb_rx, 50000000)

			if (mac_updating) {
				for (j = 0; j < nb_rx; j++) {
					m = pkts_burst[j];
					rte_prefetch0(
						rte_pktmbuf_mtod(m, void *));
					mac_update(m, dst_port);
				}
			}
/* START_COUNT_CYCLES */
			sent = rte_eth_tx_burst(dst_port, queueid, pkts_burst,
						nb_rx);
/* STOP_N_REPORT_COUNT_CYCLES((unsigned int)nb_rx,50000000) */

			if (unlikely(sent < nb_rx)) {
				do {
					rte_pktmbuf_free(pkts_burst[sent]);
				} while (++sent < nb_rx);
			}
			rx_pkts += nb_rx;
			tx_pkts += sent;
			drop_pkts += (nb_rx - sent);
		}
	}

	return 0;
}

static int
init_mem(unsigned nb_mbuf, unsigned buf_size)
{
	int socketid;
	unsigned lcore_id;
	char s[64];

	for (lcore_id = 0; lcore_id < RTE_MAX_LCORE; lcore_id++) {
		if (rte_lcore_is_enabled(lcore_id) == 0)
			continue;

		if (numa_on)
			socketid = rte_lcore_to_socket_id(lcore_id);
		else
			socketid = 0;

		if (socketid >= NB_SOCKETS) {
			rte_exit(EXIT_FAILURE,
				"Socket %d of lcore %u is out of range %d\n",
				socketid, lcore_id, NB_SOCKETS);
		}

		if (pktmbuf_pool[socketid] == NULL) {
			snprintf(s, sizeof(s), "mbuf_pool_%d", socketid);
			pktmbuf_pool[socketid] =
				rte_pktmbuf_pool_create(s, nb_mbuf,
					MEMPOOL_CACHE_SIZE, 0,
					buf_size, socketid);
			if (pktmbuf_pool[socketid] == NULL)
				rte_exit(EXIT_FAILURE,
					"Cannot init mbuf pool on socket %d\n",
					socketid);
			else
				printf("Allocated mbuf pool on socket %d\n",
					socketid);

		}
	}
	return 0;
}

/* Check the link status of all ports in up to 9s, and print them finally */
static void
check_all_ports_link_status(uint8_t port_num, uint32_t port_mask)
{
#define CHECK_INTERVAL 100 /* 100ms */
#define MAX_CHECK_TIME 90 /* 9s (90 * 100ms) in total */
	uint8_t portid, count, all_ports_up, print_flag = 0;
	struct rte_eth_link link;

	printf("\nChecking link status");
	fflush(stdout);
	for (count = 0; count <= MAX_CHECK_TIME; count++) {
		if (force_quit)
			return;
		all_ports_up = 1;
		for (portid = 0; portid < port_num; portid++) {
			if (force_quit)
				return;
			if ((port_mask & (1 << portid)) == 0)
				continue;
			memset(&link, 0, sizeof(link));
			rte_eth_link_get_nowait(portid, &link);
			/* print link status if flag set */
			if (print_flag == 1) {
				if (link.link_status)
					printf("Port %d Link Up - speed %u "
						"Mbps - %s\n", (uint8_t)portid,
						(unsigned)link.link_speed,
				(link.link_duplex == ETH_LINK_FULL_DUPLEX) ?
					("full-duplex") : ("half-duplex\n"));
				else
					printf("Port %d Link Down\n",
						(uint8_t)portid);
				continue;
			}
			/* clear all_ports_up flag if any link down */
			if (link.link_status == ETH_LINK_DOWN) {
				all_ports_up = 0;
				break;
			}
		}
		/* after finally printing all link status, get out */
		if (print_flag == 1)
			break;

		if (all_ports_up == 0) {
			printf(".");
			fflush(stdout);
			rte_delay_ms(CHECK_INTERVAL);
		}

		/* set the print_flag if all ports up or timeout */
		if (all_ports_up == 1 || count == (MAX_CHECK_TIME - 1)) {
			print_flag = 1;
			printf("done\n");
		}
	}
}

static void
signal_handler(int signum)
{
	if (signum == SIGINT || signum == SIGTERM) {
		printf("\n\nSignal %d received, preparing to exit...\n",
				signum);
		force_quit = true;
	}
}

int
main(int argc, char **argv)
{
	struct lcore_conf *qconf;
	struct rte_eth_dev_info dev_info;
	struct rte_eth_txconf *txconf;
	int ret;
	unsigned nb_ports;
	uint16_t queueid;
	unsigned lcore_id;
	uint32_t n_tx_queue, nb_lcores;
	uint8_t portid, nb_rx_queue, queue, socketid;

	rte_set_application_usage_hook(print_usage);

	/* init EAL */
	ret = rte_eal_init(argc, argv);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Invalid EAL parameters\n");
	argc -= ret;
	argv += ret;

	force_quit = false;
	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);

	/* parse application arguments (after the EAL ones) */
	ret = parse_args(argc, argv);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Invalid L2FWD parameters\n");

	if (check_lcore_params() < 0)
		rte_exit(EXIT_FAILURE, "check_lcore_params failed\n");

	ret = init_lcore_rx_queues();
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "init_lcore_rx_queues failed\n");

	nb_ports = rte_eth_dev_count();

	if (check_port_config(nb_ports) < 0)
		rte_exit(EXIT_FAILURE, "check_port_config failed\n");

	nb_lcores = rte_lcore_count();
	if (!burst_size)
		burst_size = MAX_PKT_BURST >> (nb_lcores - 1);

	printf("nb_ports: %d, nb_lcores: %d, nb_lcore_params: %d, burst_size: %d\n",
		nb_ports, nb_lcores, nb_lcore_params, burst_size);

	/* initialize all ports */
	for (portid = 0; portid < nb_ports; portid++) {
		/* skip ports that are not enabled */
		if ((enabled_port_mask & (1 << portid)) == 0) {
			printf("\nSkipping disabled port %d\n", portid);
			continue;
		}

		/* init port */
		printf("Initializing port %d ... ", portid);
		fflush(stdout);

		nb_rx_queue = get_port_n_rx_queues(portid);
		n_tx_queue = nb_lcores;
		if (n_tx_queue > MAX_TX_QUEUE_PER_PORT)
			n_tx_queue = MAX_TX_QUEUE_PER_PORT;
		printf("Creating queues: nb_rxq=%d nb_txq=%u... ",
			nb_rx_queue, (unsigned)n_tx_queue);
		ret = rte_eth_dev_configure(portid, nb_rx_queue,
					(uint16_t)n_tx_queue, &port_conf);
		if (ret < 0)
			rte_exit(EXIT_FAILURE,
				"Cannot configure device: err=%d, port=%d\n",
				ret, portid);

		rte_eth_macaddr_get(portid, &ports_eth_addr[portid]);
		print_ethaddr(" Address:", &ports_eth_addr[portid]);
		printf(", ");

		/* init memory */
		ret = init_mem(NB_MBUF, port_conf.rxmode.max_rx_pkt_len);
		if (ret < 0)
			rte_exit(EXIT_FAILURE, "init_mem failed\n");

		/* init one TX queue per couple (lcore,port) */
		queueid = 0;
		for (lcore_id = 0; lcore_id < RTE_MAX_LCORE; lcore_id++) {
			if (rte_lcore_is_enabled(lcore_id) == 0)
				continue;

			if (numa_on)
				socketid =
				(uint8_t)rte_lcore_to_socket_id(lcore_id);
			else
				socketid = 0;

			printf("txq=%u,%d,%d ", lcore_id, queueid, socketid);
			fflush(stdout);

			rte_eth_dev_info_get(portid, &dev_info);
			txconf = &dev_info.default_txconf;
			if (port_conf.rxmode.jumbo_frame)
				txconf->txq_flags = 0;
			ret = rte_eth_tx_queue_setup(portid, queueid, nb_txd,
						     socketid, txconf);
			if (ret < 0)
				rte_exit(EXIT_FAILURE,
					"rte_eth_tx_queue_setup: err=%d, "
					"port=%d\n", ret, portid);

			qconf = &lcore_conf[lcore_id];
			queueid++;

		}
		printf("\n");
	}

	for (lcore_id = 0; lcore_id < RTE_MAX_LCORE; lcore_id++) {
		if (rte_lcore_is_enabled(lcore_id) == 0)
			continue;
		qconf = &lcore_conf[lcore_id];
		printf("\nInitializing rx queues on lcore %u ...\n", lcore_id);
		fflush(stdout);
		/* init RX queues */
		for (queue = 0; queue < qconf->n_rx_queue; ++queue) {
			portid = qconf->rx_queue_list[queue].port_id;
			queueid = qconf->rx_queue_list[queue].queue_id;
			qconf->rx_queue_list[queue].dest_port_id =
							dst_ports[portid];
			if (numa_on)
				socketid =
				(uint8_t)rte_lcore_to_socket_id(lcore_id);
			else
				socketid = 0;

			printf("rxq=%d,%d,%d ", portid, queueid, socketid);
			fflush(stdout);

			ret = rte_eth_rx_queue_setup(portid, queueid,
					nb_rxd + 2*burst_size, socketid, NULL,
					pktmbuf_pool[socketid]);
			if (ret < 0)
				rte_exit(EXIT_FAILURE,
				"rte_eth_rx_queue_setup: err=%d, port=%d\n",
				ret, portid);
		}
	}


	printf("\n");

	/* start ports */
	for (portid = 0; portid < nb_ports; portid++) {
		if ((enabled_port_mask & (1 << portid)) == 0)
			continue;

		/* Start device */
		ret = rte_eth_dev_start(portid);
		if (ret < 0)
			rte_exit(EXIT_FAILURE,
				"rte_eth_dev_start: err=%d, port=%d\n",
				ret, portid);

		/*
		 * If enabled, put device in promiscuous mode.
		 * This allows IO forwarding mode to forward packets
		 * to itself through 2 cross-connected  ports of the
		 * target machine.
		 */
		if (promiscuous_on)
			rte_eth_promiscuous_enable(portid);

		if (port_conf.rxmode.jumbo_frame)
			rte_eth_dev_set_mtu(
				portid,
				ETHER_FRAME_LEN_TO_MTU(
				port_conf.rxmode.max_rx_pkt_len)
				);
	}

	printf("\n");

	for (lcore_id = 0; lcore_id < RTE_MAX_LCORE; lcore_id++) {
		if (rte_lcore_is_enabled(lcore_id) == 0)
			continue;
		qconf = &lcore_conf[lcore_id];
		for (queue = 0; queue < qconf->n_rx_queue; ++queue) {
			portid = qconf->rx_queue_list[queue].port_id;
			queueid = qconf->rx_queue_list[queue].queue_id;
		}
	}


	check_all_ports_link_status((uint8_t)nb_ports, enabled_port_mask);

	ret = 0;
	/* launch per-lcore init on every lcore */
	rte_eal_mp_remote_launch(main_loop, NULL, CALL_MASTER);
	RTE_LCORE_FOREACH_SLAVE(lcore_id) {
		if (rte_eal_wait_lcore(lcore_id) < 0) {
			ret = -1;
			break;
		}
	}

	/* stop ports */
	for (portid = 0; portid < nb_ports; portid++) {
		if ((enabled_port_mask & (1 << portid)) == 0)
			continue;
		printf("Closing port %d...", portid);
		rte_eth_dev_stop(portid);
		rte_eth_dev_close(portid);
		printf(" Done\n");
	}
	printf("Bye...\n");

	return ret;
}
