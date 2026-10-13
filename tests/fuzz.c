#include "rbtree.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Keys are "k%04u" over [0, NUM_KEYS), so their lexicographic order equals
 * their numeric index order: model[] is a reference table indexed directly
 * by that order, needing no insertion logic of its own. 10000 keeps the key
 * space comfortably larger than any plausible insert-heavy run length, so a
 * long streak of inserts before the first delete can't exhaust it. */
#define NUM_KEYS 10000u
#define VALIDATE_INTERVAL 100u
#define KEY_BUF_LEN 16

/* "k%04u" only keeps lexicographic order equal to numeric order while every
 * index fits in four digits: at 100000 keys "k10000" would sort before
 * "k9999" and collect_and_check would report order violations against a
 * correct tree. Widen the format alongside this bound, not on its own. */
static_assert(NUM_KEYS <= 10000u,
              "key format must keep strcmp order equal to index order");

struct model_entry {
    bool present;
    int value;
};

static struct model_entry model[NUM_KEYS];

/* Every operation runs on a plain tree (rb_create) and a pooled tree
 * (rb_create_pooled), each owning its own value copies, and both are held to
 * the same model: the two constructors must behave identically. */
enum { PLAIN, POOLED, NUM_TREES };
static const char *const tree_name[NUM_TREES] = {"plain", "pooled"};

/* Values each tree has handed to value_free, and how many the header contract
 * says it should have by now: one per overwrite or delete of a present key,
 * plus one per key still present at teardown. */
static unsigned long value_frees[NUM_TREES];
static unsigned long expected_frees;

static void free_plain_value(void *value) {
    value_frees[PLAIN]++;
    free(value);
}

static void free_pooled_value(void *value) {
    value_frees[POOLED]++;
    free(value);
}

/* Exits on the first tree whose destructor count has drifted from the
 * contract. */
static void check_value_frees(unsigned long iter) {
    for (int i = 0; i < NUM_TREES; i++) {
        if (value_frees[i] != expected_frees) {
            fprintf(stderr,
                    "fuzz: %s tree freed %lu values, expected %lu, at "
                    "iteration %lu\n",
                    tree_name[i], value_frees[i], expected_frees, iter);
            exit(EXIT_FAILURE);
        }
    }
}

static void key_for(unsigned index, char *buf, size_t buflen) {
    snprintf(buf, buflen, "k%04u", index);
}

struct collect_ctx {
    unsigned model_i;   /* next model index to match against */
    unsigned matched;
    unsigned mismatches;
    char prev_key[KEY_BUF_LEN];
    bool have_prev;
    bool order_violation;
};

static void collect_and_check(const char *key, void *value, void *ctx_ptr) {
    struct collect_ctx *ctx = ctx_ptr;

    if (ctx->have_prev && strcmp(ctx->prev_key, key) >= 0) {
        ctx->order_violation = true;
    }
    strncpy(ctx->prev_key, key, sizeof(ctx->prev_key) - 1);
    ctx->prev_key[sizeof(ctx->prev_key) - 1] = '\0';
    ctx->have_prev = true;

    /* invariant: model_i only ever advances past absent slots, so it lands
     * on the model's next present entry, which rb_foreach's sorted order
     * guarantees corresponds to this key if the tree matches the model. */
    while (ctx->model_i < NUM_KEYS && !model[ctx->model_i].present) {
        ctx->model_i++;
    }

    char expected_key[KEY_BUF_LEN];
    if (ctx->model_i >= NUM_KEYS) {
        ctx->mismatches++;
        return;
    }
    key_for(ctx->model_i, expected_key, sizeof(expected_key));
    if (strcmp(expected_key, key) != 0 ||
        model[ctx->model_i].value != *(const int *)value) {
        ctx->mismatches++;
    } else {
        ctx->matched++;
    }
    ctx->model_i++;
}

/* Cross-checks the whole tree against the whole model: structural invariants
 * via rb_validate, then size and full in-order content via rb_foreach.
 * Aborts loudly (not an assert -- this is a behavioral mismatch, not an
 * internal guard) on the first divergence found. */
static void validate_against_model(rbtree_t *t, unsigned long iter) {
    int rc = rb_validate(t);
    if (rc != 0) {
        fprintf(stderr, "fuzz: rb_validate failed (code %d) at iteration %lu\n",
                rc, iter);
        exit(EXIT_FAILURE);
    }

    unsigned model_count = 0;
    /* invariant: counts every present model slot, independent of tree state */
    for (unsigned i = 0; i < NUM_KEYS; i++) {
        if (model[i].present) {
            model_count++;
        }
    }
    if (rb_size(t) != model_count) {
        fprintf(stderr,
                "fuzz: rb_size mismatch at iteration %lu: tree=%zu model=%u\n",
                iter, rb_size(t), model_count);
        exit(EXIT_FAILURE);
    }

    struct collect_ctx ctx = {0};
    rb_foreach(t, collect_and_check, &ctx);
    if (ctx.order_violation) {
        fprintf(stderr, "fuzz: rb_foreach order violation at iteration %lu\n",
                iter);
        exit(EXIT_FAILURE);
    }
    if (ctx.mismatches != 0 || ctx.matched != model_count) {
        fprintf(stderr,
                "fuzz: content mismatch at iteration %lu: matched=%u "
                "mismatches=%u model_count=%u\n",
                iter, ctx.matched, ctx.mismatches, model_count);
        exit(EXIT_FAILURE);
    }
}

