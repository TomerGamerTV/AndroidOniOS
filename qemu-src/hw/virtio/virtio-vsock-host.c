/*
 * Userspace virtio-vsock device (no vhost).
 *
 * Bridges guest AF_VSOCK stream sockets to host file descriptors so that
 * hosts without vhost-vsock (macOS, iOS) can reach guest services such as
 * adbd (vsock:5555).  Host-initiated connections only: every host socket that
 * connects to one of the configured forwarders becomes a vsock connection to
 * the matching guest port.  Guest-initiated connections are refused (RST).
 *
 *   -device virtio-vsock-host-pci,guest-cid=3,forward=tcp:5555:5555;unix:/path/adb.sock:5555
 *
 * Copyright (c) 2026 AndroidOniOS contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/main-loop.h"
#include "qemu/queue.h"
#include "qemu/iov.h"
#include "qemu/error-report.h"
#include "qemu/sockets.h"
#include "qapi/error.h"
#include "hw/qdev-properties.h"
#include "hw/virtio/virtio.h"
#include "hw/virtio/virtio-access.h"
#include "hw/virtio/virtio-pci.h"
#include "migration/blocker.h"
#include "standard-headers/linux/virtio_ids.h"
#include "standard-headers/linux/virtio_vsock.h"
#include "qom/object.h"
#include "system/vsock-host.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#define TYPE_VIRTIO_VSOCK_HOST "virtio-vsock-host-device"
OBJECT_DECLARE_SIMPLE_TYPE(VirtIOVSockHost, VIRTIO_VSOCK_HOST)

#define VSOCK_HOST_CID 2
#define VSOCK_QUEUE_SIZE 256
#define VSOCK_BUF_ALLOC (256 * 1024)
#define VSOCK_HDR_SIZE sizeof(struct virtio_vsock_hdr)
#define VSOCK_MAX_PKT (64 * 1024)

typedef struct VSockHost VSockHost;

typedef struct VSockConn {
    QTAILQ_ENTRY(VSockConn) next;
    VirtIOVSockHost *dev;
    uint32_t host_port;
    uint32_t guest_port;
    int fd;
    bool connected;
    bool closing;
    bool host_eof;          /* host fd hit EOF, SHUTDOWN(SEND) queued */
    bool guest_send_closed; /* guest will not send any more */
    bool read_enabled;
    bool write_enabled;
    uint32_t peer_buf_alloc;
    uint32_t peer_fwd_cnt;
    uint32_t tx_cnt;        /* bytes sent to guest */
    uint32_t fwd_cnt;       /* bytes of guest data delivered to host fd */
    uint32_t fwd_cnt_sent;  /* fwd_cnt last advertised to guest */
    GByteArray *to_host;    /* guest data waiting to be written to fd */
} VSockConn;

typedef struct VSockCtrl {
    QSIMPLEQ_ENTRY(VSockCtrl) next;
    struct virtio_vsock_hdr hdr;
} VSockCtrl;

typedef struct VSockListener {
    int fd;
    uint32_t guest_port;
    VirtIOVSockHost *dev;
    char *unix_path;
} VSockListener;

struct VirtIOVSockHost {
    VirtIODevice parent_obj;
    VirtQueue *rx_vq;
    VirtQueue *tx_vq;
    VirtQueue *event_vq;
    uint64_t guest_cid;
    char *forward;
    char *guest_forward;    /* "PORT:unix:PATH;PORT:tcp:HOSTPORT" for guest-initiated */
    uint32_t next_port;
    QTAILQ_HEAD(, VSockConn) conns;
    QSIMPLEQ_HEAD(, VSockCtrl) ctrl;
    GPtrArray *listeners;
    QEMUBH *pump_bh;
    Error *migration_blocker;
};

static VirtIOVSockHost *vsock_host_singleton;

static void vsock_conn_update_handlers(VSockConn *c);
static void vsock_pump(VirtIOVSockHost *s);

