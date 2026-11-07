// Unpacks a rootfs archive made by tools/mkrootfs-tar.py: a pax tar whose entries carry their
// extended attributes as SCHILY.xattr.* records (guest ownership, modes, SELinux labels). Used by
// the iPhone app (onirun_install_rootfs) and by `onirun --install TAR DIR` for testing.
// Handles regular files, directories, symlinks and hard links; extracts into DIR.partial and
// renames it to DIR at the end, so an interrupted install never looks complete.
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>

#include "onx.h"

typedef struct { char *name; char *val; size_t len; } Xattr;

static uint64_t octal(const char *p, size_t n) {
    uint64_t v = 0;
    for (size_t i = 0; i < n && p[i]; i++) if (p[i] >= '0' && p[i] <= '7') v = v * 8 + (uint64_t)(p[i] - '0');
    return v;
}

static int mkdirs(char *path) {     // like mkdir -p of path's parent
    for (char *s = path + 1; *s; s++) {
        if (*s != '/') continue;
        *s = 0;
        if (mkdir(path, 0755) && errno != EEXIST) { *s = '/'; return -1; }
        *s = '/';
    }
    return 0;
}

static int read_full(int fd, void *buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, (char *)buf + got, n - got);
        if (r <= 0) return -1;
        got += (size_t)r;
    }
    return 0;
}

// progress(done_bytes, total_bytes, opaque) is called now and then; returns 0 or -errno.
int onirun_install_rootfs(const char *tar, const char *dest, void (*progress)(uint64_t, uint64_t, void *), void *opaque) {
    int fd = open(tar, O_RDONLY);
    if (fd < 0) return -errno;
    struct stat tst;
    fstat(fd, &tst);
    char tmp[1024], path[2048], lnk[2048];
    snprintf(tmp, sizeof tmp, "%s.partial", dest);
    mkdir(tmp, 0755);
    uint64_t done = 0, last = 0;
    char hdr[512];
    char *pax_path = NULL, *pax_link = NULL;
    uint64_t pax_size = 0;
    bool have_pax_size = false;
    Xattr *xa = NULL;
    int nxa = 0, rc = 0;
    char *buf = malloc(1 << 20);
    for (;;) {
        if (read_full(fd, hdr, 512)) { rc = -EIO; break; }
        done += 512;
        if (!hdr[0]) break;                                  // end of archive
        char type = hdr[156];
        uint64_t size = have_pax_size ? pax_size : octal(hdr + 124, 12);
        uint64_t padded = (size + 511) & ~511ull;
        if (type == 'x' || type == 'g') {                    // pax records: "LEN key=value\n"
            char *rec = malloc(size + 1);
            if (read_full(fd, rec, size)) { free(rec); rc = -EIO; break; }
            rec[size] = 0;
            if (padded > size) lseek(fd, (off_t)(padded - size), SEEK_CUR);
            done += padded;
            if (type == 'x') {
                for (char *p = rec; p < rec + size;) {
                    char *sp = strchr(p, ' ');
                    long len = strtol(p, NULL, 10);
                    if (!sp || len <= 0 || p + len > rec + size) break;
                    char *key = sp + 1, *eq = memchr(key, '=', (size_t)(p + len - key));
                    if (eq) {
                        size_t vlen = (size_t)(p + len - 1 - (eq + 1));   // drop the newline
                        *eq = 0;
                        if (!strcmp(key, "path")) pax_path = strndup(eq + 1, vlen);
                        else if (!strcmp(key, "linkpath")) pax_link = strndup(eq + 1, vlen);
                        else if (!strcmp(key, "size")) { pax_size = strtoull(eq + 1, NULL, 10); have_pax_size = true; }
                        else if (!strncmp(key, "SCHILY.xattr.", 13)) {
                            xa = realloc(xa, (size_t)(nxa + 1) * sizeof *xa);
                            xa[nxa++] = (Xattr){ strdup(key + 13), memcpy(malloc(vlen + 1), eq + 1, vlen), vlen };
                        }
                    }
                    p += len;
                }
            }
            free(rec);
            continue;
        }
        char name[512];
        if (pax_path) snprintf(name, sizeof name, "%s", pax_path);
        else if (hdr[345]) snprintf(name, sizeof name, "%.155s/%.100s", hdr + 345, hdr);
        else snprintf(name, sizeof name, "%.100s", hdr);
        if (pax_link) snprintf(lnk, sizeof lnk, "%s", pax_link);
        else { memcpy(lnk, hdr + 157, 100); lnk[100] = 0; }
        snprintf(path, sizeof path, "%s/%s", tmp, name);
        size_t pl = strlen(path);
        while (pl > 1 && path[pl - 1] == '/') path[--pl] = 0;
        mkdirs(path);
        mode_t mode = (mode_t)octal(hdr + 100, 8) & 07777;
        int err = 0;
        if (type == '5') {
            if (mkdir(path, mode | 0700) && errno != EEXIST) err = errno;
        } else if (type == '2') {
            unlink(path);
            if (symlink(lnk, path)) err = errno;
        } else if (type == '1') {
            char target[2048];
            snprintf(target, sizeof target, "%s/%s", tmp, lnk);
            unlink(path);
            if (link(target, path)) err = errno;
        } else if (type == '0' || type == 0) {
            int out = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode | 0600);
            if (out < 0) err = errno;
            for (uint64_t left = size; left;) {
                size_t k = left < (1 << 20) ? (size_t)left : (1 << 20);
                if (read_full(fd, buf, k)) { err = EIO; break; }
                if (out >= 0 && write(out, buf, k) != (ssize_t)k) err = errno ? errno : EIO;
                left -= k;
            }
            if (out >= 0) close(out);
            if (padded > size) lseek(fd, (off_t)(padded - size), SEEK_CUR);
            done += padded;
            size = 0;
        }
        if (size) { lseek(fd, (off_t)padded, SEEK_CUR); done += padded; }   // anything else: skip its data
        for (int i = 0; i < nxa && !err && type != '2'; i++)
            if (setxattr(path, xa[i].name, xa[i].val, xa[i].len, 0, XATTR_NOFOLLOW)) fprintf(stderr, "onirun: install: xattr %s on %s: %s\n", xa[i].name, name, strerror(errno));
        if (err) fprintf(stderr, "onirun: install: %s: %s\n", name, strerror(err));
        for (int i = 0; i < nxa; i++) { free(xa[i].name); free(xa[i].val); }
        nxa = 0;
        free(pax_path); free(pax_link);
        pax_path = pax_link = NULL;
        have_pax_size = false;
        if (progress && done - last > (64u << 20)) { progress(done, (uint64_t)tst.st_size, opaque); last = done; }
    }
    free(buf);
    free(xa);
    close(fd);
    if (rc) return rc;
    if (rename(tmp, dest)) return -errno;
    if (progress) progress((uint64_t)tst.st_size, (uint64_t)tst.st_size, opaque);
    return 0;
}
