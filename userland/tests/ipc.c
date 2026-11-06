// IPC test for onirun: memfd and ashmem shared across fork, eventfd, epoll, unix sockets with
// fd passing and peer credentials, and system properties (get, set, wait).
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/system_properties.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

int memfd_create(const char *name, unsigned flags);
uint32_t __system_property_serial(const prop_info *pi);
static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define ASHMEM_SET_NAME _IOW(0x77, 1, char[256])
#define ASHMEM_SET_SIZE _IOW(0x77, 3, size_t)
#define ASHMEM_GET_SIZE _IO(0x77, 4)

static void test_shm(void) {
    int mfd = memfd_create("oni-test", MFD_CLOEXEC);
    CHECK(mfd >= 0, "memfd_create: %s", strerror(errno));
    ftruncate(mfd, 65536);
    atomic_int *m = mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0);
    int afd = open("/dev/ashmem", O_RDWR);
    CHECK(afd >= 0, "open /dev/ashmem: %s", strerror(errno));
    ioctl(afd, ASHMEM_SET_NAME, "oni-ashmem");
    CHECK(ioctl(afd, ASHMEM_SET_SIZE, (size_t)65536) == 0, "ASHMEM_SET_SIZE");
    CHECK(ioctl(afd, ASHMEM_GET_SIZE) == 65536, "ASHMEM_GET_SIZE");
    atomic_int *a = mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_SHARED, afd, 0);
    CHECK(m != MAP_FAILED && a != MAP_FAILED, "mmap shared");
    *m = 1; *a = 1;
    pid_t pid = fork();
    if (pid == 0) {
        // Also map the inherited fds fresh in the child: same pages.
        atomic_int *m2 = mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0);
        *m += 10; *a += 100; *m2 += 1000;
        _exit(0);
    }
    waitpid(pid, NULL, 0);
    CHECK(*m == 1011 && *a == 101, "shared memory after fork: memfd %d ashmem %d", *m, *a);
    char path[64], link[256];
    snprintf(path, sizeof path, "/proc/self/fd/%d", mfd);
    ssize_t n = readlink(path, link, sizeof link - 1);
    CHECK(n > 0 && !strncmp(link, "/memfd:", 7), "memfd readlink");
    printf("memfd and ashmem: ok\n");
}

static void *efd_writer(void *arg) {
    usleep(30000);
    eventfd_write((int)(long)arg, 5);
    return NULL;
}

static void test_eventfd_epoll(void) {
    int efd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    eventfd_t v = 0;
    CHECK(eventfd_read(efd, &v) < 0 && errno == EAGAIN, "empty eventfd should be EAGAIN");
    int ep = epoll_create1(EPOLL_CLOEXEC);
    struct epoll_event ev = { .events = EPOLLIN, .data.u64 = 0x1234 }, out[4];
    CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, efd, &ev) == 0, "epoll_ctl add");
    CHECK(epoll_wait(ep, out, 4, 0) == 0, "epoll should be empty");
    pthread_t t;
    pthread_create(&t, NULL, efd_writer, (void *)(long)efd);
    int r = epoll_wait(ep, out, 4, 2000);
    CHECK(r == 1 && out[0].data.u64 == 0x1234 && (out[0].events & EPOLLIN), "epoll_wait woke: %d", r);
    CHECK(eventfd_read(efd, &v) == 0 && v == 5, "eventfd value %llu", (unsigned long long)v);
    pthread_join(t, NULL);
    CHECK(epoll_wait(ep, out, 4, 0) == 0, "epoll empty again");
    int pfd[2];
    pipe(pfd);
    ev.events = EPOLLIN | EPOLLONESHOT; ev.data.u64 = 7;
    epoll_ctl(ep, EPOLL_CTL_ADD, pfd[0], &ev);
    write(pfd[1], "x", 1);
    CHECK(epoll_wait(ep, out, 4, 100) == 1 && out[0].data.u64 == 7, "pipe in epoll");
    CHECK(epoll_wait(ep, out, 4, 0) == 0, "oneshot disarmed");
    close(pfd[0]);
    close(pfd[1]);
    close(ep);
    close(efd);
    printf("eventfd and epoll: ok\n");
}

static int unix_listen(const char *path) {
    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    strcpy(sa.sun_path, path);
    CHECK(bind(s, (struct sockaddr *)&sa, sizeof sa) == 0, "bind %s: %s", path, strerror(errno));
    CHECK(listen(s, 4) == 0, "listen");
    return s;
}

static int unix_connect(const char *path, int type) {
    int s = socket(AF_UNIX, type, 0);
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    strcpy(sa.sun_path, path);
    if (connect(s, (struct sockaddr *)&sa, sizeof sa)) { int e = errno; close(s); errno = e; return -1; }
    return s;
}

