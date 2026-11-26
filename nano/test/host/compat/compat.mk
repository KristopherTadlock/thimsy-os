# Copyright (C) 2026 The Thimsy OS Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# Nano's compat layer on the host (docs/linux/IMPLEMENTATION.md B.2, C.3): libnanocompat.a from
# nano/compat/src, its tests from this directory, and libthimsy (from the repository's libthimsy/),
# with g++ and make only.
#
# Standalone (from anywhere; it finds its own paths):
#   nice -n 19 make -f nano/test/host/compat/compat.mk -j1 check
#
# Included by nano/test/host/Makefile (G1c), which may set COMPAT_O first and gets:
#   check-compat             libthimsy's tests (unless COMPAT_WITH_LIBTHIMSY=0), then these
#   nanocompat-libs          libnanocompat.a and libthimsy.a, sanitized and not
#   NANOCOMPAT_CPPFLAGS      for every Nano translation unit built on the host: the shim headers
#                            first on the include path and nanocompat/bionic.h force-included
#   NANOCOMPAT_ASAN_LIBS     what a sanitized (-fsanitize=address,undefined) program links, in order
#   NANOCOMPAT_REL_LIBS      the same for an unsanitized program
#   COMPAT_GUARD             the memory and tool check, as a prerequisite (order-only)
#
# The machine may be running an AOSP build in the same VM: every target first checks MemAvailable
# (COMPAT_MEM_FLOOR_KB) and refuses below it; compilers run at nice 19 under a 3 GiB address-space
# limit and ask the OOM killer to take them first; test programs run with address space
# randomisation off (GCC 9's ASan hangs on WSL2's kernel otherwise) and a timeout. Builds go out of
# tree, under COMPAT_O. The image decoder links the image libraries whose headers are installed
# (the same __has_include probe its source makes); without them its decoding tests skip.

COMPAT_MK := $(abspath $(lastword $(MAKEFILE_LIST)))
COMPAT_TEST_DIR := $(patsubst %/,%,$(dir $(COMPAT_MK)))
COMPAT_NANO_DIR := $(abspath $(COMPAT_TEST_DIR)/../../..)
COMPAT_LIBTHIMSY_DIR ?= $(abspath $(COMPAT_NANO_DIR)/../libthimsy)
COMPAT_O ?= $(HOME)/.cache/thimsy/nano-host/compat
COMPAT_MEM_FLOOR_KB ?= 6000000
COMPAT_LIMIT_AS ?= 3221225472
COMPAT_TEST_TIMEOUT ?= 300
COMPAT_WITH_LIBTHIMSY ?= 1

COMPAT_STD := -std=gnu++17
COMPAT_CSTD := -std=gnu11
# -Wclobbered: the JPEG decoder's setjmp, whose locals are not used after a longjmp.
COMPAT_WARN := -Wall -Wextra -Werror -Wno-clobbered
COMPAT_ASAN_FLAGS := -O0 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined
COMPAT_REL_FLAGS := -O2 -g

NANOCOMPAT_CPPFLAGS := -I$(COMPAT_NANO_DIR)/compat/include -include nanocompat/bionic.h
COMPAT_SRC_CPPFLAGS := $(NANOCOMPAT_CPPFLAGS) -I$(COMPAT_LIBTHIMSY_DIR)/include
COMPAT_TEST_CPPFLAGS := $(COMPAT_SRC_CPPFLAGS) -I$(COMPAT_LIBTHIMSY_DIR)/testing -I$(COMPAT_NANO_DIR)/compat/src \
	-I$(COMPAT_TEST_DIR)

# The image libraries whose headers this machine has (imagedecoder.cpp uses exactly these).
compat_has_header = $(shell printf '\043include <%s>\n' '$(1)' | $(CXX) -E -x c++ - >/dev/null 2>&1 && echo yes)
NANOCOMPAT_IMAGE_LDLIBS := $(strip \
	$(if $(call compat_has_header,png.h),-lpng) \
	$(if $(call compat_has_header,jpeglib.h),-ljpeg) \
	$(if $(call compat_has_header,webp/decode.h),-lwebp) \
	$(if $(call compat_has_header,webp/demux.h),-lwebpdemux) \
	$(if $(call compat_has_header,gif_lib.h),-lgif))

COMPAT_SRCS := log properties RefBase Thread utils file imagedecoder
COMPAT_ASAN_OBJS := $(COMPAT_SRCS:%=$(COMPAT_O)/asan/%.o)
COMPAT_REL_OBJS := $(COMPAT_SRCS:%=$(COMPAT_O)/rel/%.o)
COMPAT_ASAN_LIB := $(COMPAT_O)/asan/libnanocompat.a
COMPAT_REL_LIB := $(COMPAT_O)/rel/libnanocompat.a

