// destination_server.c - UDP destination application with run_id filtering and robust run delimitation
// Uso:
//   ./destination-server -a 0.0.0.0 -p 12345 -s 2048 -n 2000
// -n: número esperado de datagramas (opcional); si no se pasa, se asume 0..max_seq

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netinet/in.h>
#include <inttypes.h>  // PRIu64
#include <limits.h>
#include <errno.h>
#include <sys/time.h>
#include <signal.h>

#define DEFAULT_PORT  12345
#define DEFAULT_BUFSZ 2048
#define DEFAULT_EXPECTED (-1)
#define SEEN_CAP_DEFAULT (1<<20)  // 1.048.576 posiciones (sobra para 2000)

static void usage(const char *prog) {
    fprintf(stderr,
        "Uso: %s [-a ip_escucha] [-p puerto] [-s tam_buffer] [-n expected]\n",
        prog);
}

/* ---- Tracking de secuencias ---- */
static uint8_t *seen = NULL;
static size_t   seen_cap = 0;
static uint64_t uniques = 0, dups = 0;
static uint32_t min_seq = UINT32_MAX, max_seq = 0;
static volatile sig_atomic_t stop_requested = 0;

static void handle_stop_signal(int signo) {
    (void)signo;
    stop_requested = 1;
}

static void run_reset(size_t cap) {
    if (seen) free(seen);
    seen_cap = (cap ? cap : SEEN_CAP_DEFAULT);
    seen = (uint8_t*)calloc(seen_cap, 1);
    uniques = 0; dups = 0;
    min_seq = UINT32_MAX; max_seq = 0;
}

static void note_seq(uint32_t seq) {
    if (seq >= seen_cap) return; // fuera de rango (no esperado)
    if (!seen[seq]) {
        seen[seq] = 1;
        uniques++;
        if (seq < min_seq) min_seq = seq;
        if (seq > max_seq) max_seq = seq;
    } else {
        dups++;
    }
}

static void print_missing_sample(int expected) {
    int shown = 0;
    printf("[destination-server] Missing sample: ");
    if (expected <= 0) expected = (int)(max_seq + 1); // suponemos 0..max
    for (int i = 0; i < expected && shown < 16; ++i) {
        if (i < (int)seen_cap && !seen[i]) { printf("%d ", i); shown++; }
    }
    if (!shown) printf("(none)");
    printf("\n");
}

/* ---- Estadísticas por bloque de 8 paquetes ---- */
static void print_block_stats(int expected) {
    if (expected <= 0) return;
    int blocks = expected / 8;
    if (blocks <= 0) return;

    int full = 0, partial = 0, empty = 0;

    for (int b = 0; b < blocks; ++b) {
        int base = b * 8;
        int count = 0;
        for (int i = 0; i < 8; ++i) {
            int idx = base + i;
            if (idx < (int)seen_cap && seen[idx]) count++;
        }
        if (count == 0) empty++;
        else if (count == 8) full++;
        else partial++;
    }

    printf("[destination-server] Blocks (size=8): total=%d full=%d partial=%d empty=%d\n",
           blocks, full, partial, empty);
}

static void print_run_summary(uint32_t run_id, uint64_t matched, int expected) {
    int total_space = (expected > 0) ? expected : (int)(max_seq + 1);
    int missing = (total_space > 0) ? (total_space - (int)uniques) : 0;

    printf("[destination-server] SUMMARY run=0x%08x:\n", run_id);
    printf("  recibidos_totales=%" PRIu64 "  unicos=%" PRIu64 "  duplicados=%" PRIu64 "\n",
           matched, uniques, dups);
    printf("  seq[min..max]=[%u..%u]  esperado=%d  missing=%d\n",
           (min_seq == UINT32_MAX ? 0 : min_seq), max_seq, (expected > 0 ? expected : -1), missing);
    if (missing > 0) print_missing_sample(expected);
    print_block_stats(expected);
    fflush(stdout);
}

