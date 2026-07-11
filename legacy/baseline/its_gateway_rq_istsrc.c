/* its_gateway_rq_istsrc.c
 *
 * ItS gateway with RaptorQ FEC (lcrq) in IST-SRC mode, no ACK:
 *  - Intercepts outgoing IPv4 packets via NFQUEUE (queue 1).
 *  - Groups packets in generations (blocks) of K packets.
 *  - For each block:
 *      * forwards each original packet immediately to the ground station
 *        as an ORIG symbol (is_source=1, ESI=0..K-1),
 *      * stores the K packets in a contiguous buffer of size F = K*T,
 *        where T = symbol size (packet length, padded to multiple of 4),
 *      * runs rq_encode(rq, data, F),
 *      * generates "repairs_per_block" repair symbols with rq_symbol()
 *        and sends them as is_source=0, ESI = arbitrary (from rq_pid).
 *
 *  - Uses two UDP sockets bound to IFACE_A and IFACE_B, like your XOR version.
 *
 *  - NOW: includes an internal Decision Engine (g_decision) that centralizes:
 *      * choose_iface()      -> interfaz A/B por símbolo (ORIG/repair)
 *      * decide_repairs()    -> nº de repairs por bloque
 *      * decide_block_size() -> tamaño K del bloque
 *      * update_link_metrics()-> hook para métricas dinámicas (link quality)
 *
 * Build (ejemplo):
 *   gcc -O2 -Wall -o its_gateway_rq_istsrc its_gateway_rq_istsrc.c \
 *       -lnetfilter_queue -llcrq
 *
 * Run (ejemplo):
 *   ./its_gateway_rq_istsrc [repairs_per_block] [policy]
 *
 *   policy:
 *     - "default"  (por defecto): K=K_DATA, repairs=fijo, reparto A/B por paridad de ESI.
 *     - "quality":              usa métricas (loss/RTT) para elegir SIEMPRE el mejor enlace.
 *     - "adaptive":             balanceo adaptativo según calidad relativa de cada enlace.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include <arpa/inet.h>
#include <netinet/ip.h>
#include <net/if.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <linux/netfilter.h>
#include <libnetfilter_queue/libnetfilter_queue.h>

#include <lcrq.h>   /* librería RaptorQ (lcrq) */

#include <time.h>
#include <signal.h>

/* --- Parámetros de topología (ajusta a tu entorno) --- */

#define IFACE_A        "term1gs1"         // interfaz hacia camino 1 LEO/GS
#define IFACE_B        "term1gs2"         // interfaz hacia camino 2 LEO/GS

#define GS_IP_A        "10.102.99.1"
#define GS_IP_B        "10.102.100.1"
#define GS_UDP_PORT    5000

#define MAX_PKT_LEN    1500
#define K_DATA         8
#define MAX_BLOCKS     1024

/* --- Computational statistics --- */

struct stats {
    uint64_t pkts_intercepted;

    uint64_t pkts_forwarded;        // data + parity
    uint64_t pkts_original;         // SOLO data

    uint64_t bytes_forwarded;       // data + parity (payload real)
    uint64_t bytes_original;        // SOLO data

    uint64_t blocks_completed;

    /* Encoding timing */
    uint64_t enc_calls;
    uint64_t enc_time_ns;

    /* Repair generation timing */
    uint64_t repair_calls;
    uint64_t repair_time_ns;

    /* NFQUEUE latency */
    uint64_t nfq_pkts;
    uint64_t nfq_latency_ns;
};

static struct stats stats = {0};
static uint64_t t_start_ns = 0;
static uint64_t t_end_ns   = 0;

/* --- Cabecera FEC mínima GW->GS --- */

struct rq_data_hdr {
    uint16_t block_id;     // id de generación / bloque
    uint16_t k;            // nº de símbolos fuente (K)
    uint32_t esi;          // Encoding Symbol ID (24 bits útiles)
    uint16_t symbol_len;   // T
    uint8_t  is_source;    // 1=ORIG, 0=repair
    uint8_t  reserved;
} __attribute__((packed));

/* ======================================================================= */
/*                         DECISION ENGINE INTERNO                         */
/* ======================================================================= */

/* Interfaces de salida hacia GS */
typedef enum {
    GW_IFACE_A = 0,
    GW_IFACE_B = 1,
} gw_iface_t;

/* Política global seleccionada */
typedef enum {
    GW_POLICY_DEFAULT  = 0,
    GW_POLICY_QUALITY  = 1,
    GW_POLICY_ADAPTIVE = 2,
} gw_policy_t;

static gw_policy_t g_policy = GW_POLICY_DEFAULT;

/* Metadatos sobre cada fragmento a enviar */
struct fragment_meta {
    uint32_t block_id;   // id de bloque/generación
    uint32_t sym_index;  // ESI o índice 0..K-1
    int      is_source;  // 1=ORIG, 0=repair
};

/* Metadatos sobre el bloque/generación */
struct block_meta {
    uint32_t block_id;
    uint16_t K;              // nº símbolos fuente
    uint16_t repairs_planned;
    uint16_t repairs_sent;
    uint32_t data_sent_bytes;
};

