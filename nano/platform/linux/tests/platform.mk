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

# The Linux platform's host tests (docs/linux/IMPLEMENTATION.md G9, G7-main): the shell gate, the
# control keys and the platform's createPlatform (with nano/NanoPlatform.cpp), against Nano's compat
# shims and libthimsy (nano/test/host/compat/compat.mk), with g++ and make only. The display back end and the EGL wraps need libdrm, GBM and EGL headers and are not
# built here (G10 runs -fsyntax-only on them).
#
#   nice -n 19 make -f nano/platform/linux/tests/platform.mk -j1 check
#
# The same memory guard, limits and sanitizers as compat.mk; output under PLAT_O.

PLAT_MK := $(abspath $(lastword $(MAKEFILE_LIST)))
PLAT_TEST_DIR := $(patsubst %/,%,$(dir $(PLAT_MK)))
PLAT_DIR := $(abspath $(PLAT_TEST_DIR)/..)
PLAT_NANO_DIR := $(abspath $(PLAT_DIR)/../..)
PLAT_O ?= $(HOME)/.cache/thimsy/nano-host/platform-linux
COMPAT_O ?= $(HOME)/.cache/thimsy/nano-host/compat
COMPAT_WITH_LIBTHIMSY ?= 0

check: check-platform-linux
.DEFAULT_GOAL := check

include $(PLAT_NANO_DIR)/test/host/compat/compat.mk

PLAT_WRAPS := $(shell sed -e 's/\#.*//' $(PLAT_DIR)/linker-wraps.txt)
PLAT_WRAP_LDFLAGS := $(foreach s,$(PLAT_WRAPS),-Wl,--wrap=$(s))
# nano/ for quoted includes only ("NanoPlatform.h"), so no header of nano/ shadows a shim.
PLAT_CPPFLAGS := $(COMPAT_TEST_CPPFLAGS) -I$(PLAT_DIR) -iquote $(PLAT_NANO_DIR)

PLAT_TESTS := shellgate_test controlkeys_test linuxplatform_test
# PLAT_SRCS_<test>: files of platform/linux; PLAT_NANO_SRCS_<test>: files of nano/.
PLAT_SRCS_shellgate_test := ShellGate.cpp
PLAT_SRCS_controlkeys_test := ControlKeys.cpp
PLAT_SRCS_linuxplatform_test := LinuxPlatform.cpp
PLAT_NANO_SRCS_linuxplatform_test := NanoPlatform.cpp
PLAT_TEST_BINS := $(PLAT_TESTS:%=$(PLAT_O)/test/%)

.PHONY: check check-platform-linux platform-linux-clean

$(PLAT_O)/obj/%.o: $(PLAT_DIR)/%.cpp | compat-guard
	@mkdir -p $(@D)
	$(COMPAT_CC_RUN) $(CXX) $(COMPAT_STD) $(COMPAT_WARN) $(COMPAT_ASAN_FLAGS) $(PLAT_CPPFLAGS) -MMD -MP -c $< -o $@

$(PLAT_O)/nano-obj/%.o: $(PLAT_NANO_DIR)/%.cpp | compat-guard
	@mkdir -p $(@D)
	$(COMPAT_CC_RUN) $(CXX) $(COMPAT_STD) $(COMPAT_WARN) $(COMPAT_ASAN_FLAGS) $(PLAT_CPPFLAGS) -MMD -MP -c $< -o $@

$(PLAT_O)/test-obj/%.o: $(PLAT_TEST_DIR)/%.cpp | compat-guard
	@mkdir -p $(@D)
	$(COMPAT_CC_RUN) $(CXX) $(COMPAT_STD) $(COMPAT_WARN) $(COMPAT_ASAN_FLAGS) $(PLAT_CPPFLAGS) -MMD -MP -c $< -o $@

define PLAT_TEST_RULE
$(PLAT_O)/test/$(1): $(PLAT_O)/test-obj/$(1).o $(PLAT_SRCS_$(1):%.cpp=$(PLAT_O)/obj/%.o) \
		$(PLAT_NANO_SRCS_$(1):%.cpp=$(PLAT_O)/nano-obj/%.o) $(COMPAT_ASAN_LIB) $(COMPAT_LIBTHIMSY_ASAN) | compat-guard
	@mkdir -p $$(@D)
	$$(COMPAT_CC_RUN) $$(CXX) $$(COMPAT_ASAN_FLAGS) $$(PLAT_WRAP_LDFLAGS) $$(filter %.o,$$^) $$(NANOCOMPAT_ASAN_LIBS) -o $$@
endef
$(foreach t,$(PLAT_TESTS),$(eval $(call PLAT_TEST_RULE,$(t))))

check-platform-linux: compat-guard $(PLAT_TEST_BINS)
	@mkdir -p $(PLAT_O)/log; total=0; failed=0; bad=""; \
	for t in $(PLAT_TESTS); do \
		echo "== $$t"; \
		if $(COMPAT_TEST_ENV) $(COMPAT_TEST_RUN) $(PLAT_O)/test/$$t >$(PLAT_O)/log/$$t.log 2>&1; then ok=1; else ok=0; fi; \
		cat $(PLAT_O)/log/$$t.log; \
		line=$$(grep -E '^[0-9]+ tests, [0-9]+ failed$$' $(PLAT_O)/log/$$t.log | tail -n 1); \
		n=$$(echo "$$line" | awk '{print $$1}'); m=$$(echo "$$line" | awk '{print $$3}'); \
		total=$$((total + $${n:-0})); failed=$$((failed + $${m:-0})); \
		if [ $$ok = 0 ] || [ -z "$$line" ]; then bad="$$bad $$t"; fi; \
	done; \
	echo "nano platform-linux: $$total tests, $$failed failed"; \
	if [ -n "$$bad" ]; then echo "nano platform-linux: FAILED:$$bad" >&2; exit 1; fi

platform-linux-clean:
	@test -n "$(PLAT_O)" && test "$(PLAT_O)" != "/"
	rm -rf -- "$(PLAT_O)/obj" "$(PLAT_O)/nano-obj" "$(PLAT_O)/test-obj" "$(PLAT_O)/test" "$(PLAT_O)/log"

-include $(wildcard $(PLAT_O)/obj/*.d $(PLAT_O)/nano-obj/*.d $(PLAT_O)/test-obj/*.d)
