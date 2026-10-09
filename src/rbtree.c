#include "rbtree.h"
#include "pool.h"
#include "../tests/fault_alloc.h"

#include <assert.h>
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
    rb_value_free_fn value_free; /* may be NULL: values not owned */
    size_t size;                 /* count of distinct keys */
    rb_pool_t *pool;             /* sole source of node storage; never NULL */
};

/* Visitor contexts for morris_inorder: foreach_ctx adapts rb_foreach's
 * key/value callback; destroy_ctx carries what destroy_visit needs to free. */
struct foreach_ctx {
    void (*fn)(const char *key, void *value, void *ctx);
    void *ctx;
};

struct destroy_ctx {
    rb_value_free_fn value_free;
    rb_pool_t *pool;
};

/* Internal helpers; defined below the public API they support. Every helper
 * that allocates or frees a node takes the tree's pool. */
static struct rb_node *new_node(const char *key, void *value, rb_pool_t *pool);
static int key_compare(const struct rb_node *n, const char *key);
static bool is_red(const struct rb_node *n);
static struct rb_node *rotate_left(struct rb_node *n);
static struct rb_node *rotate_right(struct rb_node *n);
static struct rb_node *insert_fixup(struct rb_node *n);
static struct rb_node *insert_rec(struct rb_node *n, const char *key,
                                   void *value, rb_value_free_fn value_free,
                                   bool *inserted, rb_pool_t *pool);
static struct rb_node *fixup_left_deficit(struct rb_node *n, bool *shorter);
static struct rb_node *fixup_right_deficit(struct rb_node *n, bool *shorter);
static struct rb_node *extract_min(struct rb_node *n, char **out_key,
                                    void **out_value, bool *shorter,
                                    rb_pool_t *pool);
static struct rb_node *delete_rec(struct rb_node *n, const char *key,
                                   rb_value_free_fn value_free, bool *deleted,
                                   bool *shorter, rb_pool_t *pool);
static void morris_inorder(struct rb_node *root, bool destructive,
                           void (*visit)(struct rb_node *n, void *ctx),
                           void *ctx);
static void foreach_visit(struct rb_node *n, void *ctx);
static void destroy_visit(struct rb_node *n, void *ctx);
static int validate_rec(const struct rb_node *n, const char **prev_key,
                         size_t *count, int *error);

/* Every tree is pooled: node storage always comes from the tree's pool. */
rbtree_t *rb_create(rb_value_free_fn value_free) {
    return rb_create_pooled(value_free);
}

/* Allocates an empty tree and the slab pool its node structs come from.
 * Returns NULL on allocation failure (nothing allocated survives it). */
rbtree_t *rb_create_pooled(rb_value_free_fn value_free) {
    struct rbtree *t = rb_malloc(sizeof *t);
    if (t == NULL) {
        return NULL;
    }

    t->pool = pool_create(sizeof(struct rb_node)); /* tree takes ownership of the pool */
    if (t->pool == NULL) {
        goto cleanup_tree;
    }
    t->root = NULL;
    t->value_free = value_free;
    t->size = 0;
    return t;

cleanup_tree:
    rb_free(t);
    return NULL;
}

int rb_insert(struct rbtree *t, const char *key, void *value) {
    bool inserted = false;
    struct rb_node *new_root =
        insert_rec(t->root, key, value, t->value_free, &inserted, t->pool);
    if (new_root == NULL) {
        return -1; /* allocation failure; t unchanged */
    }
    t->root = new_root;
    t->root->color = BLACK;
    if (inserted) {
        t->size++;
    }
    return 0;
}

void *rb_find(const rbtree_t *t, const char *key) {
    const struct rb_node *n = t->root;
    /* invariant: key, if present, is somewhere in the subtree rooted at n */
    while (n != NULL) {
        int cmp = key_compare(n, key);
        if (cmp == 0) {
            return n->value;
        }
        n = cmp < 0 ? n->left : n->right;
    }
    return NULL;
}

/* Removes key; frees the key copy and the value (via value_free, only if the
 * tree owns values). Returns -1 if key is absent, leaving t untouched
 * (delete_rec performs no allocation, so every intermediate reassignment on
 * the absent-key path re-writes a child pointer to its own existing value). */