/* Métricas de calidad por enlace (stub por ahora) */
struct link_metrics {
    double loss;     // 0..1 (estimación de pérdida)
    double rtt_ms;   // RTT estimado en ms
};

struct decision_metrics {
    struct link_metrics a;   // métricas de IFACE_A
    struct link_metrics b;   // métricas de IFACE_B
};

static volatile sig_atomic_t stop_flag = 0;

static inline uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

/* Leer métricas reales desde un fichero externo.
 * Formato esperado (ejemplo):
 *   loss_a=0.01
 *   rtt_a=25.0
 *   loss_b=0.05
 *   rtt_b=40.0
 */
static int fetch_metrics_from_file(const char *path, struct decision_metrics *out)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        //perror("[GW] fopen(metrics_file)");
        return -1;
    }

    char line[256];
    double la=-1, lb=-1, ra=-1, rb=-1;

    while (fgets(line, sizeof(line), f)) {
        if      (sscanf(line, "loss_a=%lf", &la) == 1) {}
        else if (sscanf(line, "loss_b=%lf", &lb) == 1) {}
        else if (sscanf(line, "rtt_a=%lf",  &ra) == 1) {}
        else if (sscanf(line, "rtt_b=%lf",  &rb) == 1) {}
    }

    fclose(f);

    if (la < 0 || lb < 0 || ra < 0 || rb < 0) {
        fprintf(stderr, "[GW] metrics_file incomplete, ignoring\n");
        return -1;
    }

    out->a.loss   = la;
    out->a.rtt_ms = ra;
    out->b.loss   = lb;
    out->b.rtt_ms = rb;

    return 0;
}

/* API del Decision Engine */
struct decision_ops {
    gw_iface_t (*choose_iface)(
        struct decision_ops *ops,
        const struct fragment_meta *frag);

    uint16_t (*decide_repairs)(
        struct decision_ops *ops,
        const struct block_meta *blk);

    uint16_t (*decide_block_size)(
        struct decision_ops *ops);

    void (*update_link_metrics)(
        struct decision_ops *ops,
        const struct decision_metrics *m);
};

/* ------------------- Implementación por defecto ------------------------- */

/*
 * Política "default":
 *  - K fijo = K_DATA.
 *  - repairs fijos = repairs_per_block.
 *  - interfaz: emula la lógica anterior basada en paridad de ESI:
 *      * iface A si sym_index par
 *      * iface B si sym_index impar
 *    (para ORIG: sym_index = 0..K-1; para repairs: sym_index = ESI devuelto por RaptorQ).
 */

struct default_decision {
    struct decision_ops ops;
    uint16_t fixed_K;
    uint16_t fixed_repairs;

    struct decision_metrics metrics;  // por si en el futuro usamos métricas también
};

static gw_iface_t default_choose_iface(struct decision_ops *ops,
                                       const struct fragment_meta *frag)
{
    struct default_decision *st = (struct default_decision *)ops;
    (void)st; /* por ahora no usamos métricas en default */

    /* Política original: paridad del índice/ESI */
    if (frag->sym_index & 1)
        return GW_IFACE_B;
    else
        return GW_IFACE_A;
}

static uint16_t default_decide_repairs(struct decision_ops *ops,
                                       const struct block_meta *blk)
{
    struct default_decision *st = (struct default_decision *)ops;
    (void)blk;
    return st->fixed_repairs;
}

static uint16_t default_decide_block_size(struct decision_ops *ops)
{
    struct default_decision *st = (struct default_decision *)ops;
    return st->fixed_K;
}

static void default_update_link_metrics(struct decision_ops *ops,
                                        const struct decision_metrics *m)
{
    struct default_decision *st = (struct default_decision *)ops;
    if (m) st->metrics = *m;  // guardamos por si se quiere loggear más adelante
}

static struct decision_ops *decision_create_default(uint16_t K,
                                                    uint16_t repairs)
{
    struct default_decision *st = calloc(1, sizeof(*st));
    if (!st) {
        perror("[GW] calloc(default_decision)");
        return NULL;
    }

    st->ops.choose_iface        = default_choose_iface;
    st->ops.decide_repairs      = default_decide_repairs;
    st->ops.decide_block_size   = default_decide_block_size;
    st->ops.update_link_metrics = default_update_link_metrics;

    st->fixed_K       = K;
    st->fixed_repairs = repairs;

    memset(&st->metrics, 0, sizeof(st->metrics));

    return &st->ops;
}

/* ------------------- Implementación "quality" -------------------------- */

/*
 * Política "quality":
 *  - Pensada para usar métricas reales (loss, RTT) y adaptar:
 *      * interfaz preferente según calidad,
 *      * nº de repairs según pérdida,
 *      * K dinámico según estabilidad del enlace.
 *  - Por ahora, si no hay métricas (loss=0, rtt=0), se comporta parecido a default.
 */

struct quality_decision {
    struct decision_ops ops;

    struct decision_metrics metrics;

    uint32_t rr_counter;   // por si queremos empatar con RR
    uint16_t base_K;       // K base (p.ej. 8)
    uint16_t min_repairs;
    uint16_t max_repairs;
};

