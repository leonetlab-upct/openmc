/* openmc_rs.c
 *
 * OpenMC processing host with Reed–Solomon FEC (Phil Karn's libfec).
 *
 * - Intercepts outgoing IPv4 packets via NFQUEUE (queue 1).
 * - Groups packets in generations of K_DATA = 8 packets.
 * - For each packet:
 *     * Immediately sends the original IP packet as a "data fragment"
 *       (type=0) with index 0..7, splitting across two interfaces
 *       (term1gs1, term1gs2) like openmc_XOR.c.
 *     * Stores a copy in an internal generation buffer.
 * - Once a generation accumulates K_DATA packets, it:
 *     * Computes r Reed–Solomon parity packets column-wise.
 *     * Sends r "parity fragments" (type=1, index 8..7+r).
 *
 * This keeps latency for the original packets minimal (like XOR),
 * while still providing RS FEC as a background redundancy layer.
 *
 * Build:
 *   gcc -O2 -Wall -o openmc-rs openmc_rs.c -lnetfilter_queue -lfec
 *
 * Run:
 *   ./openmc-rs [parity_symbols]
 *
 * Where:
 *   - parity_symbols (r) is optional, default = 2.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <limits.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <getopt.h>

#include <arpa/inet.h>
#include <netinet/ip.h>
#include <net/if.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <linux/netfilter.h>              // for NF_ACCEPT, NF_DROP
#include <libnetfilter_queue/libnetfilter_queue.h>

#include <fec.h>                          // Phil Karn's libfec

#include <time.h>
#include <signal.h>

/* Runtime configuration with bLEO-compatible defaults. */
#define OPENMC_VERSION       "0.1.1"
#define DEFAULT_IFACE_A      "term1gs1"
#define DEFAULT_IFACE_B      "term1gs2"
#define DEFAULT_PEER_A       "10.102.99.1"
#define DEFAULT_PEER_B       "10.102.100.1"
#define DEFAULT_PEER_PORT    5000
#define DEFAULT_NFQUEUE_NUM  1

#define MAX_PKT_LEN    1500
#define K_DATA         8
#define MAX_PARITY     32
#define DEFAULT_PARITY 2

/*
 * Experiment-6b: NFQUEUE hardening for offered-rate sweeps.
 *
 * The larger queue and Netlink receive buffer reduce the probability that
 * short user-space processing stalls are converted into ENOBUFS failures.
 * The effective SO_RCVBUF is logged at startup for reproducibility.
 */
#define NFQUEUE_MAXLEN         65535
#define NFQUEUE_RCVBUF_BYTES   (4 * 1024 * 1024)

struct openmc_rs_config {
    char iface_a[IFNAMSIZ];
    char iface_b[IFNAMSIZ];
    char peer_a[INET_ADDRSTRLEN];
    char peer_b[INET_ADDRSTRLEN];
    uint16_t peer_port;
    uint16_t block_size;
    uint16_t repairs;
    uint16_t nfqueue_num;
    char run_id[128];
    char summary_output[PATH_MAX];
};

static struct openmc_rs_config runtime_cfg = {
    .iface_a = DEFAULT_IFACE_A,
    .iface_b = DEFAULT_IFACE_B,
    .peer_a = DEFAULT_PEER_A,
    .peer_b = DEFAULT_PEER_B,
    .peer_port = DEFAULT_PEER_PORT,
    .block_size = K_DATA,
    .repairs = DEFAULT_PARITY,
    .nfqueue_num = DEFAULT_NFQUEUE_NUM,
    .run_id = "",
    .summary_output = ""
};


static int parse_u16(const char *text, unsigned min_value, unsigned max_value,
                     uint16_t *out)
{
    char *end = NULL;
    errno = 0;
    unsigned long value = strtoul(text, &end, 10);
    if (errno || !text || !text[0] || (end && *end) ||
        value < min_value || value > max_value)
        return -1;
    *out = (uint16_t)value;
    return 0;
}

static int set_string(char *dst, size_t size, const char *value,
                      const char *option)
{
    if (!value || strlen(value) >= size) {
        fprintf(stderr, "[OpenMC RS] Invalid %s value\n", option);
        return -1;
    }
    snprintf(dst, size, "%s", value);
    return 0;
}

