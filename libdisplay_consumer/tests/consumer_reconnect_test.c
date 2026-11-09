/*
 * Host-side state-machine test for the consumer's producer/daemon lifecycle.
 *
 * It runs the real display_daemon and the real libdisplay_consumer over a
 * temporary unix socket, with a minimal "producer" that only performs the
 * handshake. No Android, no dmabuf rendering: the point is to prove that
 *
 *   1. a producer loss puts the consumer into fallback,
 *   2. a replacement producer lifts it out of fallback again, and
 *   3. a dead daemon control socket is reported exactly once through the
 *      control-lost callback (the host must then rebuild the instance).
 *
 * Build/run: libdisplay_consumer/tests/run_reconnect_test.sh
 */
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#include "display_consumer.h"
#include "display_daemon.h"
#include "protocol.h"
#include "socket_utils.h"

struct producer {
    int ctrl;
    int data;
    int fence;
    int buf_ready;
    int shm;
    int audio;
    int got_bufs;
};

struct producer_arg {
    const char *sock;
    struct producer *p;
};

static void *daemon_thread(void *arg)
{
    daemon_run((daemon_ctx *)arg);
    return NULL;
}

static void producer_close(struct producer *p)
{
    int *fds[] = { &p->ctrl, &p->data, &p->fence, &p->buf_ready, &p->shm, &p->audio };
    for (size_t i = 0; i < sizeof(fds) / sizeof(fds[0]); i++) {
        if (*fds[i] >= 0) {
            close(*fds[i]);
            *fds[i] = -1;
        }
    }
}

static void *producer_open_thread(void *arg)
{
    struct producer_arg *pa = arg;
    struct producer *p = pa->p;

    p->ctrl = connect_unix(pa->sock);
    assert(p->ctrl >= 0);

    struct ctrl_msg hdr = { .type = CTRL_MSG_PRODUCER_HELLO, .size = 0 };
    assert(send_all(p->ctrl, &hdr, sizeof(hdr)) == 0);

    /* The daemon answers the HELLO with the cached screen info. */
    uint8_t si[sizeof(struct ctrl_msg) + sizeof(struct screen_info)];
    assert(recv_all(p->ctrl, si, sizeof(si)) == 0);

    hdr.type = CTRL_MSG_PICKUP_FDS;
    assert(send_all(p->ctrl, &hdr, sizeof(hdr)) == 0);

    int fds[5];
    int fd_count = 0;
    struct ctrl_msg ready;
    assert(recv_fds(p->ctrl, &ready, sizeof(ready), fds, 5, &fd_count) > 0);
    assert(ready.type == CTRL_MSG_FDS_READY);
    assert(fd_count == 5);
    p->buf_ready = fds[0];
    p->fence = fds[1];
    p->data = fds[2];
    p->shm = fds[3];
    p->audio = fds[4];

    /* The consumer sends BUFS_READY once its up-transition publishes the
     * stored dmabuf set. */
    struct data_msg bhdr;
    int bfds[MAX_BUFS];
    int bcount = 0;
    assert(recv_fds(p->data, &bhdr, sizeof(bhdr), bfds, MAX_BUFS, &bcount) > 0);
    assert(bhdr.type == DATA_MSG_BUFS_READY);
    assert(bhdr.size > 0 && bhdr.size % sizeof(struct buf_info) == 0);
    uint8_t *infos = malloc(bhdr.size);
    assert(infos);
    assert(recv_all(p->data, infos, bhdr.size) == 0);
    free(infos);
    for (int i = 0; i < bcount; i++)
        close(bfds[i]);
    p->got_bufs = bcount;
    return NULL;
}

static int wait_until_live(display_ctx *ctx, int attempts)
{
    for (int i = 0; i < attempts; i++) {
        int rc = consumer_try_reconnect(ctx);
        if (rc == 1)
            return 1;
        if (rc < 0)
            return -1;
        usleep(10000);
    }
    return 0;
}

