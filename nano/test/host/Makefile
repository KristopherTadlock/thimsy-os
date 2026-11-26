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

# Nano's host build with g++ 9.4 and make, no CMake (docs/linux/IMPLEMENTATION.md C.3, D.1). Reads
# nano/build/sources.txt and nano/build/unit-closure.txt (awk turns them into make variables) and
# builds out of tree under O, so nothing is written next to the sources. Run it as
#
#   nice -n 19 make -C nano/test/host -j1 check-compat check-unit-0
#
# Targets:
#   check-compat   libthimsy's and compat/'s tests (compat/compat.mk)
#   check-unit-0   every test/unit program whose closure needs no outside library, through the shims,
#                  with libthimsy/testing's GoogleTest stand-in; lists the others with their needs
#   check-unit     every test/unit program; a program whose outside headers are missing prints
#                  "SKIP <program>: missing <header>" (heavy: refused while an Android build may run)
#   syntax-core    g++ -fsyntax-only of every [core] file, no sanitizers (heavy; SKIPs without the
#                  library headers or G4's compat/stubs)
#   headless       nano-headless for the host E2E (heavy; SKIPs until G5's files and the headers exist)
#   list-unit      each test program, its production files, its needs and whether check-unit-0 has it
#   clean          removes O's unit, syntax and headless trees (compat-clean removes compat's)
#   check          check-compat and check-unit-0 (the default)
#
# Every target first runs linux/tools/build_guard.sh --light --target <the heaviest goal asked>: while
# an Android build may be running only check-compat and check-unit-0 pass (with 6 GB available), and
# nothing passes under the memory floor. The heavy targets probe their headers before the guard, so a
# machine without the libraries prints SKIP and compiles nothing. Compilers run at nice 19 under a
# 3 GiB address-space limit with OOM score 1000; test programs run with randomisation off (setarch -R:
# GCC 9's ASan hangs on WSL2 otherwise) under a timeout. nano/test/host/DEPS.md lists the packages.
#
# Variables: O (default ~/.cache/thimsy/nano-host), GTEST (standin, or system for an installed
# GoogleTest), GUARD (the guard script; empty skips it, leaving compat.mk's memory floor), PROGRAMS
# (limit check-unit-0 and check-unit to these programs), TEST_ARGS (passed to every test program),
# NANO_DATA (a tree holding the base's asset folders, ps3xmb/ and the rest: the output of
# `nano_tree.py compose`; default <repo>/work/nano-composed when it exists).
#
# Test data: the programs read test/fixtures and, some of them, the asset folders. nano/ has only the
# asset files the patch changes, so each run lays out $(O)/unit/data the way Soong installs `data`:
# test/fixtures from nano/, each asset folder from NANO_DATA overlaid with nano/'s (as compose does),
# all as symbolic links. A program whose test names an asset folder (@<folder> in unit-closure.txt)
# is skipped, with the reason, when NANO_DATA is not set.

HOST_DIR := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
NANO := $(abspath $(HOST_DIR)/../..)
REPO := $(abspath $(NANO)/..)
LIBTHIMSY := $(REPO)/libthimsy
O ?= $(HOME)/.cache/thimsy/nano-host
COMPAT_O ?= $(O)/compat
GUARD ?= $(wildcard $(REPO)/linux/tools/build_guard.sh)
GTEST ?= standin
NANO_DATA ?= $(wildcard $(REPO)/work/nano-composed)
CXX ?= g++
SOURCES_TXT := $(NANO)/build/sources.txt
CLOSURE_TXT := $(NANO)/build/unit-closure.txt

.PHONY: check check-unit-0 check-unit syntax-core headless list-unit clean host-guard
.SUFFIXES:
.DEFAULT_GOAL := check

include $(HOST_DIR)/compat/compat.mk