int rb_delete(rbtree_t *t, const char *key) {
    bool deleted = false;
    bool shorter = false;
    struct rb_node *new_root =
        delete_rec(t->root, key, t->value_free, &deleted, &shorter, t->pool);
    if (!deleted) {
        return -1; /* key absent; t left untouched */
    }

    t->root = new_root;
    if (t->root != NULL) {
        t->root->color = BLACK;
    }
    t->size--;
    return 0;
}

size_t rb_size(const rbtree_t *t) {
    return t->size;
}

void rb_foreach(const rbtree_t *t,
                void (*fn)(const char *key, void *value, void *ctx),
                void *ctx) {
    /* t is const, but t->root is a plain struct rb_node *: the threads are
     * written through node pointers and all removed before this returns. */
    struct foreach_ctx fc = {.fn = fn, .ctx = ctx};
    morris_inorder(t->root, false, foreach_visit, &fc);
}

int rb_validate(const rbtree_t *t) {
    if (is_red(t->root)) {
        return 1; /* root not black */
    }

    const char *prev_key = NULL;
    size_t count = 0;
    int error = 0;
    if (validate_rec(t->root, &prev_key, &count, &error) < 0) {
        return error;
    }
    if (count != t->size) {
        return 5; /* rb_size doesn't match actual node count */
    }
    return 0;
}

void rb_destroy(rbtree_t *t) {
    if (t == NULL) {
        return;
    }
    struct destroy_ctx dc = {.value_free = t->value_free, .pool = t->pool};
    morris_inorder(t->root, true, destroy_visit, &dc); /* tree relinquishes every node */
    pool_destroy(t->pool); /* every slab and the pool itself freed */
    rb_free(t);
}

/* Allocates a new red leaf node from pool, copying key. Returns NULL on
 * allocation failure (nothing allocated survives such a failure). The key
 * copy is taken first: pool_alloc may grow the pool, which a later failure
 * couldn't undo, so it goes last and a failed insert leaves the pool's
 * counters untouched. */
static struct rb_node *new_node(const char *key, void *value, rb_pool_t *pool) {
    size_t key_len = strlen(key) + 1;
    char *key_copy = rb_malloc(key_len);
    if (key_copy == NULL) {
        return NULL;
    }
    memcpy(key_copy, key, key_len);

    struct rb_node *n = pool_alloc(pool); /* caller takes ownership of the slot */
    if (n == NULL) {
        goto cleanup_key;
    }

    n->key = key_copy; /* node takes ownership of the key copy */
    n->value = value;  /* node takes ownership of value per the header contract */
    n->left = NULL;
    n->right = NULL;
    n->color = RED;
    return n;

cleanup_key:
    rb_free(key_copy);
    return NULL;
}

/* Compares key against n->key using strcmp's convention: negative means key
 * belongs left of n, positive means right, zero means key matches n. Shared
 * directional check for insert/delete traversal. */
static int key_compare(const struct rb_node *n, const char *key) {
    return strcmp(key, n->key);
}

/* Treats an absent child (NULL) as black, matching a valid tree's implicit
 * black leaves; avoids needing a sentinel node. */
static bool is_red(const struct rb_node *n) {
    return n != NULL && n->color == RED;
}

/* Pure structural relink: n's right child becomes the new subtree root, its
 * old left child is reattached under n. Does not touch color. */
static struct rb_node *rotate_left(struct rb_node *n) {
    struct rb_node *r = n->right;
    n->right = r->left;
    r->left = n;
    return r;
}

/* Mirror of rotate_left: n's left child becomes the new subtree root, its
 * old right child is reattached under n. Does not touch color. */
static struct rb_node *rotate_right(struct rb_node *n) {
    struct rb_node *l = n->left;
    n->left = l->right;
    l->right = n;
    return l;
}

