/* edge_receiver_rq.c
 *
 * Edge Receiver for IST-SRC with RaptorQ (lcrq), equivalent in behavior
 * to edge_receiver_rs.c but replacing Reed–Solomon with RaptorQ.
 *
 * - Listens on UDP port 5000 on TWO interfaces (term2gs3, term2gs4).
 * - Receives RaptorQ symbols from the gateway:
 *      * ORIG symbols (is_source=1) carrying original IP packets,
 *      * repair symbols (is_source=0) carrying encoded redundancy.
 * - Groups symbols by block_id (generation id).
 * - For each block:
 *      * For every ORIG received, forwards the original IP packet
 *        immediately to destination_server.c via raw IP on interface "term2term4",
 *        using ip->daddr from the original packet (no SERVER_IP needed).
 *      * Collects ORIG+repair symbols (ESI array) for RaptorQ.
 *      * When enough symbols are present, calls rq_decode() to recover
 *        any missing K source symbols and forwards those not already sent.
 *
 * Statistics:
 *   [GS-STATS] Total generations: ...
 *   [GS-STATS] Generations without loss: ...
 *   [GS-STATS] Generations repaired by FEC: ...
 *   [GS-STATS] Generations with decode failures: ...
 *   [GS-STATS] Data fragments: ..., parity fragments: ...
 *   [GS-STATS] Global FEC ratio (parity/data): ...
 *
 * Build (ejemplo):
 *   gcc -O2 -Wall -o edge-receiver-rq edge_receiver_rq.c -llcrq
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <getopt.h>
#include <errno.h>

#include <arpa/inet.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <net/if.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/select.h>

#include <lcrq.h>

#include <time.h>

#define OPENMC_VERSION       "0.1.0"
#define DEFAULT_OUT_IFACE    "term2term4"
#define DEFAULT_IN_IFACE_A   "term2gs3"
#define DEFAULT_IN_IFACE_B   "term2gs4"
#define DEFAULT_LISTEN_PORT  5000
#define DEFAULT_BLOCK_SIZE   8

#define MAX_BLOCKS         1024
#define K_DATA             8
#define MAX_SYMS_PER_BLOCK (K_DATA + 32)

struct receiver_config {
    char in_iface_a[IFNAMSIZ];
    char in_iface_b[IFNAMSIZ];
    char out_iface[IFNAMSIZ];
    uint16_t listen_port;
    uint16_t block_size;
};

static struct receiver_config g_cfg = {
    .in_iface_a = DEFAULT_IN_IFACE_A,
    .in_iface_b = DEFAULT_IN_IFACE_B,
    .out_iface = DEFAULT_OUT_IFACE,
    .listen_port = DEFAULT_LISTEN_PORT,
    .block_size = DEFAULT_BLOCK_SIZE
};

static int parse_u16(const char *text, uint16_t min_value, uint16_t max_value,
                     uint16_t *out)
{
    char *end = NULL;
    errno = 0;
    unsigned long value = strtoul(text, &end, 10);
    if (errno || !text[0] || (end && *end) ||
        value < min_value || value > max_value)
        return -1;
    *out = (uint16_t)value;
    return 0;
}

static int copy_option(char *dst, size_t dst_size, const char *src,
                       const char *name)
{
    if (!src || strlen(src) >= dst_size) {
        fprintf(stderr, "[Edge Receiver] Invalid value for %s\n", name);
        return -1;
    }
    snprintf(dst, dst_size, "%s", src);
    return 0;
}

static void print_usage(FILE *stream, const char *prog)
{
    fprintf(stream,
        "Usage: %s [options]\n"
        "\n"
        "OpenMC RaptorQ Edge Receiver options:\n"
        "  --listen-iface-a IFACE   Input interface A (default: %s)\n"
        "  --listen-iface-b IFACE   Input interface B (default: %s)\n"
        "  --output-iface IFACE     Raw output interface (default: %s)\n"
        "  --listen-port PORT       Symbol UDP port (default: %u)\n"
        "  --block-size K           Maximum source symbols, 1..%u (default: %u)\n"
        "  -h, --help               Show this help\n"
        "  -V, --version            Show version\n",
        prog, DEFAULT_IN_IFACE_A, DEFAULT_IN_IFACE_B, DEFAULT_OUT_IFACE,
        DEFAULT_LISTEN_PORT, K_DATA, DEFAULT_BLOCK_SIZE);
}

static int parse_arguments(int argc, char **argv)
{
    enum {
        OPT_LISTEN_IFACE_A = 1000, OPT_LISTEN_IFACE_B,
        OPT_OUTPUT_IFACE, OPT_LISTEN_PORT, OPT_BLOCK_SIZE
    };
    static const struct option options[] = {
        {"listen-iface-a", required_argument, NULL, OPT_LISTEN_IFACE_A},
        {"listen-iface-b", required_argument, NULL, OPT_LISTEN_IFACE_B},
        {"output-iface", required_argument, NULL, OPT_OUTPUT_IFACE},
        {"listen-port", required_argument, NULL, OPT_LISTEN_PORT},
        {"block-size", required_argument, NULL, OPT_BLOCK_SIZE},
        {"help", no_argument, NULL, 'h'},
        {"version", no_argument, NULL, 'V'},
        {NULL, 0, NULL, 0}
    };
    int option;
    while ((option = getopt_long(argc, argv, "hV", options, NULL)) != -1) {
        uint16_t parsed = 0;
        switch (option) {
        case OPT_LISTEN_IFACE_A:
            if (copy_option(g_cfg.in_iface_a, sizeof(g_cfg.in_iface_a),
                            optarg, "--listen-iface-a") != 0) return -1;
            break;
        case OPT_LISTEN_IFACE_B:
            if (copy_option(g_cfg.in_iface_b, sizeof(g_cfg.in_iface_b),
                            optarg, "--listen-iface-b") != 0) return -1;
            break;
        case OPT_OUTPUT_IFACE:
            if (copy_option(g_cfg.out_iface, sizeof(g_cfg.out_iface),
                            optarg, "--output-iface") != 0) return -1;
            break;
        case OPT_LISTEN_PORT:
            if (parse_u16(optarg, 1, 65535, &parsed) != 0) return -1;
            g_cfg.listen_port = parsed;
            break;
        case OPT_BLOCK_SIZE:
            if (parse_u16(optarg, 1, K_DATA, &parsed) != 0) return -1;
            g_cfg.block_size = parsed;
            break;
        case 'h':
            print_usage(stdout, argv[0]);
            exit(EXIT_SUCCESS);
        case 'V':
            printf("OpenMC RaptorQ Edge Receiver %s\n", OPENMC_VERSION);
            exit(EXIT_SUCCESS);
        default:
            return -1;
        }
    }
    if (optind != argc) return -1;
    if (if_nametoindex(g_cfg.in_iface_a) == 0 ||
        if_nametoindex(g_cfg.in_iface_b) == 0 ||
        if_nametoindex(g_cfg.out_iface) == 0) {
        fprintf(stderr,
                "[Edge Receiver] Configured interface unavailable: %s, %s, %s\n",
                g_cfg.in_iface_a, g_cfg.in_iface_b, g_cfg.out_iface);
        return -1;
    }
    return 0;
}

/* --- Computational statistics (GS, RQ) --- */
struct gs_stats {
    uint64_t pkts_received;        // fragmentos UDP recibidos (data + parity)

