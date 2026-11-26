# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Thimsy OS contributors
#
# thimsy-padd's host build (g++ and make only; CMakeLists.txt is for the image build).
#
#   make            thimsy-padd and padd-replay (-O2)
#   make check      the tests with AddressSanitizer and UBSan, and the replay goldens
#   make golden     rewrites tests/traces/*.golden from the current core (review the diff!)
#   make clean
#
# Same rules as libthimsy/Makefile: out of tree (O, default ~/.cache/thimsy/thimsy-padd), libthimsy
# compiled from ../../../libthimsy into O as well, a MemAvailable floor, compilers at nice 19 under a
# 3 GiB address space limit, tests under setarch -R (GCC 9's ASan on WSL2) and a timeout. Run it as
#   nice -n 19 make -C linux/src/thimsy-padd -j1 check

O ?= $(HOME)/.cache/thimsy/thimsy-padd
MEM_FLOOR_KB ?= 6000000
LIMIT_AS ?= 3221225472
TEST_TIMEOUT ?= 120

SRC := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
LIBTHIMSY ?= $(abspath $(SRC)/../../../libthimsy)
CXX ?= g++
AR ?= ar

STD := -std=c++17
WARN := -Wall -Wextra -Werror
INC := -I$(SRC)/src -I$(LIBTHIMSY)/include
REL_FLAGS := -O2 -g
ASAN_FLAGS := -O0 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined
LIBS := -pthread

LIBTHIMSY_NAMES := atomic_file config json uevent watch
CORE_NAMES := core/InputCodes core/PadSpec core/Calibrate core/Merger core/Rumble core/Replay
IO_NAMES := io/EvdevSource io/UinputSink io/Control
TESTS := input_codes_test calibrate_test padspec_test merger_test rumble_test replay_test io_test

