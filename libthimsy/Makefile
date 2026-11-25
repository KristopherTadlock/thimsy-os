# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Thimsy OS contributors
#
# libthimsy's host build (g++ and make only; CMakeLists.txt is for the image build).
#
#   make            libthimsy.a and thimsy-prop (-O2)
#   make check      also the tests, with AddressSanitizer and UBSan, and runs them
#   make clean
#
# Builds out of tree (O, default ~/.cache/thimsy/libthimsy), so no object file lands in the
# repository. The machine may be running an AOSP build in the same VM, so every target first checks
# MemAvailable (MEM_FLOOR_KB) and refuses below it; compilers run at nice 19 under a 3 GiB address
# space limit, and compilers and tests ask the OOM killer to take them first. Run it as
#   nice -n 19 make -C libthimsy -j1 check

O ?= $(HOME)/.cache/thimsy/libthimsy
MEM_FLOOR_KB ?= 6000000
LIMIT_AS ?= 3221225472
TEST_TIMEOUT ?= 300

SRC := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
CXX ?= g++
AR ?= ar

STD := -std=c++17
WARN := -Wall -Wextra -Werror
INC := -I$(SRC)/include
REL_FLAGS := -O2 -g
ASAN_FLAGS := -O0 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined
LIBS := -pthread

LIB_NAMES := atomic_file config json uevent watch
TESTS := gtest_standin_test atomic_file_test json_test uevent_test watch_test config_test thimsy_prop_test

REL_OBJS := $(LIB_NAMES:%=$(O)/rel/%.o)
ASAN_OBJS := $(LIB_NAMES:%=$(O)/asan/%.o)
TEST_BINS := $(TESTS:%=$(O)/test/%) $(O)/test/gtest_selfcheck_cases

# Last in line for the OOM killer; compilers also get the address space limit. Sanitized test
# programs can't run under an address space limit (ASan reserves terabytes of shadow), so they run
# with the OOM score and a timeout only; they are small (tens of MB).
#
# GCC 9's ASan fails at random ("AddressSanitizer:DEADLYSIGNAL", forever) on kernels with 32 bits of
# mmap randomness (WSL2's 6.6 does: about 1 run in 5), so test programs, and the programs they start,
# run with address space randomisation off (setarch -R).
RUN = sh -c 'echo 1000 > /proc/self/oom_score_adj 2>/dev/null; exec "$$@"' run
CC_RUN = $(RUN) prlimit --as=$(LIMIT_AS) -- nice -n 19
TEST_RUN = $(RUN) setarch $$(uname -m) -R timeout $(TEST_TIMEOUT)
TEST_ENV = THIMSY_PROP=$(O)/asan/thimsy-prop THIMSY_GTEST_SELFCHECK=$(O)/test/gtest_selfcheck_cases \
	ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1

.PHONY: all check clean guard
.SUFFIXES:
.SECONDARY:

all: guard $(O)/rel/libthimsy.a $(O)/rel/thimsy-prop

guard:
	@avail=$$(awk '/^MemAvailable:/ {print $$2}' /proc/meminfo 2>/dev/null); \
	if [ -z "$$avail" ] || [ "$$avail" -lt $(MEM_FLOOR_KB) ]; then \
		echo "libthimsy: refusing to build: MemAvailable $${avail:-unknown} kB is under $(MEM_FLOOR_KB) kB" >&2; \
		exit 1; \
	fi
	@for tool in prlimit setarch timeout; do \
		command -v $$tool >/dev/null || { echo "libthimsy: $$tool is needed" >&2; exit 1; }; \
	done

$(O)/rel/%.o: $(SRC)/src/%.cpp | guard
	@mkdir -p $(@D)
	$(CC_RUN) $(CXX) $(STD) $(WARN) $(REL_FLAGS) $(INC) -MMD -MP -c $< -o $@

$(O)/asan/%.o: $(SRC)/src/%.cpp | guard
	@mkdir -p $(@D)
	$(CC_RUN) $(CXX) $(STD) $(WARN) $(ASAN_FLAGS) $(INC) -MMD -MP -c $< -o $@

$(O)/rel/libthimsy.a: $(REL_OBJS)
	$(AR) rcs $@.new $^ && mv -f $@.new $@

$(O)/asan/libthimsy.a: $(ASAN_OBJS)
	$(AR) rcs $@.new $^ && mv -f $@.new $@

$(O)/rel/thimsy-prop: $(SRC)/tools/thimsy-prop.cpp $(O)/rel/libthimsy.a | guard
	$(CC_RUN) $(CXX) $(STD) $(WARN) $(REL_FLAGS) $(INC) -MMD -MP $< $(O)/rel/libthimsy.a $(LIBS) -o $@

$(O)/asan/thimsy-prop: $(SRC)/tools/thimsy-prop.cpp $(O)/asan/libthimsy.a | guard
	$(CC_RUN) $(CXX) $(STD) $(WARN) $(ASAN_FLAGS) $(INC) -MMD -MP $< $(O)/asan/libthimsy.a $(LIBS) -o $@

$(O)/test/%: $(SRC)/tests/%.cpp $(O)/asan/libthimsy.a | guard
	@mkdir -p $(@D)
	$(CC_RUN) $(CXX) $(STD) $(WARN) $(ASAN_FLAGS) $(INC) -I$(SRC)/testing -I$(SRC)/tests -MMD -MP \
		$< $(O)/asan/libthimsy.a $(LIBS) -o $@

# Runs every test program, prints its output and a total; fails if any program fails.
check: guard all $(TEST_BINS) $(O)/asan/thimsy-prop
	@mkdir -p $(O)/log; total=0; failed=0; bad=""; \
	for t in $(TESTS); do \
		echo "== $$t"; \
		if $(TEST_ENV) $(TEST_RUN) $(O)/test/$$t >$(O)/log/$$t.log 2>&1; then ok=1; else ok=0; fi; \
		cat $(O)/log/$$t.log; \
		line=$$(grep -E '^[0-9]+ tests, [0-9]+ failed$$' $(O)/log/$$t.log | tail -n 1); \
		n=$$(echo "$$line" | awk '{print $$1}'); m=$$(echo "$$line" | awk '{print $$3}'); \
		total=$$((total + $${n:-0})); failed=$$((failed + $${m:-0})); \
		if [ $$ok = 0 ] || [ -z "$$line" ]; then bad="$$bad $$t"; fi; \
	done; \
	echo "libthimsy: $$total tests, $$failed failed"; \
	if [ -n "$$bad" ]; then echo "libthimsy: FAILED:$$bad" >&2; exit 1; fi

# Removes only this Makefile's own output directories.
clean:
	@test -n "$(O)" && test "$(O)" != "/"
	rm -rf -- "$(O)/rel" "$(O)/asan" "$(O)/test" "$(O)/log"

-include $(wildcard $(O)/*/*.d)