COMPAT_LIBTHIMSY_O := $(COMPAT_O)/libthimsy
COMPAT_LIBTHIMSY_ASAN := $(COMPAT_LIBTHIMSY_O)/asan/libthimsy.a
COMPAT_LIBTHIMSY_REL := $(COMPAT_LIBTHIMSY_O)/rel/libthimsy.a

NANOCOMPAT_ASAN_LIBS := $(COMPAT_ASAN_LIB) $(COMPAT_LIBTHIMSY_ASAN) $(NANOCOMPAT_IMAGE_LDLIBS) -pthread
NANOCOMPAT_REL_LIBS := $(COMPAT_REL_LIB) $(COMPAT_LIBTHIMSY_REL) $(NANOCOMPAT_IMAGE_LDLIBS) -pthread

# One program per test; a program's extra translation units (.cpp, or .c compiled as C).
COMPAT_TESTS := properties_test system_properties_test log_test refbase_test thread_test bionic_test \
	utils_test base_test imagedecoder_test init_test
COMPAT_EXTRA_log_test := log_ndebug_on.cpp log_ndebug_off.cpp log_ndebug_default.cpp
COMPAT_EXTRA_bionic_test := bionic_c.c headers_c.c
COMPAT_TEST_BINS := $(COMPAT_TESTS:%=$(COMPAT_O)/test/%)

COMPAT_RUN = sh -c 'echo 1000 > /proc/self/oom_score_adj 2>/dev/null; exec "$$@"' run
COMPAT_CC_RUN = $(COMPAT_RUN) prlimit --as=$(COMPAT_LIMIT_AS) -- nice -n 19
COMPAT_TEST_RUN = $(COMPAT_RUN) setarch $$(uname -m) -R timeout $(COMPAT_TEST_TIMEOUT)
COMPAT_TEST_ENV = ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1
COMPAT_GUARD := compat-guard

.PHONY: check check-compat compat-guard compat-libthimsy-check nanocompat-libs compat-clean compat-force \
	compat-libthimsy-asan compat-libthimsy-rel

# The default goal only when this file is the makefile itself.
ifeq ($(firstword $(MAKEFILE_LIST)),$(lastword $(MAKEFILE_LIST)))
check: check-compat
.DEFAULT_GOAL := check
endif

compat-guard:
	@avail=$$(awk '/^MemAvailable:/ {print $$2}' /proc/meminfo 2>/dev/null); \
	if [ -z "$$avail" ] || [ "$$avail" -lt $(COMPAT_MEM_FLOOR_KB) ]; then \
		echo "nano compat: refusing to build: MemAvailable $${avail:-unknown} kB is under $(COMPAT_MEM_FLOOR_KB) kB" >&2; \
		exit 1; \
	fi
	@for tool in prlimit setarch timeout; do \
		command -v $$tool >/dev/null || { echo "nano compat: $$tool is needed" >&2; exit 1; }; \
	done

# libthimsy, built by its own Makefile into our tree (it decides whether anything is stale).
compat-libthimsy-asan: | compat-guard
	@$(MAKE) --no-print-directory -C $(COMPAT_LIBTHIMSY_DIR) O=$(COMPAT_LIBTHIMSY_O) $(COMPAT_LIBTHIMSY_ASAN)
compat-libthimsy-rel: | compat-guard
	@$(MAKE) --no-print-directory -C $(COMPAT_LIBTHIMSY_DIR) O=$(COMPAT_LIBTHIMSY_O) $(COMPAT_LIBTHIMSY_REL)
$(COMPAT_LIBTHIMSY_ASAN): compat-libthimsy-asan
$(COMPAT_LIBTHIMSY_REL): compat-libthimsy-rel

compat-libthimsy-check: | compat-guard
	@$(MAKE) --no-print-directory -C $(COMPAT_LIBTHIMSY_DIR) O=$(COMPAT_LIBTHIMSY_O) check

$(COMPAT_O)/asan/%.o: $(COMPAT_NANO_DIR)/compat/src/%.cpp | compat-guard
	@mkdir -p $(@D)
	$(COMPAT_CC_RUN) $(CXX) $(COMPAT_STD) $(COMPAT_WARN) $(COMPAT_ASAN_FLAGS) $(COMPAT_SRC_CPPFLAGS) -MMD -MP -c $< -o $@

$(COMPAT_O)/rel/%.o: $(COMPAT_NANO_DIR)/compat/src/%.cpp | compat-guard
	@mkdir -p $(@D)
	$(COMPAT_CC_RUN) $(CXX) $(COMPAT_STD) $(COMPAT_WARN) $(COMPAT_REL_FLAGS) $(COMPAT_SRC_CPPFLAGS) -MMD -MP -c $< -o $@