static void usage(FILE *stream, const char *prog)
{
    fprintf(stream,
        "Usage: %s [legacy_repairs] [options]\\n\\n"
        "  --iface-a IFACE       Path A interface (default: %s)\\n"
        "  --iface-b IFACE       Path B interface (default: %s)\\n"
        "  --peer-a ADDRESS      Path A receiver IPv4 address (default: %s)\\n"
        "  --peer-b ADDRESS      Path B receiver IPv4 address (default: %s)\\n"
        "  --peer-port PORT      Receiver UDP port (default: %u)\\n"
        "  --block-size K        Source symbols per block (1..%d; default: %d)\\n"
        "  --repairs R           Repair symbols (0..%d; default: %d)\\n"
        "  --policy default      RS supports only the default policy in v0.1.1\\n"
        "  --nfqueue-num N       NFQUEUE number (default: %u)\\n"
        "  --run-id ID            Experimental run identifier\\n"
        "  --summary-output PATH  Write structured run summary CSV\\n"
        "  -h, --help            Show this help\\n"
        "  -V, --version         Show version\\n",
        prog, DEFAULT_IFACE_A, DEFAULT_IFACE_B, DEFAULT_PEER_A,
        DEFAULT_PEER_B, DEFAULT_PEER_PORT, K_DATA, K_DATA,
        MAX_PARITY, DEFAULT_PARITY, DEFAULT_NFQUEUE_NUM);
}

static int parse_runtime_options(int argc, char **argv)
{
    enum {
        OPT_IFACE_A = 1000, OPT_IFACE_B, OPT_PEER_A, OPT_PEER_B,
        OPT_PEER_PORT, OPT_BLOCK_SIZE, OPT_REPAIRS, OPT_POLICY,
        OPT_NFQUEUE_NUM, OPT_RUN_ID, OPT_SUMMARY_OUTPUT
    };
    static const struct option opts[] = {
        {"iface-a", required_argument, NULL, OPT_IFACE_A},
        {"iface-b", required_argument, NULL, OPT_IFACE_B},
        {"peer-a", required_argument, NULL, OPT_PEER_A},
        {"peer-b", required_argument, NULL, OPT_PEER_B},
        {"peer-port", required_argument, NULL, OPT_PEER_PORT},
        {"block-size", required_argument, NULL, OPT_BLOCK_SIZE},
        {"repairs", required_argument, NULL, OPT_REPAIRS},
        {"policy", required_argument, NULL, OPT_POLICY},
        {"nfqueue-num", required_argument, NULL, OPT_NFQUEUE_NUM},
        {"run-id", required_argument, NULL, OPT_RUN_ID},
        {"summary-output", required_argument, NULL, OPT_SUMMARY_OUTPUT},
        {"help", no_argument, NULL, 'h'},
        {"version", no_argument, NULL, 'V'},
        {NULL, 0, NULL, 0}
    };

    int first_option = 1;
    if (argc > 1 && argv[1][0] != '-') {
        if (parse_u16(argv[1], 0, MAX_PARITY, &runtime_cfg.repairs) != 0) {
            fprintf(stderr, "[OpenMC RS] Invalid legacy repairs value: %s\\n",
                    argv[1]);
            return -1;
        }
        first_option = 2;
    }
    optind = first_option;

    int c;
    while ((c = getopt_long(argc, argv, "hV", opts, NULL)) != -1) {
        uint16_t number;
        switch (c) {
        case OPT_IFACE_A:
            if (set_string(runtime_cfg.iface_a, sizeof(runtime_cfg.iface_a),
                           optarg, "--iface-a") != 0) return -1;
            break;
        case OPT_IFACE_B:
            if (set_string(runtime_cfg.iface_b, sizeof(runtime_cfg.iface_b),
                           optarg, "--iface-b") != 0) return -1;
            break;
        case OPT_PEER_A:
            if (set_string(runtime_cfg.peer_a, sizeof(runtime_cfg.peer_a),
                           optarg, "--peer-a") != 0) return -1;
            break;
        case OPT_PEER_B:
            if (set_string(runtime_cfg.peer_b, sizeof(runtime_cfg.peer_b),
                           optarg, "--peer-b") != 0) return -1;
            break;
        case OPT_PEER_PORT:
            if (parse_u16(optarg, 1, 65535, &number) != 0) return -1;
            runtime_cfg.peer_port = number;
            break;
        case OPT_BLOCK_SIZE:
            if (parse_u16(optarg, 1, K_DATA, &number) != 0) return -1;
            runtime_cfg.block_size = number;
            break;
        case OPT_REPAIRS:
            if (parse_u16(optarg, 0, MAX_PARITY, &number) != 0) return -1;
            runtime_cfg.repairs = number;
            break;
        case OPT_POLICY:
            if (strcmp(optarg, "default") != 0) {
                fprintf(stderr,
                        "[OpenMC RS] Unsupported policy '%s'; use default\\n",
                        optarg);
                return -1;
            }
            break;
        case OPT_NFQUEUE_NUM:
            if (parse_u16(optarg, 0, 65535, &number) != 0) return -1;
            runtime_cfg.nfqueue_num = number;
            break;
        case OPT_RUN_ID:
            if (set_string(runtime_cfg.run_id, sizeof(runtime_cfg.run_id),
                           optarg, "--run-id") != 0) return -1;
            break;
        case OPT_SUMMARY_OUTPUT:
            if (set_string(runtime_cfg.summary_output,
                           sizeof(runtime_cfg.summary_output),
                           optarg, "--summary-output") != 0) return -1;
            break;
        case 'h':
            usage(stdout, argv[0]);
            exit(EXIT_SUCCESS);
        case 'V':
            printf("OpenMC Reed--Solomon processing host %s\n",
                   OPENMC_VERSION);
            exit(EXIT_SUCCESS);
        default:
            return -1;
        }
    }

    if (optind != argc) {
        fprintf(stderr, "[OpenMC RS] Unexpected argument: %s\\n", argv[optind]);
        return -1;
    }

    if ((runtime_cfg.summary_output[0]) && !runtime_cfg.run_id[0]) {
        fprintf(stderr,
                "[OpenMC] --run-id is required when structured output is enabled\n");
        return -1;
    }

    struct in_addr address;
    if (inet_pton(AF_INET, runtime_cfg.peer_a, &address) != 1 ||
        inet_pton(AF_INET, runtime_cfg.peer_b, &address) != 1) {
        fprintf(stderr,
                "[OpenMC RS] Peer addresses must be valid IPv4 addresses\\n");
        return -1;
    }

    if (if_nametoindex(runtime_cfg.iface_a) == 0 ||
        if_nametoindex(runtime_cfg.iface_b) == 0) {
        fprintf(stderr, "[OpenMC RS] Interfaces not found: %s, %s\\n",
                runtime_cfg.iface_a, runtime_cfg.iface_b);
        return -1;
    }

    return 0;
}

