// ELF loading and process start, the way Linux execve() sets up an arm64 process.
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "onx.h"

extern char g_exe_path[1024];

typedef struct {
    unsigned char ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} Ehdr;
typedef struct {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
} Phdr;
enum { PT_LOAD = 1, PT_INTERP = 3, PT_PHDR = 6 };

typedef struct {
    uint64_t bias, entry, phdr;
    int phnum;
    char interp[256];
} Loaded;

static int map_elf(const char *guest_path, Loaded *out) {
    char host[4096];
    int e = host_path(guest_path, -100, host, sizeof host, true);
    if (e) return e;
    int fd = open(host, O_RDONLY);
    if (fd < 0) return -ENOENT;
    Ehdr eh;
    if (pread(fd, &eh, sizeof eh, 0) != sizeof eh || memcmp(eh.ident, "\x7f" "ELF", 4) ||
        eh.ident[4] != 2 || eh.machine != 183) { close(fd); return -ENOEXEC; }
    Phdr ph[64];
    if (eh.phnum > 64 || pread(fd, ph, eh.phnum * sizeof(Phdr), (off_t)eh.phoff) != (ssize_t)(eh.phnum * sizeof(Phdr))) {
        close(fd);
        return -ENOEXEC;
    }
    uint64_t pg = (uint64_t)getpagesize();
    uint64_t lo = ~0ull, hi = 0;
    memset(out, 0, sizeof *out);
    for (int i = 0; i < eh.phnum; i++) {
        if (ph[i].type == PT_LOAD) {
            if (ph[i].vaddr < lo) lo = ph[i].vaddr;
            if (ph[i].vaddr + ph[i].memsz > hi) hi = ph[i].vaddr + ph[i].memsz;
        } else if (ph[i].type == PT_INTERP && ph[i].filesz < sizeof out->interp) {
            pread(fd, out->interp, ph[i].filesz, (off_t)ph[i].offset);
        }
    }
    lo &= ~(pg - 1);
    hi = (hi + pg - 1) & ~(pg - 1);
    uint8_t *base;
    if (eh.type == 3) {       // ET_DYN
        base = mmap(NULL, hi - lo, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (base == MAP_FAILED) { close(fd); return -ENOMEM; }
        out->bias = (uint64_t)(uintptr_t)base - lo;
    } else {
        out->bias = 0;
    }
    for (int i = 0; i < eh.phnum; i++) {
        Phdr *p = &ph[i];
        if (p->type != PT_LOAD) continue;
        uint64_t start = out->bias + p->vaddr;
        uint64_t page = start & ~(pg - 1);
        uint64_t file_end = start + p->filesz, mem_end = start + p->memsz;
        int prot = ((p->flags & 4) || (p->flags & 1) ? PROT_READ : 0) | ((p->flags & 2) ? PROT_WRITE : 0);
        if (p->filesz) {
            uint64_t off = p->offset - (start - page);
            if (off % pg) { close(fd); return -ENOEXEC; }
            uint64_t len = ((file_end + pg - 1) & ~(pg - 1)) - page;
            if (mmap((void *)(uintptr_t)page, len, prot | PROT_WRITE, MAP_PRIVATE | MAP_FIXED, fd, (off_t)off) == MAP_FAILED) {
                close(fd);
                return -ENOMEM;
            }
            sys_note_mapping(page, len, off, guest_path);
        }
        if (mem_end > file_end) {
            uint64_t zero_end = (file_end + pg - 1) & ~(pg - 1);
            if (p->filesz && zero_end > file_end) memset((void *)(uintptr_t)file_end, 0, (zero_end < mem_end ? zero_end : mem_end) - file_end);
            uint64_t anon = p->filesz ? zero_end : page;
            uint64_t anon_end = (mem_end + pg - 1) & ~(pg - 1);
            if (anon_end > anon)
                mmap((void *)(uintptr_t)anon, anon_end - anon, prot | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
        }
        mprotect((void *)(uintptr_t)page, ((mem_end + pg - 1) & ~(pg - 1)) - page, prot);
    }
    out->entry = out->bias + eh.entry;
    out->phnum = eh.phnum;
    out->phdr = 0;
    for (int i = 0; i < eh.phnum; i++)
        if (ph[i].type == PT_PHDR) out->phdr = out->bias + ph[i].vaddr;
    if (!out->phdr) out->phdr = out->bias + lo + eh.phoff;
    close(fd);
    return 0;
}

// HWCAP bits for what Apple cores run natively (no SVE/SME; ID registers aren't exposed).
#define HWCAP_ALL ((1u << 0) | (1u << 1) | (1u << 3) | (1u << 4) | (1u << 5) | (1u << 6) | (1u << 7) | \
                   (1u << 8) | (1u << 9) | (1u << 10) | (1u << 12) | (1u << 13) | (1u << 14) | (1u << 15) | \
                   (1u << 16) | (1u << 17) | (1u << 20) | (1u << 21) | (1u << 23) | (1u << 24) | (1u << 25) | \
                   (1u << 26) | (1u << 27) | (1u << 29))
#define HWCAP2_ALL ((1u << 0) | (1u << 7) | (1u << 8))

int load_exec(const char *guest_path, char *const argv[], char *const envp[], ExecImage *img) {
    Loaded exe, interp;
    int e = map_elf(guest_path, &exe);
    if (e) return e;
    uint64_t entry = exe.entry, interp_base = 0;
    if (exe.interp[0]) {
        e = map_elf(exe.interp, &interp);
        if (e) return e;
        entry = interp.entry;
        interp_base = interp.bias;
    }
    snprintf(g_exe_path, sizeof g_exe_path, "%s", guest_path);
    LOGV("onirun: exec %s entry %llx interp %s\n", guest_path, (unsigned long long)exe.entry, exe.interp);

    // Stack: 8 MiB, Linux layout (argc, argv, envp, auxv, then strings at the top).
    size_t ssz = 8 << 20;
    uint8_t *stk = mmap(NULL, ssz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (stk == MAP_FAILED) return -ENOMEM;
    uint8_t *top = stk + ssz;
    int argc = 0, envc = 0;
    while (argv[argc]) argc++;
    while (envp[envc]) envc++;
    uint64_t *argp = calloc(argc + 1, 8), *envpp = calloc(envc + 1, 8);
    #define PUSH_STR(s) ({ size_t _n = strlen(s) + 1; top -= _n; memcpy(top, s, _n); (uint64_t)(uintptr_t)top; })
    uint64_t execfn = PUSH_STR(guest_path);
    uint64_t platform = PUSH_STR("aarch64");
    for (int i = envc - 1; i >= 0; i--) envpp[i] = PUSH_STR(envp[i]);
    for (int i = argc - 1; i >= 0; i--) argp[i] = PUSH_STR(argv[i]);
    top = (uint8_t *)((uintptr_t)top & ~15ul);
    top -= 16;
    arc4random_buf(top, 16);
    uint64_t random = (uint64_t)(uintptr_t)top;
    uint64_t aux[][2] = {
        {3, exe.phdr}, {4, 56}, {5, (uint64_t)exe.phnum}, {6, (uint64_t)getpagesize()},
        {7, interp_base}, {8, 0}, {9, exe.entry}, {11, 2000}, {12, 2000}, {13, 2000}, {14, 2000},
        {17, 100}, {16, HWCAP_ALL}, {26, HWCAP2_ALL}, {23, 0}, {25, random}, {31, execfn},
        {15, platform}, {51, 5120}, {0, 0},
    };
    size_t naux = sizeof aux / sizeof aux[0];
    size_t words = 1 + argc + 1 + envc + 1 + naux * 2;
    uint64_t *sp = (uint64_t *)(((uintptr_t)top - words * 8) & ~15ul);
    uint64_t *w = sp;
    *w++ = argc;
    for (int i = 0; i < argc; i++) *w++ = argp[i];
    *w++ = 0;
    for (int i = 0; i < envc; i++) *w++ = envpp[i];
    *w++ = 0;
    for (size_t i = 0; i < naux; i++) { *w++ = aux[i][0]; *w++ = aux[i][1]; }
    free(argp);
    free(envpp);
    img->entry = entry;
    img->sp = (uint64_t)(uintptr_t)sp;
    return 0;
}
