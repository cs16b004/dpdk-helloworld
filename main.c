#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/queue.h>
#include <netinet/in.h>
#include <setjmp.h>
#include <stdarg.h>
#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdbool.h>

#include <rte_common.h>
#include <rte_log.h>
#include <rte_malloc.h>
#include <rte_memory.h>
#include <rte_memcpy.h>
#include <rte_eal.h>
#include <rte_launch.h>
#include <rte_atomic.h>
#include <rte_cycles.h>
#include <rte_prefetch.h>
#include <rte_lcore.h>
#include <rte_per_lcore.h>
#include <rte_branch_prediction.h>
#include <rte_interrupts.h>
#include <rte_random.h>
#include <rte_debug.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_mempool.h>
#include <rte_mbuf.h>


#include <rte_byteorder.h>
#include <rte_config.h>
#include <rte_errno.h>
#include <rte_ip.h>

#define APP "pingpong"

uint32_t PINGPONG_LOG_LEVEL = RTE_LOG_DEBUG;

#define NB_MBUF   8192

#define MAX_PKT_BURST 32
#define BURST_TX_DRAIN_US 100 /* TX drain every ~100us */

/*
 * Configurable number of RX/TX ring descriptors
 */
#define RTE_TEST_RX_DESC_DEFAULT 1024
#define RTE_TEST_TX_DESC_DEFAULT 1024
static uint16_t nb_rxd = RTE_TEST_RX_DESC_DEFAULT;
static uint16_t nb_txd = RTE_TEST_TX_DESC_DEFAULT;

/* ethernet addresses of ports */
static struct rte_ether_addr lsi_ports_eth_addr[1];

/* mask of enabled ports */
static uint32_t lsi_enabled_port_mask = 0;

static unsigned int lsi_rx_queue_per_lcore = 1;


#define MAX_PKT_BURST 32

#define MAX_RX_QUEUE_PER_LCORE 16
#define MAX_TX_QUEUE_PER_PORT 16
struct lcore_queue_conf {
	unsigned n_rx_port;
	unsigned rx_port_list[MAX_RX_QUEUE_PER_LCORE];
	unsigned tx_queue_id;
} __rte_cache_aligned;
struct lcore_queue_conf lcore_queue_conf;

static struct rte_eth_conf port_conf = {
	.rxmode = {
		.split_hdr_size = 0,
	},
	.txmode = {
		.mq_mode = ETH_MQ_TX_NONE,
	},
	.intr_conf = {
	//	.lsc = 1, /**< lsc interrupt feature enabled */
        .rxq=1,
	},
};



/* Per-port statistics struct */
struct lsi_port_statistics {
	uint64_t tx;
	uint64_t rx;
	uint64_t dropped;
} __rte_cache_aligned;
struct lsi_port_statistics port_statistics[RTE_MAX_ETHPORTS];




/* A tsc-based timer responsible for triggering statistics printout */
#define TIMER_MILLISECOND 2000000ULL /* around 1ms at 2 Ghz */
#define MAX_TIMER_PERIOD 86400 /* 1 day max */
static int64_t timer_period = 10 * TIMER_MILLISECOND * 1000; /* default period is 10 seconds */


/* the client side */
static struct rte_ether_addr client_ether_addr =
    {{0x0c, 0x42,0xa1,0x8e,0xe0,0xe6}};
static uint32_t client_ip_addr = RTE_IPV4(192, 168, 2, 53);

/* the server side */
static struct rte_ether_addr server_ether_addr =
    {{0xe4, 0x1d, 0x2d, 0xf2, 0x9e, 0x5c}};
static uint32_t server_ip_addr = RTE_IPV4(192, 168, 2, 52);

static uint16_t cfg_udp_src = 1000;
static uint16_t cfg_udp_dst = 1001;

#define MAX_PKT_BURST 32
#define MEMPOOL_CACHE_SIZE 128


int RTE_LOGTYPE_PINGPONG;

struct rte_mempool *pingpong_pktmbuf_pool = NULL;

static volatile bool force_quit;

/* enabled port */
static uint16_t portid = 0;
/* number of packets */
static uint64_t nb_pkts = 100;
/* server mode */
static bool server_mode = false;

