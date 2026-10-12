# Reflection

## Slab Comparison

The biggest difference between my implementation and `kmem_cache_alloc` is that Linux allows the pool to grow AND shrink dynamically. In my implementation, slabs are created when there are no more available slots, but slabs are only freed when the pool is destroyed. In a true operating system, this means a pool could grow very large and stay large even after most of its slots have been freed. Another difference is that `kmem_cache_alloc` includes safeguards for safe concurrency, like disabling interrupts, using per-CPU caches for lockless access, or spinlocks. My implementation has none of this, but assumes the program is single threaded.

## `pool_stats` Definitions

`slabs` was renamed to `nslabs` to make its purpose more explicit. It tracks the number of created slabs. `slabs` now refers to the pointer to the next slab in the list.
`free` is a pointer to the head of the free slot linked list. `free_objs`, renamed to `nfree`, is the number of free slots in the pool. `live` is the number of active slots. `free` is checked first in `pool_alloc`, and if it is null (free list is empty), `grow` is called. If `grow` fails, nothing is changed. 
`pool_stats` modifies given pointers to hold `nslabs`, `live`, `nfree`. The `live + free_objs == slabs * objs_per_slab` check lives in `test_rbtree.c` as:

```c
static struct pool_counts pool_check(const rb_pool_t *p) {
    struct pool_counts c;
    pool_stats(p, &c.slabs, &c.live, &c.free_objs);
    CHECK(c.live + c.free_objs == c.slabs * p->per_slab);
    return c;
}
```

## Slab Headers

I chose in-band slab headers over out-of-band. This puts the header at the front of the slab, shifting the first slot's address forward. I chose this for the same reason I chose no parent pointers in the last assignment: to sacrifice convenience to minimize allocations, reducing the failure surface area. I also found it practical because the size of the header was no bigger than the wasted "slack" at the end of the slab, so placing headers in-band actually minimized wasted memory and did not result in a lost slot.

## Alignment

Slabs are defined as:

```c
struct slab {
    struct slab *next;                        /* offset 0: next slab in the pool's list */
    alignas(max_align_t) unsigned char mem[]; /* FAM: slots start at first max_align_t boundary past next */
};
```

`pool_create` calculates the size of slots and number of slots per slab given the size of a slab (`SLAB_BYTES`) and the size of a node (`obj_size`). The header (pointer to the next slab) is offset by 0 and is automatically aligned because the slab itself is allocated with `malloc`. The rest of the slab after the header is filled by the `mem` array. Using `alignas(max_align_t)` for this array ensures that the whole slab is properly addressed to satisfy C's alignment requirement. `alignof` is also used when calculating slot sizes to ensure that each element is also aligned correctly. For example, if addresses must be divisible by 16, then nodes of size 15 would fall out of alignment. The pool rounds this up to 16 to keep them aligned.