/* Restores the red-black invariants at n given a possible red-red violation
 * between one of n's children and that child's same-side child. Only ever
 * does real work when n itself is black: if n is red, the pre-existing tree
 * invariant guarantees both of n's children were black before this insert
 * touched one of them, so any violation at this depth is caught one level up
 * instead (see the "every other level" fixup cadence). When it does fire, the
 * restructured subtree root always comes back red, which is why the caller
 * one level up must run this same check again.
 *
 * A red uncle (n's other child) is checked first: if both of n's children
 * are red and one of them has a red child of its own, recoloring both
 * children black and n red clears the violation without any rotation, at
 * the cost of shifting a (possibly new) violation up to n's own level for
 * the caller one level up to handle. Only once the uncle is black (or
 * absent) does a violation require the rotation cases below. */
static struct rb_node *insert_fixup(struct rb_node *n) {
    if (is_red(n)) {
        return n;
    }

    if (is_red(n->left) && is_red(n->right) &&
        (is_red(n->left->left) || is_red(n->left->right) ||
         is_red(n->right->left) || is_red(n->right->right))) { /* red uncle */
        n->left->color = BLACK;
        n->right->color = BLACK;
        n->color = RED;
        return n;
    }

    if (is_red(n->left) && is_red(n->left->left)) { /* left-left */
        n = rotate_right(n);
        n->color = RED;
        n->left->color = BLACK;
        n->right->color = BLACK;
    } else if (is_red(n->left) && is_red(n->left->right)) { /* left-right */
        n->left = rotate_left(n->left);
        n = rotate_right(n);
        n->color = RED;
        n->left->color = BLACK;
        n->right->color = BLACK;
    } else if (is_red(n->right) && is_red(n->right->right)) { /* right-right */
        n = rotate_left(n);
        n->color = RED;
        n->left->color = BLACK;
        n->right->color = BLACK;
    } else if (is_red(n->right) && is_red(n->right->left)) { /* right-left */
        n->right = rotate_right(n->right);
        n = rotate_left(n);
        n->color = RED;
        n->left->color = BLACK;
        n->right->color = BLACK;
    }

    return n;
}

/* Recursive insert helper. n is the current subtree root (NULL if the
 * subtree is empty). Returns NULL on allocation failure, in which case n and
 * every ancestor above it must be left untouched by the caller. On success,
 * sets *inserted to true iff a brand-new node was allocated (as opposed to an
 * existing key's value being overwritten), so the caller can maintain a size
 * count without re-walking the tree. */
static struct rb_node *insert_rec(struct rb_node *n, const char *key,
                                   void *value, rb_value_free_fn value_free,
                                   bool *inserted, rb_pool_t *pool) {
    if (n == NULL) {
        struct rb_node *new_n = new_node(key, value, pool);
        if (new_n != NULL) {
            *inserted = true;
        }
        return new_n;
    }

    int cmp = key_compare(n, key);
    if (cmp < 0) {
        struct rb_node *new_left =
            insert_rec(n->left, key, value, value_free, inserted, pool);
        if (new_left == NULL) {
            return NULL; /* allocation failure; n and every ancestor untouched */
        }
        n->left = new_left;
    } else if (cmp > 0) {
        struct rb_node *new_right =
            insert_rec(n->right, key, value, value_free, inserted, pool);
        if (new_right == NULL) {
            return NULL; /* allocation failure; n and every ancestor untouched */
        }
        n->right = new_right;
    } else {
        /* key already exists: free the old value (if the tree owns values),
         * then the node takes ownership of the new one in its place. No
         * allocation or structural change, so no fixup is needed. */
        if (value_free != NULL) {
            value_free(n->value);
        }
        n->value = value;
        return n;
    }

    return insert_fixup(n);
}