# --- the lists -------------------------------------------------------------------------------------
# sources.txt: SRC_<section> (dashes become underscores). unit-closure.txt: UNIT_PROGRAMS, UNIT0_PROGRAMS
# (no outside headers), UNIT_SRCS_<program>, UNIT_NEEDS_<program> (headers) and UNIT_ASSETS_<program>
# (asset folders).
$(O)/lists.mk: $(SOURCES_TXT) $(CLOSURE_TXT) $(HOST_DIR)/Makefile
	@mkdir -p $(@D)
	@awk '/^[ \t]*(#|$$)/ { next } \
	  /^\[[a-z0-9-]+\][ \t]*$$/ { s = $$0; sub(/^\[/, "", s); sub(/\][ \t]*$$/, "", s); gsub(/-/, "_", s); \
	    print "SRC_" s " :="; next } \
	  { gsub(/[ \t]/, ""); print "SRC_" s " += " $$0 }' $(SOURCES_TXT) >$@.new
	@awk -F '\t' '/^#/ || NF < 3 { next } \
	  { srcs = ($$2 == "-") ? "" : $$2; needs = ""; assets = ""; n = split(($$3 == "-") ? "" : $$3, w, " "); \
	    for (i = 1; i <= n; i++) if (w[i] ~ /^@/) assets = assets " " substr(w[i], 2); else needs = needs " " w[i]; \
	    print "UNIT_PROGRAMS += " $$1; print "UNIT_SRCS_" $$1 " := " srcs; print "UNIT_NEEDS_" $$1 " :=" needs; \
	    print "UNIT_ASSETS_" $$1 " :=" assets; if (needs == "") print "UNIT0_PROGRAMS += " $$1 }' $(CLOSURE_TXT) >>$@.new
	@mv -f $@.new $@
-include $(O)/lists.mk

ifneq ($(PROGRAMS),)
UNIT_SELECTED := $(filter $(PROGRAMS),$(UNIT_PROGRAMS))
else
UNIT_SELECTED := $(UNIT_PROGRAMS)
endif
UNIT0_SELECTED := $(filter $(UNIT0_PROGRAMS),$(UNIT_SELECTED))
# The programs that can't run for want of NANO_DATA.
NANO_DATA_ABS := $(if $(NANO_DATA),$(abspath $(NANO_DATA)))
UNIT_NODATA := $(if $(NANO_DATA_ABS),,$(foreach p,$(UNIT_PROGRAMS),$(if $(strip $(UNIT_ASSETS_$(p))),$(p))))

# --- the guard -------------------------------------------------------------------------------------
HEAVY_GOALS := headless syntax-core check-unit
GUARD_GOAL := $(firstword $(filter $(HEAVY_GOALS),$(MAKECMDGOALS)) $(filter check-unit-0 check-compat,$(MAKECMDGOALS)) check-unit-0)
host-guard:
ifneq ($(GUARD),)
	@sh $(GUARD) --light --target $(GUARD_GOAL)
else
	@echo "nano host: no linux/tools/build_guard.sh; only compat.mk's memory floor applies" >&2
endif
# compat.mk's own check (memory floor, tools) runs after ours; everything compat builds waits for it.
compat-guard: host-guard

# --- flags -----------------------------------------------------------------------------------------
# GCC-only warnings Clang does not give, so no Nano file is edited for GCC's sake (C.3). One place.
# -Wdangling-else: GCC warns on "if (c) EXPECT_TRUE(x);" although the macro blocks the else (as
# GoogleTest's does); Clang does not.
GCC_SUPPRESS := -Wno-class-memaccess -Wno-stringop-truncation -Wno-format-truncation -Wno-dangling-else
UNIT_STD := -std=gnu++17
UNIT_WARN := -Wall -Werror $(GCC_SUPPRESS)
UNIT_FLAGS := $(COMPAT_ASAN_FLAGS)
# compat/include first (NANOCOMPAT_CPPFLAGS), then nano/ and Android.bp's local_include_dirs, then the
# tests' helpers: the order tools/nano-tree/include_census.py resolves includes in.
NANO_CPPFLAGS := $(NANOCOMPAT_CPPFLAGS) -I$(NANO) -I$(NANO)/liba52/include -I$(NANO)/nanosvg
ifeq ($(GTEST),system)
GTEST_CPPFLAGS :=
GTEST_LIBS := -lgtest_main -lgtest
else
GTEST_CPPFLAGS := -I$(LIBTHIMSY)/testing
GTEST_LIBS :=
endif
UNIT_CPPFLAGS := $(NANO_CPPFLAGS) -I$(NANO)/test/unit $(GTEST_CPPFLAGS)
# The libraries behind the outside headers a program needs.
unit_ldlibs = $(strip $(if $(filter zlib.h,$(1)),-lz) $(if $(filter openssl/%,$(1)),-lcrypto) \
	$(if $(filter tinyxml2.h,$(1)),-ltinyxml2) $(if $(filter ziparchive/%,$(1)),-lziparchive))

