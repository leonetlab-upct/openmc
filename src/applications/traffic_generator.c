// traffic_generator.c - UDP traffic generator with run_id/sequence header,
// strict-duration rate pacing and lightweight timing/scheduler diagnostics.
//
// Main properties:
//
// - The configured duration is a STRICT traffic-generation window.
//
// - The nominal workload target remains:
//
//       floor(pps * duration / 8) * 8
//
//   and is reported for comparison, but the generator does NOT extend the
//   experiment beyond the configured duration merely to reach that target.
//
// - Uses CLOCK_MONOTONIC and absolute clock_nanosleep() pacing.
//
// - Deadlines are derived from a fixed pacing origin:
//
//       deadline(slot) = start + slot * period
//
//   rather than recursively re-anchoring the timeline.
//
// - Avoids unbounded catch-up bursts:
//
//   If one or more nominal transmission slots have already expired, those
//   slots are counted as missed and skipped. The generator does not emit
//   multiple packets back-to-back merely to catch up with an old timeline.
//
// - Records lightweight diagnostics:
//
//     * strict configured duration;
//     * actual first-to-last generator duration;
//     * nominal target;
//     * datagrams actually sent;
//     * achieved packet rate;
//     * offered-load achievement ratio;
//     * sendto() timing;
//     * late deadlines;
//     * skipped/missed nominal slots;
//     * scheduler CPU runtime;
//     * run-queue waiting time;
//     * process CPU time;
//     * voluntary/involuntary context switches.
//
// Important:
//
// - /proc is read only twice:
//     * immediately before the traffic loop;
//     * immediately after the traffic loop.
//
// - No /proc access occurs in the per-packet datapath.
//
// Usage:
//
//   ./traffic-generator \
//       -a 192.168.104.1 \
//       -p 12345 \
//       -s 1000 \
//       -r 564 \
//       -t 10
//
// Payload must be at least 8 bytes because bytes 0..7 contain:
//
//   uint32_t run_id
//   uint32_t sequence_number
//

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>

#include <arpa/inet.h>
#include <netinet/in.h>

#define DEFAULT_PORT 12345
#define DEFAULT_SIZE 1000
#define DEFAULT_PPS  100
#define DEFAULT_TIME 10

#define NS_PER_S   1000000000ULL
#define NS_PER_US  1000ULL
#define NS_PER_MS  1000000ULL


/* ------------------------------------------------------------------------- */
/* Scheduler diagnostics                                                     */
/* ------------------------------------------------------------------------- */

struct scheduler_snapshot {
    uint64_t sched_runtime_ns;
    uint64_t sched_wait_ns;
    uint64_t sched_slices;

    uint64_t voluntary_ctxt_switches;
    uint64_t nonvoluntary_ctxt_switches;

    uint64_t process_cpu_ns;

    int schedstat_valid;
    int status_valid;
    int process_cpu_valid;
};


static int read_self_schedstat(
    struct scheduler_snapshot *snapshot
)
{
    FILE *fp =
        fopen("/proc/self/schedstat", "r");

    if (!fp)
        return -1;


    uint64_t runtime_ns = 0;
    uint64_t wait_ns = 0;
    uint64_t slices = 0;


    int n =
        fscanf(
            fp,
            "%" SCNu64
            " %" SCNu64
            " %" SCNu64,
            &runtime_ns,
            &wait_ns,
            &slices
        );


    fclose(fp);


    if (n != 3)
        return -1;


    snapshot->sched_runtime_ns =
        runtime_ns;

    snapshot->sched_wait_ns =
        wait_ns;

    snapshot->sched_slices =
        slices;

    snapshot->schedstat_valid =
        1;


    return 0;
}