/* Rebalances n after n->right's black-height dropped by one (the deficient
 * subtree was already reattached to n->right by the caller).
 *
 * Case A (red sibling): rotate the sibling up and demote n (now red) under
 * it, then resolve n's same deficiency again against its new sibling — one
 * of s's own children, which must be black (a red node's children always
 * are), so the recursive call below is guaranteed to land in case B, C, or
 * D and never re-enter case A. The overall new subtree root is always the
 * promoted (old) sibling; only what the recursive call returns for n's own
 * slot underneath it is still undetermined at that point.
 *
 * Case B (black sibling, both nephews black): recolor the sibling red; n
 * either absorbs the deficit (if red) or passes it up (if black).
 *
 * Case C (black sibling, red far nephew — near nephew's color is
 * irrelevant, see DEVLOG/discussion): one rotation at n. The far nephew's
 * recolor supplies the missing black; near nephew's own black-height was
 * already equal to far's before the deletion, by the ordinary red-black
 * invariant that a node's two children share one black-height, so it slots
 * in under the now-black n without needing to change at all.
 *
 * Case D (black sibling, red near nephew, black far nephew): one rotation
 * at the sibling turns this into case C's shape (the old sibling becomes
 * the new "far nephew"), then case C's rotation applies at n. */
static struct rb_node *fixup_right_deficit(struct rb_node *n, bool *shorter) {
    struct rb_node *s = n->left; /* sibling: non-NULL, since it must have had
                                     greater black-height than the deficient
                                     side even before the deficit */
    assert(s != NULL);

    if (is_red(s)) {
        rb_color_t n_color = n->color;
        struct rb_node *promoted = rotate_right(n);
        promoted->color = n_color;
        n->color = RED;
        promoted->right = fixup_right_deficit(n, shorter);
        return promoted;
    }

    if (!is_red(s->left) && !is_red(s->right)) {
        s->color = RED;
        if (is_red(n)) {
            n->color = BLACK; /* n absorbs the missing black locally */
            *shorter = false;
        } else {
            *shorter = true; /* n has no black to spare; caller must fix up */
        }
        return n;
    }

    if (is_red(s->left)) {
        struct rb_node *far = s->left;
        rb_color_t n_color = n->color;
        struct rb_node *new_root = rotate_right(n);
        new_root->color = n_color;
        n->color = BLACK;
        far->color = BLACK;
        *shorter = false;
        return new_root;
    }

    struct rb_node *near = s->right;
    s->color = RED;
    near->color = BLACK;
    n->left = rotate_left(s);

    rb_color_t n_color = n->color;
    struct rb_node *new_root = rotate_right(n);
    new_root->color = n_color;
    n->color = BLACK;
    s->color = BLACK; /* s now plays the far-nephew role; absorb here too */
    *shorter = false;
    return new_root;
}

/* Mirror of fixup_right_deficit for a deficient left subtree (n->left just
 * lost a black; sibling is n->right). */
static struct rb_node *fixup_left_deficit(struct rb_node *n, bool *shorter) {
    struct rb_node *s = n->right;
    assert(s != NULL);

    if (is_red(s)) {
        rb_color_t n_color = n->color;
        struct rb_node *promoted = rotate_left(n);
        promoted->color = n_color;
        n->color = RED;
        promoted->left = fixup_left_deficit(n, shorter);
        return promoted;
    }

    if (!is_red(s->left) && !is_red(s->right)) {
        s->color = RED;
        if (is_red(n)) {
            n->color = BLACK;
            *shorter = false;
        } else {
            *shorter = true;
        }
        return n;
    }

    if (is_red(s->right)) {
        struct rb_node *far = s->right;
        rb_color_t n_color = n->color;
        struct rb_node *new_root = rotate_left(n);
        new_root->color = n_color;
        n->color = BLACK;
        far->color = BLACK;
        *shorter = false;
        return new_root;
    }

    struct rb_node *near = s->left;
    s->color = RED;
    near->color = BLACK;
    n->right = rotate_right(s);

    rb_color_t n_color = n->color;
    struct rb_node *new_root = rotate_left(n);
    new_root->color = n_color;
    n->color = BLACK;
    s->color = BLACK;
    *shorter = false;
    return new_root;
}

/* Removes and returns ownership of the minimum-keyed node in the subtree
 * rooted at n (n is never NULL: only called on a subtree already known to be
 * non-empty). out_key and out_value receive the removed node's key and value
 * pointers without freeing them — ownership transfers to the caller, which
 * is responsible for freeing or reinstalling them. *shorter reports whether
 * this subtree's black-height dropped by one. */