static struct rte_eth_dev_tx_buffer *tx_buffer;

// static struct rte_eth_conf port_conf = {
//     .rxmode = {
//         .split_hdr_size = 0,
//     },
//     .txmode = {
//         .mq_mode = ETH_MQ_TX_NONE,
//     },
// };


print_stats(void)
{
	struct rte_eth_link link;
	uint64_t total_packets_dropped, total_packets_tx, total_packets_rx;
	uint16_t portid;

	total_packets_dropped = 0;
	total_packets_tx = 0;
	total_packets_rx = 0;

	const char clr[] = { 27, '[', '2', 'J', '\0' };
	const char topLeft[] = { 27, '[', '1', ';', '1', 'H','\0' };
	int link_get_err;

		/* Clear screen and move to top left */
	printf("%s%s", clr, topLeft);

	printf("\nPort statistics ====================================");

	for (portid = 0; portid <= 0; portid++) {
		/* skip ports that are not enabled */
		if ((lsi_enabled_port_mask & (1 << portid)) == 0)
			continue;

		memset(&link, 0, sizeof(link));
		link_get_err = rte_eth_link_get_nowait(portid, &link);
		printf("\nStatistics for port %u ------------------------------"
			   "\nLink status: %25s"
			   "\nLink speed: %26s"
			   "\nLink duplex: %25s"
			   "\nPackets sent: %24"PRIu64
			   "\nPackets received: %20"PRIu64
			   "\nPackets dropped: %21"PRIu64,
			   portid,
			   link_get_err < 0 ? "Link get failed" :
			   (link.link_status ? "Link up" : "Link down"),
			   link_get_err < 0 ? "0" :
			   rte_eth_link_speed_to_str(link.link_speed),
			   link_get_err < 0 ? "Link get failed" :
			   (link.link_duplex == ETH_LINK_FULL_DUPLEX ? \
					"full-duplex" : "half-duplex"),
			   port_statistics[portid].tx,
			   port_statistics[portid].rx,
			   port_statistics[portid].dropped);

		total_packets_dropped += port_statistics[portid].dropped;
		total_packets_tx += port_statistics[portid].tx;
		total_packets_rx += port_statistics[portid].rx;
	}
	printf("\nAggregate statistics ==============================="
		   "\nTotal packets sent: %18"PRIu64
		   "\nTotal packets received: %14"PRIu64
		   "\nTotal packets dropped: %15"PRIu64,
		   total_packets_tx,
		   total_packets_rx,
		   total_packets_dropped);
	printf("\n====================================================\n");

	fflush(stdout);
}

/**
 * It will be called as the callback for specified port after a LSI interrupt
 * has been fully handled. This callback needs to be implemented carefully as
 * it will be called in the interrupt host thread which is different from the
 * application main thread.
 *
 * @param port_id
 *  Port id.
 * @param type
 *  event type.
 * @param param
 *  Pointer to(address of) the parameters.
 *
 * @return
 *  int.
 */
static int
lsi_event_callback(uint16_t port_id, enum rte_eth_event_type type, void *param,
		    void *ret_param)
{
	struct rte_eth_link link;
	int ret;
	char link_status_text[RTE_ETH_LINK_MAX_STR_LEN];

	RTE_SET_USED(param);
	RTE_SET_USED(ret_param);

	printf("\n\nIn registered callback...\n");
	printf("Event type: %s\n", type == RTE_ETH_EVENT_INTR_LSC ? "LSC interrupt" : "unknown event");
	ret = rte_eth_link_get_nowait(port_id, &link);
	if (ret < 0) {
		printf("Failed link get on port %d: %s\n",
		       port_id, rte_strerror(-ret));
		return ret;
	}
	rte_eth_link_to_str(link_status_text, sizeof(link_status_text), &link);
	printf("Port %d %s\n\n", port_id, link_status_text);

	return 0;
}

/* Per-port statistics struct */
struct pingpong_port_statistics
{
    uint64_t tx;
    uint64_t rx;
    uint64_t *rtt;
    uint64_t dropped;
} __rte_cache_aligned;
struct pingpong_port_statistics ping_port_statistics;