static void quality_update_link_metrics(struct decision_ops *ops,
                                        const struct decision_metrics *m)
{
    struct quality_decision *qd = (struct quality_decision *)ops;
    if (m) qd->metrics = *m;
}

static gw_iface_t quality_choose_iface(struct decision_ops *ops,
                                       const struct fragment_meta *frag)
{
    struct quality_decision *qd = (struct quality_decision *)ops;
    (void)frag;

    double loss_a = qd->metrics.a.loss;
    double loss_b = qd->metrics.b.loss;

    /* Si no hay métricas (todo 0), usar round-robin como fallback */
    if (loss_a == 0.0 && loss_b == 0.0) {
        return (qd->rr_counter++ & 1) ? GW_IFACE_B : GW_IFACE_A;
    }

    /* Ejemplo simple: elegir interfaz con menor pérdida */
    if (loss_a <= loss_b)
        return GW_IFACE_A;
    else
        return GW_IFACE_B;
}

static uint16_t quality_decide_repairs(struct decision_ops *ops,
                                       const struct block_meta *blk)
{
    struct quality_decision *qd = (struct quality_decision *)ops;
    (void)blk;

    double avg_loss = 0.5 * (qd->metrics.a.loss + qd->metrics.b.loss);

    if (avg_loss <= 0.0) {
        /* Sin métricas → elegir algo intermedio */
        return (qd->min_repairs + qd->max_repairs) / 2;
    }

    if (avg_loss < 0.005) { // <0.5% pérdida: red muy buena
        return qd->min_repairs;
    } else if (avg_loss < 0.02) { // <2% pérdida: red normal
        return (qd->min_repairs + qd->max_repairs) / 2;
    } else {
        /* Red mala */
        return qd->max_repairs;
    }
}

static uint16_t quality_decide_block_size(struct decision_ops *ops)
{
    struct quality_decision *qd = (struct quality_decision *)ops;

    double avg_loss = 0.5 * (qd->metrics.a.loss + qd->metrics.b.loss);

    if (avg_loss <= 0.0) {
        /* Sin métricas → devolvemos K base */
        return qd->base_K;
    }

    /* Ejemplo simple: bloques más grandes si la red es buena */
    if (avg_loss < 0.01) {
        return qd->base_K * 2;  // p.ej. 16 (se clampéa a K_DATA si es >K_DATA)
    } else {
        return qd->base_K;      // p.ej. 8
    }
}

static struct decision_ops *decision_create_quality(uint16_t base_K,
                                                    uint16_t min_rep,
                                                    uint16_t max_rep)
{
    struct quality_decision *qd = calloc(1, sizeof(*qd));
    if (!qd) {
        perror("[GW] calloc(quality_decision)");
        return NULL;
    }

    qd->ops.choose_iface        = quality_choose_iface;
    qd->ops.decide_repairs      = quality_decide_repairs;
    qd->ops.decide_block_size   = quality_decide_block_size;
    qd->ops.update_link_metrics = quality_update_link_metrics;

    qd->base_K      = base_K;
    qd->min_repairs = min_rep;
    qd->max_repairs = max_rep;
    qd->rr_counter  = 0;

    memset(&qd->metrics, 0, sizeof(qd->metrics));

    return &qd->ops;
}

/* ------------------- Implementación "adaptive" ------------------------- */

/*
 * Política "adaptive":
 *  - Usa métricas de ambos enlaces (loss, RTT) para:
 *      * calcular un "score" de calidad por enlace,
 *      * repartir tráfico de forma probabilística según la calidad relativa:
 *          pA = scoreA / (scoreA + scoreB), pB = 1 - pA
 *      * ajustar nº de repairs según pérdida media,
 *      * ajustar K dinámicamente según pérdida media.
 *  - Si no hay métricas (todo 0), actúa parecido a un RR aleatorio.
 */

struct adaptive_decision {
    struct decision_ops   ops;
    struct decision_metrics metrics;

    uint32_t rand_state;       // RNG local
    uint16_t base_K;
    uint16_t min_repairs;
    uint16_t max_repairs;
};

static void adaptive_update_link_metrics(struct decision_ops *ops,
                                         const struct decision_metrics *m)
{
    struct adaptive_decision *ad = (struct adaptive_decision *)ops;
    if (m) ad->metrics = *m;
}

/* Score inverso a pérdida y RTT (simple, sólo para ejemplo) */
static double adaptive_score_link(const struct link_metrics *m)
{
    double loss = m->loss;
    double rtt  = m->rtt_ms;

    if (loss < 0.0001) loss = 0.0001;
    if (rtt  < 1.0)    rtt  = 1.0;

    return 1.0 / (loss * 100.0 + rtt);
}

