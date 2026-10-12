/* Unity build: pulls in every `static` helper in src/rbtree.c (new_node,
 * is_red, rotate_left, rotate_right, insert_fixup, ...) so this file can
 * unit-test them directly, instead of only through the public rbtree_t
 * API.
 *
 * Inside src/rbtree.c only, pool_alloc/pool_free/pool_destroy are redirected
 * to the checked_* wrappers defined with the pool tests below: every node the
 * tree frees is poisoned, every slot it reuses is checked for intact poison
 * (use-after-free writes), every alloc/free is checked against the pool's
 * counters, and rb_destroy must have returned every slot before the pool goes.
 * rb_pool_t is declared here because the checked_* prototypes need it before
 * rbtree.c is pulled in; the macros are dropped before pool.c, whose
 * definitions must keep their real names. */
/* -std=c23 hides POSIX from glibc headers; the small-stack tests need mmap,
 * MAP_ANONYMOUS, pthread_attr_setstack and sysconf. Must precede every
 * system header, including the ones src/rbtree.c pulls in. */
#define _DEFAULT_SOURCE
typedef struct rb_pool rb_pool_t;
static void *checked_alloc(rb_pool_t *p);
static void checked_free(rb_pool_t *p, void *obj);
static void checked_destroy(rb_pool_t *p);
#define pool_alloc(p) checked_alloc(p)
#define pool_free(p, obj) checked_free(p, obj)
#define pool_destroy(p) checked_destroy(p)
#include "../src/rbtree.c"
#undef pool_alloc
#undef pool_free
#undef pool_destroy
/* Same for the slab pool: exposes struct rb_pool/struct slab so the pool
 * tests can read per_slab and walk slabs directly. */
#include "../src/pool.c"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

static int failures = 0;

/* Pool counters and checks, defined with the pool tests further down; the
 * tree fault sweep checks its tree's pool after every call with them. */
struct pool_counts {
    size_t slabs;
    size_t live;
    size_t free_objs;
};
static struct pool_counts pool_check(const rb_pool_t *p);
static void check_alloc_delta(const rb_pool_t *p, struct pool_counts b,
                              bool handed_out);
static void check_free_delta(const rb_pool_t *p, struct pool_counts b);
static bool free_list_ok(const rb_pool_t *p);

/* Node-level tests build nodes outside any tree, so they draw node slots
 * from this pool (created in main) the way a tree's helpers use t->pool. */
static rb_pool_t *test_pool;

/* When false, checked_alloc/checked_free skip straight to the pool and
 * checked_destroy keeps only its O(1) leak check: the per-call checks scan
 * every slab, far too slow for the million-node small-stack tests. Only
 * flip it for a tree's whole life (create through destroy): a slot freed
 * unchecked is never poisoned, so a checked alloc would then misreport it. */
static bool pool_checks = true;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,    \
                    #cond);                                            \
            failures++;                                                \
        }                                                              \
    } while (0)

/* --- new_node ------------------------------------------------------- */

static void test_new_node_sets_fields(void) {
    struct rb_node *n = new_node("alpha", (void *)0x1234, test_pool);
    CHECK(n != NULL);
    if (n == NULL) {
        return;
    }
    CHECK(n->value == (void *)0x1234);
    CHECK(n->color == RED);
    CHECK(n->left == NULL);
    CHECK(n->right == NULL);

    rb_free(n->key);
    checked_free(test_pool, n);
}

/* The header contract says the tree copies the key; prove new_node doesn't
 * just alias the caller's buffer by freeing that buffer and confirming the
 * node's key still reads correctly (a real alias would be a use-after-free
 * here, caught by ASan under `make asan`). */
static void test_new_node_copies_key(void) {
    char *buf = malloc(6);
    CHECK(buf != NULL);
    if (buf == NULL) {
        return;
    }
    memcpy(buf, "alpha", 6);
    uintptr_t buf_addr = (uintptr_t)buf;

    struct rb_node *n = new_node(buf, NULL, test_pool);
    free(buf);

    CHECK(n != NULL);
    if (n == NULL) {
        return;
    }
    CHECK((uintptr_t)n->key != buf_addr);
    CHECK(strcmp(n->key, "alpha") == 0);

    rb_free(n->key);
    checked_free(test_pool, n);
}

/* --- is_red ----------------------------------------------------------- */

static void test_is_red(void) {
    struct rb_node red_node = {.color = RED};
    struct rb_node black_node = {.color = BLACK};

    CHECK(is_red(&red_node));
    CHECK(!is_red(&black_node));
    CHECK(!is_red(NULL)); /* absent child counts as black */
}

/* --- rotations ---------------------------------------------------------
 * rotate_left/rotate_right are pure structural operations: they relink
 * pointers only and must not touch color (fixup recolors separately). */

static void test_rotate_left(void) {
    struct rb_node a = {.color = BLACK};
    struct rb_node b = {.color = BLACK};
    struct rb_node c = {.color = BLACK};
    struct rb_node r = {.left = &b, .right = &c, .color = RED};
    struct rb_node n = {.left = &a, .right = &r, .color = BLACK};

    struct rb_node *new_root = rotate_left(&n);

    CHECK(new_root == &r);
    if (new_root != &r) {
        return;
    }
    CHECK(r.left == &n);
    CHECK(r.right == &c);
    CHECK(n.left == &a);
    CHECK(n.right == &b);
    CHECK(n.color == BLACK); /* rotation must not recolor */
    CHECK(r.color == RED);
}

static void test_rotate_right(void) {
    struct rb_node a = {.color = BLACK};
    struct rb_node b = {.color = BLACK};
    struct rb_node c = {.color = BLACK};
    struct rb_node l = {.left = &a, .right = &b, .color = RED};
    struct rb_node n = {.left = &l, .right = &c, .color = BLACK};

    struct rb_node *new_root = rotate_right(&n);

    CHECK(new_root == &l);
    if (new_root != &l) {
        return;
    }
    CHECK(l.right == &n);
    CHECK(l.left == &a);
    CHECK(n.left == &b);
    CHECK(n.right == &c);
    CHECK(n.color == BLACK); /* rotation must not recolor */
    CHECK(l.color == RED);
}

/* --- insert_fixup --------------------------------------------------------
 * Each restructuring case (LL/LR/RR/RL) must produce a red new subtree root
 * with two black children, and must not disturb any node two levels below
 * the violation. The no-op cases must return n untouched. */

/* Red uncle: recolor parent/uncle/grandparent only, no rotation. */
static void test_insert_fixup_red_uncle(void) {
    struct rb_node a = {.color = BLACK};
    struct rb_node b = {.color = BLACK};
    struct rb_node ll = {.left = &a, .right = &b, .color = RED};
    struct rb_node l = {.left = &ll, .color = RED};
    struct rb_node r = {.color = RED}; /* uncle: red, no children */
    struct rb_node n = {.left = &l, .right = &r, .color = BLACK};

    /* Same left-left shape as test_insert_fixup_ll, but with a red uncle:
     * must recolor only (no rotation), leaving n itself in place. */
    struct rb_node *new_root = insert_fixup(&n);

    CHECK(new_root == &n);
    if (new_root != &n) {
        return;
    }
    CHECK(n.color == RED);
    CHECK(l.color == BLACK);
    CHECK(r.color == BLACK);
    CHECK(n.left == &l);
    CHECK(n.right == &r);
    CHECK(l.left == &ll); /* deeper structure untouched */
    CHECK(ll.left == &a && ll.right == &b);
}

/* Left-left violation: single right rotation at n. */
static void test_insert_fixup_ll(void) {
    struct rb_node a = {.color = BLACK};
    struct rb_node b = {.color = BLACK};
    struct rb_node c = {.color = BLACK};
    struct rb_node d = {.color = BLACK};
    struct rb_node ll = {.left = &a, .right = &b, .color = RED};
    struct rb_node l = {.left = &ll, .right = &c, .color = RED};
    struct rb_node n = {.left = &l, .right = &d, .color = BLACK};

    struct rb_node *new_root = insert_fixup(&n);

    CHECK(new_root == &l);
    if (new_root != &l) {
        return;
    }
    CHECK(l.color == RED);
    CHECK(l.left == &ll);
    CHECK(ll.color == BLACK);
    CHECK(ll.left == &a && ll.right == &b); /* deeper structure untouched */
    CHECK(l.right == &n);
    CHECK(n.color == BLACK);
    CHECK(n.left == &c);
    CHECK(n.right == &d);
}

/* Left-right violation: rotate left at l, then right at n. */
static void test_insert_fixup_lr(void) {
    struct rb_node a = {.color = BLACK};
    struct rb_node b = {.color = BLACK};
    struct rb_node c = {.color = BLACK};
    struct rb_node d = {.color = BLACK};
    struct rb_node lr = {.left = &b, .right = &c, .color = RED};
    struct rb_node l = {.left = &a, .right = &lr, .color = RED};
    struct rb_node n = {.left = &l, .right = &d, .color = BLACK};

    struct rb_node *new_root = insert_fixup(&n);

    CHECK(new_root == &lr);
    if (new_root != &lr) {
        return;
    }
    CHECK(lr.color == RED);
    CHECK(lr.left == &l);
    CHECK(l.color == BLACK);
    CHECK(l.left == &a);
    CHECK(l.right == &b);
    CHECK(lr.right == &n);
    CHECK(n.color == BLACK);
    CHECK(n.left == &c);
    CHECK(n.right == &d);
}

/* Right-right violation: single left rotation at n. */
static void test_insert_fixup_rr(void) {
    struct rb_node a = {.color = BLACK};
    struct rb_node b = {.color = BLACK};
    struct rb_node c = {.color = BLACK};
    struct rb_node d = {.color = BLACK};
    struct rb_node rr = {.left = &b, .right = &c, .color = RED};
    struct rb_node r = {.left = &a, .right = &rr, .color = RED};
    struct rb_node n = {.left = &d, .right = &r, .color = BLACK};

    struct rb_node *new_root = insert_fixup(&n);

    CHECK(new_root == &r);
    if (new_root != &r) {
        return;
    }
    CHECK(r.color == RED);
    CHECK(r.right == &rr);
    CHECK(rr.color == BLACK);
    CHECK(rr.left == &b && rr.right == &c); /* deeper structure untouched */
    CHECK(r.left == &n);
    CHECK(n.color == BLACK);
    CHECK(n.left == &d);
    CHECK(n.right == &a);
}

/* Right-left violation: rotate right at r, then left at n. */
static void test_insert_fixup_rl(void) {
    struct rb_node a = {.color = BLACK};
    struct rb_node b = {.color = BLACK};
    struct rb_node c = {.color = BLACK};
    struct rb_node d = {.color = BLACK};
    struct rb_node rl = {.left = &b, .right = &c, .color = RED};
    struct rb_node r = {.left = &rl, .right = &d, .color = RED};
    struct rb_node n = {.left = &a, .right = &r, .color = BLACK};

    struct rb_node *new_root = insert_fixup(&n);

    CHECK(new_root == &rl);
    if (new_root != &rl) {
        return;
    }
    CHECK(rl.color == RED);
    CHECK(rl.left == &n);
    CHECK(n.color == BLACK);
    CHECK(n.left == &a);
    CHECK(n.right == &b);
    CHECK(rl.right == &r);
    CHECK(r.color == BLACK);
    CHECK(r.left == &c);
    CHECK(r.right == &d);
}

/* n itself is red: no violation to fix, returns n untouched. */
static void test_insert_fixup_noop_when_n_red(void) {
    struct rb_node ll = {.color = RED};
    struct rb_node l = {.left = &ll, .color = RED};
    struct rb_node n = {.left = &l, .color = RED};

    struct rb_node *new_root = insert_fixup(&n);

    CHECK(new_root == &n);
    CHECK(n.left == &l);
    CHECK(l.left == &ll);
    CHECK(n.color == RED);
}

/* No red-red violation present: returns n untouched. */
static void test_insert_fixup_noop_when_no_violation(void) {
    struct rb_node l = {.color = BLACK};
    struct rb_node r = {.color = RED};
    struct rb_node n = {.left = &l, .right = &r, .color = BLACK};

    struct rb_node *new_root = insert_fixup(&n);

    CHECK(new_root == &n);
    CHECK(n.left == &l);
    CHECK(n.right == &r);
    CHECK(n.color == BLACK);
    CHECK(l.color == BLACK);
    CHECK(r.color == RED);
}

/* --- insert_rec ----------------------------------------------------------
 * Exercises the recursion + fixup wiring end to end, still on raw,
 * hand-built nodes (no rb_create/rbtree_t involved). */

