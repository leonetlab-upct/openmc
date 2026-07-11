/* ground_station.c
 *
 * Ground station that receives FEC fragments from the ItS gateway
 * and reconstructs missing packets using Reed–Solomon (Phil Karn's libfec).
 *
 * - Listens on UDP port 5000 on TWO interfaces (term2gs3, term2gs4).
 * - Groups fragments by generation id.
 * - For each generation:
 *    * For every data fragment received, forwards the original IP
 *      packet immediately to servidor.c via raw IP on interface "term2term4".
 *    * If some data fragments are missing but all parity fragments (r)
 *      have been received, uses RS decoding to reconstruct the missing
 *      packets and forwards them as well.
 *
 * Build:
 *   gcc -O2 -Wall -o ground_station ground_station.c -lfec
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include <arpa/inet.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <net/if.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/select.h>
#include <signal.h>

#include <fec.h>      // Phil Karn's libfec

#include <time.h>

/* Raw interface hacia servidor.c (igual que antes) */
#define RAW_OUT_IFACE "term2term4"

/* Interfaces de entrada desde los enlaces LEO (como en ground_station_XOR.c) */
#define IN_IFACE_A "term2gs3"
#define IN_IFACE_B "term2gs4"

#define MAX_PKT_LEN   1500
#define K_DATA        8
#define MAX_PARITY    32
#define DEFAULT_PARITY 2
#define MAX_GENS      256             // number of simultaneous generations tracked

/* --- Computational statistics (GS) --- */
struct gs_stats {
    uint64_t pkts_received;        // fragmentos UDP recibidos (data + parity)

    uint64_t pkts_forwarded;       // IP reenviados (ORIG + REPAIRED)
    uint64_t pkts_original;        // IP reenviados que eran ORIG (type=0)

    uint64_t bytes_forwarded;      // bytes reales reenviados (len IP real)
    uint64_t bytes_original;       // bytes reales reenviados como ORIG

    uint64_t dec_calls;            // llamadas a RS decode
    uint64_t dec_time_ns;          // tiempo total de decodificación RS

    uint64_t experiment_start_ns;
    uint64_t experiment_end_ns;

    uint64_t total_useful_bytes;
    uint64_t total_gen_latency_ns;
    uint64_t completed_generations;
};

static struct gs_stats gs_stats = {0};

/* Puerto UDP fijo para recibir fragmentos del gateway */
#define UDP_PORT      5000

struct fec_fragment_hdr {
    uint16_t gen_id;       // generation id (network byte order on wire)
    uint8_t  index;        // 0..7 data, 8..7+r parity
    uint8_t  type;         // 0=data, 1=parity
    uint16_t original_len; // IP packet length
    uint16_t reserved;     // reserved
} __attribute__((packed));

/* Per-generation state on ground station */
struct gen_state {
    int      in_use;
    uint16_t gen_id;
    int      original_len;

    uint8_t  data[K_DATA][MAX_PKT_LEN];
    int      data_present[K_DATA];
    int      data_forwarded[K_DATA];

    uint8_t  parity[MAX_PARITY][MAX_PKT_LEN];
    int      parity_present[MAX_PARITY];

    /* Estado de la generación */
    int      completed;          // todos los datos reenviados
    int      stats_reported;     // estadísticas ya contadas

    /* Flags de uso de FEC */
    int      decode_attempted;   // se ha intentado FEC al menos una vez
    int      decode_succeeded;   // FEC ha conseguido dejar missing_final=0
    int      parity_consumed_counted; // evitar doble conteo de consumed parity

    /* Timing per generation */
    uint64_t first_rx_ns;      // primer fragmento recibido
    /* last_fwd_ns tracks the timestamp of the final IP packet
     * (original or repaired) forwarded for this generation
     */
    uint64_t last_fwd_ns;      // último paquete reenviado

    /* Goodput */
    uint64_t useful_bytes;     // bytes IP útiles entregados
};

static struct gen_state gens[MAX_GENS];

/* Estadísticas globales */
static unsigned long g_total_generations         = 0;
static unsigned long g_generations_repaired      = 0;
static unsigned long g_generations_decode_failed = 0;
static unsigned long g_generations_no_loss       = 0;
static unsigned long g_total_data_fragments      = 0;
static unsigned long g_total_parity_fragments    = 0;
static unsigned long g_total_parity_consumed     = 0;

static int  parity_symbols = DEFAULT_PARITY; // r
static void *rs = NULL;                      // RS codec descriptor
static int  raw_sock = -1;                   // raw IP output socket

