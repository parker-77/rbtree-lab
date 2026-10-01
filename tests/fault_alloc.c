#include "fault_alloc.h"

#include <stdlib.h>

/* Allocations remaining until the injected failure, counting the failing one
 * itself (1 = the very next rb_malloc fails). 0 means disarmed. */
static long countdown = 0;

/* Every rb_malloc call since program start, including injected failures. */
static long total = 0;

void *rb_malloc(size_t n) {
    total++;
    if (countdown > 0 && --countdown == 0) {
        return NULL; /* injected failure; one-shot, injector is now disarmed */
    }
    return malloc(n);
}

void rb_free(void *p) {
    free(p);
}

/* n is 1-based: arm(1) fails the next allocation, arm(3) lets two succeed and
 * fails the third. n < 1 disarms. Re-arming replaces any pending failure. */
void fault_alloc_arm(long n) {
    countdown = n > 0 ? n : 0;
}

void fault_alloc_disarm(void) {
    countdown = 0;
}

long fault_alloc_total(void) {
    return total;
}