static void build_dmabufs(int *fds, struct buf_info *infos, int count)
{
    for (int i = 0; i < count; i++) {
        fds[i] = memfd_create("anland-test-buf", MFD_CLOEXEC);
        assert(fds[i] >= 0);
        memset(&infos[i], 0, sizeof(infos[i]));
        infos[i].stride = 1280 * 4;
        infos[i].width = 1280;
        infos[i].height = 720;
        infos[i].format = 1; /* RGBA8888 */
    }
}

static void connect_producer(const char *sock, display_ctx *ctx,
                             struct producer *p)
{
    memset(p, 0, sizeof(*p));
    p->ctrl = p->data = p->fence = p->buf_ready = p->shm = p->audio = -1;

    struct producer_arg pa = { .sock = sock, .p = p };
    pthread_t th;
    assert(pthread_create(&th, NULL, producer_open_thread, &pa) == 0);

    assert(wait_until_live(ctx, 500) == 1);
    assert(pthread_join(th, NULL) == 0);
    assert(p->got_bufs == 2);
    assert(!consumer_is_fallback(ctx));
}

static void lose_producer(display_ctx *ctx, struct producer *p)
{
    assert(select_dmabuf(ctx, 0) == 0);
    producer_close(p);
    /* The fence channel is a socketpair: closing the producer end makes the
     * consumer's 5 s poll return immediately with HUP. */
    assert(refresh_done(ctx) == -1);
    assert(consumer_is_fallback(ctx));
}

static int g_control_lost;

static void on_control_lost(void *userdata)
{
    (void)userdata;
    g_control_lost++;
}

int main(void)
{
    char sock[128];
    snprintf(sock, sizeof(sock), "/tmp/anland-consumer-test-%ld.sock", (long)getpid());

    daemon_ctx *daemon = NULL;
    assert(daemon_create(&daemon, sock) == 0);
    pthread_t dth;
    assert(pthread_create(&dth, NULL, daemon_thread, daemon) == 0);

    int ctrl = connect_unix(sock);
    assert(ctrl >= 0);
    display_ctx *ctx = NULL;
    assert(connect_to_deamon_with_fd(&ctx, ctrl) == 0);
    assert(consumer_is_fallback(ctx));
    /* The producer's HELLO handshake waits for this cached screen info. */
    assert(set_screen_info(ctx, 1280, 720, 1, 60000) == 0);

    int fds[2];
    struct buf_info infos[2];
    build_dmabufs(fds, infos, 2);
    assert(push_dmabufs(ctx, fds, infos, 2) == 0);

    struct producer p1;
    connect_producer(sock, ctx, &p1);
    puts("producer #1 handshake + consumer up-transition: passed");

    lose_producer(ctx, &p1);
    /* With the daemon alive but no producer, the tick must stay in the cheap
     * waiting state instead of reporting a session. */
    for (int i = 0; i < 20; i++)
        assert(consumer_try_reconnect(ctx) == 0);
    assert(consumer_is_fallback(ctx));
    puts("producer loss -> fallback: passed");

    struct producer p2;
    connect_producer(sock, ctx, &p2);
    puts("replacement producer -> auto-reconnect: passed");

    lose_producer(ctx, &p2);

    /* Daemon death while the consumer is in fallback: the control channel can
     * never recover, so the callback must fire exactly once. */
    assert(consumer_set_control_lost_callback(ctx, on_control_lost, NULL) == 0);
    daemon_stop(daemon);
    assert(pthread_join(dth, NULL) == 0);
    daemon_destroy(daemon);

    assert(consumer_try_reconnect(ctx) == -1);
    assert(g_control_lost == 1);
    assert(consumer_try_reconnect(ctx) == -1);
    assert(g_control_lost == 1);
    puts("daemon control loss -> single callback: passed");

    disconnect(ctx);
    close(fds[0]);
    close(fds[1]);
    unlink(sock);
    puts("all consumer reconnect state-machine checks passed");
    return 0;
}