static volatile sig_atomic_t stop_flag = 0;

static inline uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

/* Initialize RS decoder matching the gateway configuration:
 * same K_DATA and r, shortened code over GF(2^8).
 */
static void init_rs_codec(int r)
{
    if (r <= 0 || r > MAX_PARITY) {
        fprintf(stderr, "[GS] Invalid parity symbols r=%d (1..%d)\n", r, MAX_PARITY);
        exit(EXIT_FAILURE);
    }

    int symsize = 8;
    int gfpoly  = 0x11d;
    int fcr     = 1;
    int prim    = 1;
    int nroots  = r;
    int pad     = 255 - (K_DATA + r);  // shortened to length K_DATA + r

    rs = init_rs_char(symsize, gfpoly, fcr, prim, nroots, pad);
    if (!rs) {
        fprintf(stderr, "[GS] init_rs_char() failed\n");
        exit(EXIT_FAILURE);
    }

    fprintf(stderr, "[GS] RS codec initialized: K_DATA=%d, r=%d, n=%d\n",
            K_DATA, r, K_DATA + r);
}

/* Initialize raw IP socket to forward reconstructed packets to servidor.c
 * via interface RAW_OUT_IFACE ("term2term4").
 */
static void init_raw_socket(void)
{
    raw_sock = socket(AF_INET, SOCK_RAW, IPPROTO_RAW);
    if (raw_sock < 0) {
        perror("socket(AF_INET,SOCK_RAW)");
        exit(EXIT_FAILURE);
    }

    int one = 1;
    if (setsockopt(raw_sock, IPPROTO_IP, IP_HDRINCL, &one, sizeof(one)) < 0) {
        perror("setsockopt(IP_HDRINCL)");
        close(raw_sock);
        exit(EXIT_FAILURE);
    }

    if (setsockopt(raw_sock, SOL_SOCKET, SO_BINDTODEVICE,
                   RAW_OUT_IFACE, strlen(RAW_OUT_IFACE)) < 0) {
        perror("setsockopt(SO_BINDTODEVICE)");
        close(raw_sock);
        exit(EXIT_FAILURE);
    }

    fprintf(stderr, "[GS] Raw IP socket created, bound to interface %s\n",
            RAW_OUT_IFACE);
}

/* Bind a UDP socket to a specific interface + port (como en ground_station_XOR.c) */
static int bind_udp_socket(const char *iface_name, uint16_t port)
{
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("socket(AF_INET,SOCK_DGRAM)");
        exit(EXIT_FAILURE);
    }

    if (setsockopt(sockfd, SOL_SOCKET, SO_BINDTODEVICE,
                   iface_name, strlen(iface_name)) < 0) {
        perror("setsockopt(SO_BINDTODEVICE UDP)");
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    int sz = 4 * 1024 * 1024;
    (void)setsockopt(sockfd, SOL_SOCKET, SO_RCVBUF, &sz, sizeof(sz));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind(udp_sock)");
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    fprintf(stderr, "[GS] UDP socket bound to iface %s, port %u\n",
            iface_name, port);
    return sockfd;
}

/* Get or allocate generation state for a given gen_id. */
static struct gen_state *get_gen_state(uint16_t gen_id)
{
    struct gen_state *gs = &gens[gen_id % MAX_GENS];

    if (!gs->in_use || gs->gen_id != gen_id) {
        memset(gs, 0, sizeof(*gs));
        gs->in_use       = 1;
        gs->gen_id       = gen_id;
        gs->original_len = 0;

        /* New generation for global statistics. */
        g_total_generations++;
    }
    return gs;
}

/* Forward one IP packet to servidor.c via raw socket. */
static void forward_ip_packet(const uint8_t *pkt, int len, int is_original)
{
    if (len <= 0) return;

    if (len < (int)sizeof(struct iphdr)) {
        fprintf(stderr, "[GS] Ignoring too-short IP packet len=%d\n", len);
        return;
    }

    const struct iphdr *ip = (const struct iphdr *)pkt;

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family      = AF_INET;
    dst.sin_addr.s_addr = ip->daddr;  // use destination IP from original header

    ssize_t sent = sendto(raw_sock, pkt, len, 0,
                          (struct sockaddr *)&dst, sizeof(dst));

    if (sent >= 0) {
        gs_stats.pkts_forwarded++;
        gs_stats.bytes_forwarded += (uint64_t)len;

        if (is_original) {
            gs_stats.pkts_original++;
            gs_stats.bytes_original += (uint64_t)len;
        }
    }
    else {
        perror("sendto(raw_sock)");
    }
}