static inline void
initlize_ping_port_statistics(void)
{
    ping_port_statistics.tx = 0;
    ping_port_statistics.rx = 0;
    ping_port_statistics.rtt = malloc(sizeof(uint64_t) * nb_pkts);
    ping_port_statistics.dropped = 0;
}

static inline void
destroy_port_statistics(void)
{
    free(ping_port_statistics.rtt);
}

static inline void
print_port_statistics(void)
{
    uint64_t i, min_rtt, max_rtt, sum_rtt, avg_rtt;
    rte_log(RTE_LOG_INFO, RTE_LOGTYPE_PINGPONG, "====== hello-world statistics =====\n");
    rte_log(RTE_LOG_INFO, RTE_LOGTYPE_PINGPONG, "tx %" PRIu64 " hello packets\n", ping_port_statistics.tx);
    rte_log(RTE_LOG_INFO, RTE_LOGTYPE_PINGPONG, "rx %" PRIu64 " hello packets\n", ping_port_statistics.rx);
    rte_log(RTE_LOG_INFO, RTE_LOGTYPE_PINGPONG, "dopped %" PRIu64 " packets\n", ping_port_statistics.dropped);

    min_rtt = 999999999;
    max_rtt = 0;
    sum_rtt = 0;
    avg_rtt = 0;
    for (i = 0; i < nb_pkts; i++)
    {
        sum_rtt += ping_port_statistics.rtt[i];
        if (ping_port_statistics.rtt[i] < min_rtt)
            min_rtt = ping_port_statistics.rtt[i];
        if (ping_port_statistics.rtt[i] > max_rtt)
            max_rtt = ping_port_statistics.rtt[i];
    }
    avg_rtt = sum_rtt / nb_pkts;
    rte_log(RTE_LOG_INFO, RTE_LOGTYPE_PINGPONG, "min rtt: %" PRIu64 " us\n", min_rtt);
    rte_log(RTE_LOG_INFO, RTE_LOGTYPE_PINGPONG, "max rtt: %" PRIu64 " us\n", max_rtt);
    rte_log(RTE_LOG_INFO, RTE_LOGTYPE_PINGPONG, "average rtt: %" PRIu64 " us\n", avg_rtt);
    rte_log(RTE_LOG_INFO, RTE_LOGTYPE_PINGPONG, "=================================\n");
}

static const char short_options[] =
    "p:" /* portmask */
    "n:" /* number of packets */
    "s"  /* server mode */
    ;

#define IP_DEFTTL 64 /* from RFC 1340. */
#define IP_VERSION 0x40
#define IP_HDRLEN 0x05 /* default IP header length == five 32-bits words. */
#define IP_VHL_DEF (IP_VERSION | IP_HDRLEN)
#define IP_ADDR_FMT_SIZE 15

static inline void
ip_format_addr(char *buf, uint16_t size,
               const uint32_t ip_addr)
{
    snprintf(buf, size, "%" PRIu8 ".%" PRIu8 ".%" PRIu8 ".%" PRIu8 "\n",
             (uint8_t)((ip_addr >> 24) & 0xff),
             (uint8_t)((ip_addr >> 16) & 0xff),
             (uint8_t)((ip_addr >> 8) & 0xff),
             (uint8_t)((ip_addr)&0xff));
}

static inline uint32_t
reverse_ip_addr(const uint32_t ip_addr)
{
    return RTE_IPV4((uint8_t)(ip_addr & 0xff),
                (uint8_t)((ip_addr >> 8) & 0xff),
                (uint8_t)((ip_addr >> 16) & 0xff),
                (uint8_t)((ip_addr >> 24) & 0xff));
}

static void
signal_handler(int signum)
{
    if (signum == SIGINT || signum == SIGTERM)
    {
        rte_log(RTE_LOG_INFO, RTE_LOGTYPE_PINGPONG, "\n\nSignal %d received, preparing to exit...\n", signum);
        force_quit = true;
    }
}
/* display usage */
static void
pingpong_usage(const char *prgname)
{
    printf("%s [EAL options] --"
           "\t-p PORTID: port to configure\n"
           "\t\t\t\t\t-n PACKETS: number of packets\n"
           "\t\t\t\t\t-s: enable server mode\n",
           prgname);
}