static void vsock_fill_hdr(VirtIOVSockHost *s, VSockConn *c,
                           struct virtio_vsock_hdr *h, uint16_t op,
                           uint32_t len, uint32_t flags)
{
    memset(h, 0, sizeof(*h));
    h->src_cid = cpu_to_le64(VSOCK_HOST_CID);
    h->dst_cid = cpu_to_le64(s->guest_cid);
    h->src_port = cpu_to_le32(c->host_port);
    h->dst_port = cpu_to_le32(c->guest_port);
    h->len = cpu_to_le32(len);
    h->type = cpu_to_le16(VIRTIO_VSOCK_TYPE_STREAM);
    h->op = cpu_to_le16(op);
    h->flags = cpu_to_le32(flags);
    h->buf_alloc = cpu_to_le32(VSOCK_BUF_ALLOC);
    h->fwd_cnt = cpu_to_le32(c->fwd_cnt);
}

static void vsock_queue_ctrl_raw(VirtIOVSockHost *s, struct virtio_vsock_hdr *h)
{
    VSockCtrl *ctl = g_new0(VSockCtrl, 1);
    ctl->hdr = *h;
    QSIMPLEQ_INSERT_TAIL(&s->ctrl, ctl, next);
    qemu_bh_schedule(s->pump_bh);
}

static void vsock_queue_ctrl(VirtIOVSockHost *s, VSockConn *c, uint16_t op,
                             uint32_t flags)
{
    struct virtio_vsock_hdr h;
    vsock_fill_hdr(s, c, &h, op, 0, flags);
    if (op != VIRTIO_VSOCK_OP_RST) {
        c->fwd_cnt_sent = c->fwd_cnt;
    }
    vsock_queue_ctrl_raw(s, &h);
}

static void vsock_conn_free(VSockConn *c)
{
    VirtIOVSockHost *s = c->dev;
    if (c->fd >= 0) {
        qemu_set_fd_handler(c->fd, NULL, NULL, NULL);
        close(c->fd);
        c->fd = -1;
    }
    QTAILQ_REMOVE(&s->conns, c, next);
    g_byte_array_free(c->to_host, true);
    g_free(c);
}

/* Tear down: tell the guest and forget the connection. */
static void vsock_conn_reset(VSockConn *c, bool notify_guest)
{
    if (notify_guest) {
        vsock_queue_ctrl(c->dev, c, VIRTIO_VSOCK_OP_RST, 0);
    }
    vsock_conn_free(c);
}

static VSockConn *vsock_find(VirtIOVSockHost *s, uint32_t host_port,
                             uint32_t guest_port)
{
    VSockConn *c;
    QTAILQ_FOREACH(c, &s->conns, next) {
        if (c->host_port == host_port && c->guest_port == guest_port) {
            return c;
        }
    }
    return NULL;
}

static uint32_t vsock_peer_credit(VSockConn *c)
{
    uint32_t in_flight = c->tx_cnt - c->peer_fwd_cnt;
    if (in_flight >= c->peer_buf_alloc) {
        return 0;
    }
    return c->peer_buf_alloc - in_flight;
}

/* Write buffered guest data to the host fd. */
static void vsock_flush_to_host(VSockConn *c)
{
    while (c->fd >= 0 && c->to_host->len) {
        ssize_t n = write(c->fd, c->to_host->data, c->to_host->len);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            vsock_conn_reset(c, true);
            return;
        }
        g_byte_array_remove_range(c->to_host, 0, n);
        c->fwd_cnt += n;
    }
    if (c->fwd_cnt - c->fwd_cnt_sent >= VSOCK_BUF_ALLOC / 4) {
        vsock_queue_ctrl(c->dev, c, VIRTIO_VSOCK_OP_CREDIT_UPDATE, 0);
    }
    if (c->guest_send_closed && !c->to_host->len && c->fd >= 0) {
        shutdown(c->fd, SHUT_WR);
    }
    vsock_conn_update_handlers(c);
}

static void vsock_fd_read(void *opaque)
{
    VSockConn *c = opaque;
    qemu_bh_schedule(c->dev->pump_bh);
    /* Avoid spinning until the pump runs. */
    c->read_enabled = false;
    vsock_conn_update_handlers(c);
}

static void vsock_fd_write(void *opaque)
{
    vsock_flush_to_host(opaque);
}

static void vsock_conn_update_handlers(VSockConn *c)
{
    if (c->fd < 0) {
        return;
    }
    bool want_read = c->connected && !c->host_eof && vsock_peer_credit(c) > 0;
    bool want_write = c->to_host->len > 0;
    c->read_enabled = want_read;
    c->write_enabled = want_write;
    qemu_set_fd_handler(c->fd, want_read ? vsock_fd_read : NULL,
                        want_write ? vsock_fd_write : NULL, c);
}