/* --- Computational statistics --- */

struct stats {
    uint64_t pkts_intercepted;

    uint64_t pkts_forwarded;        // data + parity
    uint64_t pkts_original;         // SOLO data

    uint64_t bytes_forwarded;       // data + parity (payload real)
    uint64_t bytes_original;        // SOLO data

    uint64_t generations_completed;

    /* Encoding / decoding timing */
    uint64_t enc_calls;
    uint64_t enc_time_ns;

    uint64_t dec_calls;
    uint64_t dec_time_ns;

    /* NFQUEUE latency (GW only) */
    uint64_t nfq_pkts;
    uint64_t nfq_latency_ns;
};

static struct stats stats = {0};
static uint64_t t_start_ns = 0;
static uint64_t t_end_ns   = 0;

/* Wire header for each FEC fragment sent via UDP to the Edge Receiver */
struct fec_fragment_hdr {
    uint16_t gen_id;       // generation id
    uint8_t  index;        // 0..7 = data, 8..7+r = parity
    uint8_t  type;         // 0 = data, 1 = parity
    uint16_t original_len; // IP packet length in bytes (same for all in gen)
    uint16_t reserved;     // reserved, set to 0
} __attribute__((packed));

/* State for the current generation in the gateway */
struct generation_state {
    uint16_t gen_id;                        // generation id
    int      count;                         // how many data packets stored (0..K_DATA)
    int      original_len;                  // expected IP length (all must match)
    uint8_t  data[K_DATA * MAX_PKT_LEN];    // stored packets (for parity)
};

static struct generation_state cur_gen;
static int  parity_symbols = DEFAULT_PARITY; // r
static void *rs = NULL;                      // RS codec descriptor

static volatile sig_atomic_t pending_rs = 0;
static uint64_t rs_dropped = 0;
static struct generation_state pending_gen;

static void mark_generation_ready(const struct generation_state *g)
{
    if (pending_rs)
    {
        rs_dropped++;
        return;
    }

    pending_gen.gen_id        = g->gen_id;
    pending_gen.count         = g->count;
    pending_gen.original_len  = g->original_len;

    int T = g->original_len;
    memcpy(pending_gen.data, g->data, (size_t)K_DATA * (size_t)T);

    pending_rs = 1;
}