    uint64_t pkts_forwarded;       // IP reenviados (ORIG + REPAIRED)
    uint64_t pkts_original;        // IP reenviados que eran ORIG (type=0)

    uint64_t bytes_forwarded;      // bytes reales reenviados (len IP real)
    uint64_t bytes_original;       // bytes reales reenviados como ORIG

    uint64_t dec_calls;            // llamadas a rq_decode
    uint64_t dec_time_ns;          // tiempo total de decodificación RQ

    uint64_t experiment_start_ns;
    uint64_t experiment_end_ns;

    uint64_t total_useful_bytes;   // goodput
    uint64_t total_gen_latency_ns; // suma latencias por bloque
    uint64_t completed_generations;
};

static struct gs_stats gs_stats = {0};

struct rq_data_hdr {
    uint16_t block_id;
    uint16_t k;
    uint32_t esi;
    uint16_t symbol_len;
    uint8_t  is_source;
    uint8_t  reserved;
} __attribute__((packed));

/* ----------------------------- */
/* Estadísticas globales         */
/* ----------------------------- */
static uint64_t g_total_generations      = 0;
static uint64_t g_gens_no_loss           = 0;
static uint64_t g_gens_repaired          = 0;
static uint64_t g_gens_decode_fail       = 0;
static uint64_t g_total_data_fragments   = 0;
static uint64_t g_total_parity_fragments = 0;
static uint64_t g_total_parity_consumed  = 0;