/* One level of recursion: new key attaches as a red leaf, no fixup fires. */
static void test_insert_rec_attaches_new_leaf(void) {
    struct rb_node r = {.key = "zulu", .left = NULL, .right = NULL, .color = BLACK};
    struct rb_node n = {.key = "mango", .left = NULL, .right = &r, .color = BLACK};

    bool inserted = false;
    struct rb_node *new_root =
        insert_rec(&n, "apple", (void *)0xABCD, NULL, &inserted, test_pool);

    CHECK(inserted);
    CHECK(new_root == &n); /* no violation: n comes back untouched in shape */
    if (new_root != &n) {
        return;
    }
    CHECK(n.color == BLACK);
    CHECK(n.right == &r);
    CHECK(r.color == BLACK);

    struct rb_node *newleaf = n.left;
    CHECK(newleaf != NULL);
    if (newleaf == NULL) {
        return;
    }
    CHECK(newleaf->color == RED);
    CHECK(strcmp(newleaf->key, "apple") == 0);
    CHECK(newleaf->value == (void *)0xABCD);
    CHECK(newleaf->left == NULL && newleaf->right == NULL);

    rb_free(newleaf->key);
    checked_free(test_pool, newleaf);
}

/* Two levels of recursion: the new leaf attaches under the red node l, whose
 * own insert_fixup call must no-op (l is red; the violation isn't at l's
 * level), and the LL rotation must fire one level up, at n. */
static void test_insert_rec_two_level_ll_violation(void) {
    struct rb_node l = {.key = "g", .left = NULL, .right = NULL, .color = RED};
    struct rb_node n = {.key = "m", .left = &l, .right = NULL, .color = BLACK};

    bool inserted = false;
    struct rb_node *new_root =
        insert_rec(&n, "c", (void *)0xC0FFEE, NULL, &inserted, test_pool);

    CHECK(inserted);
    CHECK(new_root == &l);
    if (new_root != &l) {
        return;
    }
    CHECK(l.color == RED);
    CHECK(l.right == &n);
    CHECK(n.color == BLACK);
    CHECK(n.left == NULL);
    CHECK(n.right == NULL);

    struct rb_node *newleaf = l.left;
    CHECK(newleaf != NULL);
    if (newleaf == NULL) {
        return;
    }
    CHECK(newleaf->color == BLACK);
    CHECK(strcmp(newleaf->key, "c") == 0);
    CHECK(newleaf->value == (void *)0xC0FFEE);

    rb_free(newleaf->key);
    checked_free(test_pool, newleaf);
}

/* --- rb_create / value_free ---------------------------------------------
 * rb_destroy doesn't exist yet, so these tests free the tree (and, in the
 * overwrite test, its one node) by hand instead of through the public API. */

static void dummy_value_free(void *value) {
    (void)value;
}

static void test_rb_create_sets_fields(void) {
    rbtree_t *t = rb_create(NULL);
    CHECK(t != NULL);
    if (t == NULL) {
        return;
    }
    CHECK(t->root == NULL);
    CHECK(t->value_free == NULL);
    CHECK(t->size == 0);

    rb_destroy(t);
}

static void test_rb_create_stores_value_free(void) {
    rbtree_t *t = rb_create(dummy_value_free);
    CHECK(t != NULL);
    if (t == NULL) {
        return;
    }
    CHECK(t->value_free == dummy_value_free);

    rb_destroy(t);
}

static int free_call_count = 0;

static void counting_value_free(void *value) {
    (void)value;
    free_call_count++;
}

/* Inserting an existing key overwrites its value and frees the old one. */
static void test_rb_insert_overwrite_frees_old_value(void) {
    rbtree_t *t = rb_create(counting_value_free);
    CHECK(t != NULL);
    if (t == NULL) {
        return;
    }
    free_call_count = 0;

    CHECK(rb_insert(t, "key", (void *)0x1) == 0);
    CHECK(free_call_count == 0); /* first insert: nothing to overwrite */

    CHECK(rb_insert(t, "key", (void *)0x2) == 0);
    CHECK(free_call_count == 1); /* old value (0x1) freed exactly once */
    CHECK(rb_size(t) == 1);      /* overwrite must not change size */
    CHECK(rb_validate(t) == 0);
    CHECK(t->root != NULL);
    if (t->root != NULL) {
        CHECK(t->root->value == (void *)0x2);
        CHECK(strcmp(t->root->key, "key") == 0);
    }
    rb_destroy(t);
}

/* Re-inserting the pointer a key already holds must not free it: the node
 * keeps owning the same value, so it is released once, by rb_destroy. */
static void test_rb_insert_same_value_twice_frees_nothing(void) {
    rbtree_t *t = rb_create(counting_value_free);
    CHECK(t != NULL);
    if (t == NULL) {
        return;
    }
    free_call_count = 0;

    CHECK(rb_insert(t, "key", (void *)0x1) == 0);
    CHECK(rb_insert(t, "key", (void *)0x1) == 0);
    CHECK(free_call_count == 0); /* same pointer: nothing was replaced */
    CHECK(rb_find(t, "key") == (void *)0x1);
    CHECK(rb_size(t) == 1);
    CHECK(rb_validate(t) == 0);

    rb_destroy(t);
    CHECK(free_call_count == 1); /* released exactly once, at destroy */
}

/* Same as above with a real heap value, so a free-then-store bug shows up
 * as a use-after-free / double free under ASan and Valgrind. */
static void test_rb_insert_same_malloced_value_twice(void) {
    rbtree_t *t = rb_create(free);
    CHECK(t != NULL);
    if (t == NULL) {
        return;
    }
    int *v = malloc(sizeof *v);
    CHECK(v != NULL);
    if (v == NULL) {
        rb_destroy(t);
        return;
    }
    *v = 42;

    CHECK(rb_insert(t, "key", v) == 0); /* tree takes ownership of v */
    if (rb_insert(t, "key", v) != 0) {
        CHECK(false); /* no allocation on overwrite, so this can't fail */
    }
    int *found = rb_find(t, "key");
    CHECK(found == v);
    if (found != NULL) {
        CHECK(*found == 42); /* still live: reading it is not a use-after-free */
    }

    rb_destroy(t); /* frees v exactly once */
}

/* --- rb_size ------------------------------------------------------------- */

static void test_rb_size_tracks_distinct_keys(void) {
    rbtree_t *t = rb_create(NULL);
    CHECK(t != NULL);
    if (t == NULL) {
        return;
    }

    CHECK(rb_size(t) == 0);

    CHECK(rb_insert(t, "b", NULL) == 0);
    CHECK(rb_size(t) == 1);

    CHECK(rb_insert(t, "a", NULL) == 0);
    CHECK(rb_size(t) == 2);

    CHECK(rb_insert(t, "c", NULL) == 0);
    CHECK(rb_size(t) == 3);

    CHECK(rb_insert(t, "b", NULL) == 0); /* overwrite: size unchanged */
    CHECK(rb_size(t) == 3);
    CHECK(rb_validate(t) == 0);

    rb_destroy(t);
}

/* --- rb_find --------------------------------------------------------- */

static void test_rb_find_returns_value_for_present_key(void) {
    rbtree_t *t = rb_create(NULL);
    CHECK(t != NULL);
    if (t == NULL) {
        return;
    }

    CHECK(rb_insert(t, "b", (void *)0xB) == 0);
    CHECK(rb_insert(t, "a", (void *)0xA) == 0);
    CHECK(rb_insert(t, "c", (void *)0xC) == 0);
    CHECK(rb_validate(t) == 0);

    CHECK(rb_find(t, "a") == (void *)0xA);
    CHECK(rb_find(t, "b") == (void *)0xB);
    CHECK(rb_find(t, "c") == (void *)0xC);

    rb_destroy(t);
}

static void test_rb_find_returns_null_for_absent_key(void) {
    rbtree_t *t = rb_create(NULL);
    CHECK(t != NULL);
    if (t == NULL) {
        return;
    }

    CHECK(rb_insert(t, "b", (void *)0xB) == 0);
    CHECK(rb_validate(t) == 0);

    CHECK(rb_find(t, "z") == NULL);

    rb_destroy(t);
}

static void test_rb_find_on_empty_tree_returns_null(void) {
    rbtree_t *t = rb_create(NULL);
    CHECK(t != NULL);
    if (t == NULL) {
        return;
    }

    CHECK(rb_find(t, "anything") == NULL);

    rb_destroy(t);
}

/* --- rb_foreach --------------------------------------------------------- */

struct collect_ctx {
    const char *keys[8];
    size_t count;
};

static void collect_key(const char *key, void *value, void *ctx) {
    (void)value;
    struct collect_ctx *c = ctx;
    c->keys[c->count++] = key;
}

static void test_rb_foreach_visits_in_order(void) {
    rbtree_t *t = rb_create(NULL);
    CHECK(t != NULL);
    if (t == NULL) {
        return;
    }

    CHECK(rb_insert(t, "d", NULL) == 0);
    CHECK(rb_insert(t, "b", NULL) == 0);
    CHECK(rb_insert(t, "f", NULL) == 0);
    CHECK(rb_insert(t, "a", NULL) == 0);
    CHECK(rb_insert(t, "c", NULL) == 0);
    CHECK(rb_insert(t, "e", NULL) == 0);
    CHECK(rb_validate(t) == 0);

    struct collect_ctx c = {.count = 0};
    rb_foreach(t, collect_key, &c);

    CHECK(c.count == 6);
    if (c.count == 6) {
        const char *expected[] = {"a", "b", "c", "d", "e", "f"};
        for (size_t i = 0; i < 6; i++) {
            CHECK(strcmp(c.keys[i], expected[i]) == 0);
        }
    }

    rb_destroy(t);
}

static void test_rb_foreach_on_empty_tree_does_nothing(void) {
    rbtree_t *t = rb_create(NULL);
    CHECK(t != NULL);
    if (t == NULL) {
        return;
    }

    struct collect_ctx c = {.count = 0};
    rb_foreach(t, collect_key, &c);

    CHECK(c.count == 0);

    rb_destroy(t);
}

/* --- rb_destroy ----------------------------------------------------------
 * Unlike the tests above, these exercise the real public API end to end:
 * rb_destroy owns freeing every key, value, and node itself, so nothing is
 * freed by hand here. */

static void test_rb_destroy_frees_all_values(void) {
    rbtree_t *t = rb_create(counting_value_free);
    CHECK(t != NULL);
    if (t == NULL) {
        return;
    }

    CHECK(rb_insert(t, "b", (void *)0x1) == 0);
    CHECK(rb_insert(t, "a", (void *)0x2) == 0);
    CHECK(rb_insert(t, "c", (void *)0x3) == 0);
    CHECK(rb_insert(t, "d", (void *)0x4) == 0);
    CHECK(rb_validate(t) == 0);

    free_call_count = 0;
    rb_destroy(t);
    CHECK(free_call_count == 4); /* every distinct value freed exactly once */
}

static void test_rb_destroy_null_is_noop(void) {
    rb_destroy(NULL); /* must not crash */
}

/* --- rb_validate -----------------------------------------------------
 * Most of these hand-build a struct rbtree directly (rather than going
 * through rb_create/rb_insert) so each test can isolate exactly one rule
 * violation without the others interfering. */

static void test_rb_validate_valid_tree_is_zero(void) {
    rbtree_t *t = rb_create(NULL);
    CHECK(t != NULL);
    if (t == NULL) {
        return;
    }

    CHECK(rb_insert(t, "d", NULL) == 0);
    CHECK(rb_insert(t, "b", NULL) == 0);
    CHECK(rb_insert(t, "f", NULL) == 0);
    CHECK(rb_insert(t, "a", NULL) == 0);
    CHECK(rb_insert(t, "c", NULL) == 0);
    CHECK(rb_insert(t, "e", NULL) == 0);
    CHECK(rb_insert(t, "g", NULL) == 0);

    CHECK(rb_validate(t) == 0);

    rb_destroy(t);
}

static void test_rb_validate_empty_tree_is_zero(void) {
    rbtree_t *t = rb_create(NULL);
    CHECK(t != NULL);
    if (t == NULL) {
        return;
    }

    CHECK(rb_validate(t) == 0);

    rb_destroy(t);
}

static void test_rb_validate_detects_red_root(void) {
    struct rb_node root = {.key = "a", .color = RED};
    struct rbtree t = {.root = &root, .value_free = NULL, .size = 1};

    CHECK(rb_validate(&t) == 1);
}