# Header probes, with the include flags the library packages need. A probe runs only for a goal
# that needs it.
LIB_CPPFLAGS := $(or $(shell pkg-config --cflags freetype2 libdrm 2>/dev/null),-I/usr/include/freetype2 -I/usr/include/libdrm)
have_header = $(shell printf '\043include <%s>\n' '$(1)' | $(CXX) $(UNIT_STD) $(LIB_CPPFLAGS) -E -x c++ - >/dev/null 2>&1 && echo yes)
probe_headers = $(foreach h,$(1),$(eval HAVE_$(h) := $(call have_header,$(h))))
missing_of = $(strip $(foreach h,$(1),$(if $(HAVE_$(h)),,$(h))))

# --- unit programs ---------------------------------------------------------------------------------
UNIT_O := $(O)/unit
unit_bin = $(UNIT_O)/bin/$(1)
unit_objs = $(UNIT_O)/obj/test/unit/$(1).o $(patsubst %.cpp,$(UNIT_O)/obj/%.o,$(UNIT_SRCS_$(1)))

$(UNIT_O)/obj/%.o: $(NANO)/%.cpp | host-guard compat-guard
	@mkdir -p $(@D)
	$(COMPAT_CC_RUN) $(CXX) $(UNIT_STD) $(UNIT_WARN) $(UNIT_FLAGS) $(UNIT_CPPFLAGS) -MMD -MP -c $< -o $@

define UNIT_RULE
$(call unit_bin,$(1)): $(call unit_objs,$(1)) $(COMPAT_ASAN_LIB) $(COMPAT_LIBTHIMSY_ASAN) | host-guard compat-guard
	@mkdir -p $$(@D)
	$$(COMPAT_CC_RUN) $$(CXX) $$(UNIT_FLAGS) $$(filter %.o,$$^) $$(GTEST_LIBS) $$(call unit_ldlibs,$$(UNIT_NEEDS_$(1))) \
		$$(NANOCOMPAT_ASAN_LIBS) -o $$@
endef
$(foreach p,$(UNIT_PROGRAMS),$(eval $(call UNIT_RULE,$(p))))

# Runs the programs $(1) under the name $(2); prints each one's output and a total; fails if any
# program fails. Totals come from the stand-in's "N tests, M failed" line, or GoogleTest's own lines.
UNIT_DATA := $(UNIT_O)/data
UNIT_ENV = NANO_TEST_FIXTURES=$(UNIT_DATA)/test/fixtures NANO_DAIJISHOU_DIR=$(UNIT_DATA)/ps3xmb/daijishou \
	TMPDIR=$(UNIT_O)/tmp $(COMPAT_TEST_ENV)
ASSET_DIRS := ps3xmb nano_esde_themes nano_esde_assets
# Real directories holding links to files, so a test's "fixtures/../../ps3xmb" stays in the data tree.
define UNIT_DATA_SETUP
rm -rf $(UNIT_DATA) && mkdir -p $(UNIT_DATA)/test && cp -Rs $(NANO)/test/fixtures $(UNIT_DATA)/test/ && \
	for d in $(ASSET_DIRS); do \
		if [ -n "$(NANO_DATA_ABS)" ] && [ -d "$(NANO_DATA_ABS)/$$d" ]; then cp -Rs "$(NANO_DATA_ABS)/$$d" $(UNIT_DATA)/ || exit 1; fi; \
		if [ -d $(NANO)/$$d ]; then mkdir -p $(UNIT_DATA)/$$d && cp -Rsf $(NANO)/$$d/. $(UNIT_DATA)/$$d/ || exit 1; fi; \
	done
