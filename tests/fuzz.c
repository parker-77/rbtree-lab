#include "rbtree.h"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <iterations>\n", argv[0]);
        return 1;
    }

    char *end;
    unsigned long iterations = strtoul(argv[1], &end, 10);
    if (*end != '\0') {
        fprintf(stderr, "invalid iteration count: %s\n", argv[1]);
        return 1;
    }

    /* TODO: repeatedly rb_insert/rb_delete/rb_validate random keys against a
     * tree from rb_create, once those are implemented. */
    (void)iterations;

    return 0;
}