static struct rb_node *extract_min(struct rb_node *n, char **out_key,
                                    void **out_value, bool *shorter,
                                    rb_pool_t *pool) {
    if (n->left == NULL) {
        *out_key = n->key;     /* ownership transferred to caller */
        *out_value = n->value; /* ownership transferred to caller */
        struct rb_node *right = n->right;
        bool was_black = (n->color == BLACK);
        pool_free(pool, n); /* node struct freed; key/value ownership already moved out */

        if (right != NULL) {
            right->color = BLACK; /* case 6: red child absorbs the black n had */
            *shorter = false;
        } else {
            *shorter = was_black; /* removing a black leaf shortens this side */
        }
        return right;
    }

    struct rb_node *new_left =
        extract_min(n->left, out_key, out_value, shorter, pool);
    n->left = new_left;
    if (*shorter) {
        n = fixup_left_deficit(n, shorter);
    }
    return n;
}

/* Recursive delete helper, structured like insert_rec: n is the current
 * subtree root (NULL if absent), and the return value is the new subtree
 * root for the caller to reattach. *deleted reports whether key was found
 * anywhere in this subtree (mirrors insert_rec's *inserted); *shorter
 * reports whether the returned subtree's black-height is one less than what
 * occupied this slot before the call. */
static struct rb_node *delete_rec(struct rb_node *n, const char *key,
                                   rb_value_free_fn value_free, bool *deleted,
                                   bool *shorter, rb_pool_t *pool) {
    if (n == NULL) {
        *shorter = false; /* key absent in this subtree; nothing to change */
        return NULL;
    }

    int cmp = key_compare(n, key);
    if (cmp < 0) {
        struct rb_node *new_left =
            delete_rec(n->left, key, value_free, deleted, shorter, pool);
        n->left = new_left;
        if (*shorter) {
            n = fixup_left_deficit(n, shorter);
        }
        return n;
    }
    if (cmp > 0) {
        struct rb_node *new_right =
            delete_rec(n->right, key, value_free, deleted, shorter, pool);
        n->right = new_right;
        if (*shorter) {
            n = fixup_right_deficit(n, shorter);
        }
        return n;
    }

    /* cmp == 0: n is the node to remove. */
    *deleted = true;

    if (n->left != NULL && n->right != NULL) {
        /* Two children: move the in-order successor's key/value into n's
         * slot, then remove the successor (which has at most one child) from
         * n->right instead of removing n itself. n keeps its position, its
         * color, and n->left untouched. */
        char *succ_key;
        void *succ_value;
        bool succ_shorter = false;
        struct rb_node *new_right =
            extract_min(n->right, &succ_key, &succ_value, &succ_shorter, pool);

        if (value_free != NULL) {
            value_free(n->value); /* release n's old value */
        }
        rb_free(n->key);        /* release n's old key copy */
        n->key = succ_key;      /* ownership transferred from the successor */
        n->value = succ_value;  /* ownership transferred from the successor */
        n->right = new_right;

        *shorter = false;
        if (succ_shorter) {
            n = fixup_right_deficit(n, shorter);
        }
        return n;
    }

    if (n->left == NULL && n->right == NULL) {
        bool was_black = (n->color == BLACK);
        if (value_free != NULL) {
            value_free(n->value);
        }
        rb_free(n->key);
        pool_free(pool, n); /* leaf's node struct freed; no children to preserve */
        *shorter = was_black;
        return NULL;
    }

    /* Exactly one child. A black node with one child always has a single red
     * leaf child (see plan); a red node can never have exactly one child. */
    struct rb_node *child = (n->left != NULL) ? n->left : n->right;
    if (value_free != NULL) {
        value_free(n->value);
    }
    rb_free(n->key);
    pool_free(pool, n); /* n's node struct freed; child subtree reattached in its place */
    child->color = BLACK;
    *shorter = false;
    return child;
}

