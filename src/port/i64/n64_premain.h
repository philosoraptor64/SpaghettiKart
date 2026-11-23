/* n64_premain.h: a private area for the C++ blocks a game module makes before main, for i64 game modules
 * (header-only C/C++; copy it next to n64_game.h in each game's repo, like n64_bigblock.h).
 *
 * Why: a module that runs again in the same process (re-entry) copies its globals at the first start and puts
 * them back after each stop. Globals built before main (tables, maps, strings) own heap blocks, so the module
 * keeps a copy of those blocks too and writes them back with the globals. But code outside the module (libc++'s
 * own compiled functions, e.g. a std::string growing) can free such a block with free(), and the system allocator
 * then hands the memory to someone else. Writing the copy back overwrote that someone (on the iPhone: the process's
 * environment list; i64 crashed right after Mario Kart stopped, 2026-11-10 and again 2026-11-23). Checking
 * malloc_size() before writing back is not enough: the new owner often has the same size.
 *
 * Blocks from here live in a registered malloc zone of their own, a bump area whose free() does nothing. free(),
 * realloc() and malloc_size() from any code find the zone, so a block from here is never reused for anything
 * else, and writing it back is always safe. A pre-main heap is small (Smash 51 KB, Mario Kart under 1 MB).
 *
 *     before the pristine copy:  void* p = n64_premain_alloc(n);  if (!p) p = malloc(n);   (record p as before)
 *     afterwards:                n64_premain_seal();   (the area hands out nothing more; free() still does nothing)
 *     at restore:                n64_premain_owns(p) -> always safe to write back; else check as before
 *
 * The area's state must not be part of what the module copies back: put it in a kept section with
 * #define N64_PREMAIN_KEEP __attribute__((section("__DATA,__yourkeep"))) before including (default: none, which is
 * also fine, since nothing in it changes after the seal).
 */
#ifndef N64_PREMAIN_H
#define N64_PREMAIN_H

#include <malloc/malloc.h>
#include <os/lock.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <stdio.h>

#ifndef N64_PREMAIN_KEEP
#define N64_PREMAIN_KEEP
#endif

#define N64_PREMAIN_RESERVE ((size_t) 64 << 20) /* address space only; pages are used as blocks are made */
#define N64_PREMAIN_MAGIC 0x6E36347072656DULL   /* 'n64prem' */

typedef struct {
    uint64_t tag;  /* N64_PREMAIN_MAGIC ^ the block's address */
    uint64_t size; /* bytes asked for */
} n64_premain_header;

typedef struct {
    malloc_zone_t zone; /* first: the zone pointer is the state pointer */
    malloc_introspection_t introspect;
    char* base;
    char* next;
    char* end;
    int sealed;
    int registered;
    size_t blocks, bytes;
    os_unfair_lock lock;
} n64_premain_state;

static N64_PREMAIN_KEEP n64_premain_state n64_premain_s;

static inline int n64_premain_owns(const void* p) {
    const n64_premain_state* s = &n64_premain_s;
    if (!s->base || (const char*) p < s->base + sizeof(n64_premain_header) || (const char*) p >= s->next ||
        ((uintptr_t) p & 15) != 0) {
        return 0;
    }
    const n64_premain_header* h = (const n64_premain_header*) p - 1;
    return h->tag == (N64_PREMAIN_MAGIC ^ (uint64_t) (uintptr_t) p);
}

