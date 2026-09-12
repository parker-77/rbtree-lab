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

struct model_entry {
    bool present;
    int value;
};

static struct model_entry model[NUM_KEYS];

static void free_value(void *value) {
    free(value);
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

    rbtree_t *t = rb_create(free_value);
    if (t == NULL) {
        fprintf(stderr, "fuzz: rb_create failed\n");
        return 1;
    }

    /* invariant: each pass performs exactly one insert/delete/find against a
     * uniformly random key, checked against model[] immediately. */
    for (unsigned long iter = 0; iter < iterations; iter++) {
        unsigned key_idx = (unsigned)rand() % NUM_KEYS;
        char key[KEY_BUF_LEN];
        key_for(key_idx, key, sizeof(key));

        unsigned op = (unsigned)rand() % 10;
        if (op < 4) {
            int *value = malloc(sizeof(*value));
            if (value == NULL) {
                fprintf(stderr, "fuzz: out of memory allocating value\n");
                return 1;
            }
            *value = (int)key_idx;

            int rc = rb_insert(t, key, value);
            if (rc == 0) {
                /* ownership of value transfers to the tree on success */
                model[key_idx].present = true;
                model[key_idx].value = *value;
            } else {
                fprintf(stderr,
                        "fuzz: rb_insert unexpectedly failed at iteration %lu\n",
                        iter);
                free(value); /* not consumed on failure; still ours to free */
                return 1;
            }
        } else if (op < 8) {
            bool should_exist = model[key_idx].present;
            int rc = rb_delete(t, key);
            if (rc != (should_exist ? 0 : -1)) {
                fprintf(stderr,
                        "fuzz: rb_delete(%s) returned %d, expected %d, at "
                        "iteration %lu\n",
                        key, rc, should_exist ? 0 : -1, iter);
                return 1;
            }
            model[key_idx].present = false;
        } else {
            void *value = rb_find(t, key);
            bool mismatch = model[key_idx].present
                                 ? (value == NULL ||
                                    *(int *)value != model[key_idx].value)
                                 : (value != NULL);
            if (mismatch) {
                fprintf(stderr, "fuzz: rb_find(%s) mismatch at iteration %lu\n",
                        key, iter);
                return 1;
            }
        }

        if ((iter + 1) % VALIDATE_INTERVAL == 0) {
            validate_against_model(t, iter);
        }
    }
    validate_against_model(t, iterations);

    rb_destroy(t); /* frees every remaining node, key copy, and value */

    printf("fuzz: %lu operations OK\n", iterations);
    return 0;
}
