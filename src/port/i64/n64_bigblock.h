/* n64_bigblock.h: big C++ blocks straight from the system, for i64 game
 * modules (header-only; copy it next to n64_game.h in each game's repo).
 *
 * Why: on macOS 27 / iOS 27 the system allocator keeps every freed large
 * block (its "large cache") until the process exits; malloc_zone_pressure_
 * relief() returns nothing. Mario Kart's ~400 MB of resource buffers stayed
 * in i64's footprint after the game stopped (Mac 245 MB, iPhone 150 MB after
 * each run). Blocks from here go back to the system the moment they're freed.
 *
 * Use it in a module's own operator new/delete (or allocation hook):
 *     void* p = n64_big_alloc(n);  if (!p) p = malloc(n);
 *     ...
 *     if (!n64_big_free(p)) free(p);
 * n64_big_free() must be asked first for EVERY pointer the allocator frees.
 * Only for operator new/delete: a big block must never reach free()/realloc().
 * Alignment: 16 bytes (operator new's default); aligned new keeps its own path.
 */
#ifndef N64_BIGBLOCK_H
#define N64_BIGBLOCK_H

#include <stddef.h>
#include <stdint.h>
#include <sys/mman.h>
#include <mach/vm_page_size.h>

#define N64_BIG_MIN ((size_t) 512 << 10) /* smaller blocks stay with malloc */

typedef struct {
    uint64_t tag;  /* N64_BIG_MAGIC ^ the header's own address */
    uint64_t span; /* bytes mapped */
} n64_big_header;
#define N64_BIG_MAGIC 0x6E363462696762ULL /* 'n64bigb' */

/* NULL if n is below N64_BIG_MIN or the system has no memory (then use malloc). */
static inline void* n64_big_alloc(size_t n) {
    if (n < N64_BIG_MIN) {
        return NULL;
    }
    const size_t page = vm_page_size;
    const size_t span = (n + sizeof(n64_big_header) + page - 1) & ~(page - 1);
    void* m = mmap(NULL, span, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (m == MAP_FAILED) {
        return NULL;
    }
    n64_big_header* h = (n64_big_header*) m;
    h->tag = N64_BIG_MAGIC ^ (uint64_t) (uintptr_t) h;
    h->span = span;
    return h + 1; /* page start + 16 */
}

/* 1 if p came from n64_big_alloc (now unmapped), 0 otherwise (free it as before). Reading the 16 bytes
 * before p is safe for any heap pointer at page offset 16: they're in the same page. */
static inline int n64_big_free(void* p) {
    if (!p || ((uintptr_t) p & (vm_page_size - 1)) != sizeof(n64_big_header)) {
        return 0;
    }
    n64_big_header* h = (n64_big_header*) p - 1;
    if (h->tag != (N64_BIG_MAGIC ^ (uint64_t) (uintptr_t) h)) {
        return 0;
    }
    h->tag = 0;
    munmap(h, (size_t) h->span);
    return 1;
}

#endif