/* Try to decode missing data packets in a generation using RS.
 *
 * Only attempt decoding when:
 *   - at least one data packet is missing, and
 *   - all parity packets (r) have been received.
 */
static void try_decode_generation(struct gen_state *gs)
{
    if (!gs->in_use) return;
    if (gs->original_len <= 0) return;
    if (parity_symbols <= 0) return;     // FEC disabled

    int len = gs->original_len;

    /* Si ya hemos reenviado todos los datos, no tiene sentido
     * intentar decodificar nada (paridades tardías).
     */
    int unforwarded = 0;
    for (int i = 0; i < K_DATA; i++) {
        if (!gs->data_forwarded[i]) unforwarded++;
    }
    if (unforwarded == 0) {
        return;  // nada que reconstruir
    }

    int missing_data = 0;
    for (int i = 0; i < K_DATA; i++) {
        if (!gs->data_present[i]) missing_data++;
    }
    if (missing_data == 0) {
        // nothing to decode
        return;
    }

    /* Check that all parity symbols are present */
    for (int p = 0; p < parity_symbols; p++) {
        if (!gs->parity_present[p]) {
            // can't decode safely yet
            return;
        }
    }

    gs->decode_attempted = 1;

    /* Contabilizar "consumed parity" una única vez por generación:
     * en RS, esta implementación espera a tener TODOS los r símbolos
     * de paridad para lanzar decode, así que consideramos consumidos r.
     */
    if (!gs->parity_consumed_counted) {
        g_total_parity_consumed += (unsigned long)parity_symbols;
        gs->parity_consumed_counted = 1;
    }

    fprintf(stderr,
            "[GS] Attempting RS decode for gen=%u, len=%d, missing_data=%d, r=%d\n",
            gs->gen_id, len, missing_data, parity_symbols);

    /* Column-wise decoding */
    uint8_t codeword[K_DATA + MAX_PARITY];
    int eras_pos[K_DATA];   // at most K_DATA erasures
    int no_eras;

    const int MAX_LOGGED_FAILS = 10;
    int logged_fails = 0;

    uint64_t t0 = now_ns();

    for (int j = 0; j < len; j++) {
        no_eras = 0;

        /* Fill codeword with data symbols; mark erasures for missing data */
        for (int i = 0; i < K_DATA; i++) {
            if (gs->data_present[i]) {
                codeword[i] = gs->data[i][j];
            } else {
                codeword[i] = 0;
                eras_pos[no_eras++] = i;
            }
        }

        /* Append parity symbols (we know they are present) */
        for (int p = 0; p < parity_symbols; p++) {
            codeword[K_DATA + p] = gs->parity[p][j];
        }

        /* Decode this column. The RS codec is shortened so the effective
         * length is K_DATA + parity_symbols.
         */
        int ret = decode_rs_char(rs, codeword, eras_pos, no_eras);
        if (ret < 0 && logged_fails < MAX_LOGGED_FAILS) {
            fprintf(stderr,
                    "[GS] RS decode error on gen=%u, byte=%d (ret=%d)\n",
                    gs->gen_id, j, ret);
            if (logged_fails == MAX_LOGGED_FAILS - 1) {
                fprintf(stderr,
                        "[GS] Further RS decode errors for gen=%u will not be logged\n",
                        gs->gen_id);
            }
            logged_fails++;
        }

        /* Copy corrected bytes back to missing data packets */
        for (int e = 0; e < no_eras; e++) {
            int idx = eras_pos[e];
            if (idx >= 0 && idx < K_DATA) {
                gs->data[idx][j] = codeword[idx];
            }
        }
    }

    uint64_t t1 = now_ns();
    gs_stats.dec_calls++;
    gs_stats.dec_time_ns += (t1 - t0);

    /* Marcar como presentes y reenviar cualquier paquete que antes faltaba */
    int missing_after = 0;
    for (int i = 0; i < K_DATA; i++) {
        if (!gs->data_present[i]) {
            gs->data_present[i] = 1;
            fprintf(stderr,
                    "[GS] REPAIRED gen=%u idx=%d (len=%d) -> forwarding\n",
                    gs->gen_id, i, len);
            forward_ip_packet(gs->data[i], len, 0);
            gs->data_forwarded[i] = 1;

            gs->useful_bytes += len;
            gs->last_fwd_ns = now_ns();
        }
    }

    for (int i = 0; i < K_DATA; i++) {
        if (!gs->data_present[i]) missing_after++;
    }

    if (missing_after == 0) {
        gs->decode_succeeded = 1;
    }
}

