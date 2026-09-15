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
    CHECK(rb_validate(t) == 0);

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
    CHECK(rb_validate(t) == 0);

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
    CHECK(rb_validate(t) == 0);

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
    struct rb_node *n = new_node(key, value);
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
    struct rb_node *new_root = delete_rec(&n, "z", NULL, &deleted, &shorter);

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
    struct rb_node *new_root = extract_min(h, &out_key, &out_value, &shorter);

    CHECK(new_root == h);
    CHECK(!shorter);
    CHECK(out_key != NULL && strcmp(out_key, "e") == 0);
    CHECK(out_value == (void *)0xE);
    CHECK(h->left == g);
    CHECK(g->left == NULL);
    CHECK(g->right == NULL);
    CHECK(h->right == j);

    free(out_key); /* ownership of e's key returned to us; e's struct is
                       already freed by extract_min */
    free(g->key);
    free(g);
    free(j->key);
    free(j);
    free(h->key);
    free(h);
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
    struct rb_node *new_root = delete_rec(&n, "z", NULL, &deleted, &shorter);

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
        delete_rec(root, "b", NULL, &deleted, &shorter);

    CHECK(new_root == root);
    CHECK(deleted);
    CHECK(!shorter); /* red leaf contributes nothing to black-height */
    CHECK(root->left == NULL);
    CHECK(root->right == NULL);
    CHECK(root->color == BLACK);

    free(root->key);
    free(root);
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
        delete_rec(root, "d", NULL, &deleted, &shorter);

    CHECK(new_root == root);
    CHECK(deleted);
    CHECK(!shorter); /* e's black absorbs exactly what d contributed */
    CHECK(root->left == e);
    CHECK(e->color == BLACK);
    CHECK(e->left == NULL && e->right == NULL);
    CHECK(root->right == z);

    free(e->key);
    free(e);
    free(z->key);
    free(z);
    free(root->key);
    free(root);
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
    struct rb_node *new_root = delete_rec(d, "d", NULL, &deleted, &shorter);

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

    free(new_root->key); /* frees f's key, now owned by d's slot */
    free(new_root);       /* frees d's struct; f's struct was already freed
                              inside extract_min */
    free(b->key);
    free(b);
    free(h->key);
    free(h);
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
    struct rb_node *new_root = delete_rec(d, "d", NULL, &deleted, &shorter);

    CHECK(new_root == d);
    CHECK(deleted);
    CHECK(!shorter);
    CHECK(strcmp(new_root->key, "k") == 0); /* k's key moved into d's slot */
    CHECK(new_root->value == (void *)0x99);
    CHECK(new_root->left == b);
    CHECK(new_root->right == p); /* p absorbed k's black and moved up */
    CHECK(p->color == BLACK);
    CHECK(p->left == NULL && p->right == NULL);

    free(new_root->key); /* frees k's key, now owned by d's slot */
    free(new_root);       /* frees d's struct; k's struct was already freed
                              inside extract_min */
    free(b->key);
    free(b);
    free(p->key);
    free(p);
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
    struct rb_node *new_root = delete_rec(&p, "a", NULL, &deleted, &shorter);

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

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d check(s) failed\n", failures);
    return 1;
}