static volatile sig_atomic_t g_stop = 0;

/* ----------------------------- */
/* Tabla de bloques completados  */
/* ----------------------------- */
static uint16_t g_completed_blocks[MAX_BLOCKS];
static int      g_completed_count = 0;

/* Comprueba si un block_id ya está completado */
static int is_block_completed(uint16_t block_id)
{
    for (int i = 0; i < g_completed_count; i++)
        if (g_completed_blocks[i] == block_id)
            return 1;
    return 0;
}

/* Marca el block_id como completado */
static void mark_block_completed(uint16_t block_id)
{
    if (g_completed_count < MAX_BLOCKS)
        g_completed_blocks[g_completed_count++] = block_id;
}

/* ----------------------------- */
/* Estado de decodificación      */
/* ----------------------------- */

struct rq_block_dec {
    int      in_use;
    uint16_t block_id;

    uint16_t K;
    uint16_t T;
    uint64_t F;

    rq_t    *rq;

    uint8_t *enc;
    uint32_t ESI[MAX_SYMS_PER_BLOCK];
    uint32_t nesi;

    uint8_t *dec;

    int      have_source[K_DATA];
    int      forwarded[K_DATA];
    int      forwarded_count;

    int      decoded;
    int      used_fec;

    /* Métricas comparables con RS */
    uint16_t nsrc_unique;
    uint16_t npar_unique;
    int      parity_consumed_counted;

    /* Timing per block */
    uint64_t first_rx_ns;      // primer símbolo recibido
    uint64_t last_fwd_ns;      // último paquete IP reenviado

    /* Goodput */
    uint64_t useful_bytes;     // bytes IP útiles entregados
};

static struct rq_block_dec blocks[MAX_BLOCKS];

/* ----------------------------- */
/* Funciones auxiliares          */
/* ----------------------------- */

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

static int bind_udp_socket(const char *iface_name, uint16_t port)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) die("socket(AF_INET)");

    if (setsockopt(s, SOL_SOCKET, SO_BINDTODEVICE, iface_name,
                   strlen(iface_name)) < 0)
        die("setsockopt SO_BINDTODEVICE");

    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(port);

    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0)
        die("bind");

    return s;
}

static int create_raw_socket(const char *iface_name)
{
    int s = socket(AF_INET, SOCK_RAW, IPPROTO_RAW);
    if (s < 0) die("socket raw");

    int one = 1;
    if (setsockopt(s, IPPROTO_IP, IP_HDRINCL, &one, sizeof(one)) < 0)
        die("setsockopt IP_HDRINCL");

    if (setsockopt(s, SOL_SOCKET, SO_BINDTODEVICE,
                   iface_name, strlen(iface_name)) < 0)
        die("setsockopt SO_BINDTODEVICE raw");

    return s;
}

static int raw_sock = -1;

static int forward_ip_packet_counted(const uint8_t *pkt, int len, int is_orig)
{
    if (len < (int)sizeof(struct iphdr)) return -1;

    const struct iphdr *ip = (const struct iphdr *)pkt;

    struct sockaddr_in dst = {0};
    dst.sin_family      = AF_INET;
    dst.sin_addr.s_addr = ip->daddr;

    ssize_t sent = sendto(raw_sock, pkt, len, 0,
                          (struct sockaddr *)&dst, sizeof(dst));
    if (sent < 0) {
        perror("sendto raw");
        return -1;
    }

    /* Contabilidad “como el gateway”: SOLO si realmente se reenvía */
    gs_stats.pkts_forwarded++;
    gs_stats.bytes_forwarded += (uint64_t)len;

    if (is_orig) {
        gs_stats.pkts_original++;
        gs_stats.bytes_original += (uint64_t)len;
    }

    return 0;
}

/* ----------------------------- */
/* Gestión de bloques            */
/* ----------------------------- */

static struct rq_block_dec *get_block(uint16_t block_id)
{
    int idx = block_id % MAX_BLOCKS;
    struct rq_block_dec *b = &blocks[idx];

    if (!b->in_use) {
        memset(b, 0, sizeof(*b));
        b->in_use   = 1;
        b->block_id = block_id;
    }
    else if (b->block_id != block_id) {
        if (b->rq) rq_free(b->rq);
        free(b->enc);
        free(b->dec);

        memset(b, 0, sizeof(*b));
        b->in_use   = 1;
        b->block_id = block_id;
    }
    return b;
}