/* Parse the argument given in the command line of the application */
static int
pingpong_parse_args(int argc, char **argv)
{
    int opt, ret;
    char *prgname = argv[0];

    while ((opt = getopt(argc, argv, short_options)) != EOF)
    {
        switch (opt)
        {
        /* port id */
        case 'p':
            portid = (uint16_t)strtol(optarg, NULL, 10);
            break;

        case 'n':
            nb_pkts = (uint64_t)strtoull(optarg, NULL, 10);
            break;

        case 's':
            server_mode = true;
            break;

        default:
            pingpong_usage(prgname);
            return -1;
        }
    }

    if (optind >= 0)
        argv[optind - 1] = prgname;

    ret = optind - 1;
    optind = 1; /* reset getopt lib */
    return ret;
}

static inline uint16_t
ip_sum(const unaligned_uint16_t *hdr, int hdr_len)
{
    uint32_t sum = 0;

    while (hdr_len > 1)
    {
        sum += *hdr++;
        if (sum & 0x80000000)
            sum = (sum & 0xFFFF) + (sum >> 16);
        hdr_len -= 2;
    }

    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);

    return ~sum;
}
static int sleep_until_rx_interrupt(int num, int lcore)
{
	/*
	 * we want to track when we are woken up by traffic so that we can go
	 * back to sleep again without log spamming. Avoid cache line sharing
	 * to prevent threads stepping on each others' toes.
	 */
	static struct {
		bool wakeup;
	} __rte_cache_aligned status[RTE_MAX_LCORE];
	struct rte_epoll_event event[num];
	int n, i;
	uint16_t port_id;
	uint8_t queue_id;
	void *data;

	if (status[lcore].wakeup) {
		RTE_LOG(INFO,RTE_LOGTYPE_PINGPONG,
				"lcore %u sleeps until interrupt triggers\n",
				rte_lcore_id());
	}

	n = rte_epoll_wait(RTE_EPOLL_PER_THREAD, event, num, 10);
	for (i = 0; i < n; i++) {
		data = event[i].epdata.data;
		port_id = ((uintptr_t)data) >> CHAR_BIT;
		queue_id = ((uintptr_t)data) &
			RTE_LEN2MASK(CHAR_BIT, uint8_t);
		RTE_LOG(INFO, RTE_LOGTYPE_PINGPONG,
			"lcore %u is waked up from rx interrupt on"
			" port %d queue %d\n",
			rte_lcore_id(), port_id, queue_id);
	}
	status[lcore].wakeup = n != 0;

	return 0;
}
/* construct ping packet */
static struct rte_mbuf *
contruct_ping_packet(void)
{
    unsigned pkt_size = 1000U;
    struct rte_mbuf *pkt;
    struct rte_ether_hdr *eth_hdr;
    struct rte_ipv4_hdr *ip_hdr;
    struct rte_udp_hdr *udp_hdr;

    pkt = rte_pktmbuf_alloc(pingpong_pktmbuf_pool);
    if (!pkt)
        rte_log(RTE_LOG_ERR, RTE_LOGTYPE_PINGPONG, "fail to alloc mbuf for packet\n");

    pkt->data_len = pkt_size;
    pkt->next = NULL;

    /* Initialize Ethernet header. */
    eth_hdr = rte_pktmbuf_mtod(pkt, struct rte_ether_hdr *);
    rte_ether_addr_copy(&server_ether_addr, &eth_hdr->d_addr);
    rte_ether_addr_copy(&client_ether_addr, &eth_hdr->s_addr);
    eth_hdr->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

    /* Initialize IP header. */
    ip_hdr = (struct rte_ipv4_hdr *)(eth_hdr + 1);
    memset(ip_hdr, 0, sizeof(*ip_hdr));
    ip_hdr->version_ihl = IP_VHL_DEF;
    ip_hdr->type_of_service = 0;
    ip_hdr->fragment_offset = 0;
    ip_hdr->time_to_live = IP_DEFTTL;
    ip_hdr->next_proto_id = IPPROTO_UDP;
    ip_hdr->packet_id = 0;
    ip_hdr->src_addr = rte_cpu_to_be_32(client_ip_addr);
    ip_hdr->dst_addr = rte_cpu_to_be_32(server_ip_addr);
    ip_hdr->total_length = rte_cpu_to_be_16(pkt_size -
                                            sizeof(*eth_hdr));
    ip_hdr->hdr_checksum = ip_sum((unaligned_uint16_t *)ip_hdr,
                                  sizeof(*ip_hdr));

    /* Initialize UDP header. */
    udp_hdr = (struct rte_udp_hdr *)(ip_hdr + 1);
    udp_hdr->src_port = rte_cpu_to_be_16(cfg_udp_src);
    udp_hdr->dst_port = rte_cpu_to_be_16(cfg_udp_dst);
    udp_hdr->dgram_cksum = 0; /* No UDP checksum. */
    udp_hdr->dgram_len = rte_cpu_to_be_16(pkt_size -
                                          sizeof(*eth_hdr) -
                                          sizeof(*ip_hdr));
    pkt->nb_segs = 1;
    pkt->pkt_len = pkt_size;
    pkt->l2_len = sizeof(struct rte_ether_hdr);
    pkt->l3_len = sizeof(struct rte_ipv4_hdr);
    pkt->l4_len = sizeof(struct rte_udp_hdr);

    return pkt;
}