static VSockConn *vsock_conn_new(VirtIOVSockHost *s, int fd, uint32_t guest_port)
{
    VSockConn *c = g_new0(VSockConn, 1);
    c->dev = s;
    c->fd = fd;
    c->guest_port = guest_port;
    do {
        c->host_port = s->next_port++;
        if (s->next_port < 1024) {
            s->next_port = 1024;
        }
    } while (vsock_find(s, c->host_port, guest_port));
    c->to_host = g_byte_array_new();
    qemu_socket_set_nonblock(fd);
    QTAILQ_INSERT_TAIL(&s->conns, c, next);
    vsock_queue_ctrl(s, c, VIRTIO_VSOCK_OP_REQUEST, 0);
    return c;
}

/* Send one RX packet to the guest. Returns false if no rx buffer. */
static bool vsock_send_pkt(VirtIOVSockHost *s, const struct virtio_vsock_hdr *h,
                           VSockConn *data_conn, size_t *sent_payload)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(s);
    VirtQueueElement *elem = virtqueue_pop(s->rx_vq, sizeof(VirtQueueElement));
    if (!elem) {
        return false;
    }
    size_t cap = iov_size(elem->in_sg, elem->in_num);
    if (cap < VSOCK_HDR_SIZE) {
        virtqueue_detach_element(s->rx_vq, elem, 0);
        g_free(elem);
        return false;
    }
    struct virtio_vsock_hdr hdr = *h;
    size_t payload = 0;
    g_autofree uint8_t *buf = NULL;
    if (data_conn) {
        size_t max = MIN(cap - VSOCK_HDR_SIZE, VSOCK_MAX_PKT);
        max = MIN(max, vsock_peer_credit(data_conn));
        buf = g_malloc(max ? max : 1);
        ssize_t n = max ? read(data_conn->fd, buf, max) : 0;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            virtqueue_unpop(s->rx_vq, elem, 0);
            g_free(elem);
            *sent_payload = 0;
            return true;
        }
        if (n <= 0) {
            /* EOF or error: tell the guest we will not send any more. */
            data_conn->host_eof = true;
            vsock_fill_hdr(s, data_conn, &hdr, VIRTIO_VSOCK_OP_SHUTDOWN, 0,
                           VIRTIO_VSOCK_SHUTDOWN_SEND |
                           (n < 0 ? VIRTIO_VSOCK_SHUTDOWN_RCV : 0));
        } else {
            payload = n;
            vsock_fill_hdr(s, data_conn, &hdr, VIRTIO_VSOCK_OP_RW, payload, 0);
            data_conn->tx_cnt += payload;
            data_conn->fwd_cnt_sent = data_conn->fwd_cnt;
        }
    }
    iov_from_buf(elem->in_sg, elem->in_num, 0, &hdr, VSOCK_HDR_SIZE);
    if (payload) {
        iov_from_buf(elem->in_sg, elem->in_num, VSOCK_HDR_SIZE, buf, payload);
    }
    virtqueue_push(s->rx_vq, elem, VSOCK_HDR_SIZE + payload);
    g_free(elem);
    virtio_notify(vdev, s->rx_vq);
    if (sent_payload) {
        *sent_payload = payload;
    }
    return true;
}

static void vsock_pump(VirtIOVSockHost *s)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(s);
    if (!(vdev->status & VIRTIO_CONFIG_S_DRIVER_OK)) {
        return;
    }
    /* Control packets first. */
    while (!QSIMPLEQ_EMPTY(&s->ctrl)) {
        VSockCtrl *ctl = QSIMPLEQ_FIRST(&s->ctrl);
        if (!vsock_send_pkt(s, &ctl->hdr, NULL, NULL)) {
            return; /* wait for rx buffers */
        }
        QSIMPLEQ_REMOVE_HEAD(&s->ctrl, next);
        g_free(ctl);
    }
    /* Then data, round robin. */
    bool progress = true;
    while (progress) {
        progress = false;
        VSockConn *c, *tmp;
        QTAILQ_FOREACH_SAFE(c, &s->conns, next, tmp) {
            if (!c->connected || c->host_eof || c->fd < 0 || !vsock_peer_credit(c)) {
                continue;
            }
            struct virtio_vsock_hdr unused = {0};
            size_t sent = 0;
            if (!vsock_send_pkt(s, &unused, c, &sent)) {
                vsock_conn_update_handlers(c);
                return; /* out of rx buffers; resume on rx kick */
            }
            if (sent > 0) {
                progress = true;
            }
            vsock_conn_update_handlers(c);
        }
    }
}