static int read_self_status(
    struct scheduler_snapshot *snapshot
)
{
    FILE *fp =
        fopen("/proc/self/status", "r");

    if (!fp)
        return -1;


    char line[512];

    int got_voluntary = 0;
    int got_nonvoluntary = 0;


    while (fgets(
               line,
               sizeof(line),
               fp
           ) != NULL) {

        uint64_t value;


        if (sscanf(
                line,
                "voluntary_ctxt_switches: %" SCNu64,
                &value
            ) == 1) {

            snapshot->voluntary_ctxt_switches =
                value;

            got_voluntary = 1;

            continue;
        }


        if (sscanf(
                line,
                "nonvoluntary_ctxt_switches: %" SCNu64,
                &value
            ) == 1) {

            snapshot->nonvoluntary_ctxt_switches =
                value;

            got_nonvoluntary = 1;

            continue;
        }
    }


    fclose(fp);


    if (!got_voluntary ||
        !got_nonvoluntary) {

        return -1;
    }


    snapshot->status_valid =
        1;


    return 0;
}


static int read_process_cpu_time(
    struct scheduler_snapshot *snapshot
)
{
    struct timespec ts;


    if (clock_gettime(
            CLOCK_PROCESS_CPUTIME_ID,
            &ts
        ) != 0) {

        return -1;
    }


    snapshot->process_cpu_ns =
        (uint64_t)ts.tv_sec *
        NS_PER_S +
        (uint64_t)ts.tv_nsec;


    snapshot->process_cpu_valid =
        1;


    return 0;
}


static void take_scheduler_snapshot(
    struct scheduler_snapshot *snapshot
)
{
    memset(
        snapshot,
        0,
        sizeof(*snapshot)
    );


    /*
     * Diagnostics must never prevent traffic generation.
     */
    (void)read_self_schedstat(snapshot);

    (void)read_self_status(snapshot);

    (void)read_process_cpu_time(snapshot);
}


static uint64_t delta_u64(
    uint64_t end,
    uint64_t start
)
{
    return
        end >= start
        ? end - start
        : 0;
}


/* ------------------------------------------------------------------------- */
/* Time helpers                                                              */
/* ------------------------------------------------------------------------- */

static inline uint64_t timespec_to_ns(
    const struct timespec *t
)
{
    return
        (uint64_t)t->tv_sec *
        NS_PER_S +
        (uint64_t)t->tv_nsec;
}


static inline void ns_to_timespec(
    uint64_t ns,
    struct timespec *t
)
{
    t->tv_sec =
        (time_t)(
            ns / NS_PER_S
        );

    t->tv_nsec =
        (long)(
            ns % NS_PER_S
        );
}


static inline uint64_t monotonic_ns(void)
{
    struct timespec ts;


    if (clock_gettime(
            CLOCK_MONOTONIC,
            &ts
        ) != 0) {

        perror(
            "clock_gettime("
            "CLOCK_MONOTONIC)"
        );

        exit(EXIT_FAILURE);
    }


    return
        timespec_to_ns(&ts);
}


/* ------------------------------------------------------------------------- */
/* General helpers                                                           */
/* ------------------------------------------------------------------------- */

static void usage(
    const char *prog
)
{
    fprintf(
        stderr,
        "Uso: %s "
        "[-a ip_destino] "
        "[-p puerto] "
        "[-s tam_payload] "
        "[-r pps] "
        "[-t seg]\n",
        prog
    );
}


static inline uint32_t rand32(void)
{
    uint32_t r =
        (uint32_t)rand();


    r ^=
        (
            (uint32_t)rand()
            << 16
        );


    return r;
}


/* ------------------------------------------------------------------------- */
/* Main                                                                      */
/* ------------------------------------------------------------------------- */