int main(int argc, char **argv) {
    if (argc != 2 && argc != 3) {
        fprintf(stderr, "usage: %s <iterations> [seed]\n", argv[0]);
        return 1;
    }

    char *end;
    unsigned long iterations = strtoul(argv[1], &end, 10);
    if (*end != '\0') {
        fprintf(stderr, "invalid iteration count: %s\n", argv[1]);
        return 1;
    }

    unsigned seed;
    if (argc == 3) {
        seed = (unsigned)strtoul(argv[2], &end, 10);
        if (*end != '\0') {
            fprintf(stderr, "invalid seed: %s\n", argv[2]);
            return 1;
        }
    } else {
        seed = (unsigned)time(NULL);
    }
    fprintf(stderr, "fuzz: seed=%u (rerun with '%s %lu %u' to reproduce)\n",
            seed, argv[0], iterations, seed);
    srand(seed);

    rbtree_t *trees[NUM_TREES] = {rb_create(free_plain_value),
                                  rb_create_pooled(free_pooled_value)};
    if (trees[PLAIN] == NULL || trees[POOLED] == NULL) {
        fprintf(stderr, "fuzz: tree creation failed\n");
        return 1;
    }

    /* invariant: each pass performs exactly one insert/delete/find against a
     * uniformly random key on both trees, checked against model[] immediately,
     * and expected_frees counts every value the contract has released. */
    for (unsigned long iter = 0; iter < iterations; iter++) {
        unsigned key_idx = (unsigned)rand() % NUM_KEYS;
        char key[KEY_BUF_LEN];
        key_for(key_idx, key, sizeof(key));

        unsigned op = (unsigned)rand() % 10;
        if (op < 4) {
            /* invariant: trees[0..i-1] already hold this iteration's value */
            for (int i = 0; i < NUM_TREES; i++) {
                int *value = malloc(sizeof(*value));
                if (value == NULL) {
                    fprintf(stderr, "fuzz: out of memory allocating value\n");
                    return 1;
                }
                /* The payload is the iteration, not the key: every insert for
                 * a given key carries a value no earlier insert could have
                 * stored, so an overwrite that keeps the stale value fails
                 * the next rb_find/rb_foreach check instead of comparing
                 * equal to itself. */
                *value = (int)iter;

                int rc = rb_insert(trees[i], key, value);
                if (rc != 0) {
                    fprintf(stderr,
                            "fuzz: rb_insert unexpectedly failed on the %s "
                            "tree at iteration %lu\n",
                            tree_name[i], iter);
                    free(value); /* not consumed on failure; still ours */
                    return 1;
                }
                /* ownership of value transferred to trees[i] */
            }
            if (model[key_idx].present) {
                expected_frees++; /* the overwrite released the old value */
            }
            /* the model records iter directly rather than reading *value */
            model[key_idx].present = true;
            model[key_idx].value = (int)iter;
        } else if (op < 8) {
            bool should_exist = model[key_idx].present;
            /* invariant: trees[0..i-1] have already removed key (or refused) */
            for (int i = 0; i < NUM_TREES; i++) {
                int rc = rb_delete(trees[i], key);
                if (rc != (should_exist ? 0 : -1)) {
                    fprintf(stderr,
                            "fuzz: rb_delete(%s) on the %s tree returned %d, "
                            "expected %d, at iteration %lu\n",
                            key, tree_name[i], rc, should_exist ? 0 : -1,
                            iter);
                    return 1;
                }
            }
            if (should_exist) {
                expected_frees++; /* the delete released the value */
            }
            model[key_idx].present = false;
        } else {
            /* invariant: trees[0..i-1] agreed with the model on key */
            for (int i = 0; i < NUM_TREES; i++) {
                void *value = rb_find(trees[i], key);
                bool mismatch = model[key_idx].present
                                     ? (value == NULL ||
                                        *(int *)value != model[key_idx].value)
                                     : (value != NULL);
                if (mismatch) {
                    fprintf(stderr,
                            "fuzz: rb_find(%s) mismatch on the %s tree at "
                            "iteration %lu\n",
                            key, tree_name[i], iter);
                    return 1;
                }
            }
        }

        if ((iter + 1) % VALIDATE_INTERVAL == 0) {
            validate_against_model(trees[PLAIN], iter);
            validate_against_model(trees[POOLED], iter);
            check_value_frees(iter);
        }
    }
    validate_against_model(trees[PLAIN], iterations);
    validate_against_model(trees[POOLED], iterations);
    check_value_frees(iterations);

    expected_frees += rb_size(trees[PLAIN]); /* teardown frees what's left */
    rb_destroy(trees[PLAIN]);  /* frees every remaining node, key copy, value */
    rb_destroy(trees[POOLED]); /* same, plus every slab and the pool */
    check_value_frees(iterations);

    printf("fuzz: %lu operations OK\n", iterations);
    return 0;
}