static gw_iface_t adaptive_choose_iface(struct decision_ops *ops,
                                        const struct fragment_meta *frag)
{
    struct adaptive_decision *ad = (struct adaptive_decision *)ops;
    (void)frag;

    double sA = adaptive_score_link(&ad->metrics.a);
    double sB = adaptive_score_link(&ad->metrics.b);

    if (sA <= 0.0 && sB <= 0.0) {
        /* Fallback: si no hay info sensata, 50/50 aleatorio */
        ad->rand_state = ad->rand_state * 1103515245u + 12345u;
        return (ad->rand_state & 1u) ? GW_IFACE_B : GW_IFACE_A;
    }

    double sum = sA + sB;
    if (sum <= 0.0) {
        ad->rand_state = ad->rand_state * 1103515245u + 12345u;
        return (ad->rand_state & 1u) ? GW_IFACE_B : GW_IFACE_A;
    }

    double pA = sA / sum;

    /* RNG simple tipo LCG para generar u ~ U(0,1) */
    ad->rand_state = ad->rand_state * 1103515245u + 12345u;
    double u = (double)(ad->rand_state & 0xFFFFFFu) / (double)0x1000000u;

    return (u < pA) ? GW_IFACE_A : GW_IFACE_B;
}

static uint16_t adaptive_decide_repairs(struct decision_ops *ops,
                                        const struct block_meta *blk)
{
    struct adaptive_decision *ad = (struct adaptive_decision *)ops;
    (void)blk;

    uint16_t min_r = ad->min_repairs;
    uint16_t max_r = ad->max_repairs;
    if (max_r < min_r) {
        uint16_t tmp = max_r;
        max_r = min_r;
        min_r = tmp;
    }

    double avg_loss = 0.5 * (ad->metrics.a.loss + ad->metrics.b.loss);

    if (avg_loss <= 0.001) {
        /* red muy buena → mínimo de repairs */
        return min_r;
    } else if (avg_loss <= 0.01) {
        /* pérdida moderada */
        return (uint16_t)((min_r + max_r) / 2);
    } else {
        /* red mala → máximo de repairs */
        return max_r;
    }
}

static uint16_t adaptive_decide_block_size(struct decision_ops *ops)
{
    struct adaptive_decision *ad = (struct adaptive_decision *)ops;

    double avg_loss = 0.5 * (ad->metrics.a.loss + ad->metrics.b.loss);

    if (avg_loss <= 0.002) {
        /* red muy estable → bloques más grandes (se clampéa a K_DATA si procede) */
        return ad->base_K * 2;
    } else {
        /* red normal/mala → bloque base */
        return ad->base_K;
    }
}

static struct decision_ops *decision_create_adaptive(uint16_t base_K,
                                                     uint16_t min_rep,
                                                     uint16_t max_rep)
{
    struct adaptive_decision *ad = calloc(1, sizeof(*ad));
    if (!ad) {
        perror("[GW] calloc(adaptive_decision)");
        return NULL;
    }

    ad->ops.choose_iface        = adaptive_choose_iface;
    ad->ops.decide_repairs      = adaptive_decide_repairs;
    ad->ops.decide_block_size   = adaptive_decide_block_size;
    ad->ops.update_link_metrics = adaptive_update_link_metrics;

    ad->base_K      = base_K;
    ad->min_repairs = min_rep;
    ad->max_repairs = max_rep;
    ad->rand_state  = 0x12345678u;

    memset(&ad->metrics, 0, sizeof(ad->metrics));

    return &ad->ops;
}

/* Instancia global del Decision Engine y métricas actuales */

static struct decision_ops   *g_decision       = NULL;
static struct decision_metrics g_current_metrics;

/* Stub para actualizar métricas (más adelante leerá Prometheus/REST/tc) */
static void gateway_update_metrics(void)
{
    if (g_decision && g_decision->update_link_metrics) {
        g_decision->update_link_metrics(g_decision, &g_current_metrics);
    }
}

/* ======================================================================= */
/*              ESTADO POR BLOQUE EN EL GATEWAY (RAPTORQ ENC)             */
/* ======================================================================= */

struct rq_block_enc {
    int      in_use;
    uint16_t block_id;

    uint16_t K;           // tamaño de bloque (símbolos fuente)
    uint16_t T;           // tamaño de símbolo (T)
    uint64_t F;           // F = K*T

    int      num_source;      // cuántos ORIG (0..K)
    int      repairs_to_send; // cuántos repairs se generarán

    rq_t    *rq;          // contexto RaptorQ
    uint8_t *data;        // buffer contiguo F bytes con K símbolos fuente
};

static struct rq_block_enc blocks[MAX_BLOCKS];
static uint16_t next_block_id = 1;
static struct rq_block_enc *current_block = NULL;

/* nº de repairs a enviar por bloque (valor base configurable vía CLI) */
static int repairs_per_block = 4;

/* --- Sockets UDP hacia GS --- */

static int sock_a = -1;
static int sock_b = -1;
static struct sockaddr_in gs_addr_a;
static struct sockaddr_in gs_addr_b;

/* --- Utilidades --- */

static void die(const char *msg)
{
    perror(msg);
    exit(EXIT_FAILURE);
}

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

static const char *policy_name(gw_policy_t p)
{
    switch (p) {
    case GW_POLICY_QUALITY:  return "quality";
    case GW_POLICY_ADAPTIVE: return "adaptive";
    case GW_POLICY_DEFAULT:
    default:                 return "default";
    }
}