$(COMPAT_ASAN_LIB): $(COMPAT_ASAN_OBJS)
	$(AR) rcs $@.new $^ && mv -f $@.new $@

$(COMPAT_REL_LIB): $(COMPAT_REL_OBJS)
	$(AR) rcs $@.new $^ && mv -f $@.new $@

nanocompat-libs: $(COMPAT_ASAN_LIB) $(COMPAT_REL_LIB) $(COMPAT_LIBTHIMSY_ASAN) $(COMPAT_LIBTHIMSY_REL)

$(COMPAT_O)/test-obj/%.o: $(COMPAT_TEST_DIR)/%.cpp | compat-guard
	@mkdir -p $(@D)
	$(COMPAT_CC_RUN) $(CXX) $(COMPAT_STD) $(COMPAT_WARN) $(COMPAT_ASAN_FLAGS) $(COMPAT_TEST_CPPFLAGS) -MMD -MP -c $< -o $@

$(COMPAT_O)/test-obj/%.o: $(COMPAT_TEST_DIR)/%.c | compat-guard
	@mkdir -p $(@D)
	$(COMPAT_CC_RUN) $(CC) $(COMPAT_CSTD) $(COMPAT_WARN) $(COMPAT_ASAN_FLAGS) $(NANOCOMPAT_CPPFLAGS) -MMD -MP -c $< -o $@

define COMPAT_TEST_RULE
$(COMPAT_O)/test/$(1): $(COMPAT_O)/test-obj/$(1).o $(patsubst %.c,$(COMPAT_O)/test-obj/%.o,$(patsubst %.cpp,$(COMPAT_O)/test-obj/%.o,$(COMPAT_EXTRA_$(1)))) $(COMPAT_ASAN_LIB) $(COMPAT_LIBTHIMSY_ASAN) | compat-guard
	@mkdir -p $$(@D)
	$$(COMPAT_CC_RUN) $$(CXX) $$(COMPAT_ASAN_FLAGS) $$(filter %.o,$$^) $$(NANOCOMPAT_ASAN_LIBS) -o $$@
endef
$(foreach t,$(COMPAT_TESTS),$(eval $(call COMPAT_TEST_RULE,$(t))))

# Runs every test program, prints its output and a total; fails if any program fails.
check-compat: compat-guard $(if $(filter 1,$(COMPAT_WITH_LIBTHIMSY)),compat-libthimsy-check) $(COMPAT_TEST_BINS)
	@echo "nano compat: image libraries: $(if $(NANOCOMPAT_IMAGE_LDLIBS),$(NANOCOMPAT_IMAGE_LDLIBS),none (decoding tests skip))"
	@mkdir -p $(COMPAT_O)/log; total=0; failed=0; skipped=0; bad=""; \
	for t in $(COMPAT_TESTS); do \
		echo "== $$t"; \
		if $(COMPAT_TEST_ENV) $(COMPAT_TEST_RUN) $(COMPAT_O)/test/$$t >$(COMPAT_O)/log/$$t.log 2>&1; then ok=1; else ok=0; fi; \
		cat $(COMPAT_O)/log/$$t.log; \
		line=$$(grep -E '^[0-9]+ tests, [0-9]+ failed$$' $(COMPAT_O)/log/$$t.log | tail -n 1); \
		n=$$(echo "$$line" | awk '{print $$1}'); m=$$(echo "$$line" | awk '{print $$3}'); \
		s=$$(sed -n 's/^\[  SKIPPED \] \([0-9][0-9]*\) tests\{0,1\}, listed below:$$/\1/p' $(COMPAT_O)/log/$$t.log | tail -n 1); \
		total=$$((total + $${n:-0})); failed=$$((failed + $${m:-0})); skipped=$$((skipped + $${s:-0})); \
		if [ $$ok = 0 ] || [ -z "$$line" ]; then bad="$$bad $$t"; fi; \
	done; \
	echo "nano compat: $$total tests, $$failed failed, $$skipped skipped"; \
	if [ -n "$$bad" ]; then echo "nano compat: FAILED:$$bad" >&2; exit 1; fi

# Removes only this file's own output directories.
compat-clean:
	@test -n "$(COMPAT_O)" && test "$(COMPAT_O)" != "/"
	rm -rf -- "$(COMPAT_O)/asan" "$(COMPAT_O)/rel" "$(COMPAT_O)/test-obj" "$(COMPAT_O)/test" "$(COMPAT_O)/log"

-include $(wildcard $(COMPAT_O)/asan/*.d $(COMPAT_O)/rel/*.d $(COMPAT_O)/test-obj/*.d)
