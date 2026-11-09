/*
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements. See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * fixed_rate: open-loop, fixed-rate HTTP/1.1 load generator.
 *
 * Every keepalive connection has a schedule of intended send times spaced
 * by connections/rate. A request is sent at its intended time, or as soon as
 * the previous response on that connection arrives if the gateway is late.
 * Latency is measured from the intended send time, so queueing caused by a
 * slow response is counted (coordinated-omission correction, as in wrk2).
 * Threads busy-poll epoll for microsecond scheduling; run it on dedicated
 * CPUs. Histogram resolution is 1 us up to 1 s.
 *
 * usage: fixed_rate HOST PORT PATH RATE SECONDS CONNECTIONS THREADS [HEADER...]
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define MAX_US 1000000
#define BUFFER 65536

typedef struct {
    int fd;
    int busy;
    uint64_t intended;
    uint64_t next;
    size_t len;
    char buf[BUFFER];
} conn_t;

typedef struct {
    int first, count;
    uint64_t start, end, interval;
    uint64_t *hist;
    uint64_t done, sent, errors, non2xx, late, reconnects;
} worker_t;

static struct sockaddr_in target;
static char request[8192];
static size_t request_len;
static conn_t *conns;

static uint64_t
now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000000ULL + (uint64_t) ts.tv_nsec;
}

/* Returns bytes of one complete response, 0 if incomplete, -1 on error. */
static long
response_length(const char *buf, size_t len, int *status, int *close_after)
{
    const char *end = memmem(buf, len, "\r\n\r\n", 4), *line, *eol;
    size_t header_len, pos;
    long content_length = -1;
    int chunked = 0;
    if (!end) return 0;
    header_len = (size_t) (end - buf) + 4;
    if (len < 12 || memcmp(buf, "HTTP/1.", 7) != 0) return -1;
    *status = atoi(buf + 9);
    for (line = buf; line < end; line = eol + 2) {
        eol = memmem(line, (size_t) (end + 2 - line), "\r\n", 2);
        if (!eol) break;
        if ((size_t) (eol - line) > 15 && strncasecmp(line, "Content-Length:", 15) == 0)
            content_length = atol(line + 15);
        else if ((size_t) (eol - line) > 11 && strncasecmp(line, "Connection:", 11) == 0
                 && memmem(line, (size_t) (eol - line), "close", 5))
            *close_after = 1;
        else if ((size_t) (eol - line) > 18 && strncasecmp(line, "Transfer-Encoding:", 18) == 0
                 && memmem(line, (size_t) (eol - line), "chunked", 7))
            chunked = 1;
    }
    if (content_length >= 0)
        return len >= header_len + (size_t) content_length ? (long) (header_len + content_length) : 0;
    if (!chunked) return (long) header_len;
    for (pos = header_len;;) {
        const char *crlf = memmem(buf + pos, len - pos, "\r\n", 2);
        unsigned long size;
        if (!crlf) return 0;
        size = strtoul(buf + pos, NULL, 16);
        pos = (size_t) (crlf - buf) + 2;
        if (size == 0) return len >= pos + 2 ? (long) (pos + 2) : 0;
        if (len < pos + size + 2) return 0;
        pos += size + 2;
    }
}