/* main ping loop */
/*Only transmit do not receive*/
static void
ping_main_loop(void)
{
    unsigned lcore_id;
    uint64_t ping_tsc, pong_tsc, diff_tsc, rtt_us;
    unsigned i, nb_rx, nb_tx;
    const uint64_t tsc_hz = rte_get_tsc_hz();
    uint64_t pkt_idx = 0;
    struct rte_mbuf *pkts_burst[MAX_PKT_BURST];
    struct rte_mbuf *m = NULL;
    struct rte_ether_hdr *eth_hdr;
    struct rte_vlan_hdr *vlan_hdr;
    struct rte_ipv4_hdr *ip_hdr;
    uint16_t eth_type;
    int l2_len;
    bool received_pong = false;

    lcore_id = rte_lcore_id();

    rte_log(RTE_LOG_INFO, RTE_LOGTYPE_PINGPONG, "entering ping loop on lcore %u\n", lcore_id);

    m = contruct_ping_packet();
    if (m == NULL)
        rte_log(RTE_LOG_ERR, RTE_LOGTYPE_PINGPONG, "construct packet failed\n");

    for (pkt_idx = 0; pkt_idx < nb_pkts && !force_quit; pkt_idx++)
    {
	    rte_log(RTE_LOG_INFO, RTE_LOGTYPE_PINGPONG, 
                "Sending packet %u on lcore %u\n", pkt_idx, lcore_id);
        ping_tsc = rte_rdtsc();
        /* do ping */
        nb_tx = rte_eth_tx_burst(portid, 0, &m, 1);
        

        if (nb_tx)
            ping_port_statistics.tx += nb_tx;
        
        received_pong = false;
        //570
    }
    /* print port statistics when ping main loop finishes */
    print_port_statistics();
}

