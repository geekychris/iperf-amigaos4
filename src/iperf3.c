/*
 * iperf3.c — Minimal iperf3-wire-protocol-compatible client for AmigaOS 4.1 PPC.
 *
 * Speaks the same control-channel state machine as `esnet/iperf` 3.x
 * against a stock `iperf3 -s` server, over Roadshow's bsdsocket.library.
 * TCP-only, forward-direction (client sends), single stream.
 *
 * State machine (server-driven; client just reacts to each state byte
 * arriving on the control connection):
 *
 *   connect + write 37-byte cookie -> server state byte
 *     PARAM_EXCHANGE(9)   : client sends JSON params
 *     CREATE_STREAMS(10)  : client opens N data connections, writes cookie on each
 *     TEST_START(1)       : (ignore — just proceed to blast on TEST_RUNNING)
 *     TEST_RUNNING(2)     : blast fixed-size blocks until --time elapses; then
 *                           write TEST_END(4) back on the control channel
 *     EXCHANGE_RESULTS(13): client sends its stats JSON; reads server's
 *     DISPLAY_RESULTS(14) : (ignore)
 *     IPERF_DONE(16)      : normal exit
 *     SERVER_TERMINATE(11): server aborted — exit with error
 *     ACCESS_DENIED(-1) / SERVER_ERROR(-2): error
 *
 * References (line numbers in /tmp/iperf-assess/iperf/src at 3.21):
 *   iperf_api.h:112-129 — state defines and COOKIE_SIZE
 *   iperf_api.c:JSON_write — wire format (uint32 netorder length + body)
 *   iperf_api.c:send_parameters — JSON key names we emit
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>

#include "cjson.h"

/* Wire protocol constants — must match iperf3 exactly. */
#define TEST_START        1
#define TEST_RUNNING      2
#define TEST_END          4
#define PARAM_EXCHANGE    9
#define CREATE_STREAMS    10
#define SERVER_TERMINATE  11
#define CLIENT_TERMINATE  12
#define EXCHANGE_RESULTS  13
#define DISPLAY_RESULTS   14
#define IPERF_START       15
#define IPERF_DONE        16
#define ACCESS_DENIED     (-1)
#define SERVER_ERROR      (-2)

#define COOKIE_SIZE       37     /* ASCII UUID-ish, per iperf.h:159 */
#define DEFAULT_BLKSIZE   (128 * 1024)   /* iperf3 default TCP block */
#define DEFAULT_PORT      5201
#define DEFAULT_TIME      10

/* ------------- helpers ------------- */

static int Nread(int fd, void *buf, size_t nbytes)
{
    size_t total = 0;
    char *p = (char *)buf;
    while (total < nbytes) {
        ssize_t n = recv(fd, p + total, nbytes - total, 0);
        if (n == 0) return -1;
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        total += (size_t)n;
    }
    return (int)total;
}