endef
define UNIT_RUN
$(UNIT_DATA_SETUP); \
	mkdir -p $(UNIT_O)/log $(UNIT_O)/tmp $(UNIT_O)/run; total=0; failed=0; skipped=0; bad=""; \
	for t in $(1); do \
		echo "== $$t"; \
		log=$(UNIT_O)/log/$$t.log; \
		if (cd $(UNIT_O)/run && $(UNIT_ENV) $(COMPAT_TEST_RUN) $(UNIT_O)/bin/$$t $(TEST_ARGS)) >$$log 2>&1; then ok=1; else ok=0; fi; \
		cat $$log; \
		line=$$(grep -E '^[0-9]+ tests, [0-9]+ failed$$' $$log | tail -n 1); \
		if [ -n "$$line" ]; then \
			n=$$(echo "$$line" | awk '{print $$1}'); m=$$(echo "$$line" | awk '{print $$3}'); \
		else \
			n=$$(sed -n 's/^\[==========\] \([0-9][0-9]*\) tests\{0,1\} from .* ran\..*/\1/p' $$log | tail -n 1); \
			m=$$(sed -n 's/^\[  FAILED  \] \([0-9][0-9]*\) tests\{0,1\}, listed below:$$/\1/p' $$log | tail -n 1); \
			[ -n "$$n" ] && line=gtest; \
		fi; \
		s=$$(sed -n 's/^\[  SKIPPED \] \([0-9][0-9]*\) tests\{0,1\}, listed below:$$/\1/p' $$log | tail -n 1); \
		total=$$((total + $${n:-0})); failed=$$((failed + $${m:-0})); skipped=$$((skipped + $${s:-0})); \
		if [ $$ok = 0 ] || [ -z "$$line" ]; then bad="$$bad $$t"; fi; \
	done; \
	echo "nano $(2): $$total tests, $$failed failed, $$skipped skipped, in $(words $(1)) programs"; \
	if [ -n "$$bad" ]; then echo "nano $(2): FAILED:$$bad" >&2; exit 1; fi
endef

UNIT0_RUNNABLE := $(filter-out $(UNIT_NODATA),$(UNIT0_SELECTED))
nodata_skips = $(foreach p,$(filter $(UNIT_NODATA),$(1)),echo "SKIP $(p): needs the base's$(UNIT_ASSETS_$(p)) (set NANO_DATA, or run: py -3.12 tools/nano-tree/nano_tree.py compose work/nano-composed)";)

check-unit-0: host-guard $(foreach p,$(UNIT0_RUNNABLE),$(call unit_bin,$(p)))
	@$(foreach p,$(filter-out $(UNIT0_SELECTED),$(UNIT_SELECTED)),echo "not in check-unit-0: $(p) (needs$(UNIT_NEEDS_$(p)))";)
	@$(call nodata_skips,$(UNIT0_SELECTED))
	@$(call UNIT_RUN,$(UNIT0_RUNNABLE),check-unit-0)

ifneq ($(filter check-unit,$(MAKECMDGOALS)),)
$(call probe_headers,$(sort $(foreach p,$(UNIT_SELECTED),$(UNIT_NEEDS_$(p)))))
endif
UNIT_HEADERS_OK := $(foreach p,$(UNIT_SELECTED),$(if $(call missing_of,$(UNIT_NEEDS_$(p))),,$(p)))
UNIT_RUNNABLE := $(filter-out $(UNIT_NODATA),$(UNIT_HEADERS_OK))

check-unit: host-guard $(foreach p,$(UNIT_RUNNABLE),$(call unit_bin,$(p)))
	@$(foreach p,$(filter-out $(UNIT_HEADERS_OK),$(UNIT_SELECTED)),echo "SKIP $(p): missing $(call missing_of,$(UNIT_NEEDS_$(p)))";)
	@$(call nodata_skips,$(UNIT_HEADERS_OK))
	@$(call UNIT_RUN,$(UNIT_RUNNABLE),check-unit)

list-unit:
	@$(foreach p,$(UNIT_PROGRAMS),printf '%s\t%s\t%s\t%s\t%s\n' '$(p)' '$(if $(filter $(p),$(UNIT0_PROGRAMS)),check-unit-0,check-unit)' \
		'$(or $(UNIT_SRCS_$(p)),-)' '$(or $(strip $(UNIT_NEEDS_$(p))),-)' '$(or $(strip $(UNIT_ASSETS_$(p))),-)';)

# --- syntax-core (deferred to G10) -----------------------------------------------------------------
# The libraries [core] compiles against (DEPS.md), and G4's layer-2 stubs. Without any of them it
# prints SKIP and compiles nothing; with all of them it runs the guard first.
CORE_HEADERS := zlib.h openssl/md5.h openssl/sha.h tinyxml2.h ft2build.h png.h curl/curl.h EGL/egl.h \
	EGL/eglext.h GLES2/gl2.h GLES2/gl2ext.h GLES3/gl3.h drm.h drm_fourcc.h