static void test_rb_validate_detects_red_red(void) {
    struct rb_node gc = {.key = "z", .color = RED};
    struct rb_node child = {.key = "a", .left = &gc, .color = RED};
    struct rb_node root = {.key = "b", .left = &child, .color = BLACK};
    struct rbtree t = {.root = &root, .value_free = NULL, .size = 3};

    CHECK(rb_validate(&t) == 2);
}

static void test_rb_validate_detects_black_height_mismatch(void) {
    struct rb_node l = {.key = "b", .color = BLACK};
    struct rb_node root = {.key = "m", .left = &l, .color = BLACK};
    struct rbtree t = {.root = &root, .value_free = NULL, .size = 2};

    CHECK(rb_validate(&t) == 3);
}

static void test_rb_validate_detects_order_violation(void) {
    struct rb_node l = {.key = "z", .color = BLACK}; /* not < root's key */
    struct rb_node root = {.key = "a", .left = &l, .color = BLACK};
    struct rbtree t = {.root = &root, .value_free = NULL, .size = 2};

    CHECK(rb_validate(&t) == 4);
}

static void test_rb_validate_detects_size_mismatch(void) {
    struct rb_node root = {.key = "a", .color = BLACK};
    struct rbtree t = {.root = &root, .value_free = NULL, .size = 2};

    CHECK(rb_validate(&t) == 5);
}

/* --- rb_delete -------------------------------------------------------
 * The cases that never require a rotation: absent key, red leaf, black node
 * with one red child, two children (successor extraction), and the
 * black-sibling recolor case (5) in both parent-color outcomes. The
 * rotation-requiring cases (red sibling, or black sibling with a red
 * nephew) are covered further down, against fixup_left_deficit and
 * fixup_right_deficit directly.
 *
 * Nodes that the code under test will genuinely free are built with
 * mknode (a heap key copy via new_node, same as production nodes); nodes
 * that only get inspected or recolored, never freed, use stack structs
 * with literal keys, matching the existing insert_fixup/rotate tests. */

static struct rb_node *mknode(const char *key, void *value, rb_color_t color,
                               struct rb_node *left, struct rb_node *right) {
    struct rb_node *n = new_node(key, value, test_pool);
    if (n == NULL) {
        return NULL;
    }
    n->color = color;
    n->left = left;
    n->right = right;
    return n;
}

/* --- fixup_right_deficit / fixup_left_deficit (case 5) --------------- */

/* Case B, red n: sibling recolors red and n absorbs the deficit locally. */
static void test_fixup_right_deficit_case_b_red_parent(void) {
    struct rb_node sib = {.key = "d", .color = BLACK}; /* both nephews NULL */
    struct rb_node n = {.key = "m", .left = &sib, .right = NULL, .color = RED};

    bool shorter = true;
    struct rb_node *new_root = fixup_right_deficit(&n, &shorter);

    CHECK(new_root == &n);
    CHECK(!shorter);
    CHECK(n.color == BLACK);
    CHECK(sib.color == RED);
}

/* Case B, black n: sibling recolors red but the deficit propagates up. */
static void test_fixup_right_deficit_case_b_black_parent_propagates(void) {
    struct rb_node sib = {.key = "d", .color = BLACK};
    struct rb_node n = {.key = "m", .left = &sib, .right = NULL, .color = BLACK};

    bool shorter = true;
    struct rb_node *new_root = fixup_right_deficit(&n, &shorter);

    CHECK(new_root == &n);
    CHECK(shorter); /* n had no black to spare; caller must fix up further */
    CHECK(n.color == BLACK);
    CHECK(sib.color == RED);
}

/* Left-side mirror of case B: red n absorbs the deficit locally. */
static void test_fixup_left_deficit_case_b_red_parent(void) {
    struct rb_node sib = {.key = "s", .color = BLACK};
    struct rb_node n = {.key = "m", .left = NULL, .right = &sib, .color = RED};

    bool shorter = true;
    struct rb_node *new_root = fixup_left_deficit(&n, &shorter);

    CHECK(new_root == &n);
    CHECK(!shorter);
    CHECK(n.color == BLACK);
    CHECK(sib.color == RED);
}

/* Left-side mirror of case B: black n propagates the deficit upward. */
static void test_fixup_left_deficit_case_b_black_parent_propagates(void) {
    struct rb_node sib = {.key = "s", .color = BLACK};
    struct rb_node n = {.key = "m", .left = NULL, .right = &sib, .color = BLACK};

    bool shorter = true;
    struct rb_node *new_root = fixup_left_deficit(&n, &shorter);

    CHECK(new_root == &n);
    CHECK(shorter);
    CHECK(n.color == BLACK);
    CHECK(sib.color == RED);
}

/* --- fixup_right_deficit / fixup_left_deficit (cases A, C, D) -----------
 * These exercise the rotation-requiring cases of fixup_*_deficit, the ones
 * case B (recolor only) does not reach. Each expected result below was
 * hand-derived from rotate_left/rotate_right plus black-height bookkeeping
 * ahead of the implementation, so they encode the correct target behavior
 * rather than whatever the code happens to do. */

/* Case A: red sibling. n is red here (rather than black) specifically to
 * prove the new subtree root inherits n's *original* color rather than
 * defaulting to black. */
static void test_fixup_right_deficit_case_a_red_sibling(void) {
    struct rb_node far = {.key = "f", .color = BLACK}; /* s's far child */
    struct rb_node near = {.key = "e", .color = BLACK}; /* s's near child */
    struct rb_node s = {.key = "c", .left = &far, .right = &near, .color = RED};
    struct rb_node n = {.key = "m", .left = &s, .right = NULL, .color = RED};

    bool shorter = true;
    struct rb_node *new_root = fixup_right_deficit(&n, &shorter);

    CHECK(new_root == &s);
    CHECK(!shorter);
    CHECK(s.color == RED); /* s inherits n's original color */
    CHECK(s.left == &far);
    CHECK(far.color == BLACK);
    CHECK(s.right == &n);
    CHECK(n.color == BLACK);
    CHECK(n.left == &near); /* near reattaches under n as its new sibling */
    CHECK(near.color == RED); /* case B absorbs locally once n is red */
    CHECK(n.right == NULL);
}

/* Left-side mirror of case A (red sibling). */
static void test_fixup_left_deficit_case_a_red_sibling(void) {
    struct rb_node near = {.key = "e", .color = BLACK};
    struct rb_node far = {.key = "q", .color = BLACK};
    struct rb_node s = {.key = "p", .left = &near, .right = &far, .color = RED};
    struct rb_node n = {.key = "m", .left = NULL, .right = &s, .color = RED};

    bool shorter = true;
    struct rb_node *new_root = fixup_left_deficit(&n, &shorter);

    CHECK(new_root == &s);
    CHECK(!shorter);
    CHECK(s.color == RED);
    CHECK(s.right == &far);
    CHECK(far.color == BLACK);
    CHECK(s.left == &n);
    CHECK(n.color == BLACK);
    CHECK(n.right == &near);
    CHECK(near.color == RED);
    CHECK(n.left == NULL);
}

/* Case C: black sibling, red far nephew (near nephew is NULL/black, so this
 * is unambiguously case C, not D). n is red to prove color transfer. */
static void test_fixup_right_deficit_case_c_red_far_nephew(void) {
    struct rb_node far = {.key = "f", .color = RED};
    struct rb_node s = {.key = "c", .left = &far, .right = NULL, .color = BLACK};
    struct rb_node n = {.key = "m", .left = &s, .right = NULL, .color = RED};

    bool shorter = true;
    struct rb_node *new_root = fixup_right_deficit(&n, &shorter);

    CHECK(new_root == &s);
    CHECK(!shorter);
    CHECK(s.color == RED); /* s inherits n's original color */
    CHECK(s.left == &far);
    CHECK(far.color == BLACK); /* far nephew recolored to absorb the deficit */
    CHECK(s.right == &n);
    CHECK(n.color == BLACK); /* n always ends black in case C/D */
    CHECK(n.left == NULL);   /* near nephew (was NULL) reattaches under n */
    CHECK(n.right == NULL);  /* original deficient side, untouched */
}

/* Left-side mirror of case C (black sibling, red far nephew). */
static void test_fixup_left_deficit_case_c_red_far_nephew(void) {
    struct rb_node far = {.key = "q", .color = RED};
    struct rb_node s = {.key = "p", .left = NULL, .right = &far, .color = BLACK};
    struct rb_node n = {.key = "m", .left = NULL, .right = &s, .color = RED};

    bool shorter = true;
    struct rb_node *new_root = fixup_left_deficit(&n, &shorter);

    CHECK(new_root == &s);
    CHECK(!shorter);
    CHECK(s.color == RED);
    CHECK(s.right == &far);
    CHECK(far.color == BLACK);
    CHECK(s.left == &n);
    CHECK(n.color == BLACK);
    CHECK(n.right == NULL);
    CHECK(n.left == NULL);
}

/* Case D: black sibling, red near nephew, black (NULL) far nephew. Resolves
 * via one rotation at the sibling (turning it into case C's shape) followed
 * by case C's rotation at n. n is red to prove color transfer through both
 * steps. */
static void test_fixup_right_deficit_case_d_red_near_nephew(void) {
    struct rb_node near = {.key = "e", .left = NULL, .right = NULL, .color = RED};
    struct rb_node s = {.key = "c", .left = NULL, .right = &near, .color = BLACK};
    struct rb_node n = {.key = "m", .left = &s, .right = NULL, .color = RED};

    bool shorter = true;
    struct rb_node *new_root = fixup_right_deficit(&n, &shorter);

    CHECK(new_root == &near);
    CHECK(!shorter);
    CHECK(near.color == RED); /* near inherits n's original color */
    CHECK(near.left == &s);
    CHECK(s.color == BLACK);
    CHECK(s.left == NULL);  /* s's far nephew, unchanged */
    CHECK(s.right == NULL); /* near's old left child, unchanged */
    CHECK(near.right == &n);
    CHECK(n.color == BLACK);
    CHECK(n.left == NULL);  /* near's old right child, unchanged */
    CHECK(n.right == NULL); /* original deficient side, untouched */
}

/* Left-side mirror of case D (black sibling, red near nephew). */
static void test_fixup_left_deficit_case_d_red_near_nephew(void) {
    struct rb_node near = {.key = "j", .left = NULL, .right = NULL, .color = RED};
    struct rb_node s = {.key = "p", .left = &near, .right = NULL, .color = BLACK};
    struct rb_node n = {.key = "m", .left = NULL, .right = &s, .color = RED};

    bool shorter = true;
    struct rb_node *new_root = fixup_left_deficit(&n, &shorter);

    CHECK(new_root == &near);
    CHECK(!shorter);
    CHECK(near.color == RED);
    CHECK(near.right == &s);
    CHECK(s.color == BLACK);
    CHECK(s.right == NULL);
    CHECK(s.left == NULL);
    CHECK(near.left == &n);
    CHECK(n.color == BLACK);
    CHECK(n.right == NULL);
    CHECK(n.left == NULL);
}

/* --- delete_rec case 7: black leaf with a red (not black) sibling --------
 * End-to-end wiring test: deleting the black leaf target reports a
 * deficit at n, which must dispatch into fixup_right_deficit's case A. */
static void test_delete_rec_black_leaf_no_black_sibling_case_7(void) {
    struct rb_node sl = {.key = "a", .color = BLACK};
    struct rb_node sr = {.key = "e", .color = BLACK};
    struct rb_node s = {.key = "c", .left = &sl, .right = &sr, .color = RED};
    struct rb_node *target = mknode("z", (void *)0x99, BLACK, NULL, NULL);
    CHECK(target != NULL);
    if (target == NULL) {
        return;
    }
    struct rb_node n = {.key = "m", .left = &s, .right = target, .color = BLACK};

    bool deleted = false;
    bool shorter = false;
    struct rb_node *new_root = delete_rec(&n, "z", NULL, &deleted, &shorter, test_pool);

    CHECK(new_root == &s); /* rotation promotes s to replace n */
    CHECK(deleted);
    CHECK(!shorter);
    CHECK(s.color == BLACK); /* s inherits n's original color */
    CHECK(s.left == &sl);
    CHECK(sl.color == BLACK);
    CHECK(s.right == &n);
    CHECK(n.color == BLACK);
    CHECK(n.left == &sr);
    CHECK(sr.color == RED);
    CHECK(n.right == NULL);
    /* target was fully freed inside delete_rec; nothing to free here. */
}

/* --- extract_min -------------------------------------------------------- */

/* The minimum is two levels down a left spine; proves the recursive walk
 * and the non-deficient propagation back up (no fixup call should fire). */