static void vsock_pump_bh(void *opaque)
{
    vsock_pump(opaque);
}

static void vsock_handle_rx(VirtIODevice *vdev, VirtQueue *vq)
{
    vsock_pump(VIRTIO_VSOCK_HOST(vdev));
}

static void vsock_handle_event(VirtIODevice *vdev, VirtQueue *vq)
{
}

/* Connect to the host endpoint configured for a guest-initiated connection. */
static int vsock_guest_forward_connect(VirtIOVSockHost *s, uint32_t port)
{
    if (!s->guest_forward) {
        return -1;
    }
    g_auto(GStrv) specs = g_strsplit(s->guest_forward, ";", -1);
    for (int i = 0; specs[i]; i++) {
        g_auto(GStrv) f = g_strsplit(specs[i], ":", 3);
        if (!f[0] || !f[1] || !f[2] || strtoul(f[0], NULL, 10) != port) {
            continue;
        }
        int fd = -1;
        if (!strcmp(f[1], "unix")) {
            struct sockaddr_un sa = { .sun_family = AF_UNIX };
            g_strlcpy(sa.sun_path, f[2], sizeof(sa.sun_path));
            fd = qemu_socket(AF_UNIX, SOCK_STREAM, 0);
            if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
                close(fd);
                fd = -1;
            }
        } else if (!strcmp(f[1], "tcp")) {
            struct sockaddr_in sa = { .sin_family = AF_INET,
                                      .sin_port = htons(atoi(f[2])),
                                      .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
            fd = qemu_socket(AF_INET, SOCK_STREAM, 0);
            if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
                close(fd);
                fd = -1;
            }
        }
        return fd;
    }
    return -1;
}

static void vsock_handle_guest_pkt(VirtIOVSockHost *s,
                                   const struct virtio_vsock_hdr *h,
                                   const uint8_t *payload, size_t plen)
{
    uint32_t src_port = le32_to_cpu(h->src_port);
    uint32_t dst_port = le32_to_cpu(h->dst_port);
    uint16_t op = le16_to_cpu(h->op);
    VSockConn *c = vsock_find(s, dst_port, src_port);

    if (le64_to_cpu(h->dst_cid) != VSOCK_HOST_CID ||
        le16_to_cpu(h->type) != VIRTIO_VSOCK_TYPE_STREAM) {
        return;
    }
    if (!c && op == VIRTIO_VSOCK_OP_REQUEST) {
        int fd = vsock_guest_forward_connect(s, dst_port);
        if (fd >= 0) {
            c = g_new0(VSockConn, 1);
            c->dev = s;
            c->fd = fd;
            c->host_port = dst_port;
            c->guest_port = src_port;
            c->connected = true;
            c->to_host = g_byte_array_new();
            c->peer_buf_alloc = le32_to_cpu(h->buf_alloc);
            c->peer_fwd_cnt = le32_to_cpu(h->fwd_cnt);
            qemu_socket_set_nonblock(fd);
            QTAILQ_INSERT_TAIL(&s->conns, c, next);
            vsock_queue_ctrl(s, c, VIRTIO_VSOCK_OP_RESPONSE, 0);
            vsock_conn_update_handlers(c);
            return;
        }
    }
    if (!c) {
        if (op != VIRTIO_VSOCK_OP_RST) {
            /* Unknown or guest-initiated connection: refuse. */
            struct virtio_vsock_hdr r = {0};
            r.src_cid = cpu_to_le64(VSOCK_HOST_CID);
            r.dst_cid = cpu_to_le64(s->guest_cid);
            r.src_port = h->dst_port;
            r.dst_port = h->src_port;
            r.type = cpu_to_le16(VIRTIO_VSOCK_TYPE_STREAM);
            r.op = cpu_to_le16(VIRTIO_VSOCK_OP_RST);
            vsock_queue_ctrl_raw(s, &r);
        }
        return;
    }
    c->peer_buf_alloc = le32_to_cpu(h->buf_alloc);
    c->peer_fwd_cnt = le32_to_cpu(h->fwd_cnt);

    switch (op) {
    case VIRTIO_VSOCK_OP_RESPONSE:
        c->connected = true;
        break;
    case VIRTIO_VSOCK_OP_RST:
        vsock_conn_free(c);
        return;
    case VIRTIO_VSOCK_OP_SHUTDOWN: {
        uint32_t flags = le32_to_cpu(h->flags);
        if (flags & VIRTIO_VSOCK_SHUTDOWN_SEND) {
            c->guest_send_closed = true;
        }
        if ((flags & VIRTIO_VSOCK_SHUTDOWN_RCV) || c->host_eof) {
            if (!c->to_host->len) {
                vsock_conn_reset(c, true);
                return;
            }
        }
        vsock_flush_to_host(c);
        return;
    }
    case VIRTIO_VSOCK_OP_RW:
        if (plen) {
            g_byte_array_append(c->to_host, payload, plen);
            vsock_flush_to_host(c);
            return;
        }
        break;
    case VIRTIO_VSOCK_OP_CREDIT_REQUEST:
        vsock_queue_ctrl(s, c, VIRTIO_VSOCK_OP_CREDIT_UPDATE, 0);
        break;
    case VIRTIO_VSOCK_OP_CREDIT_UPDATE:
    default:
        break;
    }
    vsock_conn_update_handlers(c);
    qemu_bh_schedule(s->pump_bh);
}