static void finalize_block(struct rq_block_dec *b)
{
    if (!b || !b->in_use) return;

    g_total_generations++;

    int all_forwarded = 1;
    for (int i = 0; i < b->K; i++)
        if (!b->forwarded[i])
            all_forwarded = 0;

    if (all_forwarded && !b->used_fec) g_gens_no_loss++;
    else if (all_forwarded && b->used_fec) g_gens_repaired++;
    else g_gens_decode_fail++;

    /* 🔥 NUEVO → marcar este bloque como finalizado */
    mark_block_completed(b->block_id);

    if (b->first_rx_ns && b->last_fwd_ns) {
        gs_stats.total_gen_latency_ns += (b->last_fwd_ns - b->first_rx_ns);
        gs_stats.completed_generations++;
    }

    gs_stats.total_useful_bytes += b->useful_bytes;

    fprintf(stderr,
            "[GS-STATS] gen=%u DONE (len=%u)\n",
            b->block_id, b->T);

    if (b->rq) rq_free(b->rq);
    free(b->enc);
    free(b->dec);

    memset(b, 0, sizeof(*b));
}

/* Devuelve 1 si ya hemos almacenado ese ESI en el bloque (deduplicación). */
static int has_esi(const struct rq_block_dec *b, uint32_t esi)
{
    for (uint32_t i = 0; i < b->nesi; i++)
        if (b->ESI[i] == esi)
            return 1;
    return 0;
}

static void try_decode_and_forward(struct rq_block_dec *b)
{
    if (!b || !b->rq) return;
    if (b->decoded) return;

    if (b->forwarded_count >= b->K)
        return;

    if (b->nesi < b->K)
        return;

    uint64_t t0 = now_ns();

    if (rq_decode(b->rq, b->dec, b->enc, b->ESI, b->nesi) != 0)
        return;

    uint64_t t1 = now_ns();
    gs_stats.dec_calls++;
    gs_stats.dec_time_ns += (t1 - t0);

    /* Contabilizar "consumed parity" una única vez, cuando el decode tiene éxito:
     * aproximación comparable: redundancia mínima necesaria = K - sources_recibidos.
     */
    if (!b->parity_consumed_counted) {
        if (b->nsrc_unique < b->K)
            g_total_parity_consumed += (uint64_t)(b->K - b->nsrc_unique);
        b->parity_consumed_counted = 1;
    }

    for (int i = 0; i < b->K; i++) {
        if (!b->forwarded[i]) {
            const uint8_t *pkt = b->dec + (uint64_t)i * b->T;
            if (forward_ip_packet_counted(pkt, b->T, 0 /* is_orig */) == 0) {
                b->forwarded[i] = 1;
                b->used_fec     = 1;
                b->forwarded_count++;

                b->useful_bytes += b->T;
                b->last_fwd_ns = now_ns();
            }

        }
    }

    b->decoded = 1;
}

/* ----------------------------- */
/* Procesar símbolo recibido     */
/* ----------------------------- */