static void init_udp_sockets(void)
{
    sock_a = create_udp_socket_bound(IFACE_A);
    sock_b = create_udp_socket_bound(IFACE_B);

    memset(&gs_addr_a, 0, sizeof(gs_addr_a));
    gs_addr_a.sin_family = AF_INET;
    gs_addr_a.sin_port   = htons(GS_UDP_PORT);
    inet_pton(AF_INET, GS_IP_A, &gs_addr_a.sin_addr);

    memset(&gs_addr_b, 0, sizeof(gs_addr_b));
    gs_addr_b.sin_family = AF_INET;
    gs_addr_b.sin_port   = htons(GS_UDP_PORT);
    inet_pton(AF_INET, GS_IP_B, &gs_addr_b.sin_addr);

    fprintf(stderr,
            "[GW] Ground station A at %s:%u, B at %s:%u\n"
            "[GW] K_DATA=%d, repairs_per_block(base)=%d, policy=%s\n",
            GS_IP_A, GS_UDP_PORT, GS_IP_B, GS_UDP_PORT,
            K_DATA, repairs_per_block,
            policy_name(g_policy));
}

/* Enviar símbolo RaptorQ por interfaz explícita (A/B) elegida por Decision Engine */
static void send_rq_symbol(const struct rq_data_hdr *hdr,
                           const uint8_t *symbol, int symbol_len,
                           gw_iface_t iface)
{
    uint8_t buf[sizeof(struct rq_data_hdr) + MAX_PKT_LEN];

    if (symbol_len > MAX_PKT_LEN) {
        fprintf(stderr, "[GW] Ignoring symbol_len=%d > MAX_PKT_LEN\n", symbol_len);
        return;
    }

    memcpy(buf, hdr, sizeof(*hdr));
    memcpy(buf + sizeof(*hdr), symbol, symbol_len);

    int use_sock;
    struct sockaddr_in *dest;
    const char *iface_name;
    const char *ip_str;

    if (iface == GW_IFACE_B) {
        use_sock   = sock_b;
        dest       = &gs_addr_b;
        iface_name = IFACE_B;
        ip_str     = GS_IP_B;
    } else {
        use_sock   = sock_a;
        dest       = &gs_addr_a;
        iface_name = IFACE_A;
        ip_str     = GS_IP_A;
    }

    //uint32_t esi_host = ntohl(hdr->esi);

    ssize_t sent = sendto(use_sock, buf, sizeof(*hdr) + symbol_len, 0,
                          (struct sockaddr *)dest, sizeof(*dest));
    if (sent < 0) {
        perror("[GW] sendto(GS)");
    } else {
        stats.pkts_forwarded++;
        stats.bytes_forwarded += symbol_len;

        if (hdr->is_source) {
            stats.pkts_original++;
            stats.bytes_original += symbol_len;
        }

        /*fprintf(stderr,
                "[GW] block=%u esi=%u src=%u len=%u -> %s (%s)\n",
                ntohs(hdr->block_id),
                esi_host,
                hdr->is_source,
                symbol_len,
                iface_name, ip_str);*/
    }
}

/* --- Gestión de bloques --- */

static struct rq_block_enc *alloc_new_block(void)
{
    uint16_t id = next_block_id++;
    int idx = id % MAX_BLOCKS;

    struct rq_block_enc *b = &blocks[idx];

    if (b->in_use) {
        fprintf(stderr, "[GW] WARNING: overwriting active block_id=%u (slot=%d)\n",
                b->block_id, idx);
        if (b->rq)   rq_free(b->rq);
        if (b->data) free(b->data);
    }

    memset(b, 0, sizeof(*b));
    b->in_use   = 1;
    b->block_id = id;

    return b;
}

/* --- Lógica principal: paquete IP interceptado --- */

