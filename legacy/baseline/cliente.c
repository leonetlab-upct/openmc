// cliente.c - Emisor UDP con cabecera (run_id + seq) y temporización absoluta
// Cambios: envía exactamente un múltiplo de 8 (floor(pps*duración/8)*8) para no perder el último bloque.
// Uso:
//   ./cliente -a 10.102.96.2 -p 12345 -s 1000 -r 200 -t 10
// Nota: tam_payload debe ser >= 8 (4B run_id + 4B seq)

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#define DEFAULT_PORT 12345
#define DEFAULT_SIZE 1000      // bytes de payload por datagrama (>= 8)
#define DEFAULT_PPS  100       // datagramas por segundo
#define DEFAULT_TIME 10        // segundos

static void usage(const char *prog) {
    fprintf(stderr,
        "Uso: %s [-a ip_destino] [-p puerto] [-s tam_payload] [-r pps] [-t seg]\n",
        prog);
}

static inline void add_ns(struct timespec* t, long ns) {
    t->tv_nsec += ns;
    if (t->tv_nsec >= 1000000000L) { t->tv_nsec -= 1000000000L; t->tv_sec++; }
}

static inline uint32_t rand32(void) {
    uint32_t r = (uint32_t)rand();
    r ^= ((uint32_t)rand() << 16);
    return r;
}

int main(int argc, char **argv) {
    const char *ip_destino = "127.0.0.1";
    int puerto = DEFAULT_PORT;
    int tam_payload = DEFAULT_SIZE;
    int pps = DEFAULT_PPS;
    int duracion = DEFAULT_TIME;

    int opt;
    while ((opt = getopt(argc, argv, "a:p:s:r:t:h")) != -1) {
        switch (opt) {
            case 'a': ip_destino = optarg; break;
            case 'p': puerto = atoi(optarg); break;
            case 's': tam_payload = atoi(optarg); break;
            case 'r': pps = atoi(optarg); break;
            case 't': duracion = atoi(optarg); break;
            case 'h': default: usage(argv[0]); return (opt=='h')?0:1;
        }
    }

    if (tam_payload < 8 || tam_payload > 1472) {
        fprintf(stderr, "[!] tam_payload debe estar en [8..1472]\n");
        return 1;
    }
    if (pps <= 0) { fprintf(stderr, "[!] pps debe ser > 0\n"); return 1; }
    if (duracion <= 0) { fprintf(stderr, "[!] duracion debe ser > 0\n"); return 1; }

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) { perror("socket"); return 1; }

    // (opcional) buffer de envío amplio
    int sndbuf = 4*1024*1024;
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

    struct sockaddr_in addr_serv;
    memset(&addr_serv, 0, sizeof(addr_serv));
    addr_serv.sin_family = AF_INET;
    addr_serv.sin_port   = htons(puerto);
    if (inet_pton(AF_INET, ip_destino, &addr_serv.sin_addr) <= 0) {
        perror("inet_pton"); close(sock); return 1;
    }

    char *buffer = (char*)malloc(tam_payload);
    if (!buffer) { perror("malloc"); close(sock); return 1; }
    memset(buffer, 'A', tam_payload);

    // Cabecera de telemetría
    srand((unsigned)time(NULL));
    uint32_t run_id = rand32();
    uint32_t seq = 0;

    // Objetivo exacto = múltiplo de 8 más cercano por debajo a (pps * duracion)
    uint64_t raw_target = (uint64_t)pps * (uint64_t)duracion;
    uint32_t target = (uint32_t)(raw_target - (raw_target % 8ULL));

    printf("[cliente] Enviando UDP a %s:%d | %d B | %d pps | %d s | run_id=0x%08x\n",
           ip_destino, puerto, tam_payload, pps, duracion, run_id);
    printf("[cliente] Objetivo ajustado a múltiplo de 8: %u datagramas\n", target);

    long period_ns = 1000000000L / pps;
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);

    uint32_t enviados = 0;

    while (seq < target) {
        // Poner cabecera (run_id + seq) al inicio del payload
        memcpy(buffer, &run_id, 4);
        memcpy(buffer + 4, &seq, 4);
        // el resto del payload queda como estaba

        ssize_t s = sendto(sock, buffer, tam_payload, 0,
                           (struct sockaddr*)&addr_serv, sizeof(addr_serv));
        if (s != tam_payload) { perror("sendto"); break; }
        enviados++; seq++;

        add_ns(&next, period_ns);
        if (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL) != 0) {
            clock_gettime(CLOCK_MONOTONIC, &next);
            add_ns(&next, period_ns);
        }
    }

    printf("[cliente] Terminado. run_id=0x%08x, datagramas enviados=%u (target=%u)\n",
           run_id, enviados, target);
    free(buffer);
    close(sock);
    return 0;
}