/* Two UDP sockets, one per interface, as in openmc_XOR.c */
static int sock_a = -1;
static int sock_b = -1;
static struct sockaddr_in gs_addr_a;
static struct sockaddr_in gs_addr_b;

static volatile sig_atomic_t stop_flag = 0;

/* --- Utilidades --- */

static inline uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static void die(const char *msg)
{
    perror(msg);
    exit(EXIT_FAILURE);
}

/* Crea socket UDP, sube buffer y lo vincula a una interfaz dada. */
static int create_udp_socket_bound(const char *iface_name)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) die("socket(AF_INET,SOCK_DGRAM)");

    int sndbuf = 4 * 1024 * 1024;
    if (setsockopt(s, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf)) < 0)
        perror("setsockopt(SO_SNDBUF)");

    if (setsockopt(s, SOL_SOCKET, SO_BINDTODEVICE,
                   iface_name, (socklen_t)strlen(iface_name)) < 0)
        die("setsockopt(SO_BINDTODEVICE)");

    return s;
}

/* Initializes the two UDP sockets and the Edge Receiver addresses. */
static void init_udp_sockets(void)
{
    sock_a = create_udp_socket_bound(runtime_cfg.iface_a);
    sock_b = create_udp_socket_bound(runtime_cfg.iface_b);

    memset(&gs_addr_a, 0, sizeof(gs_addr_a));
    gs_addr_a.sin_family = AF_INET;
    gs_addr_a.sin_port   = htons(runtime_cfg.peer_port);
    if (inet_pton(AF_INET, runtime_cfg.peer_a, &gs_addr_a.sin_addr) != 1) {
        fprintf(stderr, "Invalid peer A address: %s\n", runtime_cfg.peer_a);
        exit(EXIT_FAILURE);
    }

    memset(&gs_addr_b, 0, sizeof(gs_addr_b));
    gs_addr_b.sin_family = AF_INET;
    gs_addr_b.sin_port   = htons(runtime_cfg.peer_port);
    if (inet_pton(AF_INET, runtime_cfg.peer_b, &gs_addr_b.sin_addr) != 1) {
        fprintf(stderr, "Invalid peer B address: %s\n", runtime_cfg.peer_b);
        exit(EXIT_FAILURE);
    }

    fprintf(stderr,
            "[GW] UDP sockets ready:\n"
            "     %s -> %s:%u\n"
            "     %s -> %s:%u\n",
            runtime_cfg.iface_a, runtime_cfg.peer_a, runtime_cfg.peer_port,
            runtime_cfg.iface_b, runtime_cfg.peer_b, runtime_cfg.peer_port);
}

/* Initialize Reed–Solomon encoder for K_DATA data symbols and r parity symbols.
 *
 * We use a shortened RS code over GF(2^8):
 *   nn = 255 symbols
 *   nroots = r (parity symbols)
 *   pad = 255 - (K_DATA + r)
 * so the effective code length is K_DATA + r and the data length is K_DATA.
 */
static void init_rs_codec(int r)
{
    if (r <= 0 || r > MAX_PARITY) {
        fprintf(stderr, "Invalid parity symbols r=%d (1..%d)\n", r, MAX_PARITY);
        exit(EXIT_FAILURE);
    }

    int symsize = 8;
    int gfpoly  = 0x11d;
    int fcr     = 1;
    int prim    = 1;
    int nroots  = r;
    int pad     = 255 - (K_DATA + r);

    rs = init_rs_char(symsize, gfpoly, fcr, prim, nroots, pad);
    if (!rs) {
        fprintf(stderr, "init_rs_char() failed\n");
        exit(EXIT_FAILURE);
    }

    fprintf(stderr, "[GW] RS codec initialized: K=%d, r=%d, n=%d\n",
            K_DATA, r, K_DATA + r);
}

/* Envía un fragmento (data o parity) a través de una de las dos interfaces.
 *
 * Política sencilla:
 *   - Si type == 0 (data):
 *       * index par  -> interfaz A / GS A
 *       * index impar -> interfaz B / GS B
 *   - Si type == 1 (parity):
 *       * index-K_DATA par  -> interfaz A / GS A
 *       * index-K_DATA impar -> interfaz B / GS B
 */