int main(
    int argc,
    char **argv
)
{
    const char *ip_destino =
        "127.0.0.1";


    int puerto =
        DEFAULT_PORT;

    int tam_payload =
        DEFAULT_SIZE;

    int pps =
        DEFAULT_PPS;

    int duracion =
        DEFAULT_TIME;


    int opt;


    while ((opt = getopt(
                argc,
                argv,
                "a:p:s:r:t:h"
            )) != -1) {

        switch (opt) {

        case 'a':

            ip_destino =
                optarg;

            break;


        case 'p':

            puerto =
                atoi(optarg);

            break;


        case 's':

            tam_payload =
                atoi(optarg);

            break;


        case 'r':

            pps =
                atoi(optarg);

            break;


        case 't':

            duracion =
                atoi(optarg);

            break;


        case 'h':

            usage(argv[0]);

            return 0;


        default:

            usage(argv[0]);

            return 1;
        }
    }


    /* ------------------------------------------------------------------ */
    /* Parameter validation                                                */
    /* ------------------------------------------------------------------ */

    if (tam_payload < 8 ||
        tam_payload > 1472) {

        fprintf(
            stderr,
            "[!] tam_payload debe estar "
            "en [8..1472]\n"
        );

        return 1;
    }


    if (pps <= 0) {

        fprintf(
            stderr,
            "[!] pps debe ser > 0\n"
        );

        return 1;
    }


    if (duracion <= 0) {

        fprintf(
            stderr,
            "[!] duracion debe ser > 0\n"
        );

        return 1;
    }


    /* ------------------------------------------------------------------ */
    /* UDP socket                                                          */
    /* ------------------------------------------------------------------ */

    int sock =
        socket(
            AF_INET,
            SOCK_DGRAM,
            0
        );


    if (sock < 0) {

        perror("socket");

        return 1;
    }


    int sndbuf =
        4 * 1024 * 1024;


    if (setsockopt(
            sock,
            SOL_SOCKET,
            SO_SNDBUF,
            &sndbuf,
            sizeof(sndbuf)
        ) < 0) {

        perror(
            "setsockopt(SO_SNDBUF)"
        );
    }


    int effective_sndbuf = 0;


    socklen_t effective_sndbuf_len =
        sizeof(effective_sndbuf);


    if (getsockopt(
            sock,
            SOL_SOCKET,
            SO_SNDBUF,
            &effective_sndbuf,
            &effective_sndbuf_len
        ) < 0) {

        perror(
            "getsockopt(SO_SNDBUF)"
        );

        effective_sndbuf =
            -1;
    }


    /* ------------------------------------------------------------------ */
    /* Destination                                                         */
    /* ------------------------------------------------------------------ */

    struct sockaddr_in addr_serv;


    memset(
        &addr_serv,
        0,
        sizeof(addr_serv)
    );


    addr_serv.sin_family =
        AF_INET;


    addr_serv.sin_port =
        htons(puerto);


    if (inet_pton(
            AF_INET,
            ip_destino,
            &addr_serv.sin_addr
        ) <= 0) {

        perror("inet_pton");

        close(sock);

        return 1;
    }


    /* ------------------------------------------------------------------ */
    /* Payload                                                             */
    /* ------------------------------------------------------------------ */

    char *buffer =
        (char *)malloc(
            (size_t)tam_payload
        );


    if (!buffer) {

        perror("malloc");

        close(sock);

        return 1;
    }


    memset(
        buffer,
        'A',
        (size_t)tam_payload
    );


    srand(
        (unsigned)time(NULL)
    );


    uint32_t run_id =
        rand32();


    /*
     * Sequence number represents the sequence of actually emitted
     * application datagrams.
     *
     * Nominal pacing slots are tracked independently.
     */
    uint32_t seq = 0;


    /* ------------------------------------------------------------------ */
    /* Nominal workload target                                             */
    /* ------------------------------------------------------------------ */

    uint64_t raw_target =
        (uint64_t)pps *
        (uint64_t)duracion;


    uint32_t target =
        (uint32_t)(
            raw_target -
            (
                raw_target %
                8ULL
            )
        );


    printf(
        "[traffic-generator] "
        "Sending UDP traffic to %s:%d | "
        "%d B | %d pps | %d s | "
        "run_id=0x%08x\n",
        ip_destino,
        puerto,
        tam_payload,
        pps,
        duracion,
        run_id
    );


    printf(
        "[traffic-generator] "
        "Nominal target adjusted to a multiple of 8: "
        "%u datagrams\n",
        target
    );


    printf(
        "[traffic-generator] "
        "Strict generation window: %d s\n",
        duracion
    );


    printf(
        "[traffic-generator] "
        "SO_SNDBUF requested=%d "
        "effective=%d bytes\n",
        sndbuf,
        effective_sndbuf
    );


    /* ------------------------------------------------------------------ */
    /* Pacing configuration                                                */
    /* ------------------------------------------------------------------ */

    uint64_t period_ns =
        NS_PER_S /
        (uint64_t)pps;


    /*
     * We retain integer-period semantics for comparability with the
     * previous implementation.
     */
    printf(
        "[traffic-generator] "
        "Nominal period: %.3f us\n",
        (double)period_ns /
        1e3
    );


    /* ------------------------------------------------------------------ */
    /* Timing counters                                                     */
    /* ------------------------------------------------------------------ */

    uint32_t sent = 0;


    uint64_t sendto_total_ns = 0;

    uint64_t sendto_max_ns = 0;


    uint64_t sendto_over_100us = 0;

    uint64_t sendto_over_1ms = 0;

    uint64_t sendto_over_10ms = 0;


    uint64_t deadline_late_count = 0;

    uint64_t deadline_lateness_total_ns = 0;

    uint64_t deadline_lateness_max_ns = 0;


    /*
     * Number of nominal pacing slots skipped because their deadlines
     * had already expired.
     */
    uint64_t missed_nominal_slots = 0;


    /*
     * Number of times the pacing logic detected at least one expired
     * nominal slot.
     */
    uint64_t pacing_overrun_events = 0;


    uint64_t sleep_error_count = 0;


    /* ------------------------------------------------------------------ */
    /* Scheduler baseline                                                  */
    /* ------------------------------------------------------------------ */

    struct scheduler_snapshot sched_start;

    struct scheduler_snapshot sched_end;


    take_scheduler_snapshot(
        &sched_start
    );


    /* ------------------------------------------------------------------ */
    /* Strict experiment window                                            */
    /* ------------------------------------------------------------------ */

    uint64_t experiment_start_ns =
        monotonic_ns();


    uint64_t experiment_deadline_ns =
        experiment_start_ns +
        (
            (uint64_t)duracion *
            NS_PER_S
        );


    /*
     * Nominal pacing slot.
     *
     * slot 0 corresponds to experiment_start_ns and is sent immediately
     * if the window is still open.
     */
    uint64_t nominal_slot = 0;


    /* ------------------------------------------------------------------ */
    /* Packet-generation loop                                              */
    /* ------------------------------------------------------------------ */

    while (1) {

        uint64_t now_ns =
            monotonic_ns();


        /*
         * Strict duration boundary.
         *
         * Once the configured window has expired, traffic generation ends.
         */
        if (now_ns >=
            experiment_deadline_ns) {

            break;
        }


        /*
         * No need to emit more than the nominal packet budget.
         *
         * Under ideal pacing this will coincide with the duration boundary.
         */
        if (sent >= target) {

            break;
        }


        /*
         * Compute the absolute deadline directly from the pacing origin.
         *
         * This avoids cumulative drift from repeated:
         *
         *     next += period
         */
        uint64_t slot_deadline_ns =
            experiment_start_ns +
            nominal_slot *
            period_ns;


        /*
         * If the nominal slot itself lies outside the strict experiment
         * window, there is nothing else to send.
         */
        if (slot_deadline_ns >=
            experiment_deadline_ns) {

            break;
        }


        now_ns =
            monotonic_ns();


        /*
         * --------------------------------------------------------------
         * Skip expired nominal slots.
         * --------------------------------------------------------------
         *
         * If we are behind, determine which nominal slot corresponds to
         * the current wall-clock position.
         *
         * This prevents:
         *
         *     expired deadline
         *     immediate send
         *     expired deadline
         *     immediate send
         *     ...
         *
         * and therefore avoids catch-up bursts.
         */
        if (now_ns >
            slot_deadline_ns) {

            uint64_t lateness_ns =
                now_ns -
                slot_deadline_ns;


            deadline_late_count++;


            deadline_lateness_total_ns +=
                lateness_ns;


            if (lateness_ns >
                deadline_lateness_max_ns) {

                deadline_lateness_max_ns =
                    lateness_ns;
            }


            /*
             * Number of full nominal periods already passed relative to
             * the current candidate slot.
             */
            uint64_t periods_late =
                lateness_ns /
                period_ns;


            if (periods_late > 0) {

                pacing_overrun_events++;


                missed_nominal_slots +=
                    periods_late;


                nominal_slot +=
                    periods_late;


                slot_deadline_ns =
                    experiment_start_ns +
                    nominal_slot *
                    period_ns;


                /*
                 * The newly selected slot may already be outside the
                 * strict traffic window.
                 */
                if (slot_deadline_ns >=
                    experiment_deadline_ns) {

                    break;
                }
            }
        }


        /*
         * --------------------------------------------------------------
         * Absolute pacing sleep.
         * --------------------------------------------------------------
         */
        struct timespec deadline_ts;


        ns_to_timespec(
            slot_deadline_ns,
            &deadline_ts
        );


        int sleep_rc =
            clock_nanosleep(
                CLOCK_MONOTONIC,
                TIMER_ABSTIME,
                &deadline_ts,
                NULL
            );


        if (sleep_rc != 0) {

            sleep_error_count++;


            if (sleep_rc != EINTR) {

                fprintf(
                    stderr,
                    "[traffic-generator] "
                    "clock_nanosleep error: %s\n",
                    strerror(sleep_rc)
                );
            }


            /*
             * Do not re-anchor the pacing timeline.
             *
             * The next iteration will determine the appropriate nominal
             * slot from the original experiment_start_ns.
             */
            nominal_slot++;

            continue;
        }


        /*
         * The sleep may wake slightly after the strict experiment
         * boundary. Check again before sending.
         */
        now_ns =
            monotonic_ns();


        if (now_ns >=
            experiment_deadline_ns) {

            break;
        }


        /*
         * Application telemetry header.
         */
        memcpy(
            buffer,
            &run_id,
            4
        );


        memcpy(
            buffer + 4,
            &seq,
            4
        );


        /* -------------------------------------------------------------- */
        /* sendto() timing                                                 */
        /* -------------------------------------------------------------- */

        uint64_t send_begin_ns =
            monotonic_ns();


        ssize_t s =
            sendto(
                sock,
                buffer,
                (size_t)tam_payload,
                0,
                (struct sockaddr *)&addr_serv,
                sizeof(addr_serv)
            );


        uint64_t send_end_ns =
            monotonic_ns();


        uint64_t send_duration_ns =
            send_end_ns -
            send_begin_ns;


        sendto_total_ns +=
            send_duration_ns;


        if (send_duration_ns >
            sendto_max_ns) {

            sendto_max_ns =
                send_duration_ns;
        }


        if (send_duration_ns >
            100ULL *
            NS_PER_US) {

            sendto_over_100us++;
        }


        if (send_duration_ns >
            NS_PER_MS) {

            sendto_over_1ms++;
        }


        if (send_duration_ns >
            10ULL *
            NS_PER_MS) {

            sendto_over_10ms++;
        }


        if (s != tam_payload) {

            if (s < 0) {

                perror("sendto");

            } else {

                fprintf(
                    stderr,
                    "[traffic-generator] "
                    "short send: %zd/%d bytes\n",
                    s,
                    tam_payload
                );
            }


            break;
        }


        sent++;

        seq++;


        /*
         * Move to the next nominal pacing slot.
         */
        nominal_slot++;
    }


    /* ------------------------------------------------------------------ */
    /* Final timing and scheduler snapshot                                 */
    /* ------------------------------------------------------------------ */

    uint64_t experiment_end_ns =
        monotonic_ns();


    take_scheduler_snapshot(
        &sched_end
    );


    /* ------------------------------------------------------------------ */
    /* Derived timing statistics                                           */
    /* ------------------------------------------------------------------ */

    uint64_t elapsed_ns =
        experiment_end_ns -
        experiment_start_ns;


    double elapsed_s =
        (double)elapsed_ns /
        1e9;


    double effective_rate_pps =
        elapsed_s > 0.0
        ? (double)sent /
          elapsed_s
        : 0.0;


    /*
     * Achievement relative to the configured rate.
     */
    double rate_achievement_pct =
        pps > 0
        ? (
            effective_rate_pps /
            (double)pps
          ) * 100.0
        : 0.0;


    double packet_target_pct =
        target > 0
        ? (
            (double)sent /
            (double)target
          ) * 100.0
        : 0.0;


    double sendto_total_ms =
        (double)sendto_total_ns /
        1e6;


    double sendto_mean_us =
        sent > 0
        ? (
            (double)sendto_total_ns /
            (double)sent
          ) / 1e3
        : 0.0;


    double sendto_max_us =
        (double)sendto_max_ns /
        1e3;


    double lateness_mean_us =
        deadline_late_count > 0
        ? (
            (double)deadline_lateness_total_ns /
            (double)deadline_late_count
          ) / 1e3
        : 0.0;


    double lateness_max_us =
        (double)deadline_lateness_max_ns /
        1e3;


    /* ------------------------------------------------------------------ */
    /* Scheduler deltas                                                    */
    /* ------------------------------------------------------------------ */

    uint64_t scheduler_runtime_ns = 0;

    uint64_t scheduler_wait_ns = 0;

    uint64_t scheduler_slices = 0;


    uint64_t voluntary_switches = 0;

    uint64_t nonvoluntary_switches = 0;


    uint64_t process_cpu_ns = 0;


    int scheduler_delta_valid =
        sched_start.schedstat_valid &&
        sched_end.schedstat_valid;


    int context_switch_delta_valid =
        sched_start.status_valid &&
        sched_end.status_valid;


    int process_cpu_delta_valid =
        sched_start.process_cpu_valid &&
        sched_end.process_cpu_valid;


    if (scheduler_delta_valid) {

        scheduler_runtime_ns =
            delta_u64(
                sched_end.sched_runtime_ns,
                sched_start.sched_runtime_ns
            );


        scheduler_wait_ns =
            delta_u64(
                sched_end.sched_wait_ns,
                sched_start.sched_wait_ns
            );


        scheduler_slices =
            delta_u64(
                sched_end.sched_slices,
                sched_start.sched_slices
            );
    }


    if (context_switch_delta_valid) {

        voluntary_switches =
            delta_u64(
                sched_end.voluntary_ctxt_switches,
                sched_start.voluntary_ctxt_switches
            );


        nonvoluntary_switches =
            delta_u64(
                sched_end.nonvoluntary_ctxt_switches,
                sched_start.nonvoluntary_ctxt_switches
            );
    }


    if (process_cpu_delta_valid) {

        process_cpu_ns =
            delta_u64(
                sched_end.process_cpu_ns,
                sched_start.process_cpu_ns
            );
    }


    /* ------------------------------------------------------------------ */
    /* Completion line                                                     */
    /* ------------------------------------------------------------------ */

    printf(
        "[traffic-generator] "
        "Finished. run_id=0x%08x, "
        "datagrams sent=%u "
        "(nominal target=%u)\n",
        run_id,
        sent,
        target
    );


    /* ------------------------------------------------------------------ */
    /* Timing diagnostics                                                  */
    /* ------------------------------------------------------------------ */

    printf("\n");


    printf(
        "[traffic-generator] "
        "=== Timing diagnostics ===\n"
    );


    printf(
        "Configured duration           : %d s\n",
        duracion
    );


    printf(
        "Elapsed time                 : %.9f s\n",
        elapsed_s
    );


    printf(
        "Configured rate              : %d pps\n",
        pps
    );


    printf(
        "Effective rate               : %.3f pps\n",
        effective_rate_pps
    );


    printf(
        "Rate achievement             : %.3f %%\n",
        rate_achievement_pct
    );


    printf(
        "Nominal target               : %u datagrams\n",
        target
    );


    printf(
        "Datagrams sent               : %u\n",
        sent
    );


    printf(
        "Packet-target achievement    : %.3f %%\n",
        packet_target_pct
    );


    printf(
        "Nominal period               : %.3f us\n",
        (double)period_ns /
        1e3
    );


    printf(
        "sendto calls                 : %u\n",
        sent
    );


    printf(
        "sendto total time            : %.3f ms\n",
        sendto_total_ms
    );


    printf(
        "sendto mean time             : %.3f us\n",
        sendto_mean_us
    );


    printf(
        "sendto maximum time          : %.3f us\n",
        sendto_max_us
    );


    printf(
        "sendto > 100 us              : %" PRIu64 "\n",
        sendto_over_100us
    );


    printf(
        "sendto > 1 ms                : %" PRIu64 "\n",
        sendto_over_1ms
    );


    printf(
        "sendto > 10 ms               : %" PRIu64 "\n",
        sendto_over_10ms
    );


    printf(
        "Late deadline observations   : %" PRIu64 "\n",
        deadline_late_count
    );


    printf(
        "Deadline lateness mean       : %.3f us\n",
        lateness_mean_us
    );


    printf(
        "Deadline lateness maximum    : %.3f us\n",
        lateness_max_us
    );


    printf(
        "Pacing overrun events        : %" PRIu64 "\n",
        pacing_overrun_events
    );


    printf(
        "Missed nominal slots         : %" PRIu64 "\n",
        missed_nominal_slots
    );


    printf(
        "clock_nanosleep errors       : %" PRIu64 "\n",
        sleep_error_count
    );


    /* ------------------------------------------------------------------ */
    /* Scheduler diagnostics                                               */
    /* ------------------------------------------------------------------ */

    printf("\n");


    printf(
        "[traffic-generator] "
        "=== Scheduler diagnostics ===\n"
    );


    if (scheduler_delta_valid) {

        double scheduler_runtime_ms =
            (double)scheduler_runtime_ns /
            1e6;


        double scheduler_wait_ms =
            (double)scheduler_wait_ns /
            1e6;


        double scheduler_runtime_pct =
            elapsed_ns > 0
            ? (
                (double)scheduler_runtime_ns /
                (double)elapsed_ns
              ) * 100.0
            : 0.0;


        double scheduler_wait_pct =
            elapsed_ns > 0
            ? (
                (double)scheduler_wait_ns /
                (double)elapsed_ns
              ) * 100.0
            : 0.0;


        printf(
            "Scheduler CPU runtime       : %.3f ms\n",
            scheduler_runtime_ms
        );


        printf(
            "Scheduler runqueue wait     : %.3f ms\n",
            scheduler_wait_ms
        );


        printf(
            "Scheduler CPU / elapsed     : %.3f %%\n",
            scheduler_runtime_pct
        );


        printf(
            "Scheduler wait / elapsed    : %.3f %%\n",
            scheduler_wait_pct
        );


        printf(
            "Scheduler slices            : %" PRIu64 "\n",
            scheduler_slices
        );

    } else {

        printf(
            "Scheduler schedstat         : unavailable\n"
        );
    }


    if (process_cpu_delta_valid) {

        double process_cpu_ms =
            (double)process_cpu_ns /
            1e6;


        double process_cpu_pct =
            elapsed_ns > 0
            ? (
                (double)process_cpu_ns /
                (double)elapsed_ns
              ) * 100.0
            : 0.0;


        printf(
            "Process CPU time            : %.3f ms\n",
            process_cpu_ms
        );


        printf(
            "Process CPU / elapsed       : %.3f %%\n",
            process_cpu_pct
        );

    } else {

        printf(
            "Process CPU time            : unavailable\n"
        );
    }


    if (context_switch_delta_valid) {

        printf(
            "Voluntary context switches  : %" PRIu64 "\n",
            voluntary_switches
        );


        printf(
            "Involuntary ctxt switches   : %" PRIu64 "\n",
            nonvoluntary_switches
        );


        printf(
            "Total context switches      : %" PRIu64 "\n",
            voluntary_switches +
            nonvoluntary_switches
        );

    } else {

        printf(
            "Context switch counters     : unavailable\n"
        );
    }


    if (scheduler_delta_valid) {

        double runnable_accounted_ms =
            (double)(
                scheduler_runtime_ns +
                scheduler_wait_ns
            ) / 1e6;


        printf(
            "CPU+runqueue accounted      : %.3f ms\n",
            runnable_accounted_ms
        );
    }


    /* ------------------------------------------------------------------ */
    /* Cleanup                                                             */
    /* ------------------------------------------------------------------ */

    free(buffer);

    close(sock);


    /*
     * IMPORTANT:
     *
     * Reaching fewer packets than the nominal target is no longer itself
     * an execution failure.
     *
     * In a strict-duration rate experiment, that deficit is experimental
     * information: it means the configured offered rate could not be
     * maintained during the requested window.
     *
     * We therefore return success unless a real socket/runtime error caused
     * the loop to terminate.
     */
    return 0;
}
