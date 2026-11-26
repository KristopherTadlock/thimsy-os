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

# Layer 2 of the platform seam on the host (docs/linux/IMPLEMENTATION.md B.4): libnanostubs.a from
# nano/compat/stubs/src, and its checks, with g++ and make only. Fork-only (upstream.json).
#
# Standalone (from anywhere; it includes compat.mk for the shims, libthimsy and the guard):
#   nice -n 19 make -f nano/compat/stubs/stubs.mk -j1 check
#
# Included by nano/test/host/Makefile, which gets:
#   check-stubs            every stub header compiles alone (C++, and the NDK ones as C too), the
#                          stub test program builds and passes, and nano/test/host/stubgen/selftest.sh
#                          passes (python3 present) or is skipped
#   NANOSTUBS_CPPFLAGS     for nano_core's translation units, after NANOCOMPAT_CPPFLAGS: the stub
#                          headers and -DEGL_NO_PLATFORM_SPECIFIC_TYPES (EGLNativeWindowType is
#                          void*, so a stub Surface* passes to eglCreateWindowSurface as on Android)
#   NANOSTUBS_ASAN_LIB, NANOSTUBS_REL_LIB   libnanostubs.a, sanitized and not (link before
#                          NANOCOMPAT_*_LIBS: its logging goes through the compat log)

STUBS_MK := $(abspath $(lastword $(MAKEFILE_LIST)))
STUBS_DIR := $(patsubst %/,%,$(dir $(STUBS_MK)))
STUBS_NANO_DIR := $(abspath $(STUBS_DIR)/../..)

ifeq ($(COMPAT_MK),)
STUBS_STANDALONE := 1
include $(STUBS_NANO_DIR)/test/host/compat/compat.mk
endif

STUBS_O ?= $(abspath $(COMPAT_O)/../stubs)
NANOSTUBS_CPPFLAGS := -I$(STUBS_DIR)/include -DEGL_NO_PLATFORM_SPECIFIC_TYPES
STUBS_CPPFLAGS := $(NANOCOMPAT_CPPFLAGS) $(NANOSTUBS_CPPFLAGS)
# The stubs' own code: as strict as compat's. The test program: Nano's own flags (-Wall), since it
# copies Nano's call patterns, some of which -Wextra flags on Android's headers too.
STUBS_TEST_WARN := -Wall -Werror

STUBS_SRCS := stub_log ndk_media ndk_system
NANOSTUBS_ASAN_LIB := $(STUBS_O)/asan/libnanostubs.a
NANOSTUBS_REL_LIB := $(STUBS_O)/rel/libnanostubs.a
STUBS_HEADERS := $(sort $(patsubst $(STUBS_DIR)/include/%,%,$(shell find $(STUBS_DIR)/include -name '*.h' 2>/dev/null)))
STUBS_PYTHON ?= $(shell command -v python3 2>/dev/null)

.PHONY: check-stubs stubs-headers stubs-clean

ifeq ($(STUBS_STANDALONE),1)
check: check-stubs
.DEFAULT_GOAL := check
endif

$(STUBS_O)/asan/%.o: $(STUBS_DIR)/src/%.cpp | compat-guard
	@mkdir -p $(@D)
	$(COMPAT_CC_RUN) $(CXX) $(COMPAT_STD) $(COMPAT_WARN) $(COMPAT_ASAN_FLAGS) $(STUBS_CPPFLAGS) -MMD -MP -c $< -o $@

$(STUBS_O)/rel/%.o: $(STUBS_DIR)/src/%.cpp | compat-guard
	@mkdir -p $(@D)
	$(COMPAT_CC_RUN) $(CXX) $(COMPAT_STD) $(COMPAT_WARN) $(COMPAT_REL_FLAGS) $(STUBS_CPPFLAGS) -MMD -MP -c $< -o $@

$(NANOSTUBS_ASAN_LIB): $(STUBS_SRCS:%=$(STUBS_O)/asan/%.o)
	$(AR) rcs $@.new $^ && mv -f $@.new $@

$(NANOSTUBS_REL_LIB): $(STUBS_SRCS:%=$(STUBS_O)/rel/%.o)
	$(AR) rcs $@.new $^ && mv -f $@.new $@

# Each header alone, as C++ with compat's warnings; the C-capable ones together as C.
stubs-headers: | compat-guard
	@mkdir -p $(STUBS_O); n=0; bad=""; \
	for h in $(STUBS_HEADERS); do \
		n=$$((n + 1)); \
		printf '#include <%s>\n#include <%s>\n' "$$h" "$$h" >$(STUBS_O)/one.cpp; \
		$(COMPAT_CC_RUN) $(CXX) $(COMPAT_STD) $(COMPAT_WARN) $(STUBS_CPPFLAGS) -fsyntax-only $(STUBS_O)/one.cpp \
			|| bad="$$bad $$h"; \
	done; \
	$(COMPAT_CC_RUN) $(CC) $(COMPAT_CSTD) $(COMPAT_WARN) $(NANOSTUBS_CPPFLAGS) -I$(COMPAT_NANO_DIR)/compat/include \
		-fsyntax-only $(STUBS_DIR)/test/c_headers.c || bad="$$bad c_headers.c"; \
	echo "nano stubs: $$n headers compile alone (and the NDK ones as C)$${bad:+; FAILED:$$bad}"; \
	[ -z "$$bad" ]

$(STUBS_O)/test/stubs_test: $(STUBS_DIR)/test/stubs_test.cpp $(NANOSTUBS_ASAN_LIB) $(COMPAT_ASAN_LIB) \
		$(COMPAT_LIBTHIMSY_ASAN) | compat-guard
	@mkdir -p $(@D)
	$(COMPAT_CC_RUN) $(CXX) $(COMPAT_STD) $(STUBS_TEST_WARN) $(COMPAT_ASAN_FLAGS) $(STUBS_CPPFLAGS) \
		-I$(COMPAT_LIBTHIMSY_DIR)/testing -I$(COMPAT_LIBTHIMSY_DIR)/include -MMD -MP \
		$< $(NANOSTUBS_ASAN_LIB) $(NANOCOMPAT_ASAN_LIBS) -o $@

check-stubs: compat-guard stubs-headers $(STUBS_O)/test/stubs_test $(NANOSTUBS_REL_LIB)
	@echo "== stubs_test"; \
	$(COMPAT_TEST_ENV) $(COMPAT_TEST_RUN) $(STUBS_O)/test/stubs_test >$(STUBS_O)/stubs_test.log 2>&1; rc=$$?; \
	cat $(STUBS_O)/stubs_test.log; \
	grep -qE '^[0-9]+ tests, 0 failed$$' $(STUBS_O)/stubs_test.log && [ $$rc = 0 ] || { echo "nano stubs: stubs_test FAILED" >&2; exit 1; }
	@if [ -n "$(STUBS_PYTHON)" ]; then \
		CXX="prlimit --as=$(COMPAT_LIMIT_AS) -- nice -n 19 $(CXX)" PYTHON=$(STUBS_PYTHON) sh $(STUBS_NANO_DIR)/test/host/stubgen/selftest.sh \
			$(STUBS_O)/stubgen-selftest; \
	else echo "SKIP stubgen selftest: no python3"; fi

stubs-clean:
	@test -n "$(STUBS_O)" && test "$(STUBS_O)" != "/"
	rm -rf -- "$(STUBS_O)"

-include $(wildcard $(STUBS_O)/asan/*.d $(STUBS_O)/rel/*.d $(STUBS_O)/test/*.d)