static int Nwrite(int fd, const void *buf, size_t nbytes)
{
    size_t total = 0;
    const char *p = (const char *)buf;
    while (total < nbytes) {
        ssize_t n = send(fd, p + total, nbytes - total, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        total += (size_t)n;
    }
    return (int)total;
}

/* iperf3 length-prefixed JSON: uint32 network-order length + body. */
static int json_write(int fd, cJSON *j)
{
    char *body = cJSON_PrintUnformatted(j);
    if (!body) return -1;
    uint32_t len = (uint32_t)strlen(body);
    uint32_t nlen = htonl(len);
    int rc = -1;
    if (Nwrite(fd, &nlen, 4) == 4 && Nwrite(fd, body, len) == (int)len)
        rc = 0;
    cJSON_free(body);
    return rc;
}

static cJSON *json_read(int fd)
{
    uint32_t nlen;
    if (Nread(fd, &nlen, 4) != 4) return NULL;
    uint32_t len = ntohl(nlen);
    if (len == 0 || len > 16 * 1024 * 1024) return NULL;
    char *body = (char *)malloc(len + 1);
    if (!body) return NULL;
    if (Nread(fd, body, len) != (int)len) { free(body); return NULL; }
    body[len] = 0;
    cJSON *j = cJSON_Parse(body);
    free(body);
    return j;
}

/* Generate a cookie the server accepts. iperf3 uses "make_cookie" that
 * fills COOKIE_SIZE-1 bytes with 'A'-'Z','a'-'z','0'-'9' picked from
 * lrand48, plus a NUL. We do the same shape with rand(). */
static void make_cookie(char cookie[COOKIE_SIZE])
{
    static const char *charset =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    for (int i = 0; i < COOKIE_SIZE - 1; i++) {
        cookie[i] = charset[rand() % 62];
    }
    cookie[COOKIE_SIZE - 1] = 0;
}

/* Connect to host:port. `host` may be a numeric IPv4 dotted string OR a
 * hostname. Roadshow's `getaddrinfo` refuses hostname lookups without a
 * reentrant resolver (usergroup.library), so we try numeric first via
 * `inet_pton` (works for any IPv4 literal) and only fall back to
 * `getaddrinfo` if that fails. */
static int connect_to(const char *host, int port)
{
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port   = htons((uint16_t)port);

    int used_dns = 0;
    if (inet_pton(AF_INET, host, &sa.sin_addr) != 1) {
        /* Not a numeric IPv4 — try DNS. May fail on OS4 Roadshow. */
        used_dns = 1;
        char portstr[16];
        snprintf(portstr, sizeof(portstr), "%d", port);
        struct addrinfo hints, *res = NULL;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family   = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        int rc = getaddrinfo(host, portstr, &hints, &res);
        if (rc != 0 || !res) {
            fprintf(stderr, "getaddrinfo(%s): %s\n",
                    host, rc ? gai_strerror(rc) : "no result");
            return -1;
        }
        memcpy(&sa, res->ai_addr, sizeof(sa));
        freeaddrinfo(res);
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        fprintf(stderr, "socket: %s\n", strerror(errno));
        return -1;
    }
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        fprintf(stderr, "connect(%s:%d%s): %s\n",
                host, port, used_dns ? " (via DNS)" : "", strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

/* ------------- state machine ------------- */

static int send_params(int ctrl, int duration, int blksize)
{
    cJSON *j = cJSON_CreateObject();
    if (!j) return -1;
    cJSON_AddTrueToObject  (j, "tcp");
    cJSON_AddNumberToObject(j, "omit", 0);
    cJSON_AddNumberToObject(j, "time", duration);
    cJSON_AddNumberToObject(j, "num", 0);
    cJSON_AddNumberToObject(j, "blockcount", 0);
    cJSON_AddNumberToObject(j, "parallel", 1);
    cJSON_AddNumberToObject(j, "len", blksize);
    cJSON_AddStringToObject(j, "client_version", "iperf-amigaos4/0.1");
    int rc = json_write(ctrl, j);
    cJSON_Delete(j);
    return rc;
}

/* Send our stats JSON at EXCHANGE_RESULTS. Server needs some fields
 * populated to print a client-side line; we fake what we don't track. */
static int send_results(int ctrl, uint64_t total_bytes, double elapsed_sec)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return -1;
    cJSON_AddNumberToObject(root, "cpu_util_total",  0.0);
    cJSON_AddNumberToObject(root, "cpu_util_user",   0.0);
    cJSON_AddNumberToObject(root, "cpu_util_system", 0.0);
    cJSON_AddNumberToObject(root, "sender_has_retransmits", -1);
    cJSON_AddNumberToObject(root, "congestion_used", 0);

    cJSON *streams = cJSON_CreateArray();
    cJSON *s = cJSON_CreateObject();
    cJSON_AddNumberToObject(s, "id",           1);
    cJSON_AddNumberToObject(s, "bytes",        (double)total_bytes);
    cJSON_AddNumberToObject(s, "retransmits",  -1);
    cJSON_AddNumberToObject(s, "jitter",       0);
    cJSON_AddNumberToObject(s, "errors",       0);
    cJSON_AddNumberToObject(s, "packets",      0);
    cJSON_AddNumberToObject(s, "start_time",   0.0);
    cJSON_AddNumberToObject(s, "end_time",     elapsed_sec);
    cJSON_AddItemToArray(streams, s);
    cJSON_AddItemToObject(root, "streams", streams);

    int rc = json_write(ctrl, root);
    cJSON_Delete(root);
    return rc;
}

/* Blast on the data socket for `duration` seconds, tallying bytes.
 * Returns (bytes_sent, elapsed) via out params. Returns 0 on ok, -1 on error. */
static int blast(int data_fd, int duration, int blksize,
                 uint64_t *out_bytes, double *out_elapsed)
{
    char *buf = (char *)malloc(blksize);
    if (!buf) return -1;
    memset(buf, 'X', blksize);

    clock_t start = clock();
    clock_t end_clock = start + (clock_t)((double)duration * CLOCKS_PER_SEC);
    uint64_t total = 0;

    for (;;) {
        clock_t now = clock();
        if (now >= end_clock) break;

        ssize_t n = send(data_fd, buf, blksize, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "send: %s\n", strerror(errno));
            free(buf);
            return -1;
        }
        total += (uint64_t)n;
    }

    clock_t end = clock();
    *out_bytes = total;
    *out_elapsed = (double)(end - start) / (double)CLOCKS_PER_SEC;
    free(buf);
    return 0;
}

/* ------------- main ------------- */

static void usage(const char *argv0)
{
    fprintf(stderr,
        "usage: %s -c <host> [-p port=5201] [-t seconds=10] [-l blksize=131072]\n",
        argv0);
}

int main(int argc, char **argv)
{
    const char *host = NULL;
    int port     = DEFAULT_PORT;
    int duration = DEFAULT_TIME;
    int blksize  = DEFAULT_BLKSIZE;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-c") && i + 1 < argc)      host = argv[++i];
        else if (!strcmp(argv[i], "-p") && i + 1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-t") && i + 1 < argc) duration = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-l") && i + 1 < argc) blksize = atoi(argv[++i]);
        else { usage(argv[0]); return 2; }
    }
    if (!host) { usage(argv[0]); return 2; }
    if (duration <= 0) duration = DEFAULT_TIME;
    if (blksize  <= 0) blksize  = DEFAULT_BLKSIZE;

    srand((unsigned)time(NULL));

    /* AmigaDOS `>file` only redirects stdout. Merge stderr so all our
     * diagnostics land in the same place when the user redirects. */
    dup2(fileno(stdout), fileno(stderr));
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("Connecting to iperf3 server %s:%d (TCP, %d s, blksize=%d)\n",
           host, port, duration, blksize);
    fflush(stdout);

    int ctrl = connect_to(host, port);
    if (ctrl < 0) return 1;

    /* 1. Send cookie. Server uses this as the test ID; data streams
     *    that arrive later matching this cookie belong to this test. */
    char cookie[COOKIE_SIZE];
    make_cookie(cookie);
    if (Nwrite(ctrl, cookie, COOKIE_SIZE) != COOKIE_SIZE) {
        fprintf(stderr, "cookie write failed\n");
        close(ctrl); return 1;
    }

    /* 2. State-machine loop reading 1-byte states from control channel. */
    int data_fd = -1;
    uint64_t total_bytes = 0;
    double   elapsed_sec = 0.0;

    for (;;) {
        int8_t state;
        if (Nread(ctrl, &state, 1) != 1) {
            fprintf(stderr, "control connection closed unexpectedly\n");
            break;
        }

        if      (state == PARAM_EXCHANGE) {
            printf("[state] PARAM_EXCHANGE\n");
            if (send_params(ctrl, duration, blksize) < 0) {
                fprintf(stderr, "send_params failed\n"); break;
            }
        }
        else if (state == CREATE_STREAMS) {
            printf("[state] CREATE_STREAMS\n");
            data_fd = connect_to(host, port);
            if (data_fd < 0) break;
            if (Nwrite(data_fd, cookie, COOKIE_SIZE) != COOKIE_SIZE) {
                fprintf(stderr, "cookie write to data conn failed\n"); break;
            }
            /* Set larger send buffer for throughput. Best-effort. */
            int sndbuf = 512 * 1024;
            setsockopt(data_fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
            int nodelay = 1;
            setsockopt(data_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
        }
        else if (state == TEST_START) {
            printf("[state] TEST_START\n");
            /* Nothing to do; wait for TEST_RUNNING. */
        }
        else if (state == TEST_RUNNING) {
            printf("[state] TEST_RUNNING — blasting for %d s\n", duration);
            if (data_fd < 0) {
                fprintf(stderr, "TEST_RUNNING without data socket\n"); break;
            }
            if (blast(data_fd, duration, blksize,
                      &total_bytes, &elapsed_sec) < 0) break;
            printf("[blast done] %llu bytes in %.2f s\n",
                   (unsigned long long)total_bytes, elapsed_sec);

            /* Tell server we're done. */
            int8_t end_state = TEST_END;
            if (Nwrite(ctrl, &end_state, 1) != 1) {
                fprintf(stderr, "TEST_END write failed\n"); break;
            }
        }
        else if (state == EXCHANGE_RESULTS) {
            printf("[state] EXCHANGE_RESULTS\n");
            if (send_results(ctrl, total_bytes, elapsed_sec) < 0) {
                fprintf(stderr, "send_results failed\n"); break;
            }
            cJSON *server_stats = json_read(ctrl);
            if (server_stats) {
                cJSON_Delete(server_stats);
                /* We don't parse it — server already prints on its side. */
            }
        }
        else if (state == DISPLAY_RESULTS) {
            printf("[state] DISPLAY_RESULTS\n");
            /* Client is supposed to display now. We printed above. */
        }
        else if (state == IPERF_DONE) {
            printf("[state] IPERF_DONE\n");
            break;
        }
        else if (state == SERVER_TERMINATE || state == ACCESS_DENIED
              || state == SERVER_ERROR) {
            fprintf(stderr, "server sent error state %d\n", state);
            break;
        }
        else {
            fprintf(stderr, "unknown state %d, continuing\n", state);
        }
    }

    if (data_fd >= 0) close(data_fd);
    close(ctrl);

    /* Local summary. */
    if (total_bytes > 0 && elapsed_sec > 0.0) {
        double mbps = ((double)total_bytes * 8.0) / (elapsed_sec * 1.0e6);
        double mbs  = (double)total_bytes / (elapsed_sec * 1024.0 * 1024.0);
        printf("\n=== summary ===\n");
        printf("bytes:      %llu\n", (unsigned long long)total_bytes);
        printf("elapsed:    %.2f s\n", elapsed_sec);
        printf("throughput: %.2f Mbit/sec  (%.2f MB/sec)\n", mbps, mbs);
    }
    return 0;
}