static void vsock_handle_tx(VirtIODevice *vdev, VirtQueue *vq)
{
    VirtIOVSockHost *s = VIRTIO_VSOCK_HOST(vdev);
    VirtQueueElement *elem;

    while ((elem = virtqueue_pop(vq, sizeof(VirtQueueElement)))) {
        struct virtio_vsock_hdr h;
        size_t total = iov_size(elem->out_sg, elem->out_num);
        if (total >= VSOCK_HDR_SIZE &&
            iov_to_buf(elem->out_sg, elem->out_num, 0, &h, VSOCK_HDR_SIZE) == VSOCK_HDR_SIZE) {
            size_t plen = MIN(le32_to_cpu(h.len), total - VSOCK_HDR_SIZE);
            g_autofree uint8_t *payload = plen ? g_malloc(plen) : NULL;
            if (plen) {
                iov_to_buf(elem->out_sg, elem->out_num, VSOCK_HDR_SIZE, payload, plen);
            }
            vsock_handle_guest_pkt(s, &h, payload, plen);
        }
        virtqueue_push(vq, elem, 0);
        g_free(elem);
    }
    virtio_notify(vdev, vq);
}

/* ---- host side entry points ---- */

int vsock_host_connect_fd(int fd, uint32_t guest_port)
{
    VirtIOVSockHost *s = vsock_host_singleton;
    if (!s) {
        return -ENODEV;
    }
    vsock_conn_new(s, fd, guest_port);
    return 0;
}

static void vsock_listener_accept(void *opaque)
{
    VSockListener *l = opaque;
    int fd = qemu_accept(l->fd, NULL, NULL);
    if (fd < 0) {
        return;
    }
    if (!l->unix_path) {
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    }
    vsock_conn_new(l->dev, fd, l->guest_port);
}

static bool vsock_add_forward(VirtIOVSockHost *s, const char *spec, Error **errp)
{
    /* tcp:HOSTPORT:GUESTPORT  or  unix:PATH:GUESTPORT */
    const char *last = strrchr(spec, ':');
    if (!last) {
        error_setg(errp, "bad vsock forward '%s'", spec);
        return false;
    }
    uint32_t guest_port = strtoul(last + 1, NULL, 10);
    g_autofree char *head = g_strndup(spec, last - spec);
    int fd = -1;
    char *unix_path = NULL;

    if (g_str_has_prefix(head, "tcp:") || g_str_has_prefix(head, "tcpany:")) {
        /* tcp: binds loopback; tcpany: binds all interfaces (debugging only) */
        bool any = g_str_has_prefix(head, "tcpany:");
        int port = atoi(head + (any ? 7 : 4));
        struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(port),
                                  .sin_addr.s_addr = htonl(any ? INADDR_ANY : INADDR_LOOPBACK) };
        int one = 1;
        fd = qemu_socket(AF_INET, SOCK_STREAM, 0);
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
            error_setg_errno(errp, errno, "vsock forward bind tcp:%d", port);
            close(fd);
            return false;
        }
    } else if (g_str_has_prefix(head, "unix:")) {
        struct sockaddr_un sa = { .sun_family = AF_UNIX };
        unix_path = g_strdup(head + 5);
        g_strlcpy(sa.sun_path, unix_path, sizeof(sa.sun_path));
        unlink(unix_path);
        fd = qemu_socket(AF_UNIX, SOCK_STREAM, 0);
        if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
            error_setg_errno(errp, errno, "vsock forward bind %s", unix_path);
            close(fd);
            g_free(unix_path);
            return false;
        }
    } else {
        error_setg(errp, "bad vsock forward '%s'", spec);
        return false;
    }
    listen(fd, 8);
    qemu_socket_set_nonblock(fd);
    VSockListener *l = g_new0(VSockListener, 1);
    l->fd = fd;
    l->guest_port = guest_port;
    l->dev = s;
    l->unix_path = unix_path;
    g_ptr_array_add(s->listeners, l);
    qemu_set_fd_handler(fd, vsock_listener_accept, NULL, l);
    return true;
}

