## 2026-09-01

STATE: 3 green, 2 red (not implemented yet)
DID: Discussed parent pointer vs recursion approach. Discussed insertion details.
  Implemented `rb_node` struct, `new_node`, and `is_red` helper w/ tests.
  **Changed Makefile to compile src/rbtree.c once.**
  **Dummy fuzz.c to allow `make test` to run.**
DECIDED: Recursion instead of parent pointers. NULL (nil leaves) rather than
  sentinel node.
LEARNED: Parent pointers are more trouble than they're worth. Some CMake tools
  not supported on Apple Silicon. Claude is eager to produce code and must be
  reigned in, maybe with CLAUDE.md additions.
NEXT FIRST STEP: Implement rotations.
OPEN: How does `value_free` function affect value ownership? How can it be NULL?
  Where is it implemented?

## 2026-09-04

STATE: All green
DID: Implemented rotations, insert fixup, insert recursion, and create.
DECIDED: No major decisions this time.
LEARNED: C isn't as scary as I thought it was.
NEXT FIRST STEP: Set up Docker container to successfully run `make test`, `make asan`,
    and `make memcheck`.
OPEN: What exactly does `typedef void (*rb_value_free_fn)(void *value)` mean?
    Is `value_free` a function or essentially a bool for whether the tree owns
    values or not? Review project spec about deletion cases that must be covered.

## 2026-09-08

STATE: All green
DID: Finish M1 implementation. Skipped Docker for now.
DECIDED: Recursion for all traversals. No internal use of `rb_foreach` because
    tree operations require passing nodes, which `rb_foreach` does not do.
LEARNED: Because of how C handles strings, sometimes it is necessary to have a pointer
    point to a char pointer to pass around the reference instead of the first character
    in the string
NEXT FIRST STEP: Set up Docker container to successfully run `make test`, `make asan`,
    and `make memcheck`.
OPEN: How to get valgrind and memory leak check in asan working. Docker may work,
    but unsure.