TRACES := $(sort $(wildcard $(SRC)/tests/traces/*.trace))

lib_objs = $(LIBTHIMSY_NAMES:%=$(O)/$(1)/libthimsy/%.o)
padd_objs = $(CORE_NAMES:%=$(O)/$(1)/%.o) $(IO_NAMES:%=$(O)/$(1)/%.o)

RUN = sh -c 'echo 1000 > /proc/self/oom_score_adj 2>/dev/null; exec "$$@"' run
CC_RUN = $(RUN) prlimit --as=$(LIMIT_AS) -- nice -n 19
TEST_RUN = $(RUN) setarch $$(uname -m) -R timeout $(TEST_TIMEOUT)
TEST_ENV = THIMSY_PADD_TRACES=$(SRC)/tests/traces ASAN_OPTIONS=detect_leaks=1 \
	UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1

.PHONY: all check clean golden guard
.SUFFIXES:
.SECONDARY:

all: guard $(O)/rel/thimsy-padd $(O)/rel/padd-replay

guard:
	@avail=$$(awk '/^MemAvailable:/ {print $$2}' /proc/meminfo 2>/dev/null); \
	if [ -z "$$avail" ] || [ "$$avail" -lt $(MEM_FLOOR_KB) ]; then \
		echo "thimsy-padd: refusing to build: MemAvailable $${avail:-unknown} kB is under $(MEM_FLOOR_KB) kB" >&2; \
		exit 1; \
	fi
	@for tool in prlimit setarch timeout; do \
		command -v $$tool >/dev/null || { echo "thimsy-padd: $$tool is needed" >&2; exit 1; }; \
	done

$(O)/rel/libthimsy/%.o: $(LIBTHIMSY)/src/%.cpp | guard
	@mkdir -p $(@D)
	$(CC_RUN) $(CXX) $(STD) $(WARN) $(REL_FLAGS) -I$(LIBTHIMSY)/include -MMD -MP -c $< -o $@

$(O)/asan/libthimsy/%.o: $(LIBTHIMSY)/src/%.cpp | guard
	@mkdir -p $(@D)
	$(CC_RUN) $(CXX) $(STD) $(WARN) $(ASAN_FLAGS) -I$(LIBTHIMSY)/include -MMD -MP -c $< -o $@

$(O)/rel/%.o: $(SRC)/src/%.cpp | guard
	@mkdir -p $(@D)
	$(CC_RUN) $(CXX) $(STD) $(WARN) $(REL_FLAGS) $(INC) -MMD -MP -c $< -o $@

$(O)/asan/%.o: $(SRC)/src/%.cpp | guard
	@mkdir -p $(@D)
	$(CC_RUN) $(CXX) $(STD) $(WARN) $(ASAN_FLAGS) $(INC) -MMD -MP -c $< -o $@

$(O)/rel/libpadd.a: $(call padd_objs,rel) $(call lib_objs,rel)
	$(AR) rcs $@.new $^ && mv -f $@.new $@

$(O)/asan/libpadd.a: $(call padd_objs,asan) $(call lib_objs,asan)
	$(AR) rcs $@.new $^ && mv -f $@.new $@

$(O)/rel/thimsy-padd: $(O)/rel/main.o $(O)/rel/libpadd.a | guard
	$(CC_RUN) $(CXX) $(REL_FLAGS) $^ $(LIBS) -o $@

$(O)/rel/padd-replay: $(SRC)/tools/padd-replay.cpp $(O)/rel/libpadd.a | guard
	$(CC_RUN) $(CXX) $(STD) $(WARN) $(REL_FLAGS) $(INC) -MMD -MP $< $(O)/rel/libpadd.a $(LIBS) -o $@

$(O)/asan/padd-replay: $(SRC)/tools/padd-replay.cpp $(O)/asan/libpadd.a | guard
	$(CC_RUN) $(CXX) $(STD) $(WARN) $(ASAN_FLAGS) $(INC) -MMD -MP $< $(O)/asan/libpadd.a $(LIBS) -o $@

$(O)/asan/thimsy-padd: $(O)/asan/main.o $(O)/asan/libpadd.a | guard
	$(CC_RUN) $(CXX) $(ASAN_FLAGS) $^ $(LIBS) -o $@

$(O)/test/%: $(SRC)/tests/%.cpp $(O)/asan/libpadd.a | guard
	@mkdir -p $(@D)
	$(CC_RUN) $(CXX) $(STD) $(WARN) $(ASAN_FLAGS) $(INC) -I$(LIBTHIMSY)/testing -I$(SRC)/tests -MMD -MP \
		$< $(O)/asan/libpadd.a $(LIBS) -o $@

# Runs every test program, then padd-replay on every trace against its golden output; prints a
# total; fails if anything failed.
check: guard all $(TESTS:%=$(O)/test/%) $(O)/asan/padd-replay $(O)/asan/thimsy-padd
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
	echo "== padd-replay goldens"; \
	for tr in $(TRACES); do \
		g=$${tr%.trace}.golden; name=$$(basename $$tr .trace); total=$$((total + 1)); \
		if $(TEST_ENV) $(TEST_RUN) $(O)/asan/padd-replay $$tr >$(O)/log/$$name.out 2>$(O)/log/$$name.err \
			&& diff -u $$g $(O)/log/$$name.out; then \
			echo "[       OK ] golden.$$name"; \
		else \
			cat $(O)/log/$$name.err; echo "[  FAILED  ] golden.$$name"; failed=$$((failed + 1)); bad="$$bad golden.$$name"; \
		fi; \
	done; \
	echo "== thimsy-padd --check"; \
	for d in $(SRC)/../../board/*/devices/*.json; do \
		total=$$((total + 1)); \
		if $(TEST_RUN) $(O)/asan/thimsy-padd --check --device $$d; then echo "[       OK ] check.$$(basename $$d)"; \
		else echo "[  FAILED  ] check.$$(basename $$d)"; failed=$$((failed + 1)); bad="$$bad check.$$(basename $$d)"; fi; \
	done; \
	echo "thimsy-padd: $$total tests, $$failed failed"; \
	if [ -n "$$bad" ]; then echo "thimsy-padd: FAILED:$$bad" >&2; exit 1; fi

golden: guard $(O)/rel/padd-replay
	@for tr in $(TRACES); do \
		$(O)/rel/padd-replay $$tr > $${tr%.trace}.golden || exit 1; echo "wrote $${tr%.trace}.golden"; \
	done

clean:
	@test -n "$(O)" && test "$(O)" != "/"
	rm -rf -- "$(O)/rel" "$(O)/asan" "$(O)/test" "$(O)/log"

-include $(wildcard $(O)/*/*.d $(O)/*/*/*.d)