static void handle_ip_packet(const uint8_t *pkt, int len, uint64_t t_nfq)
{
    if (len <= 0 || len > MAX_PKT_LEN) {
        fprintf(stderr, "[GW] Ignoring packet of invalid length %d\n", len);
        return;
    }

    /* Crear nuevo bloque si no hay actual o está completo */
    if (!current_block || current_block->num_source >= current_block->K) {
        current_block = alloc_new_block();
        struct rq_block_enc *b = current_block;

        /* Preguntar al Decision Engine el tamaño de bloque K */
        uint16_t K = K_DATA;
        if (g_decision && g_decision->decide_block_size) {
            uint16_t k_dec = g_decision->decide_block_size(g_decision);
            if (k_dec == 0 || k_dec > K_DATA) {
                /* Guardrails: por ahora no permitimos K=0 ni >K_DATA */
                K = K_DATA;
            } else {
                K = k_dec;
            }
        }

        b->K = K;
        b->T = (uint16_t)len;

        if (b->T % 4 != 0) {
            fprintf(stderr,
                    "[GW] WARNING: symbol_len T=%u not multiple of 4 (padding implied)\n",
                    b->T);
        }

        b->F = (uint64_t)b->K * (uint64_t)b->T;

        b->data = calloc(1, b->F);
        if (!b->data) {
            perror("[GW] calloc(data)");
            b->in_use = 0;
            current_block = NULL;
            return;
        }

        b->rq = rq_init(b->F, b->T);
        if (!b->rq) {
            perror("[GW] rq_init");
            free(b->data);
            b->data = NULL;
            b->in_use = 0;
            current_block = NULL;
            return;
        }

        /* Construimos meta para decidir nº de repairs */
        struct block_meta bm;
        memset(&bm, 0, sizeof(bm));
        bm.block_id         = b->block_id;
        bm.K                = b->K;
        bm.repairs_planned  = 0;
        bm.repairs_sent     = 0;
        bm.data_sent_bytes  = 0;

        uint16_t reps = (uint16_t)repairs_per_block;
        if (g_decision && g_decision->decide_repairs) {
            reps = g_decision->decide_repairs(g_decision, &bm);
        }
        b->repairs_to_send = (int)reps;
        b->num_source      = 0;

        fprintf(stderr,
                "[GW] Starting block %u (K=%u, T=%u, F=%" PRIu64 ", repairs=%d)\n",
                b->block_id, b->K, b->T, b->F, b->repairs_to_send);
    }

    struct rq_block_enc *b = current_block;

    /* Verificar longitud coherente dentro del bloque */
    if ((uint16_t)len != b->T) {
        fprintf(stderr,
                "[GW] Packet len change in block %u (T_old=%u, len_new=%d). "
                "Closing block without FEC and starting a new one.\n",
                b->block_id, b->T, len);

        if (b->rq)   rq_free(b->rq);
        if (b->data) free(b->data);
        b->rq   = NULL;
        b->data = NULL;
        b->in_use = 0;
        current_block = NULL;

        /* Reprocesar el paquete con un nuevo bloque */
        handle_ip_packet(pkt, len, t_nfq);
        return;
    }

    int index = b->num_source;
    if (index >= b->K) {
        fprintf(stderr, "[GW] Internal error: index=%d >= K=%u\n", index, b->K);
        return;
    }

    /* Copiar símbolo fuente i en data + i*T */
    uint8_t *dst = b->data + (uint64_t)index * (uint64_t)b->T;
    memcpy(dst, pkt, len);

    b->num_source++;

    /* ORIG: enviar inmediato a GS usando Decision Engine para elegir interfaz */
    struct rq_data_hdr hdr;
    hdr.block_id   = htons(b->block_id);
    hdr.k          = htons(b->K);
    hdr.esi        = htonl((uint32_t)index);
    hdr.symbol_len = htons(b->T);
    hdr.is_source  = 1;
    hdr.reserved   = 0;

    struct fragment_meta fm;
    fm.block_id  = b->block_id;
    fm.sym_index = (uint32_t)index;
    fm.is_source = 1;

    gw_iface_t iface = GW_IFACE_A;
    if (g_decision && g_decision->choose_iface) {
        iface = g_decision->choose_iface(g_decision, &fm);
    }

    uint64_t t_send = now_ns();
    send_rq_symbol(&hdr, pkt, len, iface);
    stats.nfq_pkts++;
    stats.nfq_latency_ns += (t_send - t_nfq);

    /* Si el bloque está completo, rq_encode + repairs */
    if (b->num_source == b->K) {
        /*fprintf(stderr,
                "[GW] Block %u complete, running RaptorQ encode\n",
                b->block_id);*/

        uint64_t t0 = now_ns();
        int rc = rq_encode(b->rq, b->data, (size_t)b->F);
        uint64_t t1 = now_ns();
        stats.enc_calls++;
        stats.enc_time_ns += (t1 - t0);
        if (rc != 0) {
            fprintf(stderr, "[GW] ERROR: rq_encode failed for block=%u\n",
                    b->block_id);
        } else if (b->repairs_to_send > 0) {
            /*fprintf(stderr,
                    "[GW] Block %u: generating %d repair symbols\n",
                    b->block_id, b->repairs_to_send);*/

            for (int r = 0; r < b->repairs_to_send; r++) {
                rq_pid_t pid = 0;
                uint8_t  sym[MAX_PKT_LEN];

                uint64_t tr0 = now_ns();
                rq_symbol(b->rq, &pid, sym, RQ_RAND);
                uint64_t tr1 = now_ns();
                stats.repair_calls++;
                stats.repair_time_ns += (tr1 - tr0);

                uint32_t esi = rq_pid2esi(pid);

                struct rq_data_hdr rhdr;
                rhdr.block_id   = htons(b->block_id);
                rhdr.k          = htons(b->K);
                rhdr.esi        = htonl(esi);
                rhdr.symbol_len = htons(b->T);
                rhdr.is_source  = 0;
                rhdr.reserved   = 0;

                struct fragment_meta fm_r;
                fm_r.block_id  = b->block_id;
                fm_r.sym_index = esi;
                fm_r.is_source = 0;

                gw_iface_t iface_r = GW_IFACE_A;
                if (g_decision && g_decision->choose_iface) {
                    iface_r = g_decision->choose_iface(g_decision, &fm_r);
                }

                send_rq_symbol(&rhdr, sym, b->T, iface_r);
            }
        }

        /*fprintf(stderr,
                "[GW] Block %u: sent %u ORIG + %d repairs\n",
                b->block_id, b->K, b->repairs_to_send);*/

        stats.blocks_completed++;

        /* Cerrar y liberar bloque (no usamos ACK en esta versión) */
        if (b->rq)   rq_free(b->rq);
        if (b->data) free(b->data);
        b->rq   = NULL;
        b->data = NULL;
        b->in_use = 0;
        current_block = NULL;
    }
}