static void handle_rq_symbol(const uint8_t *buf, int len)
{
    uint64_t t_rx = now_ns();

    /* Primer fragmento recibido → marcar inicio del experimento */
    if (gs_stats.experiment_start_ns == 0) {
        gs_stats.experiment_start_ns = t_rx;
    }

    /* Cada fragmento actualiza el final del experimento */
    gs_stats.experiment_end_ns = t_rx;

    gs_stats.pkts_received++;

    if (len < (int)sizeof(struct rq_data_hdr)) return;

    struct rq_data_hdr hdr;
    memcpy(&hdr, buf, sizeof(hdr));

    uint16_t block_id = ntohs(hdr.block_id);
    uint16_t k        = ntohs(hdr.k);
    uint32_t esi      = ntohl(hdr.esi);
    uint16_t T        = ntohs(hdr.symbol_len);
    uint8_t  is_src   = hdr.is_source;

    /* ---------------------------------------------
     * Contabilizar fragmentos RECIBIDOS (igual que RS),
     * incluso si luego se descartan por bloque completado
     * --------------------------------------------- */
    if (is_src)
        g_total_data_fragments++;
    else
        g_total_parity_fragments++;

    const uint8_t *symbol = buf + sizeof(hdr);
    int symbol_len = len - sizeof(hdr);

    if (symbol_len < T || T == 0) return;

    /* 🔥 NUEVO: descartar completamente símbolos de bloques finalizados */
    //if (is_block_completed(block_id))
    //    return;

    struct rq_block_dec *b = get_block(block_id);

    if (b->first_rx_ns == 0) {
        b->first_rx_ns = now_ns();
    }

    if (!b->rq) {
        b->K = (k ? k : g_cfg.block_size);
        if (b->K > g_cfg.block_size) b->K = g_cfg.block_size;

        b->T = T;
        b->F = (uint64_t)b->K * b->T;

        b->rq  = rq_init(b->F, b->T);
        b->enc = calloc(MAX_SYMS_PER_BLOCK * b->T, 1);
        b->dec = malloc(b->F);

        b->nesi            = 0;
        b->forwarded_count = 0;
        b->nsrc_unique     = 0;
        b->npar_unique     = 0;
        b->parity_consumed_counted = 0;

        fprintf(stderr,
                "[GS] New block=%u (K=%u T=%u)\n",
                block_id, b->K, b->T);
    }

    /* Contabilizar received (únicos) y almacenar solo si ESI es nuevo */
    if (has_esi(b, esi))
        goto maybe_forward_and_finish;

    if (is_src) b->nsrc_unique++;
    else        b->npar_unique++;

    if (b->nesi < MAX_SYMS_PER_BLOCK) {
        memcpy(b->enc + (uint64_t)b->nesi * b->T, symbol, T);
        b->ESI[b->nesi] = esi;
        b->nesi++;
    }

    /* Descartar símbolos tardíos SOLO después de contabilizar */
    if (is_block_completed(block_id))
        return;

maybe_forward_and_finish:
    if (is_src) {
        int idx = (int)esi;
        if (idx >= 0 && idx < b->K) {
            if (!b->forwarded[idx]) {
                if (forward_ip_packet_counted(symbol, T, 1 /* is_orig */) == 0) {
                    b->forwarded[idx] = 1;
                    b->have_source[idx] = 1;
                    b->forwarded_count++;

                    b->useful_bytes += T;
                    b->last_fwd_ns = now_ns();
                }
            }
        }
    }

    try_decode_and_forward(b);

    int all_forwarded = 1;
    for (int i = 0; i < b->K; i++)
        if (!b->forwarded[i])
            all_forwarded = 0;

    if (all_forwarded)
        finalize_block(b);
}

/* ----------------------------- */
/* Señales y estadísticas        */
/* ----------------------------- */

static void sigint_handler(int s)
{
    (void)s;
    g_stop = 1;
}

static void print_global_stats(void)
{
    fprintf(stderr, "[GS-STATS] Total generations: %"PRIu64"\n",
            g_total_generations);
    fprintf(stderr, "[GS-STATS] Generations without loss: %"PRIu64"\n",
            g_gens_no_loss);
    fprintf(stderr, "[GS-STATS] Generations repaired: %"PRIu64"\n",
            g_gens_repaired);
    fprintf(stderr, "[GS-STATS] Generations decode_fail: %"PRIu64"\n",
            g_gens_decode_fail);

    fprintf(stderr,
            "[GS-STATS] Data fragments: %"PRIu64
            ", parity fragments: %"PRIu64"\n",
            g_total_data_fragments, g_total_parity_fragments);

    double ratio = 0.0;
    if (g_total_data_fragments > 0)
        ratio = (double)g_total_parity_fragments /
                (double)g_total_data_fragments;

    //fprintf(stderr, "[GS-STATS] Global FEC ratio: %.3f\n", ratio);
    fprintf(stderr, "[GS-STATS] Global FEC ratio (parity/data): %.3f\n", ratio);

    fprintf(stderr, "[GS-STATS] Parity fragments consumed: %"PRIu64"\n", g_total_parity_consumed);
    double ratio_cons = 0.0;
    if (g_total_data_fragments > 0)
        ratio_cons = (double)g_total_parity_consumed / (double)g_total_data_fragments;
    fprintf(stderr, "[GS-STATS] Consumed FEC ratio (consumed/data): %.3f\n", ratio_cons);
}

/* ----------------------------- */
/* MAIN LOOP                     */
/* ----------------------------- */