/* main pong loop */
/*Only reveive ping packets do not transmit*/
static void
pong_main_loop(void)
{
    unsigned lcore_id;
    unsigned i, nb_rx, nb_tx;
    struct rte_mbuf *pkts_burst[MAX_PKT_BURST];
    struct rte_mbuf *m = NULL;
    struct rte_ether_hdr *eth_hdr;
    struct rte_vlan_hdr *vlan_hdr;
    struct rte_ipv4_hdr *ip_hdr;
    uint16_t eth_type;
    int l2_len;

    lcore_id = rte_lcore_id();

    rte_log(RTE_LOG_INFO, RTE_LOGTYPE_PINGPONG, "entering pong loop on lcore %u\n", lcore_id);
    rte_log(RTE_LOG_INFO, RTE_LOGTYPE_PINGPONG, "waiting ping packets\n");

    /* wait for pong */
    rte_eth_dev_rx_intr_enable(0, 0);
    rte_log(RTE_LOG_INFO, RTE_LOGTYPE_PINGPONG, "Enabled RX Interrupt\n");
    while (!force_quit)
    {   sleep_until_rx_interrupt(
							1,
							lcore_id);
        nb_rx = rte_eth_rx_burst(portid, 0, pkts_burst, MAX_PKT_BURST);
        if (nb_rx)
        {
            for (i = 0; i < nb_rx; i++)
            {
                
                m = pkts_burst[i];

                eth_hdr = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
                eth_type = rte_cpu_to_be_16(eth_hdr->ether_type);
                l2_len = sizeof(struct rte_ether_hdr);
                if (eth_type == RTE_ETHER_TYPE_VLAN)
                {
                    vlan_hdr = (struct rte_vlan_hdr *)((char *)eth_hdr + sizeof(struct rte_ether_hdr));
                    l2_len += sizeof(struct rte_vlan_hdr);
                    eth_type = rte_be_to_cpu_16(vlan_hdr->eth_proto);
                }
                if (eth_type == RTE_ETHER_TYPE_IPV4)
                {
                    ip_hdr = (struct rte_ipv4_hdr *)((char *)eth_hdr + l2_len);
                    /* compare mac & ip, confirm it is a ping packet */
                    if (rte_is_same_ether_addr(&eth_hdr->d_addr, &server_ether_addr) &&
                        (reverse_ip_addr(ip_hdr->dst_addr) == server_ip_addr))
                    {
                        rte_log(RTE_LOG_INFO, RTE_LOGTYPE_PINGPONG, "Received Packet from client\n");
                        ping_port_statistics.rx += 1;
                        /* do pong */
                        // rte_ether_addr_copy(&server_ether_addr, &eth_hdr->s_addr);
                        // rte_ether_addr_copy(&client_ether_addr, &eth_hdr->d_addr);

                        // ip_hdr->src_addr = rte_cpu_to_be_32(server_ip_addr);
                        // ip_hdr->dst_addr = rte_cpu_to_be_32(client_ip_addr);

                        // nb_tx = rte_eth_tx_burst(portid, 0, &m, 1);
                        // if (nb_tx)
                        //     port_statistics.tx += nb_tx;
                    }
                }
            }
        }

        //529
    }
    print_port_statistics();
}

static int
ping_launch_one_lcore(__attribute__((unused)) void *dummy)
{
    ping_main_loop();
    return 0;
}

static int
pong_launch_one_lcore(__attribute__((unused)) void *dummy)
{
    pong_main_loop();
    return 0;
}