static void test_extract_min_walks_to_deepest_left_leaf(void) {
    struct rb_node *e = mknode("e", (void *)0xE, RED, NULL, NULL);
    struct rb_node *g = mknode("g", (void *)0x6, BLACK, e, NULL);
    struct rb_node *j = mknode("j", (void *)0xA, BLACK, NULL, NULL);
    struct rb_node *h = mknode("h", (void *)0x8, BLACK, g, j);
    CHECK(e != NULL && g != NULL && j != NULL && h != NULL);
    if (e == NULL || g == NULL || j == NULL || h == NULL) {
        return;
    }

    char *out_key = NULL;
    void *out_value = NULL;
    bool shorter = true;
    struct rb_node *new_root = extract_min(h, &out_key, &out_value, &shorter, test_pool);

    CHECK(new_root == h);
    CHECK(!shorter);
    CHECK(out_key != NULL && strcmp(out_key, "e") == 0);
    CHECK(out_value == (void *)0xE);
    CHECK(h->left == g);
    CHECK(g->left == NULL);
    CHECK(g->right == NULL);
    CHECK(h->right == j);

    rb_free(out_key); /* ownership of e's key returned to us; e's struct is
                          already freed by extract_min */
    rb_free(g->key);
    checked_free(test_pool, g);
    rb_free(j->key);
    checked_free(test_pool, j);
    rb_free(h->key);
    checked_free(test_pool, h);
}

/* --- delete_rec ----------------------------------------------------------
 * Exercises the recursion directly, on hand-built subtrees, one case at a
 * time — same style as the insert_rec tests above. */

/* Case 1: key not found; subtree returned unchanged, *deleted stays false. */
static void test_delete_rec_absent_key_leaves_subtree_unchanged(void) {
    struct rb_node a = {.key = "a", .color = BLACK};
    struct rb_node c = {.key = "c", .color = BLACK};
    struct rb_node n = {.key = "b", .left = &a, .right = &c, .color = BLACK};

    bool deleted = false;
    bool shorter = false;
    struct rb_node *new_root = delete_rec(&n, "z", NULL, &deleted, &shorter, test_pool);

    CHECK(new_root == &n);
    CHECK(!deleted);
    CHECK(!shorter);
    CHECK(n.left == &a && n.right == &c);
    CHECK(n.color == BLACK);
}

/* Case 3: red leaf is simply freed, no black-height change. */
static void test_delete_rec_red_leaf_no_fixup(void) {
    struct rb_node *leaf = mknode("b", (void *)0xB, RED, NULL, NULL);
    struct rb_node *root = mknode("m", (void *)0x1, BLACK, leaf, NULL);
    CHECK(leaf != NULL && root != NULL);
    if (leaf == NULL || root == NULL) {
        return;
    }

    bool deleted = false;
    bool shorter = true;
    struct rb_node *new_root =
        delete_rec(root, "b", NULL, &deleted, &shorter, test_pool);

    CHECK(new_root == root);
    CHECK(deleted);
    CHECK(!shorter); /* red leaf contributes nothing to black-height */
    CHECK(root->left == NULL);
    CHECK(root->right == NULL);
    CHECK(root->color == BLACK);

    rb_free(root->key);
    checked_free(test_pool, root);
}

/* Case 6: black node with one red child; child is recolored black and promoted. */
static void test_delete_rec_black_node_with_one_red_child(void) {
    struct rb_node *e = mknode("e", (void *)0xE, RED, NULL, NULL);
    struct rb_node *d = mknode("d", (void *)0xD, BLACK, NULL, e);
    struct rb_node *z = mknode("z", (void *)0x2, BLACK, NULL, NULL);
    struct rb_node *root = mknode("m", (void *)0x1, BLACK, d, z);
    CHECK(e != NULL && d != NULL && z != NULL && root != NULL);
    if (e == NULL || d == NULL || z == NULL || root == NULL) {
        return;
    }

    bool deleted = false;
    bool shorter = true;
    struct rb_node *new_root =
        delete_rec(root, "d", NULL, &deleted, &shorter, test_pool);

    CHECK(new_root == root);
    CHECK(deleted);
    CHECK(!shorter); /* e's black absorbs exactly what d contributed */
    CHECK(root->left == e);
    CHECK(e->color == BLACK);
    CHECK(e->left == NULL && e->right == NULL);
    CHECK(root->right == z);

    rb_free(e->key);
    checked_free(test_pool, e);
    rb_free(z->key);
    checked_free(test_pool, z);
    rb_free(root->key);
    checked_free(test_pool, root);
}

/* Two children; the in-order successor is itself a red leaf, so extracting
 * it is a no-deficit case-3 removal one level down. */
static void test_delete_rec_two_children_successor_is_red_leaf(void) {
    struct rb_node *b = mknode("b", (void *)0xB, BLACK, NULL, NULL);
    struct rb_node *f = mknode("f", (void *)0xF, RED, NULL, NULL);
    struct rb_node *h = mknode("h", (void *)0xAA, BLACK, f, NULL);
    struct rb_node *d = mknode("d", (void *)0xD, BLACK, b, h);
    CHECK(b != NULL && f != NULL && h != NULL && d != NULL);
    if (b == NULL || f == NULL || h == NULL || d == NULL) {
        return;
    }

    bool deleted = false;
    bool shorter = true;
    struct rb_node *new_root = delete_rec(d, "d", NULL, &deleted, &shorter, test_pool);

    CHECK(new_root == d); /* d keeps its slot and color; only its payload moved */
    CHECK(deleted);
    CHECK(!shorter);
    CHECK(strcmp(new_root->key, "f") == 0); /* f's key moved into d's slot */
    CHECK(new_root->value == (void *)0xF);
    CHECK(new_root->color == BLACK);
    CHECK(new_root->left == b);
    CHECK(new_root->right == h);
    CHECK(h->left == NULL); /* f's old slot under h is now empty */
    CHECK(h->right == NULL);

    rb_free(new_root->key); /* frees f's key, now owned by d's slot */
    checked_free(test_pool, new_root); /* frees d's struct; f's struct was already freed
                                       inside extract_min */
    rb_free(b->key);
    checked_free(test_pool, b);
    rb_free(h->key);
    checked_free(test_pool, h);
}

/* Two children; the in-order successor is a black node with one red child,
 * so extracting it is a no-deficit case-6 removal one level down. */
static void test_delete_rec_two_children_successor_has_red_child(void) {
    struct rb_node *b = mknode("b", (void *)0xB, BLACK, NULL, NULL);
    struct rb_node *p = mknode("p", (void *)0x77, RED, NULL, NULL);
    struct rb_node *k = mknode("k", (void *)0x99, BLACK, NULL, p);
    struct rb_node *d = mknode("d", (void *)0xD, BLACK, b, k);
    CHECK(b != NULL && p != NULL && k != NULL && d != NULL);
    if (b == NULL || p == NULL || k == NULL || d == NULL) {
        return;
    }

    bool deleted = false;
    bool shorter = true;
    struct rb_node *new_root = delete_rec(d, "d", NULL, &deleted, &shorter, test_pool);

    CHECK(new_root == d);
    CHECK(deleted);
    CHECK(!shorter);
    CHECK(strcmp(new_root->key, "k") == 0); /* k's key moved into d's slot */
    CHECK(new_root->value == (void *)0x99);
    CHECK(new_root->left == b);
    CHECK(new_root->right == p); /* p absorbed k's black and moved up */
    CHECK(p->color == BLACK);
    CHECK(p->left == NULL && p->right == NULL);

    rb_free(new_root->key); /* frees k's key, now owned by d's slot */
    checked_free(test_pool, new_root); /* frees d's struct; k's struct was already freed
                                       inside extract_min */
    rb_free(b->key);
    checked_free(test_pool, b);
    rb_free(p->key);
    checked_free(test_pool, p);
}

/* Black leaf deleted under a red parent whose other child is a black
 * sibling with two black nephews: case 5 fires and the red parent absorbs
 * the deficit locally (wiring test for the n->left = delete_rec(...); if
 * (*shorter) fixup_left_deficit(...); path). */
static void test_delete_rec_black_leaf_case_5_absorbed_by_red_parent(void) {
    struct rb_node c = {.key = "c", .color = BLACK}; /* sibling, untouched by frees */
    struct rb_node p = {.key = "p", .left = NULL, .right = &c, .color = RED};
    struct rb_node *a = mknode("a", (void *)0xA, BLACK, NULL, NULL);
    CHECK(a != NULL);
    if (a == NULL) {
        return;
    }
    p.left = a;

    bool deleted = false;
    bool shorter = false;
    struct rb_node *new_root = delete_rec(&p, "a", NULL, &deleted, &shorter, test_pool);

    CHECK(new_root == &p);
    CHECK(deleted);
    CHECK(!shorter);
    CHECK(p.color == BLACK); /* recolored to absorb the deficit */
    CHECK(p.left == NULL);
    CHECK(p.right == &c);
    CHECK(c.color == RED); /* sibling recolored red */
    /* a was fully freed inside delete_rec; nothing left to free here. */
}

/* --- rb_delete (public API) ---------------------------------------------
 * Black-box tests through rb_create/rb_insert/rb_delete/rb_destroy, using
 * trees small enough that no delete in them can require a rotation. */

/* Public API: deleting an absent key returns -1 and leaves the tree untouched. */
static void test_rb_delete_absent_key_leaves_tree_unchanged(void) {
    rbtree_t *t = rb_create(NULL);
    CHECK(t != NULL);
    if (t == NULL) {
        return;
    }

    CHECK(rb_insert(t, "b", (void *)0xB) == 0);
    CHECK(rb_insert(t, "a", (void *)0xA) == 0);
    CHECK(rb_insert(t, "c", (void *)0xC) == 0);

    CHECK(rb_delete(t, "z") == -1);
    CHECK(rb_size(t) == 3);
    CHECK(rb_validate(t) == 0);
    CHECK(rb_find(t, "a") == (void *)0xA);
    CHECK(rb_find(t, "b") == (void *)0xB);
    CHECK(rb_find(t, "c") == (void *)0xC);

    rb_destroy(t);
}

/* Public API: deleting from an empty tree returns -1. */
static void test_rb_delete_empty_tree_returns_error(void) {
    rbtree_t *t = rb_create(NULL);
    CHECK(t != NULL);
    if (t == NULL) {
        return;
    }

    CHECK(rb_delete(t, "anything") == -1);
    CHECK(rb_size(t) == 0);

    rb_destroy(t);
}

/* Public API: deleting the only node empties the tree, which remains usable. */
static void test_rb_delete_only_node_empties_tree_and_is_reusable(void) {
    rbtree_t *t = rb_create(NULL);
    CHECK(t != NULL);
    if (t == NULL) {
        return;
    }

    CHECK(rb_insert(t, "x", (void *)0x1234) == 0);
    CHECK(rb_delete(t, "x") == 0);
    CHECK(rb_size(t) == 0);
    CHECK(t->root == NULL);
    CHECK(rb_validate(t) == 0);
    CHECK(rb_find(t, "x") == NULL);

    /* the tree must still be usable after emptying */
    CHECK(rb_insert(t, "y", (void *)0x5678) == 0);
    CHECK(rb_size(t) == 1);
    CHECK(rb_validate(t) == 0);
    CHECK(rb_find(t, "y") == (void *)0x5678);

    rb_destroy(t);
}

/* Public API: deleting a key frees its value exactly once. */
static void test_rb_delete_frees_value_exactly_once(void) {
    rbtree_t *t = rb_create(counting_value_free);
    CHECK(t != NULL);
    if (t == NULL) {
        return;
    }

    CHECK(rb_insert(t, "k", (void *)0x1) == 0);
    free_call_count = 0;

    CHECK(rb_delete(t, "k") == 0);
    CHECK(free_call_count == 1);
    CHECK(rb_size(t) == 0);
    CHECK(rb_validate(t) == 0);

    rb_destroy(t);
}

/* Builds the minimal 3-node tree (root black, both children red leaves —
 * insert_fixup's red-uncle check requires a red grandchild too, so two red
 * leaves under a black root triggers no rotation), then deletes both red
 * leaves via the public API and checks ordering and size throughout. */