STUBS_INCLUDE := $(wildcard $(NANO)/compat/stubs/include)
CORE_CPPFLAGS := $(NANO_CPPFLAGS) $(addprefix -I,$(STUBS_INCLUDE)) $(LIB_CPPFLAGS) -DGL_GLEXT_PROTOTYPES -DEGL_EGLEXT_PROTOTYPES
ifneq ($(filter syntax-core headless,$(MAKECMDGOALS)),)
$(call probe_headers,$(CORE_HEADERS))
endif
CORE_MISSING = $(if $(STUBS_INCLUDE),,nano/compat/stubs/include (G4)) $(call missing_of,$(CORE_HEADERS))

syntax-core:
	@if [ -n "$(strip $(CORE_MISSING))" ]; then echo "SKIP syntax-core: missing $(strip $(CORE_MISSING))"; exit 0; fi; \
	$(if $(GUARD),sh $(GUARD) --light --target syntax-core || exit $$?;) \
	mkdir -p $(O)/syntax; bad=""; n=0; \
	for f in $(SRC_core); do \
		n=$$((n + 1)); echo "== $$f"; \
		$(COMPAT_CC_RUN) $(CXX) $(UNIT_STD) -Wall -Werror $(GCC_SUPPRESS) $(CORE_CPPFLAGS) -fsyntax-only $(NANO)/$$f \
			|| bad="$$bad $$f"; \
	done; \
	echo "nano syntax-core: $$n files$${bad:+, failed:$$bad}"; \
	[ -z "$$bad" ]

# --- headless (written, not compiled: G5 writes its sources, G10 builds and runs it) ---------------
HEADLESS_MAIN := $(NANO)/test/host/headless/main_headless.cpp
HEADLESS_O := $(O)/headless
HEADLESS_SRCS := $(SRC_core) $(SRC_host)
HEADLESS_OBJS := $(patsubst %.cpp,$(HEADLESS_O)/obj/%.o,$(filter %.cpp,$(HEADLESS_SRCS)))
GLSTUB_OBJS := $(patsubst $(NANO)/%.c,$(HEADLESS_O)/obj/%.o,$(wildcard $(NANO)/test/host/glstub/*.c))
HEADLESS_WRAP := -Wl,--wrap=system,--wrap=popen,--wrap=pclose,--wrap=execv,--wrap=execvp,--wrap=execve,--wrap=ioctl
HEADLESS_LDLIBS := -ltinyxml2 -lcurl -lssl -lcrypto -lz -lfreetype -lpng
HEADLESS_MISSING = $(if $(wildcard $(HEADLESS_MAIN)),,nano/test/host/headless/main_headless.cpp (G5)) \
	$(if $(SRC_host),,sources.txt [host] (G5)) $(CORE_MISSING)

$(HEADLESS_O)/obj/%.o: $(NANO)/%.cpp | host-guard compat-guard
	@mkdir -p $(@D)
	$(COMPAT_CC_RUN) $(CXX) $(UNIT_STD) -Wall $(GCC_SUPPRESS) $(UNIT_FLAGS) $(CORE_CPPFLAGS) -MMD -MP -c $< -o $@
$(HEADLESS_O)/obj/%.o: $(NANO)/%.c | host-guard compat-guard
	@mkdir -p $(@D)
	$(COMPAT_CC_RUN) $(CC) $(COMPAT_CSTD) -Wall $(UNIT_FLAGS) -c $< -o $@
$(HEADLESS_O)/nano-headless: $(HEADLESS_OBJS) $(GLSTUB_OBJS) $(COMPAT_ASAN_LIB) $(COMPAT_LIBTHIMSY_ASAN) | host-guard compat-guard
	$(COMPAT_CC_RUN) $(CXX) $(UNIT_FLAGS) $(HEADLESS_WRAP) $(filter %.o,$^) $(HEADLESS_LDLIBS) $(NANOCOMPAT_ASAN_LIBS) -o $@

ifeq ($(strip $(HEADLESS_MISSING)),)
headless: host-guard $(HEADLESS_O)/nano-headless
	@echo "nano headless: built $(HEADLESS_O)/nano-headless"
else
headless:
	@echo "SKIP headless: missing $(strip $(HEADLESS_MISSING))"
endif

check: check-compat check-unit-0

clean:
	@test -n "$(O)" && test "$(O)" != "/"
	rm -rf -- "$(UNIT_O)" "$(O)/syntax" "$(HEADLESS_O)" "$(O)/lists.mk"

-include $(wildcard $(UNIT_O)/obj/*.d $(UNIT_O)/obj/*/*.d $(UNIT_O)/obj/*/*/*.d $(HEADLESS_O)/obj/*.d $(HEADLESS_O)/obj/*/*.d)