static void send_fragment(uint16_t gen_id, uint8_t index, uint8_t type,
                          uint16_t len, const uint8_t *pkt)
{
    uint8_t buf[sizeof(struct fec_fragment_hdr) + MAX_PKT_LEN];

    struct fec_fragment_hdr hdr;
    hdr.gen_id       = htons(gen_id);
    hdr.index        = index;
    hdr.type         = type;
    hdr.original_len = htons(len);
    hdr.reserved     = 0;

    memcpy(buf, &hdr, sizeof(hdr));
    memcpy(buf + sizeof(hdr), pkt, len);

    int use_sock = sock_a;
    struct sockaddr_in *dest = &gs_addr_a;
    const char *iface_name = runtime_cfg.iface_a;
    const char *ip_str = runtime_cfg.peer_a;

    if (type == 0) {
        if (index & 1) {
            use_sock   = sock_b;
            dest       = &gs_addr_b;
            iface_name = runtime_cfg.iface_b;
            ip_str     = runtime_cfg.peer_b;
        }
    } else {
        int p = index - K_DATA;
        if (p & 1) {
            use_sock   = sock_b;
            dest       = &gs_addr_b;
            iface_name = runtime_cfg.iface_b;
            ip_str     = runtime_cfg.peer_b;
        }
    }

    ssize_t sent = sendto(use_sock, buf, sizeof(hdr) + len, 0,
                          (struct sockaddr *)dest, sizeof(*dest));

    if (sent < 0) {
        perror("sendto(udp_sock)");
    } else {
        stats.pkts_forwarded++;
        stats.bytes_forwarded += (sizeof(struct fec_fragment_hdr) + len);

        if (type == 0) {
            stats.pkts_original++;
            stats.bytes_original += len;
        }

        /*fprintf(stderr,
                "[GW] gen=%u idx=%u type=%u len=%u -> via %s to %s\n",
                gen_id, index, type, len, iface_name, ip_str);*/
    }

    (void)iface_name;
    (void)ip_str;
}

/* Flush the current generation:
 *
 * - If we have < K_DATA packets, there is nothing more to do:
 *   originals were already sent and we cannot compute FEC reliably.
 * - If we have exactly K_DATA and r > 0, we:
 *    * Build r parity packets using RS column-wise encoding.
 *    * Send the r parity packets.
 *
 * Original data packets are NOT resent here; they were already forwarded
 * immediately when they arrived.
 */
static void flush_generation(const struct generation_state *g)
{
    if (!g) return;

    if (g->count == K_DATA)
        stats.generations_completed++;

    if (g->count == 0)
        return;

    uint16_t gen_id = g->gen_id;
    int k   = g->count;
    int T   = g->original_len;

    (void)k;

    if (g->count == K_DATA && parity_symbols > 0 && rs != NULL) {
        uint64_t t0 = now_ns();

        uint8_t parity[MAX_PARITY][MAX_PKT_LEN];
        uint8_t data_vec[K_DATA];
        uint8_t parity_vec[MAX_PARITY];

        for (int j = 0; j < T; j++) {
            for (int i = 0; i < K_DATA; i++) {
                data_vec[i] = g->data[i * T + j];
            }

            encode_rs_char(rs, data_vec, parity_vec);

            for (int p = 0; p < parity_symbols; p++) {
                parity[p][j] = parity_vec[p];
            }
        }

        uint64_t t1 = now_ns();
        stats.enc_calls++;
        stats.enc_time_ns += (t1 - t0);

        for (int p = 0; p < parity_symbols; p++) {
            uint8_t idx = (uint8_t)(K_DATA + p);
            send_fragment(gen_id, idx, 1, (uint16_t)T, parity[p]);
        }
    }
}