static void test_sockets(void) {
    int ls = unix_listen("/dev/socket/oni-test");
    CHECK(unix_connect("/dev/socket/nobody", SOCK_STREAM) < 0 && errno == ECONNREFUSED, "connect to nothing");
    int pfd[2];
    pipe(pfd);
    pid_t pid = fork();
    if (pid == 0) {
        close(ls);
        int s = unix_connect("/dev/socket/oni-test", SOCK_STREAM);
        if (s < 0) _exit(1);
        // Send a message carrying the pipe's write end.
        struct iovec iov = { "hello", 5 };
        char ctl[CMSG_SPACE(sizeof(int))];
        struct msghdr mh = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = ctl, .msg_controllen = sizeof ctl };
        struct cmsghdr *cm = CMSG_FIRSTHDR(&mh);
        cm->cmsg_level = SOL_SOCKET; cm->cmsg_type = SCM_RIGHTS; cm->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(cm), &pfd[1], sizeof(int));
        if (sendmsg(s, &mh, MSG_NOSIGNAL) != 5) _exit(2);
        char reply[16] = {0};
        if (read(s, reply, sizeof reply) <= 0 || strcmp(reply, "world")) _exit(3);
        _exit(0);
    }
    close(pfd[1]);
    struct pollfd pf = { ls, POLLIN, 0 };
    CHECK(poll(&pf, 1, 2000) == 1, "listener readable");
    int s = accept4(ls, NULL, NULL, SOCK_CLOEXEC);
    CHECK(s >= 0, "accept: %s", strerror(errno));
    struct ucred cr = {0};
    socklen_t cl = sizeof cr;
    CHECK(getsockopt(s, SOL_SOCKET, SO_PEERCRED, &cr, &cl) == 0 && cr.pid == pid, "SO_PEERCRED pid %d want %d", cr.pid, pid);
    char buf[16] = {0}, ctl[256];
    struct iovec iov = { buf, sizeof buf };
    struct msghdr mh = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = ctl, .msg_controllen = sizeof ctl };
    ssize_t n = recvmsg(s, &mh, MSG_CMSG_CLOEXEC);
    CHECK(n == 5 && !memcmp(buf, "hello", 5), "recvmsg data %zd", n);
    int got = -1;
    for (struct cmsghdr *cm = CMSG_FIRSTHDR(&mh); cm; cm = CMSG_NXTHDR(&mh, cm))
        if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_RIGHTS) memcpy(&got, CMSG_DATA(cm), sizeof(int));
    CHECK(got >= 0, "no SCM_RIGHTS fd");
    if (got >= 0) {
        write(got, "via passed fd", 13);
        char pb[32] = {0};
        CHECK(read(pfd[0], pb, sizeof pb) == 13 && !strcmp(pb, "via passed fd"), "passed fd works");
        close(got);
    }
    write(s, "world", 6);
    int st;
    waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "socket child status %x", st);
    close(s);
    close(ls);
    close(pfd[0]);
    // socketpair with a datagram type and a seqpacket connection
    int sp[2];
    CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sp) == 0, "socketpair seqpacket");
    write(sp[0], "ab", 2); write(sp[0], "cd", 2);
    CHECK(read(sp[1], buf, sizeof buf) == 2, "seqpacket keeps boundaries");
    close(sp[0]); close(sp[1]);
    printf("unix sockets, fd passing, peer credentials: ok\n");
}

static void *prop_waiter(void *arg) {
    const prop_info *pi = __system_property_find("debug.oni.wait");
    uint32_t serial = __system_property_serial(pi);
    __system_property_wait(pi, serial, &serial, NULL);
    char v[PROP_VALUE_MAX];
    __system_property_get("debug.oni.wait", v);
    *(int *)arg = atoi(v);
    return NULL;
}

static void test_props(void) {
    char v[PROP_VALUE_MAX] = {0};
    CHECK(__system_property_get("ro.build.version.sdk", v) > 0 && atoi(v) >= 30, "ro.build.version.sdk = '%s'", v);
    CHECK(__system_property_set("debug.oni.test", "value one") == 0, "set debug.oni.test");
    CHECK(__system_property_get("debug.oni.test", v) == 9 && !strcmp(v, "value one"), "get after set: '%s'", v);
    CHECK(__system_property_set("debug.oni.test", "two") == 0, "update");
    CHECK(__system_property_get("debug.oni.test", v) == 3 && !strcmp(v, "two"), "get after update: '%s'", v);
    CHECK(__system_property_set("ro.build.version.sdk", "1") != 0, "ro. property must be read-only");
    CHECK(__system_property_set("debug.oni.wait", "0") == 0, "set wait prop");
    int seen = -1;
    pthread_t t;
    pthread_create(&t, NULL, prop_waiter, &seen);
    usleep(50000);
    __system_property_set("debug.oni.wait", "42");
    pthread_join(t, NULL);
    CHECK(seen == 42, "waiter saw %d", seen);
    // Another process sees it too, and sees its own changes reflected here.
    pid_t pid = fork();
    if (pid == 0) {
        __system_property_get("debug.oni.wait", v);
        if (strcmp(v, "42")) _exit(1);
        __system_property_set("debug.oni.child", "from child");
        _exit(0);
    }
    int st;
    waitpid(pid, &st, 0);
    CHECK(WEXITSTATUS(st) == 0, "child property view");
    CHECK(__system_property_get("debug.oni.child", v) > 0 && !strcmp(v, "from child"), "child's set visible: '%s'", v);
    printf("properties: ok\n");
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    test_shm();
    test_eventfd_epoll();
    test_sockets();
    test_props();
    printf("%s\n", failures ? "IPC FAILED" : "IPC OK");
    return failures != 0;
}