static void test_rb_delete_red_leaves_preserve_order(void) {
    rbtree_t *t = rb_create(NULL);
    CHECK(t != NULL);
    if (t == NULL) {
        return;
    }

    CHECK(rb_insert(t, "b", NULL) == 0);
    CHECK(rb_insert(t, "a", NULL) == 0);
    CHECK(rb_insert(t, "c", NULL) == 0);
    CHECK(rb_validate(t) == 0);
    CHECK(t->root != NULL && strcmp(t->root->key, "b") == 0);
    CHECK(t->root->color == BLACK);

    CHECK(rb_delete(t, "a") == 0);
    CHECK(rb_size(t) == 2);
    CHECK(rb_validate(t) == 0);

    struct collect_ctx c1 = {.count = 0};
    rb_foreach(t, collect_key, &c1);
    CHECK(c1.count == 2);
    if (c1.count == 2) {
        CHECK(strcmp(c1.keys[0], "b") == 0);
        CHECK(strcmp(c1.keys[1], "c") == 0);
    }

    CHECK(rb_delete(t, "c") == 0);
    CHECK(rb_size(t) == 1);
    CHECK(rb_validate(t) == 0);

    struct collect_ctx c2 = {.count = 0};
    rb_foreach(t, collect_key, &c2);
    CHECK(c2.count == 1);
    if (c2.count == 1) {
        CHECK(strcmp(c2.keys[0], "b") == 0);
    }

    rb_destroy(t);
}

/* --- allocation fault sweep ---------------------------------------------
 * One seeded scenario -- rb_create, SWEEP_OPS random insert/overwrite/
 * delete calls, then rb_destroy -- is dry-run to count its allocations k
 * and note which call makes each one, then rerun k times with
 * fault_alloc_arm(n) for n = 1..k. The RNG is private and reseeded every
 * run, so every run issues the same call sequence and the dry run's note
 * says exactly which call allocation n will fail in. That call -- whatever
 * kind it is -- gets the run's one snapshot taken just before it, and must
 * leave the tree's structure and ownership identical.
 *
 * The key space is larger than a slab and inserts outnumber deletes, so
 * live nodes climb past per_slab and the pool must grow mid-run (the dry
 * run checks this): every grow is then also a fault point with a non-empty
 * tree. After every call the tree's pool must have moved exactly the slot
 * the call implies -- one taken by a new key, one returned by a delete,
 * none on an overwrite or a failure -- and hold one live slot per key. */

#define SWEEP_OPS 1000
/* > per_slab (102 on macOS, 85 on Linux); 70% inserts settle near
 * 0.7 * 256 ~ 179 live keys, so the pool reaches a second slab */
#define SWEEP_KEYS 256u
#define SWEEP_INSERT_PCT 70u
#define SWEEP_KEY_LEN 16
/* rb_create (tree + pool) + 2 per insert (key copy, and a slab grow at most) */
#define SWEEP_MAX_ALLOCS (2 + 2 * SWEEP_OPS)
#define SWEEP_OP_CREATE (-1) /* the rb_create call, as a call index */
#define SWEEP_NO_FAULT (-2)  /* dry run: no call is expected to fail */

/* Everything about one node that a failed call could disturb. */
struct sweep_node_snap {
    const struct rb_node *node; /* identity of the node allocation */
    const char *key;            /* identity of the key-copy allocation */
    char key_text[SWEEP_KEY_LEN];
    void *value; /* which value the node owns */
    const struct rb_node *left;
    const struct rb_node *right;
    rb_color_t color;
};

struct sweep_snapshot {
    const struct rb_node *root;
    size_t size;
    int free_calls; /* values the tree has released via value_free so far */
    size_t count;   /* nodes reached from root */
    struct sweep_node_snap nodes[SWEEP_KEYS];
};

/* sweep_alloc_op[i] is the call that made the dry run's (i+1)-th allocation. */
static int sweep_alloc_op[SWEEP_MAX_ALLOCS];
static long sweep_alloc_count;

static uint32_t sweep_rng;

/* xorshift32: private so the sweep neither consumes nor depends on rand(). */
static uint32_t sweep_next(void) {
    sweep_rng ^= sweep_rng << 13;
    sweep_rng ^= sweep_rng >> 17;
    sweep_rng ^= sweep_rng << 5;
    return sweep_rng;
}

static void sweep_key(unsigned k, char *buf) {
    snprintf(buf, SWEEP_KEY_LEN, "k%02u", k);
}

/* Pre-order walk straight over the node structs (not rb_foreach), so the
 * snapshot records shape and addresses rather than just contents. */
static void sweep_walk(const struct rb_node *n, struct sweep_snapshot *s) {
    if (n == NULL) {
        return;
    }
    if (s->count < SWEEP_KEYS) {
        struct sweep_node_snap *snap = &s->nodes[s->count];
        snap->node = n;
        snap->key = n->key;
        snprintf(snap->key_text, SWEEP_KEY_LEN, "%s", n->key);
        snap->value = n->value;
        snap->left = n->left;
        snap->right = n->right;
        snap->color = n->color;
    }
    s->count++;
    sweep_walk(n->left, s);
    sweep_walk(n->right, s);
}

static void sweep_take(const rbtree_t *t, struct sweep_snapshot *s) {
    s->root = t->root;
    s->size = t->size;
    s->free_calls = free_call_count;
    s->count = 0;
    sweep_walk(t->root, s);
}

static bool sweep_same(const struct sweep_snapshot *a,
                       const struct sweep_snapshot *b) {
    if (a->root != b->root || a->size != b->size ||
        a->free_calls != b->free_calls || a->count != b->count ||
        a->count > SWEEP_KEYS) {
        return false;
    }
    for (size_t i = 0; i < a->count; i++) {
        const struct sweep_node_snap *x = &a->nodes[i];
        const struct sweep_node_snap *y = &b->nodes[i];
        if (x->node != y->node || x->key != y->key ||
            strcmp(x->key_text, y->key_text) != 0 || x->value != y->value ||
            x->left != y->left || x->right != y->right ||
            x->color != y->color) {
            return false;
        }
    }
    return true;
}

/* Dry run only: notes that call `op` made every allocation since `since`. */
static void sweep_attribute(long since, int op) {
    /* invariant: allocations before `a` are already attributed to a call */
    for (long a = since; a < fault_alloc_total(); a++) {
        if (sweep_alloc_count < SWEEP_MAX_ALLOCS) {
            sweep_alloc_op[sweep_alloc_count] = op;
        }
        sweep_alloc_count++;
    }
}

/* Runs the scenario once. fault_op is the call the armed fault will land in
 * (SWEEP_NO_FAULT for the dry run, which also fills sweep_alloc_op). Returns
 * how many calls reported an allocation failure: rb_create returning NULL,
 * rb_insert returning -1, or rb_delete refusing a key that is present. */
static int sweep_run(int fault_op) {
    struct {
        bool present;
        void *value;
    } model[SWEEP_KEYS] = {0};
    size_t model_size = 0;
    size_t peak_size = 0; /* most keys live at once: inserts - deletes */
    int failures_seen = 0;
    struct sweep_snapshot before = {0};
    bool dry_run = fault_op == SWEEP_NO_FAULT;

    sweep_rng = 0x2545F491u;
    free_call_count = 0;
    if (dry_run) {
        sweep_alloc_count = 0;
    }

    long allocs_before = fault_alloc_total();
    rbtree_t *t = rb_create(counting_value_free);
    if (t == NULL) {
        return 1; /* create failed; nothing was allocated that could leak */
    }
    if (dry_run) {
        sweep_attribute(allocs_before, SWEEP_OP_CREATE);
    }

    /* invariant: model[]/model_size describe t exactly, and free_call_count
     * equals the number of values t has handed to value_free so far. */
    for (int op = 0; op < SWEEP_OPS; op++) {
        unsigned k = sweep_next() % SWEEP_KEYS;
        unsigned kind = sweep_next() % 100;
        char key[SWEEP_KEY_LEN];
        sweep_key(k, key);
        int frees_before = free_call_count;
        bool alloc_failed = false;
        int dlive = 0; /* pool slots this call should take (+1) or return (-1) */
        struct pool_counts pool_before = pool_check(t->pool);
        allocs_before = fault_alloc_total();

        if (op == fault_op) {
            sweep_take(t, &before); /* the run's only "before" snapshot */
        }

        if (kind < SWEEP_INSERT_PCT) {
            void *value = (void *)(uintptr_t)(op + 1); /* unique, non-NULL */
            if (rb_insert(t, key, value) == 0) {
                /* an overwrite releases exactly the old value; a new key
                 * releases nothing */
                CHECK(free_call_count ==
                      frees_before + (model[k].present ? 1 : 0));
                if (!model[k].present) {
                    model_size++;
                    dlive = 1; /* only a new key takes a node slot */
                }
                model[k].present = true;
                model[k].value = value; /* ownership of value moved to t */
            } else {
                alloc_failed = true; /* value not consumed; still ours */
            }
        } else {
            int rc = rb_delete(t, key);
            if (!model[k].present) {
                CHECK(rc == -1);
                CHECK(free_call_count == frees_before);
                /* -1 is rb_delete's only error code, so a fault landing
                 * here surfaces as the same -1 that "absent" does */
                alloc_failed = op == fault_op;
            } else if (rc == 0) {
                CHECK(free_call_count == frees_before + 1);
                model[k].present = false; /* t released the node, key, value */
                model_size--;
                dlive = -1;
            } else {
                /* a present key was not removed: only a failed allocation
                 * explains that, so t must still own all of it */
                alloc_failed = true;
            }
        }

        if (op == fault_op) {
            struct sweep_snapshot after = {0};
            sweep_take(t, &after);
            CHECK(alloc_failed); /* the injected fault surfaced as an error */
            /* same nodes, same key copies, same values, same links and
             * colors, and nothing handed to value_free */
            CHECK(sweep_same(&before, &after));
            CHECK(free_list_ok(t->pool)); /* failure left every free slot intact */
        } else {
            CHECK(!alloc_failed);
        }
        failures_seen += alloc_failed;
        if (dry_run) {
            sweep_attribute(allocs_before, op);
        }

        CHECK(rb_validate(t) == 0);
        CHECK(rb_size(t) == model_size);
        if (dlive < 0) {
            check_free_delta(t->pool, pool_before);
        } else {
            check_alloc_delta(t->pool, pool_before, dlive > 0);
        }
        CHECK(pool_check(t->pool).live == rb_size(t)); /* one slot per key */
        if (model_size > peak_size) {
            peak_size = model_size;
        }
    }
    if (dry_run) {
        /* the scenario really did outgrow one slab */
        CHECK(peak_size > t->pool->per_slab);
        CHECK(pool_check(t->pool).slabs >= 2);
    }

    /* invariant: every key below k has been checked against the model */
    for (unsigned k = 0; k < SWEEP_KEYS; k++) {
        char key[SWEEP_KEY_LEN];
        sweep_key(k, key);
        CHECK(rb_find(t, key) == (model[k].present ? model[k].value : NULL));
    }

    int frees_before = free_call_count;
    rb_destroy(t); /* t's nodes, key copies, pool, and tree struct freed;
                      checked_destroy confirms every slot came back first */
    CHECK(free_call_count == frees_before + (int)model_size);
    return failures_seen;
}

static void test_fault_sweep(void) {
    long before = fault_alloc_total();
    CHECK(sweep_run(SWEEP_NO_FAULT) == 0); /* dry run: nothing may fail */
    long total = fault_alloc_total() - before;
    CHECK(total > 0);
    /* every allocation was attributed to the call that made it */
    CHECK(total == sweep_alloc_count && total <= SWEEP_MAX_ALLOCS);
    if (total != sweep_alloc_count || total > SWEEP_MAX_ALLOCS) {
        return;
    }

    /* invariant: runs 1..n-1 each injected exactly one failure, and every
     * check around it passed or was reported with its n */
    for (long n = 1; n <= total; n++) {
        int failures_before = failures;
        long start = fault_alloc_total();

        fault_alloc_arm(n);
        int seen = sweep_run(sweep_alloc_op[n - 1]);
        fault_alloc_disarm();

        CHECK(fault_alloc_total() - start >= n); /* the n-th alloc happened */
        CHECK(seen == 1);
        if (failures != failures_before) {
            fprintf(stderr, "  ^ fault sweep: failed with n=%ld of %ld\n", n,
                    total);
        }
    }
}

/* --- slab pool (src/pool.c) ---------------------------------------------
 * Every pool_alloc/pool_free below goes through checked_alloc/checked_free,
 * which assert the slot-accounting invariant before and after the call and
 * the exact counter change the call must cause. The pointer-validity checks
 * deliberately kept out of pool.c live here instead.
 *
 * Use-after-free detection: every free slot carries POOL_POISON in each byte
 * past its free-list link. checked_free poisons a slot before returning it,
 * checked_alloc poisons a freshly grown slab's other slots, and every slot
 * handed back out is checked for intact poison first -- so any write through
 * a dangling pointer while the slot sat on the free list is caught at reuse
 * (or sooner, by free_list_ok). The sanitizers can't see these writes: the
 * whole slab is one live malloc block. */