/* Morris in-order traversal: O(1) auxiliary space, no recursion. Calls
 * visit(n, ctx) on every node in ascending strcmp order.
 *
 * Instead of a stack, it borrows the NULL right pointer of each node's
 * in-order predecessor (the rightmost node of its left subtree) as a
 * temporary "thread" back up to that node. A node with a left child is
 * reached twice: on the first arrival the thread is set and the walk goes
 * left; on the second (via the thread) the left subtree is known finished.
 *
 * Non-destructive: the second arrival removes the thread before visiting,
 * so every pointer is back to its original value when this returns. While
 * it runs the tree contains cycles, so visit must not search or modify it.
 *
 * Destructive: the first arrival also cuts cur->left, so a node is only
 * ever visited from the no-left-child branch and the second-arrival walk
 * (which would read the already-freed left subtree) never happens. visit
 * may free n: n->right is always read before visit is called. */
static void morris_inorder(struct rb_node *root, bool destructive,
                           void (*visit)(struct rb_node *n, void *ctx),
                           void *ctx) {
    struct rb_node *cur = root;
    /* invariant: every thread still set points from some node to an
     * ancestor of cur whose left subtree is not yet finished; all other
     * right pointers hold their original values */
    while (cur != NULL) {
        if (cur->left == NULL) {
            struct rb_node *next = cur->right; /* child or thread; read before visit may free cur */
            visit(cur, ctx);
            cur = next;
            continue;
        }

        struct rb_node *pred = cur->left;
        /* invariant: pred lies on cur->left's right spine, before any thread */
        while (pred->right != NULL && pred->right != cur) {
            pred = pred->right;
        }

        if (pred->right == NULL) {
            pred->right = cur; /* first arrival: thread pred -> cur */
            struct rb_node *left = cur->left;
            if (destructive) {
                cur->left = NULL; /* cur no longer owns its left subtree; the walk below does */
            }
            cur = left;
        } else {
            pred->right = NULL; /* second arrival: thread removed, pred restored */
            struct rb_node *next = cur->right;
            visit(cur, ctx);
            cur = next;
        }
    }
}

static void foreach_visit(struct rb_node *n, void *ctx) {
    struct foreach_ctx *fc = ctx;
    fc->fn(n->key, n->value, fc->ctx);
}

/* Frees one node during a destructive morris_inorder. By the time n is
 * visited nothing still reachable points to it: its parent either cut the
 * link (left child) or was freed already (right child), and the thread into
 * it lived in a predecessor that was freed already. */
static void destroy_visit(struct rb_node *n, void *ctx) {
    struct destroy_ctx *dc = ctx;
    if (dc->value_free != NULL) {
        dc->value_free(n->value); /* tree relinquishes ownership of value */
    }
    rb_free(n->key);
    pool_free(dc->pool, n); /* tree relinquishes ownership of the node itself */
}

/* Returns this subtree's black-height (>= 0), or -1 if a violation was found
 * anywhere in it, in which case *error holds the failing rule's code
 * (2 = red node has a red child, 3 = unequal black-height across paths,
 * 4 = keys not strictly increasing in-order) and the caller must propagate
 * -1 immediately without trusting *count or *prev_key further.
 *
 * *prev_key threads the most recently in-order-visited key across the whole
 * traversal (shared storage, not a per-call copy) so this node can be
 * compared against whichever node was visited immediately before it,
 * however far apart in the recursion that was. *count threads a running
 * node count the same way. */
static int validate_rec(const struct rb_node *n, const char **prev_key,
                         size_t *count, int *error) {
    if (n == NULL) {
        return 0;
    }

    if (is_red(n) && (is_red(n->left) || is_red(n->right))) {
        *error = 2; /* red node has a red child */
        return -1;
    }

    int left_bh = validate_rec(n->left, prev_key, count, error);
    if (left_bh < 0) {
        return -1;
    }

    if (*prev_key != NULL && strcmp(*prev_key, n->key) >= 0) {
        *error = 4; /* keys not strictly increasing in-order */
        return -1;
    }
    *prev_key = n->key;
    (*count)++;

    int right_bh = validate_rec(n->right, prev_key, count, error);
    if (right_bh < 0) {
        return -1;
    }

    if (left_bh != right_bh) {
        *error = 3; /* unequal black-height across paths */
        return -1;
    }
    return left_bh + (n->color == BLACK ? 1 : 0);
}
