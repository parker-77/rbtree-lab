/* Unity build: pulls in every `static` helper in src/rbtree.c (new_node,
 * is_red, rotate_left, rotate_right, fixup, ...) so this file can unit-test
 * them directly, instead of only through the public rbtree_t API. */
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

int main(void) {
    test_new_node_sets_fields();
    test_new_node_copies_key();
    test_is_red();
    test_rotate_left();
    test_rotate_right();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d check(s) failed\n", failures);
    return 1;
}
