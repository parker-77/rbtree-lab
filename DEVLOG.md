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