/* Handle one outgoing IPv4 packet captured by NFQUEUE. */
static void handle_ip_packet(const uint8_t *pkt, int len, uint64_t t_nfq)
{
    if (len <= 0 || len > MAX_PKT_LEN) {
        fprintf(stderr, "[GW] Ignoring packet of invalid length %d\n", len);
        return;
    }

    if (cur_gen.count == 0) {
        cur_gen.gen_id++;
        cur_gen.original_len = len;
        fprintf(stderr, "[GW] Starting generation %u (len=%d)\n",
                cur_gen.gen_id, len);
    } else {
        if (len != cur_gen.original_len) {
            fprintf(stderr,
                    "[GW] Packet length changed within generation "
                    "(old=%d, new=%d). Flushing current generation.\n",
                    cur_gen.original_len, len);

            if (!pending_rs && cur_gen.count == K_DATA)
                mark_generation_ready(&cur_gen);

            cur_gen.gen_id++;
            cur_gen.original_len = len;
            cur_gen.count = 0;

            fprintf(stderr,
                    "[GW] Starting new generation %u (len=%d)\n",
                    cur_gen.gen_id, len);
        }
    }

    if (cur_gen.count >= K_DATA) {
        fprintf(stderr, "[GW] Generation %u overflow, forcing flush\n",
                cur_gen.gen_id);

        if (!pending_rs && cur_gen.count == K_DATA)
            mark_generation_ready(&cur_gen);

        cur_gen.gen_id++;
        cur_gen.original_len = len;
        cur_gen.count = 0;

        fprintf(stderr,
                "[GW] Starting new generation %u (len=%d)\n",
                cur_gen.gen_id, len);
    }

    int index = cur_gen.count;
    if (index >= K_DATA) {
        fprintf(stderr,
                "[GW] Internal error: index=%d>=K_DATA, dropping packet\n",
                index);
        return;
    }

    int T = cur_gen.original_len;
    uint8_t *dst = &cur_gen.data[index * T];
    memcpy(dst, pkt, T);
    cur_gen.count++;

    uint64_t t_send = now_ns();
    send_fragment(cur_gen.gen_id, (uint8_t)index, 0, (uint16_t)len, pkt);
    stats.nfq_pkts++;
    stats.nfq_latency_ns += (t_send - t_nfq);

    if (cur_gen.count == K_DATA) {
        mark_generation_ready(&cur_gen);
        cur_gen.count = 0;
        cur_gen.original_len = 0;
    }
}

/* NFQUEUE callback: called for each packet in queue 1 */
static int nfq_cb(struct nfq_q_handle *qh, struct nfgenmsg *nfmsg,
                  struct nfq_data *nfa, void *data)
{
    (void)nfmsg;
    (void)data;

    uint64_t t_nfq = now_ns();

    if (t_start_ns == 0)
        t_start_ns = t_nfq;

    t_end_ns = t_nfq;

    stats.pkts_intercepted++;

    struct nfqnl_msg_packet_hdr *ph = nfq_get_msg_packet_hdr(nfa);
    uint32_t id = 0;

    if (ph) {
        id = ntohl(ph->packet_id);
    }

    unsigned char *payload = NULL;
    int len = nfq_get_payload(nfa, &payload);

    if (len >= 0 && payload != NULL) {
        handle_ip_packet(payload, len, t_nfq);
    } else {
        fprintf(stderr, "[GW] nfq_get_payload() returned %d\n", len);
    }

    return nfq_set_verdict(qh, id, NF_DROP, 0, NULL);
}

static int flush_sync_close(FILE **fp_ptr, const char *label)
{
    if (!fp_ptr || !*fp_ptr) return 0;

    FILE *fp = *fp_ptr;
    int rc = 0;

    if (fflush(fp) != 0) {
        fprintf(stderr, "%s: fflush failed: %s\n",
                label, strerror(errno));
        rc = -1;
    }

    int fd = fileno(fp);
    if (fd >= 0 && fsync(fd) != 0) {
        fprintf(stderr, "%s: fsync failed: %s\n",
                label, strerror(errno));
        rc = -1;
    }

    if (fclose(fp) != 0) {
        fprintf(stderr, "%s: fclose failed: %s\n",
                label, strerror(errno));
        rc = -1;
    }

    *fp_ptr = NULL;
    return rc;
}

static int write_summary_csv(double duration_s, double thr_fwd_pps,
                             double thr_orig_pps, double thr_fwd_mbps,
                             double thr_orig_mbps)
{
    if (!runtime_cfg.summary_output[0]) return 0;

    FILE *fp = fopen(runtime_cfg.summary_output, "w");
    if (!fp) {
        perror("[GW] fopen(summary-output)");
        return -1;
    }

    fprintf(fp,
            "run_id,component,backend,policy,duration_s,packets_intercepted,"
            "symbols_forwarded,originals_forwarded,bytes_forwarded,bytes_original,"
            "blocks_completed,throughput_pps,original_throughput_pps,throughput_mbps,"
            "original_throughput_mbps,encoding_calls,encoding_mean_us,"
            "nfqueue_samples,nfqueue_mean_us\n");

    fprintf(fp,
            "%s,gateway,Reed-Solomon,default,%.9f,%" PRIu64 ",%" PRIu64 ",%" PRIu64
            ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%.9f,%.9f,%.9f,%.9f,"
            "%" PRIu64 ",%.9f,%" PRIu64 ",%.9f\n",
            runtime_cfg.run_id,
            duration_s,
            stats.pkts_intercepted,
            stats.pkts_forwarded,
            stats.pkts_original,
            stats.bytes_forwarded,
            stats.bytes_original,
            stats.generations_completed,
            thr_fwd_pps,
            thr_orig_pps,
            thr_fwd_mbps,
            thr_orig_mbps,
            stats.enc_calls,
            stats.enc_calls
                ? (stats.enc_time_ns / (double)stats.enc_calls) / 1000.0
                : 0.0,
            stats.nfq_pkts,
            stats.nfq_pkts
                ? (stats.nfq_latency_ns / (double)stats.nfq_pkts) / 1000.0
                : 0.0);

    if (flush_sync_close(&fp, "[GW] summary-output") != 0)
        return -1;

    return 0;
}