int main(int argc, char *argv[])
{
    if (parse_arguments(argc, argv) != 0) {
        print_usage(stderr, argv[0]);
        return 2;
    }

    memset(blocks, 0, sizeof(blocks));
    memset(g_completed_blocks, 0, sizeof(g_completed_blocks));

    int udp_a = bind_udp_socket(g_cfg.in_iface_a, g_cfg.listen_port);
    int udp_b = bind_udp_socket(g_cfg.in_iface_b, g_cfg.listen_port);
    raw_sock  = create_raw_socket(g_cfg.out_iface);

    fprintf(stderr,
            "[GS] Listening on %s & %s, forwarding via %s\n",
            g_cfg.in_iface_a, g_cfg.in_iface_b, g_cfg.out_iface);

    struct sigaction sa = {0};
    sa.sa_handler = sigint_handler;
    sigaction(SIGINT, &sa, NULL);

    uint8_t buf[2048];
    int maxfd = (udp_a > udp_b ? udp_a : udp_b);

    gs_stats.experiment_start_ns = 0;

    while (!g_stop) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(udp_a, &rfds);
        FD_SET(udp_b, &rfds);

        if (select(maxfd + 1, &rfds, NULL, NULL, NULL) < 0) {
            if (errno == EINTR) continue;
            die("select");
        }

        if (FD_ISSET(udp_a, &rfds)) {
            ssize_t n = recvfrom(udp_a, buf, sizeof(buf), 0, NULL, NULL);
            if (n > 0) handle_rq_symbol(buf, (int)n);
        }
        if (FD_ISSET(udp_b, &rfds)) {
            ssize_t n = recvfrom(udp_b, buf, sizeof(buf), 0, NULL, NULL);
            if (n > 0) handle_rq_symbol(buf, (int)n);
        }
    }

    //gs_stats.experiment_end_ns = now_ns();

    fprintf(stderr, "[GS] Stopping...\n");

    /* Finalizar bloques activos ANTES de imprimir, para estadísticas coherentes */
    //for (int i = 0; i < MAX_BLOCKS; i++)
    //    if (blocks[i].in_use)
    //        finalize_block(&blocks[i]);

    print_global_stats();

    double duration_s = (double)(gs_stats.experiment_end_ns - gs_stats.experiment_start_ns) / 1e9;
    double thr_fwd_pps  = duration_s > 0 ? (double)gs_stats.pkts_forwarded / duration_s : 0.0;
    double thr_orig_pps = duration_s > 0 ? (double)gs_stats.pkts_original  / duration_s : 0.0;
    double thr_fwd_mbps  = duration_s > 0 ? (gs_stats.bytes_forwarded * 8.0) / (duration_s * 1e6) : 0.0;
    double thr_orig_mbps = duration_s > 0 ? (gs_stats.bytes_original  * 8.0) / (duration_s * 1e6) : 0.0;
    double avg_gen_latency_ms = 0.0;
    if (gs_stats.completed_generations > 0) {
        avg_gen_latency_ms = (gs_stats.total_gen_latency_ns / gs_stats.completed_generations) / 1e6;
    }
    double goodput_mbps = 0.0;
    if (duration_s > 0) {
        goodput_mbps = (gs_stats.total_useful_bytes * 8.0) / (duration_s * 1e6);
    }


    fprintf(stderr,
            "\n[GS-RQ] === Computational statistics ===\n"
            "Experiment duration        : %.3f s\n"
            "UDP symbols received       : %"PRIu64"\n"
            "IP packets forwarded       : %"PRIu64"\n"
            "Original packets (data)    : %"PRIu64"\n"
            "\n"
            "Throughput (forwarded)     : %.2f pkts/s\n"
            "Throughput (original)      : %.2f pkts/s\n"
            "Throughput (forwarded)     : %.2f Mbps\n"
            "Throughput (original)      : %.2f Mbps\n"
            "\n"
            "RQ decode calls            : %"PRIu64"\n"
            "RQ decode time (avg)       : %.2f us\n",
            duration_s,
            gs_stats.pkts_received,
            gs_stats.pkts_forwarded,
            gs_stats.pkts_original,
            thr_fwd_pps,
            thr_orig_pps,
            thr_fwd_mbps,
            thr_orig_mbps,
            gs_stats.dec_calls,
            gs_stats.dec_calls ? (gs_stats.dec_time_ns / gs_stats.dec_calls) / 1000.0 : 0.0
            );

    fprintf(stderr,
            "\n[GS-RQ] === End-to-End Performance ===\n"
            "Average block latency      : %.2f ms\n"
            "Effective goodput          : %.2f Mbps\n"
            "Completed blocks            : %lu\n",
            avg_gen_latency_ms,
            goodput_mbps,
            gs_stats.completed_generations
            );

    close(udp_a);
    close(udp_b);
    close(raw_sock);

    return 0;
}