/* After receiving any fragment, we:
 *   - forward any newly received data packet not yet forwarded;
 *   - if there are missing data packets and all parity is present,
 *     try to decode them.
 */
static void process_generation(struct gen_state *gs)
{
    if (!gs->in_use) return;
    int len = gs->original_len;
    if (len <= 0) return;

    /* Forward immediate data packets that have not yet been forwarded */
    for (int i = 0; i < K_DATA; i++) {
        if (gs->data_present[i] && !gs->data_forwarded[i]) {
            fprintf(stderr,
                    "[GS] ORIG(gen=%u idx=%d) -> send %d bytes via %s\n",
                    gs->gen_id, i, len, RAW_OUT_IFACE);
            forward_ip_packet(gs->data[i], len, 1);
            gs->data_forwarded[i] = 1;

            gs->useful_bytes += len;
            gs->last_fwd_ns = now_ns();
        }
    }

    /* Try RS decoding if needed (esto también puede reenviar reparados) */
    try_decode_generation(gs);

    /* Check if generation can be considered completed:
     * all data packets have been forwarded.
     */
    int all_forwarded = 1;
    for (int i = 0; i < K_DATA; i++) {
        if (!gs->data_forwarded[i]) {
            all_forwarded = 0;
            break;
        }
    }

    if (all_forwarded) {
        gs->completed = 1;

        /* Actualizamos estadísticas solo la primera vez */
        if (!gs->stats_reported) {
            gs->stats_reported = 1;

            int missing_final = 0;
            for (int i = 0; i < K_DATA; i++) {
                if (!gs->data_present[i])
                    missing_final++;
            }

            if (missing_final == 0) {
                if (!gs->decode_attempted) {
                    // No hubo pérdidas y no se usó FEC
                    g_generations_no_loss++;
                } else if (gs->decode_succeeded) {
                    // Hubo pérdidas y FEC las cubrió
                    g_generations_repaired++;
                } else {
                    // Se intentó FEC pero sigue faltando algo (raro aquí)
                    g_generations_decode_failed++;
                }
            } else {
                // Termina con pérdidas irreparables desde el punto de vista de la GS
                g_generations_decode_failed++;
            }

            if (gs->first_rx_ns && gs->last_fwd_ns) {
                gs_stats.total_gen_latency_ns += (gs->last_fwd_ns - gs->first_rx_ns);
                gs_stats.completed_generations++;
            }

            gs_stats.total_useful_bytes += gs->useful_bytes;

            fprintf(stderr,
                    "[GS-STATS] gen=%u DONE (len=%d) [loss=%d]\n",
                    gs->gen_id, len, missing_final);
        }

        /* NO hacemos memset(gs,0); conservamos el slot hasta ser
         * reutilizado por otra gen_id en get_gen_state().
         */
    }
}

