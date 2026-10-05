#include <stddef.h>

#include "../tests/fault_alloc.h"

typedef struct rb_pool rb_pool_t;

/* slabs are 4096 bytes */
rb_pool_t *pool_create(size_t obj_size);

/* O(1), from a free list */
void *pool_alloc(rb_pool_t *p);

/* O(1), returns to the free list */
void pool_free(rb_pool_t *p, void *obj);

void pool_stats(const rb_pool_t *p, size_t *slabs, size_t *live, size_t *free_objs);

/* releases all slabs */
void pool_destroy(rb_pool_t *p);

#define SLAB_BYTES ((size_t)4096)

/* Overlays a free slot: the link occupies the slot's first bytes (intrusive). */
struct free_slot {
    struct free_slot *next;
};

/* In-band header: the struct is a map laid over each raw 4096-byte rb_malloc block. */
struct slab {
    struct slab *next;                        /* offset 0: next slab in the pool's list */
    alignas(max_align_t) unsigned char mem[]; /* FAM: slots start at first max_align_t boundary past next */
};

struct rb_pool {
    size_t slot_size;       /* set once in pool_create: max(obj_size, link) rounded to max_align_t */
    size_t per_slab;        /* set once in pool_create: slots that fit after the header (floored) */
    struct slab *slabs;     /* every slab, newest first; walked only by pool_destroy */
    struct free_slot *free; /* LIFO stack of free slots spanning all slabs */
    size_t nslabs;          /* slabs allocated so far */
    size_t live;            /* slots owned by callers */
    size_t nfree;           /* slots on the free list; invariant: live + nfree == nslabs * per_slab */
};

/* Rounds n up to a multiple of a; the bit trick is valid because a is a power of two. */
static size_t round_up(size_t n, size_t a) {
    return (n + a - 1) & ~(a - 1);
}

/* Adds one slab and pushes all of its slots; on failure the pool is unchanged. */
static bool grow(struct rb_pool *p) {
    struct slab *s = rb_malloc(SLAB_BYTES);
    if (s == NULL) {
        return false; /* nothing modified yet, so failure leaves p untouched */
    }
    s->next = p->slabs;
    p->slabs = s; /* pool takes ownership of slab s until pool_destroy */

    /* Pushed last-to-first so slot 0 ends up at the head (ascending addresses on alloc).
     * invariant: slots i+1..per_slab-1 are on the free list, lowest address first */
    for (size_t i = p->per_slab; i-- > 0;) {
        struct free_slot *slot = (void *)&s->mem[i * p->slot_size];
        slot->next = p->free;
        p->free = slot;
    }
    p->nslabs++;
    p->nfree += p->per_slab; /* free count jumps by a whole slab on growth */
    return true;
}

rb_pool_t *pool_create(size_t obj_size) {
    const size_t header = offsetof(struct slab, mem); /* 8 on macOS arm64, 16 on Linux aarch64 */
    if (obj_size == 0 || obj_size > SLAB_BYTES - header) {
        return NULL; /* also guards round_up against size_t overflow */
    }
    size_t need = obj_size > sizeof(struct free_slot) ? obj_size : sizeof(struct free_slot);
    size_t slot_size = round_up(need, alignof(max_align_t)); /* keeps every slot aligned */
    size_t per_slab = (SLAB_BYTES - header) / slot_size;     /* floor: last slot ends <= 4096 */
    if (per_slab == 0) {
        return NULL; /* rounding pushed a single slot past the slab */
    }

    struct rb_pool *p = rb_malloc(sizeof *p);
    if (p == NULL) {
        return NULL;
    }
    p->slot_size = slot_size;
    p->per_slab = per_slab;
    p->slabs = NULL;
    p->free = NULL;
    p->nslabs = 0;
    p->live = 0;
    p->nfree = 0;
    return p; /* caller owns p; no slab allocated until first pool_alloc */
}

void *pool_alloc(rb_pool_t *p) {
    if (p->free == NULL && !grow(p)) {
        return NULL; /* growth failed: slabs, live, nfree all unchanged */
    }
    struct free_slot *slot = p->free;
    p->free = slot->next; /* pop: O(1), head only */
    p->nfree--;
    p->live++;
    return slot; /* caller takes ownership of the slot */
}

void pool_free(rb_pool_t *p, void *obj) {
    if (obj == NULL) {
        return;
    }
    struct free_slot *slot = obj;
    slot->next = p->free; /* push: O(1); LIFO, so this slot is reused next */
    p->free = slot;       /* pool reclaims ownership of the slot */
    p->live--;
    p->nfree++;
}

void pool_stats(const rb_pool_t *p, size_t *slabs, size_t *live, size_t *free_objs) {
    if (slabs != NULL) {
        *slabs = p->nslabs;
    }
    if (live != NULL) {
        *live = p->live;
    }
    if (free_objs != NULL) {
        *free_objs = p->nfree; /* O(1): counter, never a list walk */
    }
}

void pool_destroy(rb_pool_t *p) {
    if (p == NULL) {
        return;
    }
    struct slab *s = p->slabs;
    /* invariant: s and every slab after it are still allocated; all before it are freed */
    while (s != NULL) {
        struct slab *next = s->next; /* read the in-band link before s is freed */
        rb_free(s);
        s = next;
    }
    rb_free(p);
}
