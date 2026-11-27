# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Thimsy OS contributors
#
# thimsy-session's host build (g++ and make only; CMakeLists.txt is for the image build). The same
# pattern as libthimsy/Makefile, whose sources it compiles alongside its own.
#
#   make            thimsy-session (-O2)
#   make check      also the core and io tests (AddressSanitizer and UBSan) and the fake-hardware E2E
#                   (tests/e2e, Python 3.8+, the sanitized binary), and runs them
#   make clean
#
# Builds out of tree (O, default ~/.cache/thimsy/thimsy-session). The machine may be running an AOSP
# build in the same VM, so every target first checks MemAvailable (MEM_FLOOR_KB) and refuses below
# it; compilers run at nice 19 under a 3 GiB address space limit, and compilers and tests ask the
# OOM killer to take them first. Run it as
#   nice -n 19 make -C linux/src/thimsy-session -j1 check

O ?= $(HOME)/.cache/thimsy/thimsy-session
MEM_FLOOR_KB ?= 6000000
LIMIT_AS ?= 3221225472
TEST_TIMEOUT ?= 300
PYTHON ?= python3

SRC := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
LIBTHIMSY ?= $(abspath $(SRC)/../../../libthimsy)
CXX ?= g++

STD := -std=c++17
WARN := -Wall -Wextra -Werror
INC := -I$(SRC)/src -I$(LIBTHIMSY)/include
REL_FLAGS := -O2 -g
ASAN_FLAGS := -O0 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined
LIBS := -pthread

LIB_NAMES := atomic_file config json uevent watch
CORE_NAMES := core/InputCodes core/KeyMachine core/SleepPlan core/VolumePolicy core/Supervisor core/Requests \
	core/NanoJail core/GpuEnv io/Sysfs io/Blkid io/Mounter io/Idmap io/Proc io/Netlink io/Evdev
APP_NAMES := Session main
TESTS := key_machine_test sleep_plan_test volume_policy_test supervisor_test requests_test nano_jail_test \
	blkid_test io_test gpu_env_test idmap_test

REL_OBJS := $(LIB_NAMES:%=$(O)/rel/lib/%.o) $(CORE_NAMES:%=$(O)/rel/%.o) $(APP_NAMES:%=$(O)/rel/%.o)
ASAN_LIB_OBJS := $(LIB_NAMES:%=$(O)/asan/lib/%.o) $(CORE_NAMES:%=$(O)/asan/%.o)
ASAN_APP_OBJS := $(APP_NAMES:%=$(O)/asan/%.o)
TEST_BINS := $(TESTS:%=$(O)/test/%)

RUN = sh -c 'echo 1000 > /proc/self/oom_score_adj 2>/dev/null; exec "$$@"' run
CC_RUN = $(RUN) prlimit --as=$(LIMIT_AS) -- nice -n 19
# GCC 9's ASan hangs at random on WSL2's kernel unless address space randomisation is off
# (libthimsy/Makefile has the numbers), so test programs and the E2E run under setarch -R.
TEST_RUN = $(RUN) setarch $$(uname -m) -R timeout $(TEST_TIMEOUT)
TEST_ENV = ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1

.PHONY: all check unit e2e clean guard
.SUFFIXES:
.SECONDARY:

all: guard $(O)/rel/thimsy-session

guard:
	@avail=$$(awk '/^MemAvailable:/ {print $$2}' /proc/meminfo 2>/dev/null); \
	if [ -z "$$avail" ] || [ "$$avail" -lt $(MEM_FLOOR_KB) ]; then \
		echo "thimsy-session: refusing to build: MemAvailable $${avail:-unknown} kB is under $(MEM_FLOOR_KB) kB" >&2; \
		exit 1; \
	fi
	@for tool in prlimit setarch timeout; do \
		command -v $$tool >/dev/null || { echo "thimsy-session: $$tool is needed" >&2; exit 1; }; \
	done

$(O)/rel/lib/%.o: $(LIBTHIMSY)/src/%.cpp | guard
	@mkdir -p $(@D)
	$(CC_RUN) $(CXX) $(STD) $(WARN) $(REL_FLAGS) $(INC) -MMD -MP -c $< -o $@

$(O)/asan/lib/%.o: $(LIBTHIMSY)/src/%.cpp | guard
	@mkdir -p $(@D)
	$(CC_RUN) $(CXX) $(STD) $(WARN) $(ASAN_FLAGS) $(INC) -MMD -MP -c $< -o $@

$(O)/rel/%.o: $(SRC)/src/%.cpp | guard
	@mkdir -p $(@D)
	$(CC_RUN) $(CXX) $(STD) $(WARN) $(REL_FLAGS) $(INC) -MMD -MP -c $< -o $@

$(O)/asan/%.o: $(SRC)/src/%.cpp | guard
	@mkdir -p $(@D)
	$(CC_RUN) $(CXX) $(STD) $(WARN) $(ASAN_FLAGS) $(INC) -MMD -MP -c $< -o $@

$(O)/rel/thimsy-session: $(REL_OBJS)
	$(CC_RUN) $(CXX) $(REL_FLAGS) $^ $(LIBS) -o $@

$(O)/asan/thimsy-session: $(ASAN_LIB_OBJS) $(ASAN_APP_OBJS)
	$(CC_RUN) $(CXX) $(ASAN_FLAGS) $^ $(LIBS) -o $@

$(O)/test/%: $(SRC)/tests/%.cpp $(ASAN_LIB_OBJS) | guard
	@mkdir -p $(@D)
	$(CC_RUN) $(CXX) $(STD) $(WARN) $(ASAN_FLAGS) $(INC) -I$(LIBTHIMSY)/testing -I$(SRC)/tests -MMD -MP \
		$< $(ASAN_LIB_OBJS) $(LIBS) -o $@

# Runs every test program, prints its output and a total; fails if any program fails.
unit: guard $(TEST_BINS)
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
	echo "thimsy-session unit: $$total tests, $$failed failed"; \
	if [ -n "$$bad" ]; then echo "thimsy-session: FAILED:$$bad" >&2; exit 1; fi

e2e: guard $(O)/asan/thimsy-session
	$(TEST_ENV) THIMSY_SESSION_BIN=$(O)/asan/thimsy-session $(TEST_RUN) \
		$(PYTHON) -B $(SRC)/tests/e2e/test_e2e.py

check: guard all unit e2e

# Removes only this Makefile's own output directories.
clean:
	@test -n "$(O)" && test "$(O)" != "/"
	rm -rf -- "$(O)/rel" "$(O)/asan" "$(O)/test" "$(O)/log"

-include $(wildcard $(O)/*/*.d $(O)/*/*/*.d)
