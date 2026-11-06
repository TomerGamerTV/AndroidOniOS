// Guest address spaces. Each process gets a window of host address space reserved PROT_NONE, and
// guest mappings are host mappings placed at base + guest address. fork() copies a window with
// mach_vm_remap: private memory becomes copy-on-write, MAP_SHARED memory stays shared.
#include <errno.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "sys.h"

#define RESV_ALIGN (1ull << 20)

static int host_prot(int p) {
    int r = p & (PROT_READ | PROT_WRITE);
    if (p & PROT_EXEC) r |= PROT_READ;   // guest code is read by the translator, never run in place
    return r;
}

// ---- the sorted mapping list ----
static void vma_reserve(Mm *m, int extra) {
    if (m->n + extra <= m->cap) return;
    m->cap = (m->n + extra) * 2;
    m->v = realloc(m->v, (size_t)m->cap * sizeof(Vma));
}

// Removes [s, e) from the list, trimming or splitting mappings that straddle it.
static void vma_cut(Mm *m, uint64_t s, uint64_t e) {
    vma_reserve(m, 1);
    for (int i = 0; i < m->n; i++) {
        Vma *v = &m->v[i];
        if (v->end <= s || v->start >= e) continue;
        if (v->start < s && v->end > e) {
            Vma tail = *v;
            tail.off += e - v->start;
            tail.start = e;
            tail.path = v->path ? strdup(v->path) : NULL;
            v->end = s;
            memmove(&m->v[i + 2], &m->v[i + 1], (size_t)(m->n - i - 1) * sizeof(Vma));
            m->v[i + 1] = tail;
            m->n++;
            return;
        }
        if (v->start < s) { v->end = s; continue; }
        if (v->end > e) { v->off += e - v->start; v->start = e; continue; }
        free(v->path);
        memmove(v, v + 1, (size_t)(m->n - i - 1) * sizeof(Vma));
        m->n--;
        i--;
    }
}

static void vma_insert(Mm *m, Vma nv) {
    vma_reserve(m, 1);
    int i = 0;
    while (i < m->n && m->v[i].start < nv.start) i++;
    memmove(&m->v[i + 1], &m->v[i], (size_t)(m->n - i) * sizeof(Vma));
    m->v[i] = nv;
    m->n++;
}

// Splits the mapping containing x (if any) so that x is a boundary.
static void vma_split_at(Mm *m, uint64_t x) {
    for (int i = 0; i < m->n; i++) {
        Vma *v = &m->v[i];
        if (v->start < x && v->end > x) {
            vma_reserve(m, 1);
            v = &m->v[i];
            Vma tail = *v;
            tail.off += x - v->start;
            tail.start = x;
            tail.path = v->path ? strdup(v->path) : NULL;
            v->end = x;
            memmove(&m->v[i + 2], &m->v[i + 1], (size_t)(m->n - i - 1) * sizeof(Vma));
            m->v[i + 1] = tail;
            m->n++;
            return;
        }
    }
}

static bool vma_free(Mm *m, uint64_t s, uint64_t e) {
    for (int i = 0; i < m->n; i++)
        if (m->v[i].start < e && m->v[i].end > s) return false;
    return true;
}

static Vma *vma_find(Mm *m, uint64_t a) {
    for (int i = 0; i < m->n; i++)
        if (a >= m->v[i].start && a < m->v[i].end) return &m->v[i];
    return NULL;
}

// Highest free range of len bytes below MMAP_TOP, or 0.
static uint64_t vma_gap(Mm *m, uint64_t len) {
    uint64_t top = MMAP_TOP;
    for (int i = m->n - 1; i >= -1; i--) {
        if (i >= 0 && m->v[i].start >= top) continue;
        if (i >= 0 && m->v[i].end > top) { top = m->v[i].start; continue; }
        uint64_t lo = i >= 0 ? m->v[i].end : MMAP_MIN;
        if (top >= lo && top - lo >= len) return top - len;
        if (i >= 0) top = m->v[i].start;
    }
    return 0;
}