/* ---- Main ---- */
int main(int argc, char **argv) {
    const char *ip_escucha = "0.0.0.0";
    int puerto = DEFAULT_PORT;
    int buf_sz = DEFAULT_BUFSZ;
    int expected = DEFAULT_EXPECTED;

    int opt;
    while ((opt = getopt(argc, argv, "a:p:s:n:h")) != -1) {
        switch (opt) {
            case 'a': ip_escucha = optarg; break;
            case 'p': puerto = atoi(optarg); break;
            case 's': buf_sz = atoi(optarg); break;
            case 'n': expected = atoi(optarg); break;
            case 'h': default: usage(argv[0]); return (opt=='h')?0:1;
        }
    }

    if (buf_sz < 8) { fprintf(stderr, "[!] tam_buffer debe ser >= 8\n"); return 1; }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_stop_signal;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGINT, &sa, NULL) < 0 || sigaction(SIGTERM, &sa, NULL) < 0) {
        perror("sigaction");
        return 1;
    }

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) { perror("socket"); return 1; }

    // (opcional) buffer de recepción amplio
    int rcvbuf = 4*1024*1024;
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    // Timeout de recepción: permite despertar periódicamente sin delimitar el run.
    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 500000; // 1.5 s
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(puerto);
    if (inet_pton(AF_INET, ip_escucha, &addr.sin_addr) <= 0) {
        perror("inet_pton"); close(sock); return 1;
    }
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(sock); return 1;
    }

    printf("[destination-server] Listening for UDP on %s:%d (buffer %d B) expected=%d\n",
           ip_escucha, puerto, buf_sz, expected);

    char *buffer = (char*)malloc(buf_sz);
    if (!buffer) { perror("malloc"); close(sock); return 1; }

    uint32_t current_run_id = 0;
    int have_run = 0;
    int summary_emitted = 0;
    uint64_t matched = 0;

    // Inicializa el bitmap (capacidad según expected o por defecto)
    run_reset( (expected>0) ? (size_t)expected : SEEN_CAP_DEFAULT );

    while (!stop_requested) {
        struct sockaddr_in cli; socklen_t clen = sizeof(cli);
        ssize_t n = recvfrom(sock, buffer, buf_sz, 0, (struct sockaddr*)&cli, &clen);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // A receive timeout is not an experiment boundary. Keep the
                // current run active so late/reordered packets with the same
                // run_id remain part of the same measurement.
                continue;
            }
            if (errno == EINTR && stop_requested) break;
            if (errno == EINTR) continue;
            perror("recvfrom");
            break;
        }

        // Extrae cabecera si existe
        uint32_t rid = 0, seq = 0;
        if (n >= 8) {
            memcpy(&rid, buffer, 4);
            memcpy(&seq, buffer+4, 4);
        }

        // AUTO-RESET: si ya teníamos run y llega otro run_id con seq==0, cambiamos de run
        if (n >= 8 && have_run && rid != current_run_id && seq == 0) {
            current_run_id = rid; matched = 0; summary_emitted = 0;
            printf("[destination-server] New run_id detected: 0x%08x (auto-reset)\n", current_run_id);
            run_reset( (expected>0) ? (size_t)expected : SEEN_CAP_DEFAULT );
        }

        // Primer paquete con cabecera válida => fijamos run
        if (!have_run && n >= 8) {
            current_run_id = rid; have_run = 1; matched = 0; summary_emitted = 0;
            printf("[destination-server] New run_id set: 0x%08x\n", current_run_id);
            run_reset( (expected>0) ? (size_t)expected : SEEN_CAP_DEFAULT );
        }

        // Contabiliza solo si pertenece al run activo
        if (have_run && n >= 8 && rid == current_run_id) {
            matched++;
            note_seq(seq);

            printf("[destination-server] #%" PRIu64 " (run=%08x seq=%u) from %s:%d (%zd bytes)\n",
                   matched, rid, seq, inet_ntoa(cli.sin_addr), ntohs(cli.sin_port), n);

            if (expected > 0 && uniques >= (uint64_t)expected && !summary_emitted) {
                print_run_summary(current_run_id, matched, expected);
                summary_emitted = 1;
            }
        } else {
            // Tráfico de otros runs u otros flujos: ignorado
            // printf("[destination-server] (otros) %zd bytes de %s:%d\n", n, inet_ntoa(cli.sin_addr), ntohs(cli.sin_port));
        }
    }

    // Emit a partial/final summary only when the server is explicitly stopped
    // (or exits on a non-timeout receive error). A summary already emitted after
    // receiving every expected unique packet is not duplicated here.
    if (have_run && !summary_emitted) {
        print_run_summary(current_run_id, matched, expected);
    }

    free(seen);
    free(buffer);
    close(sock);
    return 0;
}