int main(int argc, char **argv)
{
    int ret;
    unsigned int nb_mbufs;
    unsigned int nb_lcores;
    unsigned int lcore_id;
    struct lcore_queue_conf *qconf;
	uint16_t nb_ports;
	uint16_t portid, portid_last = 0;
	unsigned rx_lcore_id;
	unsigned nb_ports_in_mask = 0;

    /* init EAL */
    ret = rte_eal_init(argc, argv);
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "Invalid EAL arguments\n");
    argc -= ret;
    argv += ret;

    /* init log */
    RTE_LOGTYPE_PINGPONG = rte_log_register(APP);
    ret = rte_log_set_level(RTE_LOGTYPE_PINGPONG, PINGPONG_LOG_LEVEL);
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "Set log level to %u failed\n", PINGPONG_LOG_LEVEL);
    
    nb_lcores = rte_lcore_count();
    if (nb_lcores < 2)
        rte_exit(EXIT_FAILURE, "Number of CPU cores should be no less than 2.");

    nb_ports = rte_eth_dev_count_avail();
    if (nb_ports == 0)
        rte_exit(EXIT_FAILURE, "No Ethernet ports, bye...\n");

    rte_log(RTE_LOG_DEBUG, RTE_LOGTYPE_PINGPONG, "%u port(s) available\n", nb_ports);

    /* parse application arguments (after the EAL ones) */
    ret = pingpong_parse_args(argc, argv);
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "Invalid pingpong arguments\n");
    rte_log(RTE_LOG_DEBUG, RTE_LOGTYPE_PINGPONG, "Enabled port: %u\n", portid);
    if (portid > nb_ports - 1)
        rte_exit(EXIT_FAILURE, "Invalid port id %u, port id should be in range [0, %u]\n", portid, nb_ports - 1);

    force_quit = false;
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    nb_mbufs = RTE_MAX((unsigned int)(nb_ports * (nb_rxd + nb_txd + MAX_PKT_BURST + MEMPOOL_CACHE_SIZE)), 8192U);
    pingpong_pktmbuf_pool = rte_pktmbuf_pool_create("mbuf_pool", nb_mbufs,
                                                    MEMPOOL_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE,
                                                    rte_socket_id());
    if (pingpong_pktmbuf_pool == NULL)
        rte_exit(EXIT_FAILURE, "Cannot init mbuf pool\n");

    struct rte_eth_rxconf rxq_conf;
    struct rte_eth_txconf txq_conf;
    struct rte_eth_conf local_port_conf = port_conf;
    struct rte_eth_dev_info dev_info;

    rte_log(RTE_LOG_DEBUG, RTE_LOGTYPE_PINGPONG, "Initializing port %u...\n", portid);
    fflush(stdout);

    /* init port */
    rte_eth_dev_info_get(portid, &dev_info);
    if (dev_info.tx_offload_capa & DEV_TX_OFFLOAD_MBUF_FAST_FREE)
        local_port_conf.txmode.offloads |=
            DEV_TX_OFFLOAD_MBUF_FAST_FREE;

    ret = rte_eth_dev_configure(portid, 1, 1, &local_port_conf);
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "Cannot configure device: err=%d, port=%u\n",
                 ret, portid);

    // ret = rte_eth_dev_adjust_nb_rx_tx_desc(portid, &nb_rxd,
    //                                        &nb_txd);
    // if (ret < 0)
    //     rte_exit(EXIT_FAILURE,
    //              "Cannot adjust number of descriptors: err=%d, port=%u\n",
    //              ret, portid);

    /* register lsi interrupt callback, need to be after
		 * rte_eth_dev_configure(). if (intr_conf.lsc == 0), no
		 * lsc interrupt will be present, and below callback to
		 * be registered will never be called.
		 */
	//rte_eth_dev_callback_register(0,
	//	RTE_ETH_EVENT_INTR_LSC, lsi_event_callback, NULL);
    

	

    /* init one RX queue */
    fflush(stdout);
    rxq_conf = dev_info.default_rxconf;

    rxq_conf.offloads = local_port_conf.rxmode.offloads;
    ret = rte_eth_rx_queue_setup(portid, 0, nb_rxd,
                                 rte_eth_dev_socket_id(portid),
                                 &rxq_conf,
                                 pingpong_pktmbuf_pool);
    
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "rte_eth_rx_queue_setup:err=%d, port=%u\n",
                 ret, portid);

    /*Register event*/
    ret = rte_eth_dev_rx_intr_ctl_q(portid,0,
						RTE_EPOLL_PER_THREAD,
						RTE_INTR_EVENT_ADD,
						(void *)((uintptr_t)data));
    //
    

    /* init one TX queue on each port */
    fflush(stdout);
    txq_conf = dev_info.default_txconf;
    txq_conf.offloads = local_port_conf.txmode.offloads;
    ret = rte_eth_tx_queue_setup(portid, 0, nb_txd,
                                 rte_eth_dev_socket_id(portid),
                                 &txq_conf);
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "rte_eth_tx_queue_setup:err=%d, port=%u\n",
                 ret, portid);

    /* Initialize TX buffers */
    tx_buffer = rte_zmalloc_socket("tx_buffer",
                                   RTE_ETH_TX_BUFFER_SIZE(MAX_PKT_BURST), 0,
                                   rte_eth_dev_socket_id(portid));
    if (tx_buffer == NULL)
        rte_exit(EXIT_FAILURE, "Cannot allocate buffer for tx on port %u\n",
                 portid);

    rte_eth_tx_buffer_init(tx_buffer, MAX_PKT_BURST);

    ret = rte_eth_tx_buffer_set_err_callback(tx_buffer,
                                             rte_eth_tx_buffer_count_callback,
                                             &ping_port_statistics.dropped);
    if (ret < 0)
        rte_exit(EXIT_FAILURE,
                 "Cannot set error callback for tx buffer on port %u\n",
                 portid);

    /* Start device */
    ret = rte_eth_dev_start(portid);
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "rte_eth_dev_start:err=%d, port=%u\n",
                 ret, portid);
    
    ret = rte_eth_promiscuous_enable(portid);
	if (ret != 0)
		rte_exit(EXIT_FAILURE,
			"rte_eth_promiscuous_enable: err=%s, port=%u\n",
			rte_strerror(-ret), portid);


    /* initialize port stats */
    initlize_ping_port_statistics();

    rte_log(RTE_LOG_DEBUG, RTE_LOGTYPE_PINGPONG, "Initilize port %u done.\n", portid);

    lcore_id = rte_get_next_lcore(0, true, false);

    ret = 0;
    if (server_mode)
    {
        rte_eal_remote_launch(pong_launch_one_lcore, NULL, lcore_id);
    }
    else
    {
        rte_eal_remote_launch(ping_launch_one_lcore, NULL, lcore_id);
    }

    if (rte_eal_wait_lcore(lcore_id) < 0)
    {
        ret = -1;
    }

    rte_eth_dev_stop(portid);
    rte_eth_dev_close(portid);
    destroy_port_statistics();
    rte_log(RTE_LOG_DEBUG, RTE_LOGTYPE_PINGPONG, "Bye.\n");

    return 0;
}


    