static int
connect_one(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    if (fd < 0) return -1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    /* Blocking connect (loopback), then non-blocking I/O. */
    if (connect(fd, (struct sockaddr *) &target, sizeof(target)) != 0) {
        close(fd);
        return -1;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    return fd;
}

/* NGINX closes a keepalive connection after keepalive_requests responses. */
static int
reconnect(int epfd, conn_t *c, uint32_t index, worker_t *w)
{
    struct epoll_event ev = {.events = EPOLLIN, .data.u32 = index};
    epoll_ctl(epfd, EPOLL_CTL_DEL, c->fd, NULL);
    close(c->fd);
    c->len = 0;
    c->busy = 0;
    c->fd = connect_one();
    if (c->fd < 0 || epoll_ctl(epfd, EPOLL_CTL_ADD, c->fd, &ev) != 0) { w->errors++; c->fd = -1; return -1; }
    w->reconnects++;
    return 0;
}

static void *
run(void *data)
{
    worker_t *w = data;
    struct epoll_event events[256];
    int epfd = epoll_create1(0), i, n, active;
    for (i = w->first; i < w->first + w->count; i++) {
        struct epoll_event ev = {.events = EPOLLIN, .data.u32 = (uint32_t) i};
        epoll_ctl(epfd, EPOLL_CTL_ADD, conns[i].fd, &ev);
    }
    for (;;) {
        uint64_t now = now_ns();
        active = 0;
        for (i = w->first; i < w->first + w->count; i++) {
            conn_t *c = &conns[i];
            if (c->fd < 0) continue;
            if (!c->busy && c->next < w->end && c->next <= now) {
                ssize_t written = write(c->fd, request, request_len);
                if (written != (ssize_t) request_len) { w->errors++; close(c->fd); c->fd = -1; continue; }
                if (now - c->next > 1000000) w->late++;
                c->intended = c->next;
                c->next += w->interval;
                c->busy = 1;
                w->sent++;
            }
            if (c->busy || c->next < w->end) active = 1;
        }
        if (!active || now > w->end + 5000000000ULL) break;
        n = epoll_wait(epfd, events, 256, 0);
        for (int e = 0; e < n; e++) {
            conn_t *c = &conns[events[e].data.u32];
            ssize_t got = read(c->fd, c->buf + c->len, BUFFER - c->len);
            if (got <= 0) {
                if (got < 0 && errno == EAGAIN) continue;
                if (got == 0 && !c->busy) { reconnect(epfd, c, events[e].data.u32, w); continue; }
                w->errors++; close(c->fd); c->fd = -1; continue;
            }
            c->len += (size_t) got;
            for (;;) {
                int status = 0, close_after = 0;
                long used = response_length(c->buf, c->len, &status, &close_after);
                if (used == 0) break;
                if (used < 0 || !c->busy) { w->errors++; close(c->fd); c->fd = -1; break; }
                uint64_t latency = (now_ns() - c->intended) / 1000;
                w->hist[latency > MAX_US ? MAX_US : latency]++;
                if (status < 200 || status > 299) w->non2xx++;
                w->done++;
                c->busy = 0;
                memmove(c->buf, c->buf + used, c->len - (size_t) used);
                c->len -= (size_t) used;
                if (close_after) { reconnect(epfd, c, events[e].data.u32, w); break; }
            }
        }
    }
    close(epfd);
    return NULL;
}

static double
percentile(const uint64_t *hist, uint64_t total, double p)
{
    uint64_t rank = (uint64_t) (p / 100.0 * (double) total), seen = 0;
    if (rank >= total) rank = total ? total - 1 : 0;
    for (int us = 0; us <= MAX_US; us++) {
        seen += hist[us];
        if (seen > rank) return us;
    }
    return MAX_US;
}

int
main(int argc, char **argv)
{
    if (argc < 8) {
        fprintf(stderr, "usage: %s HOST PORT PATH RATE SECONDS CONNECTIONS THREADS [HEADER...]\n", argv[0]);
        return 2;
    }
    double rate = atof(argv[4]), seconds = atof(argv[5]);
    int connections = atoi(argv[6]), threads = atoi(argv[7]), i;
    target.sin_family = AF_INET;
    target.sin_port = htons((uint16_t) atoi(argv[2]));
    if (inet_pton(AF_INET, argv[1], &target.sin_addr) != 1 || rate <= 0 || connections <= 0 || threads <= 0) return 2;
    request_len = (size_t) snprintf(request, sizeof(request), "GET %s HTTP/1.1\r\nHost: %s:%s\r\n", argv[3], argv[1], argv[2]);
    for (i = 8; i < argc; i++) request_len += (size_t) snprintf(request + request_len, sizeof(request) - request_len, "%s\r\n", argv[i]);
    request_len += (size_t) snprintf(request + request_len, sizeof(request) - request_len, "\r\n");

    conns = calloc((size_t) connections, sizeof(conn_t));
    worker_t *workers = calloc((size_t) threads, sizeof(worker_t));
    pthread_t *ids = calloc((size_t) threads, sizeof(pthread_t));
    if (!conns || !workers || !ids) return 1;
    for (i = 0; i < connections; i++) {
        if ((conns[i].fd = connect_one()) < 0) { perror("connect"); return 1; }
    }
    uint64_t interval = (uint64_t) ((double) connections * 1e9 / rate);
    uint64_t start = now_ns() + 100000000ULL, end = start + (uint64_t) (seconds * 1e9);
    for (i = 0; i < connections; i++) conns[i].next = start + (uint64_t) i * interval / (uint64_t) connections;
    for (i = 0; i < threads; i++) {
        workers[i].first = i * connections / threads;
        workers[i].count = (i + 1) * connections / threads - workers[i].first;
        workers[i].start = start; workers[i].end = end; workers[i].interval = interval;
        workers[i].hist = calloc(MAX_US + 1, sizeof(uint64_t));
        if (!workers[i].hist || pthread_create(&ids[i], NULL, run, &workers[i]) != 0) return 1;
    }
    uint64_t *hist = calloc(MAX_US + 1, sizeof(uint64_t)), done = 0, sent = 0, errors = 0, non2xx = 0, late = 0, reconnects = 0;
    double sum = 0;
    int max = 0;
    if (!hist) return 1;
    for (i = 0; i < threads; i++) {
        pthread_join(ids[i], NULL);
        for (int us = 0; us <= MAX_US; us++) hist[us] += workers[i].hist[us];
        done += workers[i].done; sent += workers[i].sent; errors += workers[i].errors;
        non2xx += workers[i].non2xx; late += workers[i].late; reconnects += workers[i].reconnects;
    }
    for (int us = 0; us <= MAX_US; us++) {
        sum += (double) hist[us] * us;
        if (hist[us]) max = us;
    }
    printf("fixed_rate target=%.0f achieved=%.1f sent=%lu requests=%lu errors=%lu non2xx=%lu late_sends=%lu reconnects=%lu "
           "mean_us=%.1f p50_us=%.0f p90_us=%.0f p99_us=%.0f p999_us=%.0f max_us=%d\n",
           rate, (double) done / seconds, (unsigned long) sent, (unsigned long) done, (unsigned long) errors,
           (unsigned long) non2xx, (unsigned long) late, (unsigned long) reconnects, done ? sum / (double) done : 0,
           percentile(hist, done, 50), percentile(hist, done, 90), percentile(hist, done, 99),
           percentile(hist, done, 99.9), max);
    return errors || non2xx || done != sent ? 1 : 0;
}