// ---- lifetime ----
Mm *mm_new(void) {
    size_t len = IB_SIZE + WIN_SIZE + RESV_ALIGN;
    uint8_t *r = mmap(NULL, len, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (r == MAP_FAILED) return NULL;
    uint8_t *base = (uint8_t *)(((uintptr_t)r + IB_SIZE + RESV_ALIGN - 1) & ~(uintptr_t)(RESV_ALIGN - 1));
    if (mprotect(base - IB_SIZE, IB_SIZE, PROT_READ | PROT_WRITE)) { munmap(r, len); return NULL; }
    Mm *m = calloc(1, sizeof *m);
    m->base = base;
    m->resv = r;
    m->resv_len = len;
    m->refs = 1;
    pthread_mutex_init(&m->lock, NULL);
    m->tr = tr_space_new(base);
    // The signal-return trampoline ("mov x8, #rt_sigreturn; svc #0"), like the kernel's vDSO.
    if (mm_map(m, VDSO_ADDR, PG, PROT_READ | PROT_WRITE, LX_MAP_FIXED | LX_MAP_ANON, -1, 0, "[vdso]") < 0) {
        mm_put(m);
        return NULL;
    }
    uint32_t *t = (uint32_t *)(void *)(base + VDSO_ADDR);
    t[0] = 0xd2801168;
    t[1] = 0xd4000001;
    mm_protect(m, VDSO_ADDR, PG, PROT_READ | PROT_EXEC);
    return m;
}

Mm *mm_fork(Mm *p) {
    Mm *c = mm_new();
    if (!c) return NULL;
    vma_cut(c, 0, WIN_SIZE);
    pthread_mutex_lock(&p->lock);
    for (int i = 0; i < p->n; i++) {
        Vma v = p->v[i];
        mach_vm_address_t dst = (mach_vm_address_t)(uintptr_t)(c->base + v.start);
        vm_prot_t cur, max;
        kern_return_t kr = mach_vm_remap(mach_task_self(), &dst, v.end - v.start, 0,
                                         VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, mach_task_self(),
                                         (mach_vm_address_t)(uintptr_t)(p->base + v.start), !v.shared,
                                         &cur, &max, VM_INHERIT_DEFAULT);
        if (kr != KERN_SUCCESS) {
            pthread_mutex_unlock(&p->lock);
            fprintf(stderr, "onirun: fork: cannot copy %llx-%llx (kr %d)\n", (unsigned long long)v.start,
                    (unsigned long long)v.end, kr);
            mm_put(c);
            return NULL;
        }
        if (v.path) v.path = strdup(v.path);
        vma_insert(c, v);
    }
    pthread_mutex_unlock(&p->lock);
    return c;
}

void mm_get(Mm *m) { __atomic_add_fetch(&m->refs, 1, __ATOMIC_SEQ_CST); }

void mm_put(Mm *m) {
    if (__atomic_sub_fetch(&m->refs, 1, __ATOMIC_SEQ_CST)) return;
    tr_space_free(m->tr);
    munmap(m->resv, m->resv_len);
    for (int i = 0; i < m->n; i++) free(m->v[i].path);
    free(m->v);
    pthread_mutex_destroy(&m->lock);
    free(m);
}

// ---- mapping calls ----
int64_t mm_map(Mm *m, uint64_t addr, uint64_t len, int prot, int fl, int hfd, uint64_t off, const char *path) {
    len = PG_UP(len);
    if (!len) return -EINVAL;
    bool fixed = fl & LX_MAP_FIXED, noreplace = fl & LX_MAP_FIXED_NOREPLACE;
    bool shared = fl & LX_MAP_SHARED, anon = fl & LX_MAP_ANON;
    int64_t r;
    pthread_mutex_lock(&m->lock);
    if (fixed || noreplace) {
        if (addr & (PG - 1)) { r = -EINVAL; goto out; }
        if (addr + len > WIN_SIZE || addr + len < addr) { r = -ENOMEM; goto out; }
        if (!fixed && !vma_free(m, addr, addr + len)) { r = -EEXIST; goto out; }
    } else {
        addr &= ~(PG - 1);
        if (!(addr >= MMAP_MIN && addr + len <= WIN_SIZE && vma_free(m, addr, addr + len)))
            addr = vma_gap(m, len);
        if (!addr) { r = -ENOMEM; goto out; }
    }
    uint8_t *h = m->base + addr;
    int hp = host_prot(prot);
    void *res;
    if (anon) {
        res = mmap(h, len, hp, MAP_FIXED | (shared ? MAP_SHARED : MAP_PRIVATE) | MAP_ANON, -1, 0);
    } else if (off % PG) {
        // Offset not aligned to the 16 KiB host page: copy instead of mapping the file.
        res = mmap(h, len, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0);
        if (res != MAP_FAILED) {
            for (uint64_t done = 0; done < len;) {
                ssize_t got = pread(hfd, h + done, len - done, (off_t)(off + done));
                if (got < 0 && errno == EINTR) continue;
                if (got <= 0) break;
                done += (uint64_t)got;
            }
            mprotect(h, len, hp);
        }
        shared = false;
    } else {
        res = mmap(h, len, hp, MAP_FIXED | (shared ? MAP_SHARED : MAP_PRIVATE), hfd, (off_t)off);
    }
    if (res == MAP_FAILED) {
        r = -lx_errno(errno);
        if (vma_free(m, addr, addr + len)) mmap(h, len, PROT_NONE, MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0);
        goto out;
    }
    vma_cut(m, addr, addr + len);
    vma_insert(m, (Vma){ addr, addr + len, anon ? 0 : off, prot, shared, !anon, path ? strdup(path) : NULL });
    r = (int64_t)addr;
out:
    pthread_mutex_unlock(&m->lock);
    return r;
}

int mm_unmap(Mm *m, uint64_t addr, uint64_t len) {
    len = PG_UP(len);
    if ((addr & (PG - 1)) || !len || addr + len > WIN_SIZE || addr + len < addr) return -EINVAL;
    pthread_mutex_lock(&m->lock);
    mmap(m->base + addr, len, PROT_NONE, MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0);
    vma_cut(m, addr, addr + len);
    pthread_mutex_unlock(&m->lock);
    return 0;
}

int mm_protect(Mm *m, uint64_t addr, uint64_t len, int prot) {
    len = PG_UP(len);
    if ((addr & (PG - 1)) || addr + len > WIN_SIZE || addr + len < addr) return -EINVAL;
    pthread_mutex_lock(&m->lock);
    int r = mprotect(m->base + addr, len, host_prot(prot)) ? -lx_errno(errno) : 0;
    if (!r) {
        vma_split_at(m, addr);
        vma_split_at(m, addr + len);
        for (int i = 0; i < m->n; i++)
            if (m->v[i].start >= addr && m->v[i].end <= addr + len) m->v[i].prot = prot;
    }
    pthread_mutex_unlock(&m->lock);
    return r;
}

// MADV_DONTNEED: Linux hands back zero pages for private anonymous memory; Darwin would keep the
// data, so map fresh pages over it.
int mm_dontneed(Mm *m, uint64_t addr, uint64_t len) {
    len = PG_UP(len);
    pthread_mutex_lock(&m->lock);
    for (int i = 0; i < m->n; i++) {
        Vma *v = &m->v[i];
        if (v->shared || v->file || v->end <= addr || v->start >= addr + len) continue;
        uint64_t s = v->start > addr ? v->start : addr, e = v->end < addr + len ? v->end : addr + len;
        mmap(m->base + s, e - s, host_prot(v->prot), MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0);
    }
    pthread_mutex_unlock(&m->lock);
    return 0;
}

// Moves [old, old+oldsz) to [dst, dst+newsz) without copying (the pages themselves are remapped),
// zero-filling any growth. m->lock held; dst..dst+newsz must not overlap the old range.
static int move_locked(Mm *m, uint64_t old, uint64_t oldsz, uint64_t newsz, uint64_t dst, const Vma *attrs) {
    uint64_t keep = oldsz < newsz ? oldsz : newsz;
    mach_vm_address_t d = (mach_vm_address_t)(uintptr_t)(m->base + dst);
    vm_prot_t cur, max;
    if (mach_vm_remap(mach_task_self(), &d, keep, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, mach_task_self(),
                      (mach_vm_address_t)(uintptr_t)(m->base + old), FALSE, &cur, &max, VM_INHERIT_DEFAULT) != KERN_SUCCESS)
        return -ENOMEM;
    Vma *moved = calloc((size_t)m->n + 1, sizeof(Vma));
    int nm = 0;
    for (int i = 0; i < m->n; i++) {
        Vma w = m->v[i];
        if (w.end <= old || w.start >= old + keep) continue;
        uint64_t s = w.start > old ? w.start : old, e = w.end < old + keep ? w.end : old + keep;
        moved[nm++] = (Vma){ dst + (s - old), dst + (e - old), w.off + (s - w.start), w.prot, w.shared, w.file,
                             w.path ? strdup(w.path) : NULL };
    }
    vma_cut(m, dst, dst + newsz);
    for (int i = 0; i < nm; i++) vma_insert(m, moved[i]);
    free(moved);
    if (newsz > keep) {
        mmap(m->base + dst + keep, newsz - keep, host_prot(attrs->prot),
             MAP_FIXED | (attrs->shared ? MAP_SHARED : MAP_PRIVATE) | MAP_ANON, -1, 0);
        vma_insert(m, (Vma){ dst + keep, dst + newsz, 0, attrs->prot, attrs->shared, false, NULL });
    }
    mmap(m->base + old, oldsz, PROT_NONE, MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0);
    vma_cut(m, old, old + oldsz);
    return 0;
}

int64_t mm_remap(Mm *m, uint64_t old, uint64_t oldsz, uint64_t newsz, int flags, uint64_t newaddr) {
    oldsz = PG_UP(oldsz);
    newsz = PG_UP(newsz);
    bool fixed = flags & 2;                                          // MREMAP_FIXED
    if ((old & (PG - 1)) || !newsz || (fixed && !(flags & 1))) return -EINVAL;
    if (fixed && ((newaddr & (PG - 1)) || newaddr + newsz > WIN_SIZE ||
                  (newaddr < old + oldsz && old < newaddr + newsz))) return -EINVAL;
    if (!fixed && newsz <= oldsz) {
        if (newsz < oldsz) mm_unmap(m, old + newsz, oldsz - newsz);
        return (int64_t)old;
    }
    int64_t r;
    pthread_mutex_lock(&m->lock);
    Vma *v = vma_find(m, old);
    if (!v) { r = -EFAULT; goto out; }
    // Linux merges adjacent mappings with identical attributes; here mprotect leaves them split
    // (bionic's atexit table toggles its protection), so accept a contiguous run of such pieces.
    uint64_t need = old + (oldsz < newsz ? oldsz : newsz), reach = v->end;
    for (Vma *w; reach < need && (w = vma_find(m, reach)) && w->start == reach && !w->file && !v->file &&
                 w->prot == v->prot && w->shared == v->shared;) reach = w->end;
    if (reach < need) { r = -EFAULT; goto out; }
    Vma attrs = *v;
    if (fixed) {
        int e = move_locked(m, old, oldsz, newsz, newaddr, &attrs);
        r = e ? e : (int64_t)newaddr;
        goto out;
    }
    if (!attrs.file && old + newsz <= WIN_SIZE && vma_free(m, old + oldsz, old + newsz)) {
        mmap(m->base + old + oldsz, newsz - oldsz, host_prot(attrs.prot),
             MAP_FIXED | (attrs.shared ? MAP_SHARED : MAP_PRIVATE) | MAP_ANON, -1, 0);
        vma_insert(m, (Vma){ old + oldsz, old + newsz, 0, attrs.prot, attrs.shared, false, NULL });
        r = (int64_t)old;
        goto out;
    }
    if (!(flags & 1)) { r = -ENOMEM; goto out; }                     // MREMAP_MAYMOVE
    uint64_t na = vma_gap(m, newsz);
    if (!na) { r = -ENOMEM; goto out; }
    int e = move_locked(m, old, oldsz, newsz, na, &attrs);
    r = e ? e : (int64_t)na;
out:
    pthread_mutex_unlock(&m->lock);
    return r;
}

// Maps host memory (a GPU buffer the host owns) into the window at a free guest address, sharing
// the pages. Returns the guest address or a negative errno.
int64_t mm_map_host(Mm *m, uint64_t addr, uint64_t len, int prot, void *host, const char *path) {
    len = PG_UP(len);
    if (!len || ((uintptr_t)host & (PG - 1))) return -EINVAL;
    int64_t r;
    pthread_mutex_lock(&m->lock);
    addr &= ~(PG - 1);
    if (!(addr >= MMAP_MIN && addr + len <= WIN_SIZE && vma_free(m, addr, addr + len))) addr = vma_gap(m, len);
    if (!addr) { r = -ENOMEM; goto out; }
    mach_vm_address_t dst = (mach_vm_address_t)(uintptr_t)(m->base + addr);
    vm_prot_t cur, max;
    kern_return_t kr = mach_vm_remap(mach_task_self(), &dst, len, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, mach_task_self(),
                                     (mach_vm_address_t)(uintptr_t)host, FALSE, &cur, &max, VM_INHERIT_SHARE);
    if (kr != KERN_SUCCESS) { r = -ENOMEM; goto out; }
    if (mprotect(m->base + addr, len, host_prot(prot))) { r = -lx_errno(errno); goto out; }
    vma_cut(m, addr, addr + len);
    vma_insert(m, (Vma){ addr, addr + len, 0, prot, true, false, path ? strdup(path) : NULL });
    r = (int64_t)addr;
out:
    pthread_mutex_unlock(&m->lock);
    return r;
}

// ---- queries ----
uint64_t mm_exec_end(Mm *m, uint64_t pc) {
    pthread_mutex_lock(&m->lock);
    Vma *v = vma_find(m, pc);
    uint64_t r = (v && (v->prot & PROT_EXEC)) ? v->end : 0;
    pthread_mutex_unlock(&m->lock);
    return r;
}

bool mm_readable(Mm *m, uint64_t addr, uint64_t len) {
    pthread_mutex_lock(&m->lock);
    bool ok = true;
    for (uint64_t a = addr; ok && a < addr + len;) {
        Vma *v = vma_find(m, a);
        if (!v || !(v->prot & PROT_READ)) ok = false;
        else a = v->end;
    }
    pthread_mutex_unlock(&m->lock);
    return ok;
}

void mm_symbolize(Mm *m, uint64_t addr, char *out, size_t n) {
    pthread_mutex_lock(&m->lock);
    Vma *v = vma_find(m, addr);
    if (v && v->path) snprintf(out, n, "%s+0x%llx", v->path, (unsigned long long)(addr - v->start + v->off));
    else snprintf(out, n, "0x%llx", (unsigned long long)addr);
    pthread_mutex_unlock(&m->lock);
}
