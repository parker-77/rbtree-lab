CC           := gcc
OPT          ?= -O1
EXTRA_CFLAGS ?=
CFLAGS       := -std=c23 -Wall -Wextra -Werror -g $(OPT) -Iinclude $(EXTRA_CFLAGS)

SRC     := src/rbtree.c
TSRC    := tests/test_rbtree.c
FAULT   := tests/fault_alloc.c
BIN     := build/test_rbtree
FUZZBIN := build/fuzz

# -fno-sanitize-recover=all is what makes UBSan fail the build; on its own
# -fsanitize=undefined prints a report and still exits 0.
SANFLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer \
            -fno-sanitize-recover=all
export UBSAN_OPTIONS := print_stacktrace=1

IMAGE := rbtree-dev
# build/ is an anonymous volume, not part of the bind mount: the host builds
# arm64 Mach-O and the container builds aarch64 ELF into the same path, and
# whichever ran last would otherwise look "up to date" to the other and fail
# with "cannot execute binary file". --rm drops the volume, so every container
# run also starts from an empty build/.
DOCKER_RUN := docker run --rm -v "$(CURDIR)":/work -v /work/build -w /work $(IMAGE)

all: $(BIN) $(FUZZBIN)

# tests/test_rbtree.c #includes src/rbtree.c (unity build), so $(SRC) is a
# prerequisite but must NOT be handed to the compiler -- that would define
# every symbol twice.
$(BIN): $(SRC) $(TSRC) $(FAULT) tests/fault_alloc.h include/rbtree.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(TSRC) $(FAULT) -o $@

$(FUZZBIN): $(SRC) tests/fuzz.c $(FAULT) tests/fault_alloc.h include/rbtree.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(SRC) tests/fuzz.c $(FAULT) -o $@

test: $(BIN) $(FUZZBIN)
	./$(BIN) && ./$(FUZZBIN) 100000

# build/ holds one flavor at a time, so asan and memcheck rebuild from scratch
# through a recursive make. That orders the clean before the build even under
# -j, which a plain `asan: clean test` prerequisite list does not guarantee.
# A bare `make test` straight after `make asan` still runs the sanitized
# binaries -- use `make check` for the real gate.
asan:
	@$(MAKE) clean
	@$(MAKE) EXTRA_CFLAGS='$(SANFLAGS)' test

memcheck:
	@$(MAKE) clean
	@$(MAKE) OPT=-O0 all
	valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes \
		--error-exitcode=1 ./$(BIN)
	valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes \
		--error-exitcode=1 ./$(FUZZBIN) 20000

# The DONE gate. Ordered so it finishes on a plain -O0 build/, which is a safe
# state for a follow-up `make test`.
check:
	@$(MAKE) clean
	@$(MAKE) test
	@$(MAKE) asan
	@$(MAKE) memcheck

docker-image:
	docker build -t $(IMAGE) .

docker-test: docker-image
	$(DOCKER_RUN) make test

docker-asan: docker-image
	$(DOCKER_RUN) make asan

docker-memcheck: docker-image
	$(DOCKER_RUN) make memcheck

docker-check: docker-image
	$(DOCKER_RUN) make check

docker-shell: docker-image
	docker run --rm -it -v "$(CURDIR)":/work -v /work/build -w /work $(IMAGE) bash

# Empties build/ rather than removing it, then drops the directory only if it
# can: inside the container build/ is a mount point, and `rm -rf build` there
# fails with "Device or resource busy". -delete implies -depth, so nested
# .dSYM bundles go too.
clean:
	@if [ -d build ]; then find build -mindepth 1 -delete; fi
	@rmdir build 2>/dev/null || true

.PHONY: all test asan memcheck check clean \
        docker-image docker-test docker-asan docker-memcheck docker-check \
        docker-shell