/* Slot 0 is aligned iff the in-band header size preserves malloc's alignment. */
static_assert(offsetof(struct slab, mem) % alignof(max_align_t) == 0,
              "slab header must preserve max_align_t alignment");

#define POOL_HEADER offsetof(struct slab, mem)
#define POOL_MAX_HELD 1024u /* > 2 * per_slab for any obj_size >= 1 */
#define POOL_SWEEP_OPS 600  /* ~70% allocs: spans at least two slabs */
#define POOL_POISON 0xDD    /* fill for free slots, past the link */

/* struct pool_counts is defined at the top of the file; the sweep uses it. */

/* Reads stats and checks the slot-accounting invariant. */
static struct pool_counts pool_check(const rb_pool_t *p) {
    struct pool_counts c;
    pool_stats(p, &c.slabs, &c.live, &c.free_objs);
    CHECK(c.live + c.free_objs == c.slabs * p->per_slab);
    return c;
}

/* True iff obj is exactly a slot boundary inside one of p's slabs. */
static bool slot_owned(const rb_pool_t *p, const void *obj) {
    uintptr_t a = (uintptr_t)obj; /* integer compare: relational ops across distinct objects are UB */
    /* invariant: obj lies in none of the slabs visited before s */
    for (const struct slab *s = p->slabs; s != NULL; s = s->next) {
        uintptr_t lo = (uintptr_t)s->mem;
        uintptr_t hi = lo + p->per_slab * p->slot_size;
        if (a >= lo && a < hi) {
            return (a - lo) % p->slot_size == 0; /* rejects mid-slot pointers */
        }
    }
    return false;
}

/* Not handed out (failure, or a tree call that takes no slot): nothing
 * changes. Handed out: live+1, and free-1 unless the call grew the pool, in
 * which case slabs+1 and free jumps by a whole slab minus one. */
static void check_alloc_delta(const rb_pool_t *p, struct pool_counts b,
                              bool handed_out) {
    struct pool_counts a = pool_check(p);
    if (!handed_out) {
        CHECK(a.slabs == b.slabs && a.live == b.live &&
              a.free_objs == b.free_objs);
    } else if (a.slabs == b.slabs + 1) {
        CHECK(b.free_objs == 0); /* only an empty free list may grow */
        CHECK(a.live == b.live + 1 &&
              a.free_objs == b.free_objs + p->per_slab - 1);
    } else {
        CHECK(a.slabs == b.slabs && a.live == b.live + 1 &&
              a.free_objs == b.free_objs - 1);
    }
}

/* pool_free of a live slot: live-1, free+1, slabs unchanged. */
static void check_free_delta(const rb_pool_t *p, struct pool_counts b) {
    struct pool_counts a = pool_check(p);
    CHECK(a.slabs == b.slabs && a.live == b.live - 1 &&
          a.free_objs == b.free_objs + 1);
}

/* Fills every byte of obj's slot past the free-list link with POOL_POISON;
 * the link's bytes belong to the pool and are left alone. */
static void poison_slot(const rb_pool_t *p, void *obj) {
    memset((unsigned char *)obj + sizeof(struct free_slot), POOL_POISON,
           p->slot_size - sizeof(struct free_slot));
}

/* True iff every byte of obj's slot past the link still holds POOL_POISON. */
static bool poison_intact(const rb_pool_t *p, const void *obj) {
    const unsigned char *bytes = obj;
    /* invariant: bytes[link..j-1] all hold POOL_POISON */
    for (size_t j = sizeof(struct free_slot); j < p->slot_size; j++) {
        if (bytes[j] != POOL_POISON) {
            return false;
        }
    }
    return true;
}

/* Poisons every slot of the newest slab except got, which the caller is
 * about to own: fresh free slots then look exactly like recycled ones. */
static void poison_fresh_slab(const rb_pool_t *p, const void *got) {
    /* invariant: slots 0..i-1 of the newest slab, other than got, are poisoned */
    for (size_t i = 0; i < p->per_slab; i++) {
        void *slot = &p->slabs->mem[i * p->slot_size];
        if (slot != got) {
            poison_slot(p, slot);
        }
    }
}

/* Walks the free list without trusting it: each entry must be an owned slot
 * (checked before its link is followed) with intact poison, and the walk
 * must end after exactly nfree entries, so a cycle or lost tail fails too. */
static bool free_list_ok(const rb_pool_t *p) {
    size_t count = 0;
    /* invariant: the count entries before s were owned, poisoned slots */
    for (const struct free_slot *s = p->free; s != NULL; s = s->next) {
        if (count == p->nfree || !slot_owned(p, s) || !poison_intact(p, s)) {
            return false;
        }
        count++;
    }
    return count == p->nfree;
}

/* pool_alloc wrapped in the invariant, delta, ownership, alignment and
 * poison checks. */
static void *checked_alloc(rb_pool_t *p) {
    if (!pool_checks) {
        return pool_alloc(p); /* caller now owns the slot (if non-NULL) */
    }
    struct pool_counts b = pool_check(p);
    void *got = pool_alloc(p);
    check_alloc_delta(p, b, got != NULL);
    if (got != NULL) {
        CHECK(slot_owned(p, got));
        CHECK((uintptr_t)got % alignof(max_align_t) == 0);
        if (pool_check(p).slabs > b.slabs) {
            poison_fresh_slab(p, got); /* got itself was never freed */
        } else {
            CHECK(poison_intact(p, got)); /* nothing wrote to it while free */
        }
    }
    return got; /* caller now owns the slot (if non-NULL) */
}

/* pool_free wrapped in the same checks; refuses pointers the pool doesn't
 * own, and poisons the slot on its way back. */
static void checked_free(rb_pool_t *p, void *obj) {
    if (!pool_checks) {
        pool_free(p, obj); /* slot ownership returns to the pool */
        return;
    }
    bool owned = slot_owned(p, obj);
    CHECK(owned);
    if (!owned) {
        return; /* a foreign pointer would poison the free list */
    }
    struct pool_counts b = pool_check(p);
    poison_slot(p, obj); /* still ours: mark everything past the link freed */
    pool_free(p, obj);   /* slot ownership returns to the pool */
    check_free_delta(p, b);
}

/* pool_destroy for pools whose every slot should be back: a live slot is a
 * leaked node (pool_destroy would release it silently, hiding it from
 * Valgrind), and the final free-list walk catches use-after-free writes to
 * slots that were never handed out again. */
static void checked_destroy(rb_pool_t *p) {
    if (p != NULL) {
        CHECK(pool_check(p).live == 0);
        CHECK(!pool_checks || free_list_ok(p)); /* unchecked frees left no poison */
    }
    pool_destroy(p); /* every slab and p freed */
}

static void test_pool_create_rejects_bad_sizes(void) {
    long before = fault_alloc_total();
    CHECK(pool_create(0) == NULL);
    CHECK(pool_create(SLAB_BYTES - POOL_HEADER + 1) == NULL); /* can't fit one slot */
    CHECK(pool_create(SIZE_MAX) == NULL); /* would overflow round_up unguarded */
    CHECK(fault_alloc_total() == before); /* rejected before any allocation */
}

static void check_pool_geometry(size_t obj_size) {
    rb_pool_t *p = pool_create(obj_size);
    CHECK(p != NULL);
    if (p == NULL) {
        return;
    }
    CHECK(p->slot_size >= obj_size);
    CHECK(p->slot_size >= sizeof(struct free_slot)); /* room for the link */
    CHECK(p->slot_size % alignof(max_align_t) == 0);
    CHECK(p->per_slab >= 1);
    CHECK(POOL_HEADER + p->per_slab * p->slot_size <= SLAB_BYTES);
    CHECK(POOL_HEADER + (p->per_slab + 1) * p->slot_size > SLAB_BYTES); /* maximal */
    struct pool_counts c = pool_check(p);
    CHECK(c.slabs == 0 && c.live == 0 && c.free_objs == 0); /* slabs are lazy */
    pool_destroy(p); /* p freed; it owned no slabs */
}

static void test_pool_create_geometry(void) {
    check_pool_geometry(sizeof(struct rb_node));
    check_pool_geometry(1);
    check_pool_geometry(SLAB_BYTES - POOL_HEADER); /* largest accepted size */

    rb_pool_t *p = pool_create(SLAB_BYTES - POOL_HEADER);
    CHECK(p != NULL && p->per_slab == 1);
    pool_destroy(p); /* p freed; it owned no slabs */
}

static void test_pool_alloc_grows_and_reuses_lifo(void) {
    rb_pool_t *p = pool_create(sizeof(struct rb_node));
    CHECK(p != NULL);
    if (p == NULL) {
        return;
    }
    size_t n = p->per_slab + 1; /* one past a full slab forces a second slab */
    CHECK(n <= POOL_MAX_HELD);
    void *held[POOL_MAX_HELD];

    /* invariant: held[0..i-1] are live, and those from the first slab are
     * consecutive slots in ascending address order */
    for (size_t i = 0; i < n; i++) {
        held[i] = checked_alloc(p); /* test owns held[i] */
        CHECK(held[i] != NULL);
        if (i > 0 && i < p->per_slab) {
            CHECK((uintptr_t)held[i] == (uintptr_t)held[i - 1] + p->slot_size);
        }
    }
    CHECK(pool_check(p).slabs == 2);
    /* invariant: the second slab's slot differs from every slot before j */
    for (size_t j = 0; j + 1 < n; j++) {
        CHECK(held[j] != held[n - 1]);
    }

    /* LIFO: the last slot freed is the next one handed out */
    void *a = held[1];
    void *b = held[n / 2];
    checked_free(p, a);
    checked_free(p, b);
    CHECK(checked_alloc(p) == b);
    CHECK(checked_alloc(p) == a); /* test owns a and b again */

    /* invariant: held[i..n-1] are still live */
    for (size_t i = 0; i < n; i++) {
        checked_free(p, held[i]);
    }
    struct pool_counts c = pool_check(p);
    CHECK(c.live == 0 && c.slabs == 2 && c.free_objs == 2 * p->per_slab);

    /* freed slots from both slabs are reused before the pool grows again */
    size_t full = 2 * p->per_slab;
    CHECK(full <= POOL_MAX_HELD);
    /* invariant: held[0..i-1] are live and no third slab exists */
    for (size_t i = 0; i < full; i++) {
        held[i] = checked_alloc(p);
        CHECK(held[i] != NULL);
    }
    CHECK(pool_check(p).slabs == 2);
    /* invariant: held[i..full-1] are still live */
    for (size_t i = 0; i < full; i++) {
        checked_free(p, held[i]);
    }
    pool_destroy(p); /* both slabs and p freed */
}

static unsigned char slot_pattern(size_t i) {
    return (unsigned char)(i % 251 + 1); /* adjacent slots always differ */
}

/* Fills every byte of every slot in two slabs, then confirms each slot kept
 * its own pattern: slots tile without overlapping each other or the header
 * (pool_destroy then walks the in-band links, which a stray write would have
 * corrupted; ASan catches anything past the 4096-byte block). */
static void test_pool_full_slot_writes(void) {
    rb_pool_t *p = pool_create(sizeof(struct rb_node));
    CHECK(p != NULL);
    if (p == NULL) {
        return;
    }
    size_t n = 2 * p->per_slab;
    CHECK(n <= POOL_MAX_HELD);
    void *held[POOL_MAX_HELD];

    /* invariant: held[0..i-1] are live and filled with their own pattern */
    for (size_t i = 0; i < n; i++) {
        held[i] = checked_alloc(p); /* test owns held[i] */
        CHECK(held[i] != NULL);
        if (held[i] == NULL) {
            n = i; /* only free what was handed out */
            break;
        }
        memset(held[i], slot_pattern(i), p->slot_size);
    }
    /* invariant: slots 0..i-1 still hold exactly their own pattern */
    for (size_t i = 0; i < n; i++) {
        const unsigned char *bytes = held[i];
        bool intact = true;
        /* invariant: bytes[0..j-1] match slot i's pattern */
        for (size_t j = 0; j < p->slot_size; j++) {
            intact = intact && bytes[j] == slot_pattern(i);
        }
        CHECK(intact);
    }
    /* invariant: held[i..n-1] are still live */
    for (size_t i = 0; i < n; i++) {
        checked_free(p, held[i]);
    }
    pool_destroy(p); /* both slabs and p freed */
}

