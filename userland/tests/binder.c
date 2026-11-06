// Binder test for onirun, speaking the driver protocol directly: a server becomes the context
// manager; a client sends a transaction carrying a string, a binder object and a file descriptor;
// the server replies with a string and objects; refcount commands flow; a oneway call arrives;
// and when the server dies the client gets its death notification.
#include <errno.h>
#include <fcntl.h>
#include <linux/android/binder.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define VM_SIZE ((1 << 20) - 2 * 4096)

typedef struct { int fd; uint8_t *map; uint8_t rbuf[4096]; size_t rlen, rpos; } Dev;

static int dev_open(Dev *d) {
    d->fd = open("/dev/binder", O_RDWR | O_CLOEXEC);
    if (d->fd < 0) { printf("open /dev/binder: %s\n", strerror(errno)); return -1; }
    struct binder_version v = {0};
    CHECK(ioctl(d->fd, BINDER_VERSION, &v) == 0 && v.protocol_version == BINDER_CURRENT_PROTOCOL_VERSION, "version %d", v.protocol_version);
    d->map = mmap(NULL, VM_SIZE, PROT_READ, MAP_PRIVATE | MAP_NORESERVE, d->fd, 0);
    CHECK(d->map != MAP_FAILED, "mmap binder: %s", strerror(errno));
    uint32_t max = 4;
    ioctl(d->fd, BINDER_SET_MAX_THREADS, &max);
    return 0;
}

static int dev_write(Dev *d, const void *buf, size_t n) {
    struct binder_write_read bwr = { .write_size = n, .write_buffer = (binder_uintptr_t)buf };
    return ioctl(d->fd, BINDER_WRITE_READ, &bwr);
}

// Reads one command, refilling from the driver when the buffer is empty. Returns the command.
static uint32_t dev_next(Dev *d, const void **arg) {
    for (;;) {
        if (d->rpos < d->rlen) {
            uint32_t cmd;
            memcpy(&cmd, d->rbuf + d->rpos, 4);
            *arg = d->rbuf + d->rpos + 4;
            d->rpos += 4 + _IOC_SIZE(cmd);
            if (cmd == BR_NOOP) continue;
            return cmd;
        }
        struct binder_write_read bwr = { .read_size = sizeof d->rbuf, .read_buffer = (binder_uintptr_t)d->rbuf };
        if (ioctl(d->fd, BINDER_WRITE_READ, &bwr) < 0) { if (errno == EINTR) continue; printf("read: %s\n", strerror(errno)); exit(2); }
        d->rlen = bwr.read_consumed;
        d->rpos = 0;
    }
}

static void free_buffer(Dev *d, binder_uintptr_t buf) {
    struct { uint32_t cmd; binder_uintptr_t ptr; } __attribute__((packed)) c = { BC_FREE_BUFFER, buf };
    dev_write(d, &c, sizeof c);
}

// Serves node refcount commands by acknowledging them; returns false for anything else.
static int handle_refs(Dev *d, uint32_t cmd, const void *arg) {
    if (cmd == BR_INCREFS || cmd == BR_ACQUIRE) {
        struct { uint32_t cmd; struct binder_ptr_cookie pc; } __attribute__((packed)) ack;
        ack.cmd = cmd == BR_INCREFS ? BC_INCREFS_DONE : BC_ACQUIRE_DONE;
        memcpy(&ack.pc, arg, sizeof ack.pc);
        dev_write(d, &ack, sizeof ack);
        return 1;
    }
    return cmd == BR_RELEASE || cmd == BR_DECREFS || cmd == BR_SPAWN_LOOPER || cmd == BR_TRANSACTION_COMPLETE;
}