/* ---- virtio plumbing ---- */

static uint64_t vsock_get_features(VirtIODevice *vdev, uint64_t features, Error **errp)
{
    return features;
}

static void vsock_get_config(VirtIODevice *vdev, uint8_t *config)
{
    VirtIOVSockHost *s = VIRTIO_VSOCK_HOST(vdev);
    struct virtio_vsock_config cfg = {};
    virtio_stq_p(vdev, &cfg.guest_cid, s->guest_cid);
    memcpy(config, &cfg, sizeof(cfg));
}

static void vsock_reset(VirtIODevice *vdev)
{
    VirtIOVSockHost *s = VIRTIO_VSOCK_HOST(vdev);
    VSockConn *c, *tmp;
    QTAILQ_FOREACH_SAFE(c, &s->conns, next, tmp) {
        vsock_conn_free(c);
    }
    while (!QSIMPLEQ_EMPTY(&s->ctrl)) {
        VSockCtrl *ctl = QSIMPLEQ_FIRST(&s->ctrl);
        QSIMPLEQ_REMOVE_HEAD(&s->ctrl, next);
        g_free(ctl);
    }
}

static void vsock_set_status(VirtIODevice *vdev, uint8_t status)
{
    if (status & VIRTIO_CONFIG_S_DRIVER_OK) {
        qemu_bh_schedule(VIRTIO_VSOCK_HOST(vdev)->pump_bh);
    }
}

static void vsock_realize(DeviceState *dev, Error **errp)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(dev);
    VirtIOVSockHost *s = VIRTIO_VSOCK_HOST(dev);

    if (s->guest_cid <= VSOCK_HOST_CID || s->guest_cid >= UINT32_MAX) {
        error_setg(errp, "guest-cid must be > 2");
        return;
    }
    virtio_init(vdev, VIRTIO_ID_VSOCK, sizeof(struct virtio_vsock_config));
    s->rx_vq = virtio_add_queue(vdev, VSOCK_QUEUE_SIZE, vsock_handle_rx);
    s->tx_vq = virtio_add_queue(vdev, VSOCK_QUEUE_SIZE, vsock_handle_tx);
    s->event_vq = virtio_add_queue(vdev, VSOCK_QUEUE_SIZE, vsock_handle_event);
    QTAILQ_INIT(&s->conns);
    QSIMPLEQ_INIT(&s->ctrl);
    s->listeners = g_ptr_array_new();
    s->next_port = 49152;
    s->pump_bh = qemu_bh_new(vsock_pump_bh, s);

    if (s->forward) {
        g_auto(GStrv) specs = g_strsplit(s->forward, ";", -1);
        for (int i = 0; specs[i]; i++) {
            if (*specs[i] && !vsock_add_forward(s, specs[i], errp)) {
                return;
            }
        }
    }
    error_setg(&s->migration_blocker, "virtio-vsock-host does not support migration");
    migrate_add_blocker(&s->migration_blocker, NULL);
    vsock_host_singleton = s;
}