static void test_pool_null_handling(void) {
    pool_destroy(NULL); /* no-op */

    rb_pool_t *p = pool_create(sizeof(struct rb_node));
    CHECK(p != NULL);
    if (p == NULL) {
        return;
    }
    void *obj = checked_alloc(p); /* test owns obj */
    CHECK(obj != NULL);

    struct pool_counts b = pool_check(p);
    pool_free(p, NULL); /* no-op: nothing to return */
    struct pool_counts a = pool_check(p);
    CHECK(a.slabs == b.slabs && a.live == b.live && a.free_objs == b.free_objs);

    size_t live = 0;
    pool_stats(p, NULL, &live, NULL); /* NULL out-params are skipped */
    CHECK(live == 1);
    pool_stats(p, NULL, NULL, NULL);

    checked_free(p, obj);
    pool_destroy(p); /* the slab and p freed */
}

/* A freed node's slot holds the free-list link followed by poison, so a
 * dangling read sees 0xDD bytes instead of the node's old contents. */
static void test_pool_poison_fills_freed_slot(void) {
    rb_pool_t *p = pool_create(sizeof(struct rb_node));
    CHECK(p != NULL);
    if (p == NULL) {
        return;
    }
    struct rb_node *n = checked_alloc(p); /* test owns n */
    CHECK(n != NULL);
    if (n == NULL) {
        pool_destroy(p);
        return;
    }
    memset(n, 0xAA, sizeof *n); /* stand-in for live node contents */

    const struct free_slot *head_before = p->free;
    checked_free(p, n); /* n now dangles; its slot is the pool's */

    CHECK(poison_intact(p, n));
    struct free_slot link;
    memcpy(&link, n, sizeof link); /* the first bytes are the link, not poison */
    CHECK(link.next == head_before);
    CHECK(p->free == (const struct free_slot *)n); /* n's slot is the new head */

    /* what a stale n->value read would observe: poison, not 0xAA */
    void *stale_value;
    void *poison_value;
    memcpy(&stale_value, &n->value, sizeof stale_value);
    memset(&poison_value, POOL_POISON, sizeof poison_value);
    CHECK(memcmp(&stale_value, &poison_value, sizeof stale_value) == 0);

    CHECK(free_list_ok(p));
    pool_destroy(p); /* the slab and p freed */
}

/* Poison written at free survives untouched until the slot is handed out
 * again, across both slabs and in any free order. */
static void test_pool_poison_holds_until_reuse(void) {
    rb_pool_t *p = pool_create(sizeof(struct rb_node));
    CHECK(p != NULL);
    if (p == NULL) {
        return;
    }
    size_t n = 2 * p->per_slab;
    CHECK(n <= POOL_MAX_HELD);
    void *held[POOL_MAX_HELD];

    /* invariant: held[0..i-1] are live and filled with non-poison bytes */
    for (size_t i = 0; i < n; i++) {
        held[i] = checked_alloc(p); /* test owns held[i] */
        CHECK(held[i] != NULL);
        if (held[i] == NULL) {
            n = i; /* only free what was handed out */
            break;
        }
        memset(held[i], 0xAA, p->slot_size);
    }
    /* free evens then odds, so the free list interleaves both slabs */
    /* invariant: every even index below i has been freed */
    for (size_t i = 0; i < n; i += 2) {
        checked_free(p, held[i]);
    }
    /* invariant: every odd index below i has been freed */
    for (size_t i = 1; i < n; i += 2) {
        checked_free(p, held[i]);
    }
    CHECK(free_list_ok(p));

    /* checked_alloc verifies each recycled slot's poison before handing it out */
    /* invariant: held[0..i-1] are live again, each found with intact poison */
    for (size_t i = 0; i < n; i++) {
        held[i] = checked_alloc(p);
        CHECK(held[i] != NULL);
    }
    CHECK(pool_check(p).slabs == 2); /* all reuse, no growth */
    CHECK(p->free == NULL && free_list_ok(p));

    /* invariant: held[i..n-1] are still live */
    for (size_t i = 0; i < n; i++) {
        checked_free(p, held[i]);
    }
    CHECK(free_list_ok(p));
    pool_destroy(p); /* both slabs and p freed */
}

/* Deliberate writes through a dangling pointer, standing in for an rbtree
 * use-after-free: the detectors must flag each one. Each write lands inside
 * a live slab, which is exactly the case ASan and Valgrind cannot see. */
static void test_pool_poison_detects_write_after_free(void) {
    rb_pool_t *p = pool_create(sizeof(struct rb_node));
    CHECK(p != NULL);
    if (p == NULL) {
        return;
    }
    struct rb_node *n = checked_alloc(p);    /* test owns n */
    struct rb_node *keep = checked_alloc(p); /* test owns keep */
    CHECK(n != NULL && keep != NULL);
    if (n == NULL || keep == NULL) {
        pool_free(p, n);
        pool_free(p, keep);
        pool_destroy(p);
        return;
    }
    checked_free(p, n); /* n dangles from here on */
    CHECK(free_list_ok(p));

    /* 1. a stale write past the link (n->value) breaks the poison */
    n->value = (void *)0x1234;
    CHECK(!poison_intact(p, n));
    CHECK(!free_list_ok(p));
    poison_slot(p, n); /* repair so the pool stays usable */
    CHECK(free_list_ok(p));

    /* 2. a stale write to n->key lands on the link itself, which poison can't
     * cover; the free-list walk catches it because the link stops pointing at
     * an owned slot. Written as a free_slot so the walk's read of the link
     * stays well-typed; it stands in for `n->key = (char *)0x10`. */
    struct free_slot saved;
    memcpy(&saved, n, sizeof saved);
    struct free_slot bogus = {.next = (struct free_slot *)(uintptr_t)0x10};
    memcpy(n, &bogus, sizeof bogus);
    CHECK(!free_list_ok(p));
    memcpy(n, &saved, sizeof saved); /* repair the link */
    CHECK(free_list_ok(p));

    /* 3. the slot is reused cleanly once repaired: checked_alloc's poison
     * check passes and hands n's slot back (LIFO) */
    CHECK(checked_alloc(p) == n); /* test owns n again */

    checked_free(p, n);
    checked_free(p, keep);
    CHECK(free_list_ok(p));
    pool_destroy(p); /* the slab and p freed */
}

/* Runs the seeded alloc/free scenario once. Returns how many calls reported
 * an allocation failure: pool_create returning NULL or pool_alloc returning
 * NULL. Every run frees everything it holds and destroys the pool, so
 * Valgrind sees any leak on a failure path. */
static int pool_sweep_run(void) {
    void *held[POOL_MAX_HELD];
    size_t nheld = 0;
    int failures_seen = 0;

    sweep_rng = 0x9E3779B9u; /* reseeded: every run makes the same calls up
                                to the injected failure */
    rb_pool_t *p = pool_create(sizeof(struct rb_node));
    if (p == NULL) {
        return 1; /* create failed; nothing was allocated that could leak */
    }

    /* invariant: held[0..nheld-1] are exactly the pool's live slots */
    for (int op = 0; op < POOL_SWEEP_OPS; op++) {
        unsigned kind = sweep_next() % 10;
        if ((kind < 7 || nheld == 0) && nheld < POOL_MAX_HELD) {
            void *got = checked_alloc(p);
            if (got == NULL) {
                failures_seen++; /* pool unchanged (checked); keep going */
            } else {
                held[nheld++] = got; /* test owns the slot */
            }
        } else if (nheld > 0) {
            size_t i = sweep_next() % nheld;
            checked_free(p, held[i]);
            held[i] = held[--nheld];
        }
        CHECK(pool_check(p).live == nheld);
        CHECK(free_list_ok(p)); /* every free slot owned, poisoned, counted */
    }

    /* invariant: held[0..nheld-1] are still live */
    while (nheld > 0) {
        checked_free(p, held[--nheld]);
    }
    CHECK(pool_check(p).live == 0);
    pool_destroy(p); /* every slab and p freed */
    return failures_seen;
}

static void test_pool_fault_sweep(void) {
    long before = fault_alloc_total();
    CHECK(pool_sweep_run() == 0); /* dry run: nothing may fail */
    long total = fault_alloc_total() - before;
    CHECK(total >= 3); /* pool_create plus at least two slabs */

    /* invariant: runs 1..n-1 each injected exactly one failure, and every
     * check around it passed or was reported with its n */
    for (long n = 1; n <= total; n++) {
        int failures_before = failures;
        long start = fault_alloc_total();

        fault_alloc_arm(n);
        int seen = pool_sweep_run();
        fault_alloc_disarm();

        CHECK(fault_alloc_total() - start >= n); /* the n-th alloc happened */
        CHECK(seen == 1);
        if (failures != failures_before) {
            fprintf(stderr, "  ^ pool fault sweep: failed with n=%ld of %ld\n",
                    n, total);
        }
    }
}

/* --- tree pool state ----------------------------------------------------- */

#define POOLED_KEY_LEN 24 /* "p" + up to 20 digits of a size_t + NUL */

static void pooled_key(size_t i, char *buf) {
    snprintf(buf, POOLED_KEY_LEN, "p%04zu", i);
}

/* A tree holds exactly one live slot per key through growth across three
 * slabs, deletes, an overwrite and reinsertion; deleted nodes' slots are
 * reused before the pool grows again; and rb_destroy returns every slot
 * before the pool goes (checked_destroy fails the run otherwise). */
static void test_rb_pool_live_tracks_size(void) {
    rbtree_t *t = rb_create_pooled(counting_value_free);
    CHECK(t != NULL);
    if (t == NULL) {
        return;
    }
    size_t n = 2 * t->pool->per_slab + 5; /* forces a third slab */
    char key[POOLED_KEY_LEN];

    /* invariant: keys 0..i-1 are present, one slot each */
    for (size_t i = 0; i < n; i++) {
        pooled_key(i, key);
        CHECK(rb_insert(t, key, (void *)(uintptr_t)(i + 1)) == 0);
        CHECK(pool_check(t->pool).live == rb_size(t));
    }
    CHECK(rb_validate(t) == 0);
    struct pool_counts full = pool_check(t->pool);
    CHECK(full.slabs == 3 && full.live == n);

    /* invariant: every even key below i has been deleted */
    for (size_t i = 0; i < n; i += 2) {
        pooled_key(i, key);
        CHECK(rb_delete(t, key) == 0);
        CHECK(pool_check(t->pool).live == rb_size(t));
    }
    CHECK(rb_validate(t) == 0);
    CHECK(free_list_ok(t->pool)); /* every freed node poisoned and untouched */

    /* overwriting an existing key takes no slot */
    struct pool_counts b = pool_check(t->pool);
    pooled_key(1, key);
    CHECK(rb_insert(t, key, (void *)(uintptr_t)0xBEEF) == 0);
    check_alloc_delta(t->pool, b, false);

    /* invariant: every even key below i is present again */
    for (size_t i = 0; i < n; i += 2) {
        pooled_key(i, key);
        CHECK(rb_insert(t, key, (void *)(uintptr_t)(i + 1)) == 0);
        CHECK(pool_check(t->pool).live == rb_size(t));
    }
    CHECK(pool_check(t->pool).slabs == 3); /* freed slots reused, no growth */
    CHECK(rb_validate(t) == 0 && rb_size(t) == n);

    int frees_before = free_call_count;
    rb_destroy(t); /* every node slot, key copy, slab, the pool, and t freed */
    CHECK(free_call_count == frees_before + (int)n);
}

/* --- Bounded stack (M3) --------------------------------------------------
 * rb_foreach and rb_destroy promise O(1) auxiliary space with no recursion.
 * Each runs here on a thread that can touch at most 64 KiB of stack, over
 * 2^20-node trees: one built by rb_insert (balanced, ~40 deep, so this one
 * checks scale rather than depth), and one left-only chain built by hand
 * (2^20 deep: a recursive walk would need tens of MiB of stack). */

#define SMALL_STACK_BYTES ((size_t)64 * 1024) /* a multiple of 4 and 16 KiB pages */
#define BIG_N ((size_t)1 << 20)
#define BIG_KEY_LEN 8 /* "%07zu" of at most BIG_N - 1, plus NUL */

/* Runs fn(arg) on a pthread that can touch at most SMALL_STACK_BYTES of
 * stack. glibc refuses stacks under _SC_THREAD_STACK_MIN (128 KiB on
 * aarch64), so the thread gets a region of at least that size whose low end
 * is PROT_NONE: the stack grows down from the top, and any access past the
 * top SMALL_STACK_BYTES faults. glibc also keeps the thread descriptor and
 * static TLS at the top of a caller-supplied stack, so the usable budget is
 * a little under SMALL_STACK_BYTES, never over. Returns false iff fn never
 * ran (the region or thread couldn't be set up). */