static size_t n64_premain_z_size(malloc_zone_t* z, const void* p) {
    (void) z;
    return n64_premain_owns(p) ? (size_t) (((const n64_premain_header*) p - 1)->size) : 0;
}
static void* n64_premain_bump(size_t n, size_t align) {
    n64_premain_state* s = &n64_premain_s;
    if (align < 16) align = 16;
    void* out = NULL;
    os_unfair_lock_lock(&s->lock);
    if (!s->sealed && s->base) {
        uintptr_t start = ((uintptr_t) s->next + sizeof(n64_premain_header) + align - 1) & ~(uintptr_t) (align - 1);
        const size_t span = (n + 15) & ~(size_t) 15;
        if (start + span <= (uintptr_t) s->end) {
            n64_premain_header* h = (n64_premain_header*) start - 1;
            h->tag = N64_PREMAIN_MAGIC ^ (uint64_t) start;
            h->size = n;
            s->next = (char*) (start + span);
            s->blocks++;
            s->bytes += n;
            out = (void*) start;
        }
    }
    os_unfair_lock_unlock(&s->lock);
    return out; /* fresh pages: already zero */
}
static void* n64_premain_z_malloc(malloc_zone_t* z, size_t n) {
    (void) z;
    return n64_premain_bump(n ? n : 1, 16);
}
static void* n64_premain_z_calloc(malloc_zone_t* z, size_t k, size_t n) {
    (void) z;
    if (n && k > SIZE_MAX / n) return NULL;
    return n64_premain_bump(k * n ? k * n : 1, 16);
}
static void* n64_premain_z_valloc(malloc_zone_t* z, size_t n) {
    (void) z;
    return n64_premain_bump(n ? n : 1, 16384);
}
static void* n64_premain_z_memalign(malloc_zone_t* z, size_t a, size_t n) {
    (void) z;
    return n64_premain_bump(n ? n : 1, a);
}
static void n64_premain_z_free(malloc_zone_t* z, void* p) {
    (void) z;
    (void) p; /* never reused: the module may write its copy back here */
}
static void n64_premain_z_free_definite(malloc_zone_t* z, void* p, size_t n) {
    (void) z;
    (void) p;
    (void) n;
}
static void* n64_premain_z_realloc(malloc_zone_t* z, void* p, size_t n) {
    (void) z;
    void* q = n64_premain_bump(n ? n : 1, 16); /* NULL once sealed: then the normal heap */
    if (!q) q = malloc(n ? n : 1);
    if (q && p) {
        const size_t old = n64_premain_z_size(z, p);
        memcpy(q, p, old < n ? old : n);
    }
    return q; /* the old block stays as it is */
}
static void n64_premain_z_destroy(malloc_zone_t* z) {
    (void) z;
}
static size_t n64_premain_z_pressure(malloc_zone_t* z, size_t goal) {
    (void) z;
    (void) goal;
    return 0;
}
static boolean_t n64_premain_z_claimed(malloc_zone_t* z, void* p) {
    (void) z;
    const n64_premain_state* s = &n64_premain_s;
    return s->base && (char*) p >= s->base && (char*) p < s->end;
}
static kern_return_t n64_premain_i_enumerator(task_t t, void* c, unsigned m, vm_address_t a, memory_reader_t r,
                                              vm_range_recorder_t rec) {
    (void) t; (void) c; (void) m; (void) a; (void) r; (void) rec;
    return KERN_SUCCESS;
}
static size_t n64_premain_i_good_size(malloc_zone_t* z, size_t n) {
    (void) z;
    return (n + 15) & ~(size_t) 15;
}
static boolean_t n64_premain_i_check(malloc_zone_t* z) {
    (void) z;
    return 1;
}
static void n64_premain_i_print(malloc_zone_t* z, boolean_t v) {
    (void) z;
    (void) v;
}
static void n64_premain_i_log(malloc_zone_t* z, void* a) {
    (void) z;
    (void) a;
}
static void n64_premain_i_lock(malloc_zone_t* z) {
    (void) z;
    os_unfair_lock_lock(&n64_premain_s.lock);
}
static void n64_premain_i_unlock(malloc_zone_t* z) {
    (void) z;
    os_unfair_lock_unlock(&n64_premain_s.lock);
}
static void n64_premain_i_reinit(malloc_zone_t* z) {
    (void) z;
    n64_premain_s.lock = OS_UNFAIR_LOCK_INIT;
}
static void n64_premain_i_stats(malloc_zone_t* z, malloc_statistics_t* st) {
    (void) z;
    st->blocks_in_use = (unsigned) n64_premain_s.blocks;
    st->size_in_use = st->max_size_in_use = n64_premain_s.bytes;
    st->size_allocated = (size_t) (n64_premain_s.next - n64_premain_s.base);
}
static boolean_t n64_premain_i_locked(malloc_zone_t* z) {
    (void) z;
    return 0;
}

/* Sets up and registers the zone (once). 0 if the system refused the address space. */
static inline int n64_premain_init(const char* name) {
    n64_premain_state* s = &n64_premain_s;
    if (s->registered) return 1;
    void* m = mmap(NULL, N64_PREMAIN_RESERVE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (m == MAP_FAILED) return 0;
    s->base = s->next = (char*) m;
    s->end = (char*) m + N64_PREMAIN_RESERVE;
    s->lock = OS_UNFAIR_LOCK_INIT;
    malloc_introspection_t* in = &s->introspect;
    memset(in, 0, sizeof(*in));
    in->enumerator = n64_premain_i_enumerator;
    in->good_size = n64_premain_i_good_size;
    in->check = n64_premain_i_check;
    in->print = n64_premain_i_print;
    in->log = n64_premain_i_log;
    in->force_lock = n64_premain_i_lock;
    in->force_unlock = n64_premain_i_unlock;
    in->statistics = n64_premain_i_stats;
    in->zone_locked = n64_premain_i_locked;
    in->reinit_lock = n64_premain_i_reinit;
    malloc_zone_t* z = &s->zone;
    memset(z, 0, sizeof(*z));
    z->size = n64_premain_z_size;
    z->malloc = n64_premain_z_malloc;
    z->calloc = n64_premain_z_calloc;
    z->valloc = n64_premain_z_valloc;
    z->free = n64_premain_z_free;
    z->realloc = n64_premain_z_realloc;
    z->destroy = n64_premain_z_destroy;
    z->zone_name = name;
    z->introspect = in;
    z->version = 10;
    z->memalign = n64_premain_z_memalign;
    z->free_definite_size = n64_premain_z_free_definite;
    z->pressure_relief = n64_premain_z_pressure;
    z->claimed_address = n64_premain_z_claimed;
    malloc_zone_register(z);
    s->registered = 1;
    return 1;
}

/* A block for code running before the pristine copy (NULL: use the normal heap). */
static inline void* n64_premain_alloc(size_t n, size_t align) {
    if (n64_premain_s.sealed) return NULL;
    if (!n64_premain_s.registered && !n64_premain_init("n64 pre-main")) return NULL;
    return n64_premain_bump(n ? n : 1, align);
}

/* The pristine copy is taken: nothing more from the area. */
static inline void n64_premain_seal(void) {
    os_unfair_lock_lock(&n64_premain_s.lock);
    n64_premain_s.sealed = 1;
    os_unfair_lock_unlock(&n64_premain_s.lock);
}

#endif