static void vsock_unrealize(DeviceState *dev)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(dev);
    VirtIOVSockHost *s = VIRTIO_VSOCK_HOST(dev);
    vsock_reset(vdev);
    for (guint i = 0; i < s->listeners->len; i++) {
        VSockListener *l = g_ptr_array_index(s->listeners, i);
        qemu_set_fd_handler(l->fd, NULL, NULL, NULL);
        close(l->fd);
        if (l->unix_path) {
            unlink(l->unix_path);
            g_free(l->unix_path);
        }
        g_free(l);
    }
    g_ptr_array_free(s->listeners, true);
    qemu_bh_delete(s->pump_bh);
    migrate_del_blocker(&s->migration_blocker);
    virtio_delete_queue(s->rx_vq);
    virtio_delete_queue(s->tx_vq);
    virtio_delete_queue(s->event_vq);
    virtio_cleanup(vdev);
    if (vsock_host_singleton == s) {
        vsock_host_singleton = NULL;
    }
}

static const Property vsock_props[] = {
    DEFINE_PROP_UINT64("guest-cid", VirtIOVSockHost, guest_cid, 3),
    DEFINE_PROP_STRING("forward", VirtIOVSockHost, forward),
    DEFINE_PROP_STRING("guest-forward", VirtIOVSockHost, guest_forward),
};

static void vsock_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    VirtioDeviceClass *vdc = VIRTIO_DEVICE_CLASS(klass);
    device_class_set_props(dc, vsock_props);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    vdc->realize = vsock_realize;
    vdc->unrealize = vsock_unrealize;
    vdc->get_features = vsock_get_features;
    vdc->get_config = vsock_get_config;
    vdc->reset = vsock_reset;
    vdc->set_status = vsock_set_status;
}

static const TypeInfo vsock_info = {
    .name = TYPE_VIRTIO_VSOCK_HOST,
    .parent = TYPE_VIRTIO_DEVICE,
    .instance_size = sizeof(VirtIOVSockHost),
    .class_init = vsock_class_init,
};

/* ---- PCI proxy ---- */

typedef struct VirtIOVSockHostPCI {
    VirtIOPCIProxy parent_obj;
    VirtIOVSockHost vdev;
} VirtIOVSockHostPCI;

#define TYPE_VIRTIO_VSOCK_HOST_PCI "virtio-vsock-host-pci-base"
DECLARE_INSTANCE_CHECKER(VirtIOVSockHostPCI, VIRTIO_VSOCK_HOST_PCI,
                         TYPE_VIRTIO_VSOCK_HOST_PCI)

static const Property vsock_pci_props[] = {
    DEFINE_PROP_UINT32("vectors", VirtIOPCIProxy, nvectors, 3),
};

static void vsock_pci_realize(VirtIOPCIProxy *vpci_dev, Error **errp)
{
    VirtIOVSockHostPCI *dev = VIRTIO_VSOCK_HOST_PCI(vpci_dev);
    virtio_pci_force_virtio_1(vpci_dev);
    qdev_realize(DEVICE(&dev->vdev), BUS(&vpci_dev->bus), errp);
}

static void vsock_pci_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    VirtioPCIClass *k = VIRTIO_PCI_CLASS(klass);
    PCIDeviceClass *pcidev_k = PCI_DEVICE_CLASS(klass);
    k->realize = vsock_pci_realize;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    device_class_set_props(dc, vsock_pci_props);
    pcidev_k->vendor_id = PCI_VENDOR_ID_REDHAT_QUMRANET;
    pcidev_k->device_id = PCI_DEVICE_ID_VIRTIO_VSOCK;
    pcidev_k->revision = 0x00;
    pcidev_k->class_id = PCI_CLASS_COMMUNICATION_OTHER;
}

static void vsock_pci_instance_init(Object *obj)
{
    VirtIOVSockHostPCI *dev = VIRTIO_VSOCK_HOST_PCI(obj);
    virtio_instance_init_common(obj, &dev->vdev, sizeof(dev->vdev),
                                TYPE_VIRTIO_VSOCK_HOST);
}

static const VirtioPCIDeviceTypeInfo vsock_pci_info = {
    .base_name = TYPE_VIRTIO_VSOCK_HOST_PCI,
    .generic_name = "virtio-vsock-host-pci",
    .non_transitional_name = "virtio-vsock-host-pci-non-transitional",
    .instance_size = sizeof(VirtIOVSockHostPCI),
    .instance_init = vsock_pci_instance_init,
    .class_init = vsock_pci_class_init,
};

static void vsock_register_types(void)
{
    type_register_static(&vsock_info);
    virtio_pci_types_register(&vsock_pci_info);
}

type_init(vsock_register_types)