static void handle_fragment(const uint8_t *buf, ssize_t len)
{
    uint64_t t_rx = now_ns();

    /* Primer fragmento recibido → marcar inicio del experimento */
    if (gs_stats.experiment_start_ns == 0) {
        gs_stats.experiment_start_ns = t_rx;
    }

    /* Cada fragmento actualiza el final del experimento */
    gs_stats.experiment_end_ns = t_rx;

    gs_stats.pkts_received++;

    if (len < (ssize_t)sizeof(struct fec_fragment_hdr)) {
        fprintf(stderr, "[GS] Received too-short fragment (%zd bytes)\n", len);
        return;
    }

    struct fec_fragment_hdr hdr;
    memcpy(&hdr, buf, sizeof(hdr));

    uint16_t gen_id       = ntohs(hdr.gen_id);
    uint8_t  index        = hdr.index;
    uint8_t  type         = hdr.type;
    uint16_t original_len = ntohs(hdr.original_len);

    const uint8_t *payload = buf + sizeof(hdr);
    ssize_t payload_len    = len - sizeof(hdr);

    if (payload_len != original_len) {
        fprintf(stderr,
                "[GS] Warning: payload_len=%zd != original_len=%u for gen=%u idx=%u\n",
                payload_len, original_len, gen_id, index);
        if (payload_len < original_len) {
            // we cannot fix this
            return;
        }
        // if payload_len > original_len, we ignore extra bytes
    }

    if (original_len <= 0 || original_len > MAX_PKT_LEN) {
        fprintf(stderr,
                "[GS] Invalid original_len=%u for gen=%u idx=%u\n",
                original_len, gen_id, index);
        return;
    }

    struct gen_state *gs = get_gen_state(gen_id);

    if (gs->first_rx_ns == 0) {
        gs->first_rx_ns = now_ns();
    }

    if (gs->original_len == 0) {
        gs->original_len = original_len;
    } else if (gs->original_len != (int)original_len) {
        fprintf(stderr,
                "[GS] gen=%u length mismatch (old=%d, new=%u), resetting generation\n",
                gen_id, gs->original_len, original_len);
        memset(gs, 0, sizeof(*gs));
        gs->first_rx_ns  = now_ns();
        gs->in_use       = 1;
        gs->gen_id       = gen_id;
        gs->original_len = original_len;
        g_total_generations++;
    }

    if (type == 0) {
        /* Data fragment (index 0..7) */
        if (index >= K_DATA) {
            fprintf(stderr,
                    "[GS] Invalid data index=%u for gen=%u\n", index, gen_id);
            return;
        }
        if (!gs->data_present[index]) {
            memcpy(gs->data[index], payload, original_len);
            gs->data_present[index] = 1;

            /* Count unique data fragments */
            g_total_data_fragments++;
        }
    } else if (type == 1) {
        /* Parity fragment (index 8..7+r => parity_idx 0..r-1) */
        if (index < K_DATA) {
            fprintf(stderr,
                    "[GS] Invalid parity index=%u (must be >=%d)\n",
                    index, K_DATA);
            return;
        }
        int p = index - K_DATA;
        if (p < 0 || p >= parity_symbols) {
            fprintf(stderr,
                    "[GS] Parity index out of range p=%d for gen=%u\n", p, gen_id);
            return;
        }
        if (!gs->parity_present[p]) {
            memcpy(gs->parity[p], payload, original_len);
            gs->parity_present[p] = 1;

            /* Count unique parity fragments */
            g_total_parity_fragments++;
        }
    } else {
        fprintf(stderr,
                "[GS] Unknown fragment type=%u for gen=%u idx=%u\n",
                type, gen_id, index);
        return;
    }

    process_generation(gs);
}

static void sigint_handler(int sig)
{
    (void)sig;
    stop_flag = 1;
}

/* Registrar SIGINT/SIGTERM con sigaction sin SA_RESTART */
static void setup_signals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;  // importante: sin SA_RESTART

    if (sigaction(SIGINT, &sa, NULL) < 0) {
        perror("sigaction(SIGINT)");
        exit(EXIT_FAILURE);
    }
    if (sigaction(SIGTERM, &sa, NULL) < 0) {
        perror("sigaction(SIGTERM)");
        exit(EXIT_FAILURE);
    }
}