static void sigint_handler(int sig)
{
    (void)sig;
    stop_flag = 1;
}

int main(int argc, char *argv[])
{
    if (parse_runtime_options(argc, argv) != 0) {
        usage(stderr, argv[0]);
        return 2;
    }

    parity_symbols = runtime_cfg.repairs;

    fprintf(stderr,
            "[OpenMC RS] Edge Receiver endpoints:\n"
            "     A: %s:%u via %s\n"
            "     B: %s:%u via %s\n"
            "[OpenMC RS] block-size=%u, repairs=%u, nfqueue=%u\n",
            runtime_cfg.peer_a, runtime_cfg.peer_port, runtime_cfg.iface_a,
            runtime_cfg.peer_b, runtime_cfg.peer_port, runtime_cfg.iface_b,
            runtime_cfg.block_size, runtime_cfg.repairs,
            runtime_cfg.nfqueue_num);

    if (parity_symbols == 0) {
        fprintf(stderr,
                "[GW] FEC disabled (r=0), will send only original packets\n");
    } else {
        init_rs_codec(parity_symbols);
    }

    init_udp_sockets();
    memset(&cur_gen, 0, sizeof(cur_gen));

    /* NFQUEUE setup */
    struct nfq_handle    *h  = nfq_open();
    struct nfq_q_handle  *qh = NULL;
    int fd, rv;
    char buf[4096] __attribute__((aligned));

    if (!h) {
        fprintf(stderr, "Error opening nfq handle\n");
        return EXIT_FAILURE;
    }

    if (nfq_unbind_pf(h, AF_INET) < 0) {
        fprintf(stderr, "Error nfq_unbind_pf(AF_INET)\n");
        nfq_close(h);
        return EXIT_FAILURE;
    }

    if (nfq_bind_pf(h, AF_INET) < 0) {
        fprintf(stderr, "Error nfq_bind_pf(AF_INET)\n");
        nfq_close(h);
        return EXIT_FAILURE;
    }

    qh = nfq_create_queue(h, runtime_cfg.nfqueue_num, &nfq_cb, NULL);
    if (!qh) {
        fprintf(stderr,
                "Error nfq_create_queue(%u)\n",
                runtime_cfg.nfqueue_num);
        nfq_close(h);
        return EXIT_FAILURE;
    }

    if (nfq_set_mode(qh, NFQNL_COPY_PACKET, 0xffff) < 0) {
        fprintf(stderr, "Error setting nfq mode\n");
        nfq_destroy_queue(qh);
        nfq_close(h);
        return EXIT_FAILURE;
    }

    /*
     * Experiment-6b NFQUEUE hardening.
     *
     * Increase the maximum number of packets that the kernel may retain in
     * this queue.  Failure is reported but is not fatal so that deployments
     * with more restrictive kernel settings remain diagnosable.
     */
    if (nfq_set_queue_maxlen(qh, NFQUEUE_MAXLEN) < 0) {
        fprintf(stderr,
                "[GW] Warning: nfq_set_queue_maxlen(%u) failed\n",
                (unsigned)NFQUEUE_MAXLEN);
    }

    fd = nfq_fd(h);

    /*
     * Increase the receive buffer of the Netlink socket used by NFQUEUE.
     * The effective value is queried and logged because Linux may clamp or
     * internally adjust the requested SO_RCVBUF value.
     */
    {
        int requested_rcvbuf = NFQUEUE_RCVBUF_BYTES;
        int effective_rcvbuf = 0;
        socklen_t optlen = sizeof(effective_rcvbuf);

        if (setsockopt(fd,
                       SOL_SOCKET,
                       SO_RCVBUF,
                       &requested_rcvbuf,
                       sizeof(requested_rcvbuf)) < 0) {
            perror("[GW] setsockopt(SO_RCVBUF)");
        }

        if (getsockopt(fd,
                       SOL_SOCKET,
                       SO_RCVBUF,
                       &effective_rcvbuf,
                       &optlen) < 0) {
            perror("[GW] getsockopt(SO_RCVBUF)");
        } else {
            fprintf(stderr,
                    "[GW] NFQUEUE queue_maxlen=%u "
                    "SO_RCVBUF requested=%d effective=%d bytes\n",
                    (unsigned)NFQUEUE_MAXLEN,
                    requested_rcvbuf,
                    effective_rcvbuf);
        }
    }

    struct sigaction sa = {0};
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    fprintf(stderr,
            "[GW] Listening on NFQUEUE %u...\n",
            runtime_cfg.nfqueue_num);

    rv = 0;

    while (!stop_flag) {
        rv = recv(fd, buf, sizeof(buf), 0);

        if (rv < 0) {
            if (errno == EINTR) {
                if (stop_flag)
                    break;
                continue;
            }

            if (errno == ENOBUFS) {
                fprintf(stderr,
                        "[GW] NFQUEUE receive failure: ENOBUFS "
                        "(kernel/user-space queue overflow)\n");
            } else {
                perror("[GW] recv");
            }

            break;
        }

        nfq_handle_packet(h, buf, rv);

        if (pending_rs) {
            flush_generation(&pending_gen);
            pending_rs = 0;
        }
    }

    fprintf(stderr, "[GW] Stopping...\n");

    if (!stop_flag && rv < 0)
        fprintf(stderr,
                "[GW] receive loop ended with rv=%d, errno=%d\n",
                rv, errno);

    double duration_s =
        (t_start_ns != 0 && t_end_ns >= t_start_ns)
            ? (double)(t_end_ns - t_start_ns) / 1e9
            : 0.0;

    double thr_fwd_pps =
        duration_s > 0
            ? (double)stats.pkts_forwarded / duration_s
            : 0.0;

    double thr_orig_pps =
        duration_s > 0
            ? (double)stats.pkts_original / duration_s
            : 0.0;

    double thr_fwd_mbps =
        duration_s > 0
            ? (stats.bytes_forwarded * 8.0) / (duration_s * 1e6)
            : 0.0;

    double thr_orig_mbps =
        duration_s > 0
            ? (stats.bytes_original * 8.0) / (duration_s * 1e6)
            : 0.0;

    fprintf(stderr,
            "\n[GW] === Computational statistics ===\n"
            "Intercepted packets        : %lu\n"
            "Forwarded packets (total)  : %lu\n"
            "Original packets (data)    : %lu\n"
            "Completed generations      : %lu\n"
            "Experiment duration        : %.3f s\n"
            "\n"
            "Throughput (forwarded)     : %.2f pkts/s\n"
            "Throughput (original)      : %.2f pkts/s\n"
            "Throughput (forwarded)     : %.2f Mbps\n"
            "Throughput (original)      : %.2f Mbps\n"
            "\n"
            "RS enc calls               : %lu\n"
            "RS enc time (avg)          : %.2f us\n"
            "NFQUEUE pkts               : %lu\n"
            "NFQUEUE latency (avg)      : %.2f us\n",
            stats.pkts_intercepted,
            stats.pkts_forwarded,
            stats.pkts_original,
            stats.generations_completed,
            duration_s,
            thr_fwd_pps,
            thr_orig_pps,
            thr_fwd_mbps,
            thr_orig_mbps,
            stats.enc_calls,
            stats.enc_calls
                ? (stats.enc_time_ns / stats.enc_calls) / 1000.0
                : 0.0,
            stats.nfq_pkts,
            stats.nfq_pkts
                ? (stats.nfq_latency_ns / stats.nfq_pkts) / 1000.0
                : 0.0);

    /*
     * Flush any remaining completed generation.
     * Originals have already been sent.
     */
    if (pending_rs) {
        flush_generation(&pending_gen);
        pending_rs = 0;
    }

    if (write_summary_csv(duration_s,
                          thr_fwd_pps,
                          thr_orig_pps,
                          thr_fwd_mbps,
                          thr_orig_mbps) != 0) {
        fprintf(stderr,
                "[GW] WARNING: structured summary was not written\n");
    }

    if (qh)
        nfq_destroy_queue(qh);

    if (h)
        nfq_close(h);

    if (sock_a >= 0)
        close(sock_a);

    if (sock_b >= 0)
        close(sock_b);

    if (rs)
        free_rs_char(rs);

    return EXIT_SUCCESS;
}
