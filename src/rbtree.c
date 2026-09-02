#include "rbtree.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

typedef enum { RED, BLACK } rb_color_t;

struct rb_node {
    char *key;   /* heap copy; the tree owns it */
    void *value; /* ownership per the header contract */
    struct rb_node *left;
    struct rb_node *right;
    rb_color_t color;
};

struct rbtree {
    struct rb_node *root;
};

/* Allocates a new red leaf node, copying key. Returns NULL on allocation
 * failure (nothing allocated survives such a failure). */
static struct rb_node *new_node(const char *key, void *value) {
    struct rb_node *n = malloc(sizeof *n);
    if (n == NULL) {
        return NULL;
    }

    size_t key_len = strlen(key) + 1;
    char *key_copy = malloc(key_len);
    if (key_copy == NULL) {
        goto cleanup_node;
    }
    memcpy(key_copy, key, key_len);

    n->key = key_copy; /* node takes ownership of the key copy */
    n->value = value;  /* node takes ownership of value per the header contract */
    n->left = NULL;
    n->right = NULL;
    n->color = RED;
    return n;

cleanup_node:
    free(n);
    return NULL;
}

/* Treats an absent child (NULL) as black, matching a valid tree's implicit
 * black leaves; avoids needing a sentinel node. */
[[maybe_unused]] static bool is_red(const struct rb_node *n) {
    return n != NULL && n->color == RED;
}

/* TODO: not yet implemented. */
[[maybe_unused]] static struct rb_node *rotate_left(struct rb_node *n) {
    (void)n;
    return NULL;
}

/* TODO: not yet implemented. */
[[maybe_unused]] static struct rb_node *rotate_right(struct rb_node *n) {
    (void)n;
    return NULL;
}

/* Restores the red-black invariants at n given a possible red-red violation
 * between n and its just-modified child. TODO: currently a no-op
 * pass-through; needs the is_red/rotate_left/rotate_right pattern match. */
[[maybe_unused]] static struct rb_node *fixup(struct rb_node *n) {
    return n;
}

/* Recursive insert helper. n is the current subtree root (NULL if the
 * subtree is empty). Returns NULL on allocation failure, in which case n and
 * every ancestor above it must be left untouched by the caller. */
static struct rb_node *insert_rec(struct rb_node *n, const char *key,
                                   void *value) {
    if (n == NULL) {
        return new_node(key, value);
    }
    /* TODO: strcmp against n->key, recurse into the correct side, reattach
     * the (possibly restructured) child, then return fixup(n). */
    return n;
}

int rb_insert(struct rbtree *t, const char *key, void *value) {
    struct rb_node *new_root = insert_rec(t->root, key, value);
    if (new_root == NULL) {
        return -1; /* allocation failure; t unchanged */
    }
    t->root = new_root;
    t->root->color = BLACK;
    return 0; /* TODO: size bookkeeping once struct rbtree gets a size field */
}