// ---- server ----
static int server(int notify_fd) {
    Dev d = {0};
    if (dev_open(&d)) return 1;
    struct flat_binder_object mgr = { .hdr.type = BINDER_TYPE_BINDER, .flags = FLAT_BINDER_FLAG_ACCEPTS_FDS | FLAT_BINDER_FLAG_TXN_SECURITY_CTX,
                                      .binder = 0xA, .cookie = 0xB };
    CHECK(ioctl(d.fd, BINDER_SET_CONTEXT_MGR_EXT, &mgr) == 0, "SET_CONTEXT_MGR_EXT: %s", strerror(errno));
    uint32_t enter = BC_ENTER_LOOPER;
    dev_write(&d, &enter, 4);
    int got_sync = 0, got_oneway = 0;
    while (!(got_sync && got_oneway)) {
        const void *arg;
        uint32_t cmd = dev_next(&d, &arg);
        if (handle_refs(&d, cmd, arg)) continue;
        if (cmd != BR_TRANSACTION && cmd != BR_TRANSACTION_SEC_CTX) { printf("server: unexpected %08x\n", cmd); failures++; continue; }
        struct binder_transaction_data tr;
        memcpy(&tr, arg, sizeof tr);
        CHECK(tr.target.ptr == 0xA && tr.cookie == 0xB, "target %llx/%llx", (unsigned long long)tr.target.ptr, (unsigned long long)tr.cookie);
        const uint8_t *data = (const uint8_t *)tr.data.ptr.buffer;
        if (tr.flags & TF_ONE_WAY) {
            CHECK(tr.code == 2 && tr.sender_pid == 0 && !memcmp(data, "fire", 4), "oneway call");
            got_oneway = 1;
            free_buffer(&d, tr.data.ptr.buffer);
            continue;
        }
        CHECK(tr.code == 1 && tr.sender_pid > 0 && tr.sender_euid == getuid(), "sync call code %u pid %d", tr.code, tr.sender_pid);
        if (cmd == BR_TRANSACTION_SEC_CTX) {
            binder_uintptr_t sc;
            memcpy(&sc, (const uint8_t *)arg + sizeof tr, 8);
            CHECK(!strcmp((const char *)sc, "u:r:shell:s0"), "secctx '%s'", (const char *)sc);
        } else failures++, printf("FAIL: no security context\n");
        CHECK(tr.data_size == 8 + 24 + 24 && tr.offsets_size == 16, "sizes %llu %llu", (unsigned long long)tr.data_size, (unsigned long long)tr.offsets_size);
        CHECK(!memcmp(data, "ping", 4), "payload");
        struct flat_binder_object obj;
        memcpy(&obj, data + 8, 24);
        CHECK(obj.hdr.type == BINDER_TYPE_HANDLE && obj.handle != 0, "client object arrived as handle %u (type %x)", obj.handle, obj.hdr.type);
        struct binder_fd_object fdo;
        memcpy(&fdo, data + 32, 24);
        char fbuf[8] = {0};
        CHECK(fdo.hdr.type == BINDER_TYPE_FD && read(fdo.fd, fbuf, 5) == 5 && !strcmp(fbuf, "fdok!"), "fd object: type %x read '%s'", fdo.hdr.type, fbuf);
        close(fdo.fd);
        // Keep a strong ref on the client's object (as a real server would), then reply with it and one of ours.
        struct { uint32_t cmd; uint32_t h; } acq = { BC_ACQUIRE, obj.handle };
        dev_write(&d, &acq, sizeof acq);
        uint8_t rdata[8 + 24 + 24];
        memcpy(rdata, "pong\0\0\0", 8);
        memcpy(rdata + 8, &obj, 24);
        struct flat_binder_object mine = { .hdr.type = BINDER_TYPE_BINDER, .binder = 0x300, .cookie = 0x301 };
        memcpy(rdata + 32, &mine, 24);
        binder_size_t offs[2] = { 8, 32 };
        struct { uint32_t cmd; struct binder_transaction_data tr; } __attribute__((packed)) rep = {
            BC_REPLY, { .flags = TF_ACCEPT_FDS, .data_size = sizeof rdata, .offsets_size = sizeof offs,
                        .data.ptr.buffer = (binder_uintptr_t)rdata, .data.ptr.offsets = (binder_uintptr_t)offs } };
        free_buffer(&d, tr.data.ptr.buffer);
        CHECK(dev_write(&d, &rep, sizeof rep) == 0, "reply: %s", strerror(errno));
        got_sync = 1;
    }
    // Our node 0x300 should be referenced now: expect BR_INCREFS/BR_ACQUIRE to have come through.
    char b;
    read(notify_fd, &b, 1);              // the client says it's done: die and trigger the death notice
    printf("server: %s\n", failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}

// ---- client ----
static int client(int notify_fd) {
    Dev d = {0};
    if (dev_open(&d)) return 1;
    usleep(100000);                      // let the server become context manager
    int pfd[2];
    pipe(pfd);
    write(pfd[1], "fdok!", 5);
    uint8_t data[8 + 24 + 24];
    memcpy(data, "ping\0\0\0", 8);
    struct flat_binder_object mine = { .hdr.type = BINDER_TYPE_BINDER, .flags = FLAT_BINDER_FLAG_ACCEPTS_FDS, .binder = 0x100, .cookie = 0x200 };
    memcpy(data + 8, &mine, 24);
    struct binder_fd_object fdo = { .hdr.type = BINDER_TYPE_FD, .fd = pfd[0] };
    memcpy(data + 32, &fdo, 24);
    binder_size_t offs[2] = { 8, 32 };
    struct { uint32_t cmd; struct binder_transaction_data tr; } __attribute__((packed)) req = {
        BC_TRANSACTION, { .target.handle = 0, .code = 1, .flags = TF_ACCEPT_FDS, .data_size = sizeof data, .offsets_size = sizeof offs,
                          .data.ptr.buffer = (binder_uintptr_t)data, .data.ptr.offsets = (binder_uintptr_t)offs } };
    CHECK(dev_write(&d, &req, sizeof req) == 0, "transaction: %s", strerror(errno));
    int complete = 0, replied = 0, refs = 0;
    uint32_t server_handle = 0;
    while (!replied) {
        const void *arg;
        uint32_t cmd = dev_next(&d, &arg);
        if (cmd == BR_TRANSACTION_COMPLETE) { complete++; continue; }
        if (cmd == BR_INCREFS || cmd == BR_ACQUIRE) { refs++; handle_refs(&d, cmd, arg); continue; }
        if (cmd == BR_DEAD_REPLY || cmd == BR_FAILED_REPLY) { printf("FAIL: reply error %08x\n", cmd); failures++; break; }
        if (cmd != BR_REPLY) { printf("client: unexpected %08x\n", cmd); failures++; continue; }
        struct binder_transaction_data tr;
        memcpy(&tr, arg, sizeof tr);
        const uint8_t *rd = (const uint8_t *)tr.data.ptr.buffer;
        CHECK(tr.data_size == 56 && !memcmp(rd, "pong", 4), "reply payload");
        struct flat_binder_object back, theirs;
        memcpy(&back, rd + 8, 24);
        memcpy(&theirs, rd + 32, 24);
        CHECK(back.hdr.type == BINDER_TYPE_BINDER && back.binder == 0x100 && back.cookie == 0x200, "our object came back as %x %llx", back.hdr.type, (unsigned long long)back.binder);
        CHECK(theirs.hdr.type == BINDER_TYPE_HANDLE && theirs.handle != 0, "server object as handle %u", theirs.handle);
        server_handle = theirs.handle;
        free_buffer(&d, tr.data.ptr.buffer);
        replied = 1;
    }
    CHECK(complete == 1, "one BR_TRANSACTION_COMPLETE, got %d", complete);
    CHECK(refs >= 1, "the server kept a reference to our object (%d ref commands)", refs);
    // Oneway call.
    struct { uint32_t cmd; struct binder_transaction_data tr; } __attribute__((packed)) one = {
        BC_TRANSACTION, { .target.handle = 0, .code = 2, .flags = TF_ONE_WAY, .data_size = 8, .data.ptr.buffer = (binder_uintptr_t)"fire\0\0\0" } };
    CHECK(dev_write(&d, &one, sizeof one) == 0, "oneway: %s", strerror(errno));
    const void *arg;
    CHECK(dev_next(&d, &arg) == BR_TRANSACTION_COMPLETE, "oneway completion");
    // Death notification on the context manager, then tell the server to exit. Death notices are
    // process work, which only looper threads receive, so become one.
    struct { uint32_t cmd; struct binder_handle_cookie hc; } __attribute__((packed)) dn = { BC_REQUEST_DEATH_NOTIFICATION, { 0, 0xDEAD } };
    struct { uint32_t cmd; uint32_t h; } inc = { BC_INCREFS, 0 };
    dev_write(&d, &inc, sizeof inc);
    uint32_t enter = BC_ENTER_LOOPER;
    dev_write(&d, &enter, 4);
    CHECK(dev_write(&d, &dn, sizeof dn) == 0, "death notification request");
    write(notify_fd, "x", 1);
    uint32_t cmd = dev_next(&d, &arg);
    while (cmd == BR_RELEASE || cmd == BR_DECREFS || cmd == BR_INCREFS || cmd == BR_ACQUIRE) { handle_refs(&d, cmd, arg); cmd = dev_next(&d, &arg); }
    binder_uintptr_t cookie = 0;
    if (cmd == BR_DEAD_BINDER) memcpy(&cookie, arg, 8);
    CHECK(cmd == BR_DEAD_BINDER && cookie == 0xDEAD, "death notice: cmd %08x cookie %llx", cmd, (unsigned long long)cookie);
    struct { uint32_t cmd; binder_uintptr_t c; } __attribute__((packed)) done = { BC_DEAD_BINDER_DONE, cookie };
    dev_write(&d, &done, sizeof done);
    // A call to the dead manager fails cleanly.
    CHECK(dev_write(&d, &one, sizeof one) == 0, "write after death");
    cmd = dev_next(&d, &arg);
    while (cmd == BR_SPAWN_LOOPER || cmd == BR_RELEASE || cmd == BR_DECREFS) cmd = dev_next(&d, &arg);
    CHECK(cmd == BR_DEAD_REPLY || cmd == BR_FAILED_REPLY, "call to dead target: %08x", cmd);
    (void)server_handle;
    return failures ? 1 : 0;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    int np[2];
    pipe(np);
    pid_t srv = fork();
    if (srv == 0) { close(np[1]); _exit(server(np[0])); }
    close(np[0]);
    int rc = client(np[1]);
    int st;
    waitpid(srv, &st, 0);
    int ok = rc == 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0;
    printf("%s\n", ok ? "BINDER OK" : "BINDER FAILED");
    return !ok;
}
