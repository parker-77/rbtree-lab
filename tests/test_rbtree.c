/* Unity build: pulls in every `static` helper in src/rbtree.c (new_node,
 * is_red, rotate_left, rotate_right, insert_fixup, ...) so this file can
 * unit-test them directly, instead of only through the public rbtree_t
 * API. */
#include "../src/rbtree.c"

#include <stdint.h>
#include <stdio.h>

static int failures = 0;

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
    struct rb_node *n = new_node("alpha", (void *)0x1234);
    CHECK(n != NULL);
    if (n == NULL) {
        return;
    }
    CHECK(n->value == (void *)0x1234);
    CHECK(n->color == RED);
    CHECK(n->left == NULL);
    CHECK(n->right == NULL);

    free(n->key);
    free(n);
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

    struct rb_node *n = new_node(buf, NULL);
    free(buf);

    CHECK(n != NULL);
    if (n == NULL) {
        return;
    }
    CHECK((uintptr_t)n->key != buf_addr);
    CHECK(strcmp(n->key, "alpha") == 0);

    free(n->key);
    free(n);
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

static void test_insert_rec_attaches_new_leaf(void) {
    struct rb_node r = {.key = "zulu", .left = NULL, .right = NULL, .color = BLACK};
    struct rb_node n = {.key = "mango", .left = NULL, .right = &r, .color = BLACK};

    bool inserted = false;
    struct rb_node *new_root =
        insert_rec(&n, "apple", (void *)0xABCD, NULL, &inserted);

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

    free(newleaf->key);
    free(newleaf);
}

/* Two levels of recursion: the new leaf attaches under the red node l, whose
 * own insert_fixup call must no-op (l is red; the violation isn't at l's
 * level), and the LL rotation must fire one level up, at n. */
static void test_insert_rec_two_level_ll_violation(void) {
    struct rb_node l = {.key = "g", .left = NULL, .right = NULL, .color = RED};
    struct rb_node n = {.key = "m", .left = &l, .right = NULL, .color = BLACK};

    bool inserted = false;
    struct rb_node *new_root =
        insert_rec(&n, "c", (void *)0xC0FFEE, NULL, &inserted);

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

    free(newleaf->key);
    free(newleaf);
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

    free(t);
}

static void test_rb_create_stores_value_free(void) {
    rbtree_t *t = rb_create(dummy_value_free);
    CHECK(t != NULL);
    if (t == NULL) {
        return;
    }
    CHECK(t->value_free == dummy_value_free);

    free(t);
}

static int free_call_count = 0;

static void counting_value_free(void *value) {
    (void)value;
    free_call_count++;
}

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
    CHECK(t->root != NULL);
    if (t->root != NULL) {
        CHECK(t->root->value == (void *)0x2);
        CHECK(strcmp(t->root->key, "key") == 0);
        free(t->root->key);
        free(t->root);
    }
    free(t);
}

/* --- rb_size -------------------------------------------------------------
 * rb_destroy doesn't exist yet, so this test frees the tree's nodes by hand
 * via a small recursive walk (test-local; not part of the public API). */

static void free_node_manual(struct rb_node *n) {
    if (n == NULL) {
        return;
    }
    free_node_manual(n->left);
    free_node_manual(n->right);
    free(n->key);
    free(n);
}

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

    free_node_manual(t->root);
    free(t);
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

    CHECK(rb_find(t, "a") == (void *)0xA);
    CHECK(rb_find(t, "b") == (void *)0xB);
    CHECK(rb_find(t, "c") == (void *)0xC);

    free_node_manual(t->root);
    free(t);
}

static void test_rb_find_returns_null_for_absent_key(void) {
    rbtree_t *t = rb_create(NULL);
    CHECK(t != NULL);
    if (t == NULL) {
        return;
    }

    CHECK(rb_insert(t, "b", (void *)0xB) == 0);

    CHECK(rb_find(t, "z") == NULL);

    free_node_manual(t->root);
    free(t);
}

static void test_rb_find_on_empty_tree_returns_null(void) {
    rbtree_t *t = rb_create(NULL);
    CHECK(t != NULL);
    if (t == NULL) {
        return;
    }

    CHECK(rb_find(t, "anything") == NULL);

    free(t);
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

    struct collect_ctx c = {.count = 0};
    rb_foreach(t, collect_key, &c);

    CHECK(c.count == 6);
    if (c.count == 6) {
        const char *expected[] = {"a", "b", "c", "d", "e", "f"};
        for (size_t i = 0; i < 6; i++) {
            CHECK(strcmp(c.keys[i], expected[i]) == 0);
        }
    }

    free_node_manual(t->root);
    free(t);
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

    free(t);
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

    free_call_count = 0;
    rb_destroy(t);
    CHECK(free_call_count == 4); /* every distinct value freed exactly once */
}

static void test_rb_destroy_null_is_noop(void) {
    rb_destroy(NULL); /* must not crash */
}

int main(void) {
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
    test_rb_size_tracks_distinct_keys();
    test_rb_find_returns_value_for_present_key();
    test_rb_find_returns_null_for_absent_key();
    test_rb_find_on_empty_tree_returns_null();
    test_rb_foreach_visits_in_order();
    test_rb_foreach_on_empty_tree_does_nothing();
    test_rb_destroy_frees_all_values();
    test_rb_destroy_null_is_noop();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d check(s) failed\n", failures);
    return 1;
}
