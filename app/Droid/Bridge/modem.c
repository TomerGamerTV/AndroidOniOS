/*
 * Minimal AT-command modem for the guest's ranchu radio HAL.
 *
 * QEMU exposes a virtio-serial port named "modem" whose chardev connects to the
 * unix socket served here.  We present a registered LTE network with a ready
 * SIM so Android brings up mobile data on eth0; the IP configuration handed out
 * in +CGCONTRDP matches QEMU's user-mode network (10.0.2.0/24).
 *
 * Responses use '\r' line endings and end with "OK\r" (or an error), matching
 * the HAL's parser (device/generic/goldfish/hals/radio/AtResponse.cpp).
 *
 * Also builds as a standalone tool for the macOS dev harness:
 *   cc -DMODEM_MAIN -o modemsim modem.c && ./modemsim /path/to/socket
 */
#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define MCC_MNC "310260"
#define OPERATOR "Android"
#define IMSI "310260000000001"
#define ICCID "89014103211118510720"
#define IMEI "358240051111110"
#define MSISDN "15555215554"

typedef struct {
    int fd;
    int radio_on;
    int cid_active[16];
    char apn[16][96];
    char log_path[1024];
} modem_t;

static void mlog(modem_t *m, const char *fmt, ...) {
    if (!m->log_path[0]) return;
    FILE *f = fopen(m->log_path, "a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

static void out(modem_t *m, const char *fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if (n > (int)sizeof(buf)) n = sizeof(buf);
    const char *p = buf;
    while (n > 0) {
        ssize_t w = write(m->fd, p, n);
        if (w <= 0) return;
        p += w;
        n -= w;
    }
}

#define OK() out(m, "OK\r")

static const char *kCSQ = "+CSQ: 31,99,-1,-1,-1,-1,-1,31,-85,-9,300,15,2147483647,2147483647\r";

static void reg_lines(modem_t *m, const char *cmd, int unsol) {
    int state = m->radio_on ? 1 : 0;
    if (unsol) {
        out(m, "%s: %d,\"0001\",\"0000000a\",14\r", cmd, state);
    } else {
        out(m, "%s: 2,%d,\"0001\",\"0000000a\",14\r", cmd, state);
    }
}

static void send_unsolicited_registration(modem_t *m) {
    reg_lines(m, "+CREG", 1);
    reg_lines(m, "+CGREG", 1);
    reg_lines(m, "+CEREG", 1);
    out(m, "%s", kCSQ);
}

/* SIM elementary files we know about. Everything else -> 6A82 (file not found). */
static void crsm(modem_t *m, int command, int fileid, int p1, int p2, int p3) {
    (void)p1; (void)p2; (void)p3;
    const char *data = NULL;
    /* Body bytes for READ BINARY / READ RECORD. */
    switch (fileid) {
    case 0x2FE2: data = "98101430121181157002"; break;          /* EF_ICCID (BCD swapped) */
    case 0x6FAD: data = "00000003"; break;                      /* EF_AD: 3-digit MNC */
    case 0x6F46: data = "01416E64726F6964FFFFFFFFFFFFFFFFFF"; break; /* EF_SPN "Android" */
    case 0x6F07: data = "080910101000000010"; break;            /* EF_IMSI 310260000000001 */
    case 0x6F38: data = "9EFFBF7F0000000000000000"; break;     /* EF_UST */
    }
    if (command == 192) { /* GET RESPONSE -> FCP template */
        if (!data) { out(m, "+CRSM: 106,130\r"); OK(); return; }
        int len = (int)strlen(data) / 2;
        out(m, "+CRSM: 144,0,62178202412183022F%02X8A01058B032F0606800200%02X8800\r", fileid & 0xff, len);
        OK();
        return;
    }
    if ((command == 176 || command == 178) && data) {
        out(m, "+CRSM: 144,0,%s\r", data);
        OK();
        return;
    }
    if (command == 214 || command == 220) { out(m, "+CRSM: 144,0\r"); OK(); return; } /* UPDATE */
    out(m, "+CRSM: 106,130\r");
    OK();
}

static int parse_ints(const char *s, int *v, int max) {
    int n = 0;
    while (*s && n < max) {
        while (*s == ' ' || *s == '"') s++;
        if (!*s) break;
        v[n++] = (int)strtol(s, (char **)&s, 0);
        while (*s && *s != ',') s++;
        if (*s == ',') s++;
    }
    return n;
}

static void handle(modem_t *m, const char *cmd) {
    mlog(m, "> %s", cmd);
    /* Compound operator query: AT+COPS=3,0;+COPS?;+COPS=3,1;+COPS?;+COPS=3,2;+COPS? */
    if (strstr(cmd, "+COPS=3,0;+COPS?")) {
        if (!m->radio_on) { out(m, "+COPS: 0,0,0\r"); OK(); return; }
        out(m, "+COPS: 0,0," OPERATOR "\r+COPS: 0,1," OPERATOR "\r+COPS: 0,2," MCC_MNC "\r");
        OK();
        return;
    }
    if (!strncmp(cmd, "AT+CFUN?", 8)) { out(m, "+CFUN: %d\r", m->radio_on); OK(); return; }
    if (!strncmp(cmd, "AT+CFUN=", 8)) {
        int on = atoi(cmd + 8) ? 1 : 0;
        int changed = on != m->radio_on;
        m->radio_on = on;
        OK();
        if (changed) send_unsolicited_registration(m);
        return;
    }
    if (!strcmp(cmd, "AT+CPIN?")) { out(m, "+CPIN: READY\r"); OK(); return; }
    if (!strncmp(cmd, "AT+CPINR", 8)) { out(m, "+CPINR: SIM PIN,3,3\r"); OK(); return; }
    if (!strcmp(cmd, "AT+CICCID")) { out(m, ICCID "\rOK\r"); return; }
    if (!strcmp(cmd, "AT+CIMI")) { out(m, IMSI "\rOK\r"); return; }
    if (!strncmp(cmd, "AT+CGSN", 7)) { out(m, IMEI "\rOK\r"); return; }
    if (!strcmp(cmd, "AT+CNUM")) { out(m, "+CNUM: ,\"" MSISDN "\",129\r"); OK(); return; }
    if (!strcmp(cmd, "AT+CSQ")) { out(m, "%s", kCSQ); OK(); return; }
    if (!strcmp(cmd, "AT+CREG?")) { reg_lines(m, "+CREG", 0); OK(); return; }
    if (!strcmp(cmd, "AT+CGREG?")) { reg_lines(m, "+CGREG", 0); OK(); return; }
    if (!strcmp(cmd, "AT+CEREG?")) { reg_lines(m, "+CEREG", 0); OK(); return; }
    if (!strcmp(cmd, "AT+COPS?")) { out(m, "+COPS: 0,2," MCC_MNC "\r"); OK(); return; }
    if (!strcmp(cmd, "AT+COPS=?")) { out(m, "+COPS: (2," OPERATOR "," OPERATOR "," MCC_MNC ")\r"); OK(); return; }
    if (!strcmp(cmd, "AT+CTEC=?")) { out(m, "+CTEC: 0,1,5\r"); OK(); return; }
    if (!strcmp(cmd, "AT+CTEC?")) { out(m, "+CTEC: 32,\"23\"\r"); OK(); return; }
    if (!strncmp(cmd, "AT+CTEC=", 8)) { out(m, "+CTEC: 32\r"); OK(); return; }
    if (!strcmp(cmd, "AT+WRMP?")) { out(m, "+WRMP: 0\r"); OK(); return; }
    if (!strcmp(cmd, "AT+CCSS?")) { out(m, "+CCSS: 0\r"); OK(); return; }
    if (!strcmp(cmd, "AT+CLCC")) { OK(); return; }
    if (!strcmp(cmd, "AT+CLIP?")) { out(m, "+CLIP: 1,1\r"); OK(); return; }
    if (!strcmp(cmd, "AT+CLIR?")) { out(m, "+CLIR: 0,0\r"); OK(); return; }
    if (!strcmp(cmd, "AT+CMUT?")) { out(m, "+CMUT: 0\r"); OK(); return; }
    if (!strcmp(cmd, "AT+CSCA?")) { out(m, "+CSCA: \"+15555551234\",145\r"); OK(); return; }
    if (!strcmp(cmd, "AT+CSCB?")) { out(m, "+CSCB: 0,\"\",\"\"\r"); OK(); return; }
    if (!strcmp(cmd, "AT+CUSATD?")) { out(m, "+CUSATD: 0,0\r"); OK(); return; }
    if (!strncmp(cmd, "AT+CLCK=", 8)) {
        if (strstr(cmd, ",2")) { out(m, "+CLCK: 0\r"); }
        OK();
        return;
    }
    if (!strncmp(cmd, "AT+CRSM=", 8)) {
        int v[5] = {0};
        parse_ints(cmd + 8, v, 5);
        crsm(m, v[0], v[1], v[2], v[3], v[4]);
        return;
    }
    if (!strcmp(cmd, "AT+CGDCONT?")) {
        int any = 0;
        for (int i = 1; i < 16; i++) {
            if (m->apn[i][0]) {
                out(m, "+CGDCONT: %d,\"IPV4V6\",\"%s\",,0,0\r", i, m->apn[i]);
                any = 1;
            }
        }
        if (!any) out(m, "+CGDCONT: 1,\"IP\",\"internet\",,0,0\r");
        OK();
        return;
    }
    if (!strncmp(cmd, "AT+CGDCONT=", 11)) {
        int cid = atoi(cmd + 11);
        if (cid > 0 && cid < 16) {
            const char *q = strchr(cmd + 11, ',');
            q = q ? strchr(q + 1, ',') : NULL;  /* skip type */
            m->apn[cid][0] = 0;
            if (q) {
                q++;
                if (*q == '"') q++;
                size_t i = 0;
                while (*q && *q != '"' && i + 1 < sizeof(m->apn[cid])) m->apn[cid][i++] = *q++;
                m->apn[cid][i] = 0;
            }
            if (!m->apn[cid][0]) strcpy(m->apn[cid], "internet");
            m->cid_active[cid] = 1;
        }
        OK();
        return;
    }
    if (!strncmp(cmd, "AT+CGACT=", 9)) { OK(); return; }
    if (!strncmp(cmd, "AT+CGCONTRDP=", 13)) {
        int cid = atoi(cmd + 13);
        if (cid <= 0 || cid >= 16) cid = 1;
        out(m, "+CGCONTRDP: %d,5,\"%s\",10.0.2.15/24,10.0.2.2,10.0.2.3\r", cid, m->apn[cid][0] ? m->apn[cid] : "internet");
        OK();
        return;
    }
    if (!strncmp(cmd, "AT+CGDATA", 9)) { out(m, "CONNECT\r"); return; }
    if (!strncmp(cmd, "AT+CMGS", 7) || !strncmp(cmd, "AT+CMGW", 7)) { out(m, "+CMS ERROR: 303\r"); return; }
    if (!strncmp(cmd, "ATD", 3)) { out(m, "+CME ERROR: 30\r"); return; }
    if (!strncmp(cmd, "AT^MBAU", 7)) { out(m, "+CME ERROR: 4\r"); return; }
    if (!strncmp(cmd, "AT+CSIM", 7) || !strncmp(cmd, "AT+CGLA", 7) || !strncmp(cmd, "AT+CCHO", 7)) {
        out(m, "+CME ERROR: 4\r");
        return;
    }
    /* Default: accept. Covers ATE0Q0V1, +CMEE, +CREG=n, +CSSN, +CMGF, +CNMA, ... */
    OK();
}

static void serve(modem_t *m) {
    char line[4096];
    size_t len = 0;
    char ch;
    while (read(m->fd, &ch, 1) == 1) {
        if (ch == '\r' || ch == '\n') {
            if (len) {
                line[len] = 0;
                /* trim */
                char *s = line;
                while (*s == ' ') s++;
                handle(m, s);
                len = 0;
            }
            continue;
        }
        if (len + 1 < sizeof(line)) line[len++] = ch;
    }
}

typedef struct { int listen_fd; char log_path[1024]; } server_t;

static void *server_thread(void *arg) {
    server_t *srv = arg;
    for (;;) {
        int c = accept(srv->listen_fd, NULL, NULL);
        if (c < 0) {
            if (errno == EINTR) continue;
            break;
        }
        int one = 1;
        setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
        modem_t m;
        memset(&m, 0, sizeof(m));
        m.fd = c;
        m.radio_on = 0;
        strncpy(m.log_path, srv->log_path, sizeof(m.log_path) - 1);
        mlog(&m, "-- modem connected");
        serve(&m);
        close(c);
    }
    return NULL;
}

int droid_modem_start(const char *socket_path, const char *log_path) {
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    if (strlen(socket_path) >= sizeof(sa.sun_path)) return -1;
    strcpy(sa.sun_path, socket_path);
    unlink(socket_path);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 || listen(fd, 2) < 0) {
        close(fd);
        return -1;
    }
    server_t *srv = calloc(1, sizeof(*srv));
    srv->listen_fd = fd;
    if (log_path) strncpy(srv->log_path, log_path, sizeof(srv->log_path) - 1);
    pthread_t t;
    pthread_create(&t, NULL, server_thread, srv);
    pthread_detach(t);
    return 0;
}

/*
 * Silent endpoint for serial ports whose real peer (the emulator's simulated Bluetooth,
 * UWB and NFC radios) doesn't exist here. Accepts the connection and discards data, so
 * the guest sees a connected port that never answers instead of EOF.
 */
static void *sink_conn(void *arg) {
    int c = (int)(intptr_t)arg;
    char buf[4096];
    while (read(c, buf, sizeof(buf)) > 0) {}
    close(c);
    return NULL;
}

static void *sink_accept(void *arg) {
    int fd = (int)(intptr_t)arg;
    for (;;) {
        int c = accept(fd, NULL, NULL);
        if (c < 0) {
            if (errno == EINTR) continue;
            break;
        }
        pthread_t t;
        pthread_create(&t, NULL, sink_conn, (void *)(intptr_t)c);
        pthread_detach(t);
    }
    return NULL;
}

int droid_sink_start(const char *socket_path) {
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    if (strlen(socket_path) >= sizeof(sa.sun_path)) return -1;
    strcpy(sa.sun_path, socket_path);
    unlink(socket_path);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 || listen(fd, 2) < 0) {
        close(fd);
        return -1;
    }
    pthread_t t;
    pthread_create(&t, NULL, sink_accept, (void *)(intptr_t)fd);
    pthread_detach(t);
    return 0;
}

#ifdef MODEM_MAIN
int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <socket> [log]\n", argv[0]); return 1; }
    if (droid_modem_start(argv[1], argc > 2 ? argv[2] : NULL) != 0) { perror("modem"); return 1; }
    /* Extra args after the log path: silent radio sockets (bluetooth/uwb/nfc). */
    for (int i = 3; i < argc; i++) {
        if (droid_sink_start(argv[i]) != 0) { perror("sink"); return 1; }
    }
    for (;;) pause();
}
#endif