static bool run_on_small_stack(void *(*fn)(void *), void *arg) {
    long page_l = sysconf(_SC_PAGESIZE);
    long min_l = sysconf(_SC_THREAD_STACK_MIN);
    if (page_l <= 0) {
        return false;
    }
    size_t page = (size_t)page_l;
    size_t region = SMALL_STACK_BYTES + page; /* at least one guard page */
    if (min_l > 0 && (size_t)min_l > region) {
        region = (size_t)min_l;
    }
    region = (region + page - 1) / page * page;

    unsigned char *base = mmap(NULL, region, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) {
        return false;
    }
    bool ran = false;
    pthread_attr_t attr;
    pthread_t tid;
    if (mprotect(base, region - SMALL_STACK_BYTES, PROT_NONE) != 0) {
        goto cleanup_map;
    }
    if (pthread_attr_init(&attr) != 0) {
        goto cleanup_map;
    }
    if (pthread_attr_setstack(&attr, base, region) == 0 &&
        pthread_create(&tid, &attr, fn, arg) == 0) {
        ran = true;
        CHECK(pthread_join(tid, NULL) == 0);
    }
    pthread_attr_destroy(&attr);
cleanup_map:
    munmap(base, region); /* guard and stack pages released together */
    return ran;
}

static size_t big_frees = 0;

/* value_free for the big trees, whose values are malloc'd size_t indices. */
static void big_value_free(void *value) {
    free(value); /* the index the test malloc'd is freed */
    big_frees++;
}

/* Zero-padded so strcmp order matches numeric order. */
static void big_key(size_t i, char key[BIG_KEY_LEN]) {
    snprintf(key, BIG_KEY_LEN, "%07zu", i);
}

struct big_walk {
    rbtree_t *t;
    size_t visited;
    size_t mismatches; /* visits whose key or value wasn't the expected one */
};

/* rb_foreach callback: keys 0..n-1 are all present, so the i-th visit must
 * be key i holding value i. Reads only its arguments, never the tree. */
static void big_visit(const char *key, void *value, void *ctx) {
    struct big_walk *w = ctx;
    char expected[BIG_KEY_LEN];
    big_key(w->visited, expected);
    if (strcmp(key, expected) != 0 || *(const size_t *)value != w->visited) {
        w->mismatches++;
    }
    w->visited++;
}

static void *big_foreach_thread(void *arg) {
    struct big_walk *w = arg;
    rb_foreach(w->t, big_visit, w);
    return NULL;
}

static void *big_destroy_thread(void *arg) {
    rb_destroy(arg); /* tree relinquishes every node, key copy and value */
    return NULL;
}

/* One node's links and color, recorded before rb_foreach threads the tree. */
struct node_shape {
    const struct rb_node *node;
    const struct rb_node *left;
    const struct rb_node *right;
    rb_color_t color;
};

/* Records every node's shape, pre-order, with a heap stack (test code, so
 * no space bound; and no recursion, since the chain is 2^20 deep). Returns
 * NULL on allocation failure; the caller owns the returned array. */
static struct node_shape *shape_snapshot(const rbtree_t *t) {
    size_t n = rb_size(t);
    struct node_shape *shapes = malloc((n > 0 ? n : 1) * sizeof *shapes);
    const struct rb_node **stack = malloc((n > 0 ? n : 1) * sizeof *stack);
    if (shapes == NULL || stack == NULL) {
        free(shapes);
        free(stack);
        return NULL;
    }
    size_t top = 0;
    size_t out = 0;
    if (t->root != NULL) {
        stack[top++] = t->root;
    }
    /* invariant: shapes[0..out-1] are recorded and stack[0..top-1] holds the
     * roots of the unrecorded subtrees; each node is pushed once, so
     * out + top <= n in a well-formed tree */
    while (top > 0 && out < n) {
        const struct rb_node *x = stack[--top];
        shapes[out++] = (struct node_shape){x, x->left, x->right, x->color};
        if (x->right != NULL && top < n) {
            stack[top++] = x->right;
        }
        if (x->left != NULL && top < n) {
            stack[top++] = x->left;
        }
    }
    free(stack);
    CHECK(out == n && top == 0); /* exactly rb_size nodes reachable */
    return shapes;
}

/* rb_foreach on a 64 KiB stack visits every key in order and leaves every
 * node's links and color exactly as it found them. Compares against the
 * snapshot instead of walking the tree, so a leftover thread (a cycle)
 * fails a CHECK rather than hanging the run. */
static void check_big_foreach(rbtree_t *t) {
    struct node_shape *before = shape_snapshot(t);
    CHECK(before != NULL);
    if (before == NULL) {
        return;
    }
    struct big_walk w = {.t = t};
    CHECK(run_on_small_stack(big_foreach_thread, &w));
    CHECK(w.visited == rb_size(t) && w.mismatches == 0);

    size_t changed = 0;
    /* invariant: changed counts the nodes in before[0..i-1] whose links or
     * color differ from what was recorded */
    for (size_t i = 0; i < rb_size(t); i++) {
        const struct rb_node *x = before[i].node;
        if (x->left != before[i].left || x->right != before[i].right ||
            x->color != before[i].color) {
            changed++;
        }
    }
    CHECK(changed == 0);
    free(before); /* the snapshot array, owned here */
}

/* rb_destroy on a 64 KiB stack frees every value exactly once; the pool's
 * leak check in checked_destroy and Valgrind/ASan cover nodes and keys. If
 * the small-stack thread can't start, t is destroyed here instead so the
 * run stays leak-free. */
static void check_big_destroy(rbtree_t *t) {
    size_t n = rb_size(t);
    big_frees = 0;
    bool ran = run_on_small_stack(big_destroy_thread, t);
    CHECK(ran);
    if (!ran) {
        rb_destroy(t); /* tree relinquishes everything, on the main stack */
    }
    CHECK(big_frees == n);
}

/* 2^20 keys inserted in a scrambled order, each owning a malloc'd index. */
static void test_morris_balanced_tree_on_small_stack(void) {
    pool_checks = false; /* this tree's whole life runs unchecked; see pool_checks */
    rbtree_t *t = rb_create(big_value_free);
    CHECK(t != NULL);
    if (t == NULL) {
        pool_checks = true;
        return;
    }

    bool built = true;
    char key[BIG_KEY_LEN];
    /* invariant: the first i keys of the permutation are in t, each owning
     * a malloc'd copy of its index */
    for (size_t i = 0; i < BIG_N; i++) {
        size_t k = (i * (size_t)2654435761u) % BIG_N; /* odd multiplier: a permutation of 0..BIG_N-1 */
        size_t *v = malloc(sizeof *v);
        if (v == NULL) {
            built = false;
            break;
        }
        *v = k;
        big_key(k, key);
        if (rb_insert(t, key, v) != 0) {
            free(v); /* v not consumed on failure; still ours */
            built = false;
            break;
        }
    }
    CHECK(built);
    if (built) {
        CHECK(rb_size(t) == BIG_N && rb_validate(t) == 0);
        check_big_foreach(t);
        CHECK(rb_validate(t) == 0); /* still a valid red-black tree */
        check_big_destroy(t);
    } else {
        rb_destroy(t); /* tree relinquishes the partial build */
    }
    pool_checks = true;
}

/* 2^20 nodes linked by hand into a left-only chain, largest key at the
 * root: not a valid red-black tree, but a valid BST, and the deepest shape
 * possible. Morris threads every node of it (each node's predecessor is its
 * left child). */
static void test_morris_left_chain_on_small_stack(void) {
    pool_checks = false; /* this tree's whole life runs unchecked; see pool_checks */
    rbtree_t *t = rb_create(big_value_free);
    CHECK(t != NULL);
    if (t == NULL) {
        pool_checks = true;
        return;
    }

    bool built = true;
    char key[BIG_KEY_LEN];
    /* invariant: t->root is a left-only chain of keys 0..i-1, largest at the
     * root, and t->size == i */
    for (size_t i = 0; i < BIG_N; i++) {
        size_t *v = malloc(sizeof *v);
        if (v == NULL) {
            built = false;
            break;
        }
        *v = i;
        big_key(i, key);
        struct rb_node *n = new_node(key, v, t->pool);
        if (n == NULL) {
            free(v); /* new_node frees its own partial work; v is still ours */
            built = false;
            break;
        }
        n->color = BLACK;
        n->left = t->root; /* n takes ownership of the chain built so far */
        t->root = n;       /* tree takes ownership of n (and its value) */
        t->size++;
    }
    CHECK(built);
    if (built) {
        check_big_foreach(t);
        check_big_destroy(t);
    } else {
        rb_destroy(t); /* tree relinquishes the partial chain */
    }
    pool_checks = true;
}

int main(void) {
    test_pool = pool_create(sizeof(struct rb_node)); /* main owns test_pool */
    if (test_pool == NULL) {
        printf("test_pool creation failed\n");
        return 1;
    }

    test_new_node_sets_fields();
    test_new_node_copies_key();
    test_is_red();
    test_rotate_left();
    test_rotate_right();
    test_insert_fixup_red_uncle();
    test_insert_fixup_ll();
    test_insert_fixup_lr();
    test_insert_fixup_rr();
    test_insert_fixup_rl();
    test_insert_fixup_noop_when_n_red();
    test_insert_fixup_noop_when_no_violation();
    test_insert_rec_attaches_new_leaf();
    test_insert_rec_two_level_ll_violation();
    test_rb_create_sets_fields();
    test_rb_create_stores_value_free();
    test_rb_insert_overwrite_frees_old_value();
    test_rb_insert_same_value_twice_frees_nothing();
    test_rb_insert_same_malloced_value_twice();
    test_rb_size_tracks_distinct_keys();
    test_rb_find_returns_value_for_present_key();
    test_rb_find_returns_null_for_absent_key();
    test_rb_find_on_empty_tree_returns_null();
    test_rb_foreach_visits_in_order();
    test_rb_foreach_on_empty_tree_does_nothing();
    test_rb_destroy_frees_all_values();
    test_rb_destroy_null_is_noop();
    test_rb_validate_valid_tree_is_zero();
    test_rb_validate_empty_tree_is_zero();
    test_rb_validate_detects_red_root();
    test_rb_validate_detects_red_red();
    test_rb_validate_detects_black_height_mismatch();
    test_rb_validate_detects_order_violation();
    test_rb_validate_detects_size_mismatch();
    test_fixup_right_deficit_case_b_red_parent();
    test_fixup_right_deficit_case_b_black_parent_propagates();
    test_fixup_left_deficit_case_b_red_parent();
    test_fixup_left_deficit_case_b_black_parent_propagates();
    test_extract_min_walks_to_deepest_left_leaf();
    test_delete_rec_absent_key_leaves_subtree_unchanged();
    test_delete_rec_red_leaf_no_fixup();
    test_delete_rec_black_node_with_one_red_child();
    test_delete_rec_two_children_successor_is_red_leaf();
    test_delete_rec_two_children_successor_has_red_child();
    test_delete_rec_black_leaf_case_5_absorbed_by_red_parent();
    test_rb_delete_absent_key_leaves_tree_unchanged();
    test_rb_delete_empty_tree_returns_error();
    test_rb_delete_only_node_empties_tree_and_is_reusable();
    test_rb_delete_frees_value_exactly_once();
    test_rb_delete_red_leaves_preserve_order();
    test_fixup_right_deficit_case_a_red_sibling();
    test_fixup_left_deficit_case_a_red_sibling();
    test_fixup_right_deficit_case_c_red_far_nephew();
    test_fixup_left_deficit_case_c_red_far_nephew();
    test_fixup_right_deficit_case_d_red_near_nephew();
    test_fixup_left_deficit_case_d_red_near_nephew();
    test_delete_rec_black_leaf_no_black_sibling_case_7();
    test_fault_sweep();
    test_pool_create_rejects_bad_sizes();
    test_pool_create_geometry();
    test_pool_alloc_grows_and_reuses_lifo();
    test_pool_full_slot_writes();
    test_pool_null_handling();
    test_pool_poison_fills_freed_slot();
    test_pool_poison_holds_until_reuse();
    test_pool_poison_detects_write_after_free();
    test_pool_fault_sweep();
    test_rb_pool_live_tracks_size();
    test_morris_balanced_tree_on_small_stack();
    test_morris_left_chain_on_small_stack();

    /* every node a node-level test built went back, and no free slot was
     * written after its free */
    checked_destroy(test_pool); /* every test_pool slab and test_pool freed */

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d check(s) failed\n", failures);
    return 1;
}