int main(int argc, char *argv[])
{
    /* Usar valor por defecto si no se indica número de símbolos de paridad */
    parity_symbols = DEFAULT_PARITY;

    if (argc > 2) {
        fprintf(stderr,
            "Usage: %s [parity_symbols]\n"
            "  K_DATA = %d data packets per generation\n"
            "  Listening on UDP_PORT = %d (two interfaces)\n"
            "  Default parity_symbols = %d\n",
            argv[0], K_DATA, UDP_PORT, DEFAULT_PARITY);
        return EXIT_FAILURE;
    }

    if (argc == 2) {
        int r = atoi(argv[1]);
        if (r < 0 || r > MAX_PARITY) {
            fprintf(stderr,
                    "[GS] Invalid parity_symbols=%d (must be 0..%d)\n",
                    r, MAX_PARITY);
            return EXIT_FAILURE;
        }
        parity_symbols = r;
    }

    fprintf(stderr,
        "[GS] K_DATA=%d, parity_symbols(r)=%d\n",
        K_DATA, parity_symbols);

    if (parity_symbols == 0) {
        fprintf(stderr,
            "[GS] FEC disabled (r=0), will only forward original data packets\n");
    } else {
        init_rs_codec(parity_symbols);
    }

    /* Dos sockets UDP, uno por interfaz, como en ground_station_XOR.c */
    int sock_a = bind_udp_socket(IN_IFACE_A, UDP_PORT);
    int sock_b = bind_udp_socket(IN_IFACE_B, UDP_PORT);

    init_raw_socket();
    memset(gens, 0, sizeof(gens));
    setup_signals();

    fprintf(stderr,
            "[GS] Waiting for fragments on %s and %s (UDP port %d), raw out on %s\n",
            IN_IFACE_A, IN_IFACE_B, UDP_PORT, RAW_OUT_IFACE);

    uint8_t buf[sizeof(struct fec_fragment_hdr) + MAX_PKT_LEN];

    gs_stats.experiment_start_ns = 0;

    while (!stop_flag) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(sock_a, &readfds);
        FD_SET(sock_b, &readfds);
        int maxfd = (sock_a > sock_b) ? sock_a : sock_b;

        int sel = select(maxfd + 1, &readfds, NULL, NULL, NULL);
        if (sel < 0) {
            if (errno == EINTR) {
                if (stop_flag) break;
                else continue;
            }
            perror("select");
            break;
        }

        int recv_sock = -1;
        if (FD_ISSET(sock_a, &readfds)) recv_sock = sock_a;
        else if (FD_ISSET(sock_b, &readfds)) recv_sock = sock_b;
        else continue;

        ssize_t n = recvfrom(recv_sock, buf, sizeof(buf), 0, NULL, NULL);
        if (n < 0) {
            if (errno == EINTR) {
                if (stop_flag) break;
                else continue;
            }
            perror("recvfrom");
            break;
        }

        handle_fragment(buf, n);

        if (stop_flag)
            break;
    }

    //gs_stats.experiment_end_ns = now_ns();

    fprintf(stderr, "[GS] Stopping...\n");

    /* Print aggregated statistics */
    fprintf(stderr, "[GS-STATS] Total generations: %lu\n", g_total_generations);
    fprintf(stderr, "[GS-STATS] Generations without loss: %lu\n",
            g_generations_no_loss);
    fprintf(stderr, "[GS-STATS] Generations repaired by FEC: %lu\n",
            g_generations_repaired);
    fprintf(stderr, "[GS-STATS] Generations with decode failures: %lu\n",
            g_generations_decode_failed);
    fprintf(stderr,
            "[GS-STATS] Data fragments: %lu, parity fragments: %lu\n",
            g_total_data_fragments, g_total_parity_fragments);
    double fec_ratio = 0.0;
    if (g_total_data_fragments > 0) {
        fec_ratio = (double)g_total_parity_fragments /
                    (double)g_total_data_fragments;
    }
    fprintf(stderr, "[GS-STATS] Global FEC ratio (parity/data): %.3f\n",
            fec_ratio);

    fprintf(stderr, "[GS-STATS] Parity fragments consumed: %lu\n", g_total_parity_consumed);
    double fec_ratio_cons = 0.0;
    if (g_total_data_fragments > 0) {
        fec_ratio_cons = (double)g_total_parity_consumed / (double)g_total_data_fragments;
    }
    fprintf(stderr, "[GS-STATS] Consumed FEC ratio (consumed/data): %.3f\n", fec_ratio_cons);

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
            "\n[GS] === Computational statistics ===\n"
            "Experiment duration        : %.3f s\n"
            "UDP fragments received     : %lu\n"
            "IP packets forwarded (total): %lu\n"
            "Original packets (data)    : %lu\n"
            "\n"
            "Throughput (forwarded)     : %.2f pkts/s\n"
            "Throughput (original)      : %.2f pkts/s\n"
            "Throughput (forwarded)     : %.2f Mbps\n"
            "Throughput (original)      : %.2f Mbps\n"
            "\n"
            "RS decode calls            : %lu\n"
            "RS decode time (avg)       : %.2f us\n",
            duration_s,
            gs_stats.pkts_received,
            gs_stats.pkts_forwarded,
            gs_stats.pkts_original,
            thr_fwd_pps,
            thr_orig_pps,
            thr_fwd_mbps,
            thr_orig_mbps,
            gs_stats.dec_calls,
            gs_stats.dec_calls ?
            (gs_stats.dec_time_ns / gs_stats.dec_calls) / 1000.0 : 0.0
            );


    fprintf(stderr,
            "\n[GS] === End-to-End Performance ===\n"
            "Average generation latency : %.2f ms\n"
            "Effective goodput           : %.2f Mbps\n"
            "Completed generations            : %lu\n",
            avg_gen_latency_ms,
            goodput_mbps,
            gs_stats.completed_generations
            );

    if (sock_a >= 0) close(sock_a);
    if (sock_b >= 0) close(sock_b);
    if (raw_sock >= 0) close(raw_sock);
    if (rs) free_rs_char(rs);

    return EXIT_SUCCESS;
}