/* --- NFQUEUE callback --- */

static int nfq_cb(struct nfq_q_handle *qh, struct nfgenmsg *nfmsg,
                  struct nfq_data *nfa, void *data)
{
    (void)nfmsg;
    (void)data;

    uint64_t t_nfq = now_ns();

    if (t_start_ns == 0)
        t_start_ns = t_nfq;   // primer paquete real

    t_end_ns = t_nfq;         // ← 🔥 ACTUALIZAR SIEMPRE

    stats.pkts_intercepted++;

    struct nfqnl_msg_packet_hdr *ph = nfq_get_msg_packet_hdr(nfa);
    uint32_t id = 0;
    if (ph) id = ntohl(ph->packet_id);

    unsigned char *payload = NULL;
    int len = nfq_get_payload(nfa, &payload);
    if (len >= 0 && payload != NULL) {
        handle_ip_packet(payload, len, t_nfq);
    } else {
        fprintf(stderr, "[GW] nfq_get_payload() returned %d\n", len);
    }

    /* Drop del original: ya lo hemos reenviado vía UDP + FEC. */
    return nfq_set_verdict(qh, id, NF_DROP, 0, NULL);
}

static void sigint_handler(int sig)
{
    (void)sig;
    stop_flag = 1;
}

int main(int argc, char *argv[])
{
    /* Parseo simple de CLI:
     *   argv[1] = repairs_per_block (opcional, por defecto 4)
     *   argv[2] = policy: "default" | "quality" | "adaptive"
     *                    (opcional, por defecto "default")
     */
    if (argc >= 2) {
        repairs_per_block = atoi(argv[1]);
        if (repairs_per_block < 0) repairs_per_block = 0;
    }

    if (argc >= 3) {
        if (strcmp(argv[2], "quality") == 0) {
            g_policy = GW_POLICY_QUALITY;
        } else if (strcmp(argv[2], "adaptive") == 0) {
            g_policy = GW_POLICY_ADAPTIVE;
        } else {
            g_policy = GW_POLICY_DEFAULT;
        }
    }

    fprintf(stderr,
            "[GW] repairs_per_block(CLI)=%d, policy=%s\n",
            repairs_per_block,
            policy_name(g_policy));

    /* Crear Decision Engine según política */
    if (g_policy == GW_POLICY_QUALITY) {
        /* Ejemplo: base_K=K_DATA, min_rep=2, max_rep=repairs_per_block */
        uint16_t min_rep = 2;
        uint16_t max_rep = (repairs_per_block > 0) ? (uint16_t)repairs_per_block : 2;
        g_decision = decision_create_quality(K_DATA, min_rep, max_rep);
    } else if (g_policy == GW_POLICY_ADAPTIVE) {
        /* Adaptive: rango de repairs más amplio, p.ej. [1, repairs_per_block] */
        uint16_t min_rep = 1;
        uint16_t max_rep = (repairs_per_block > 0) ? (uint16_t)repairs_per_block : 2;
        g_decision = decision_create_adaptive(K_DATA, min_rep, max_rep);
    } else {
        g_decision = decision_create_default(K_DATA, (uint16_t)repairs_per_block);
    }

    if (!g_decision) {
        fprintf(stderr, "[GW] ERROR: cannot create decision engine\n");
        return EXIT_FAILURE;
    }

    memset(blocks, 0, sizeof(blocks));
    memset(&g_current_metrics, 0, sizeof(g_current_metrics));

    init_udp_sockets();

    struct nfq_handle    *h  = nfq_open();
    struct nfq_q_handle  *qh = NULL;
    int fd, rv;
    char buf[4096] __attribute__((aligned));

    if (!h) die("nfq_open");

    if (nfq_unbind_pf(h, AF_INET) < 0) {
        fprintf(stderr, "[GW] nfq_unbind_pf failed\n");
        nfq_close(h);
        return EXIT_FAILURE;
    }
    if (nfq_bind_pf(h, AF_INET) < 0) {
        fprintf(stderr, "[GW] nfq_bind_pf failed\n");
        nfq_close(h);
        return EXIT_FAILURE;
    }

    qh = nfq_create_queue(h, 1, &nfq_cb, NULL);
    if (!qh) {
        fprintf(stderr, "[GW] nfq_create_queue failed\n");
        nfq_close(h);
        return EXIT_FAILURE;
    }

    if (nfq_set_mode(qh, NFQNL_COPY_PACKET, 0xffff) < 0) {
        fprintf(stderr, "[GW] nfq_set_mode failed\n");
        nfq_destroy_queue(qh);
        nfq_close(h);
        return EXIT_FAILURE;
    }

    fd = nfq_fd(h);

    struct sigaction sa = {0};
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    fprintf(stderr, "[GW] Listening on NFQUEUE 1...\n");

    //t_start_ns = now_ns();

    while (!stop_flag && (rv = recv(fd, buf, sizeof(buf), 0)) >= 0) {
        static uint64_t pkt_counter = 0;
        pkt_counter++;

        /* ------------------------------------------------------------------ */
        /*  NUEVO: métricas desde fichero (si existe)                         */
        /* ------------------------------------------------------------------ */
        if (fetch_metrics_from_file("/tmp/its_metrics.txt", &g_current_metrics) == 0) {
            /* OK: g_current_metrics actualizado desde fichero */
        } else {
            /* fallback: patrón sintético como antes */
            /* Patrón de variación de pérdida/RTT por fases (demo) */
            if (pkt_counter <= 400) {
                /* Fase 1: ambos buenos, A un poco más rápido */
                g_current_metrics.a.loss   = 0.00;
                g_current_metrics.a.rtt_ms = 20.0;

                g_current_metrics.b.loss   = 0.00;
                g_current_metrics.b.rtt_ms = 40.0;

            } else if (pkt_counter <= 800) {
                /* Fase 2: B empeora (pérdida) */
                g_current_metrics.a.loss   = 0.00;
                g_current_metrics.a.rtt_ms = 25.0;

                g_current_metrics.b.loss   = 0.05;  // 5% pérdida
                g_current_metrics.b.rtt_ms = 45.0;

            } else if (pkt_counter <= 1200) {
                /* Fase 3: A se degrada fuerte, B mejora */
                g_current_metrics.a.loss   = 0.10;  // 10% pérdida
                g_current_metrics.a.rtt_ms = 60.0;

                g_current_metrics.b.loss   = 0.02;  // 2% pérdida
                g_current_metrics.b.rtt_ms = 35.0;

            } else if (pkt_counter <= 1600) {
                /* Fase 4: ambos malos */
                g_current_metrics.a.loss   = 0.08;
                g_current_metrics.a.rtt_ms = 80.0;

                g_current_metrics.b.loss   = 0.08;
                g_current_metrics.b.rtt_ms = 80.0;

            } else {
                /* Fase 5: recuperación parcial */
                g_current_metrics.a.loss   = 0.01;
                g_current_metrics.a.rtt_ms = 30.0;

                g_current_metrics.b.loss   = 0.03;
                g_current_metrics.b.rtt_ms = 35.0;
            }
        }

        /* Notificamos al Decision Engine las métricas actualizadas */
        gateway_update_metrics();

        /* Procesamos el paquete de NFQUEUE */
        nfq_handle_packet(h, buf, rv);
    }

    //t_end_ns = now_ns();
    fprintf(stderr, "[GW] Stopping...\n");

    fprintf(stderr, "[GW] recv() failed, rv=%d, errno=%d\n", rv, errno);

    double duration_s = (double)(t_end_ns - t_start_ns) / 1e9;
    double thr_fwd_pps = duration_s > 0 ? (double)stats.pkts_forwarded / duration_s : 0.0;
    double thr_orig_pps = duration_s > 0 ? (double)stats.pkts_original / duration_s : 0.0;
    double thr_fwd_mbps = duration_s > 0 ? (stats.bytes_forwarded * 8.0) / (duration_s * 1e6) : 0.0;
    double thr_orig_mbps = duration_s > 0 ? (stats.bytes_original * 8.0) / (duration_s * 1e6) : 0.0;

    fprintf(stderr,
            "\n[GW] === Computational statistics (RQ) ===\n"
            "Intercepted packets      : %lu\n"
            "Forwarded symbols (all)  : %lu\n"
            "Forwarded originals     : %lu\n"
            "Completed blocks        : %lu\n"
            "Experiment duration     : %.3f s\n"
            "\n"
            "Throughput (all symbols): %.2f pkts/s | %.2f Mbps\n"
            "Throughput (originals)  : %.2f pkts/s | %.2f Mbps\n"
            "\n"
            "RQ encode calls         : %lu\n"
            "RQ encode time avg      : %.2f us\n"
            "RQ repair calls         : %lu\n"
            "RQ repair time avg      : %.2f us\n"
            "NFQUEUE pkts            : %lu\n"
            "NFQUEUE latency avg     : %.2f us\n",
            stats.pkts_intercepted,
            stats.pkts_forwarded,
            stats.pkts_original,
            stats.blocks_completed,
            duration_s,
            thr_fwd_pps,
            thr_fwd_mbps,
            thr_orig_pps,
            thr_orig_mbps,
            stats.enc_calls,
            stats.enc_calls ? (stats.enc_time_ns / stats.enc_calls) / 1000.0 : 0.0,
            stats.repair_calls,
            stats.repair_calls ? (stats.repair_time_ns / stats.repair_calls) / 1000.0 : 0.0,
            stats.nfq_pkts,
            stats.nfq_pkts ? (stats.nfq_latency_ns / stats.nfq_pkts) / 1000.0 : 0.0
            );


    nfq_destroy_queue(qh);
    nfq_close(h);

    if (sock_a >= 0) close(sock_a);
    if (sock_b >= 0) close(sock_b);

    return 0;
}