// ------
    // /* wait for pong */
    //     while (!received_pong && !force_quit)
    //     {
	//     rte_log(RTE_LOG_INFO, RTE_LOGTYPE_PINGPONG, "Waiting pong on lcore %u\n", lcore_id);
    //         nb_rx = rte_eth_rx_burst(portid, 0, pkts_burst, MAX_PKT_BURST);
    //         pong_tsc = rte_rdtsc();
    //         if (nb_rx)
    //         {
    //             /* only 1 packet expected */
    //             if (nb_rx > 1)
    //                 rte_log(RTE_LOG_WARNING, RTE_LOGTYPE_PINGPONG, "%u packets received, 1 expected.\n", nb_rx);

    //             for (i = 0; i < nb_rx; i++)
    //             {
    //                 m = pkts_burst[i];

    //                 eth_hdr = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
    //                 eth_type = rte_cpu_to_be_16(eth_hdr->ether_type);
    //                 l2_len = sizeof(struct rte_ether_hdr);
    //                 if (eth_type == RTE_ETHER_TYPE_VLAN)
    //                 {
    //                     vlan_hdr = (struct rte_vlan_hdr *)((char *)eth_hdr + sizeof(struct rte_ether_hdr));
    //                     l2_len += sizeof(struct rte_vlan_hdr);
    //                     eth_type = rte_be_to_cpu_16(vlan_hdr->eth_proto);
    //                 }
    //                 if (eth_type == RTE_ETHER_TYPE_IPV4)
    //                 {
    //                     ip_hdr = (struct rte_ipv4_hdr *)((char *)eth_hdr + l2_len);
    //                     /* compare mac & ip, confirm it is a pong packet */
    //                     if (rte_is_same_ether_addr(&eth_hdr->d_addr, &client_ether_addr) &&
    //                         reverse_ip_addr(ip_hdr->dst_addr) == client_ip_addr)
    //                     {
    //                         diff_tsc = pong_tsc - ping_tsc;
    //                         rtt_us = diff_tsc * US_PER_S / tsc_hz;
    //                         port_statistics.rtt[port_statistics.rx] = rtt_us;

    //                         rte_ether_addr_copy(&client_ether_addr, &eth_hdr->s_addr);
    //                         rte_ether_addr_copy(&server_ether_addr, &eth_hdr->d_addr);

    //                         ip_hdr->src_addr = rte_cpu_to_be_32(client_ip_addr);
    //                         ip_hdr->dst_addr = rte_cpu_to_be_32(server_ip_addr);

    //                         received_pong = true;

    //                         port_statistics.rx += 1;

    //                         break;
    //                     }
    //                 }
    //             }
    //         }
    //     }
    

