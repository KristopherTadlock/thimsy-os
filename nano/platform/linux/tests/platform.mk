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

# The Linux platform's host tests (docs/linux/IMPLEMENTATION.md G9, G7-main, G7-display, G7-launch-b,
# G7-audio, G7-volumes, G7-power, G7-net, W8-EMU): the shell gate, the control keys, the display
# (display/Display.cpp and PanelOrientation.cpp over fakes), the launcher (Launcher.cpp, with real
# children), the platform's createPlatform (with nano/NanoPlatform.cpp), the audio streams' threads
# (AudioPull.cpp), the Volumes (on data/'s files), the power (Power.cpp over a fake sysfs), and the
# system actions and the network (actions/, over a fake wpa_supplicant), against Nano's compat
# shims and libthimsy (nano/test/host/compat/compat.mk), with g++ and make only. The display back end, the EGL wraps
# and the display's KMS binding (display/DisplayKms.cpp) need libdrm, GBM and EGL headers, and
# AudioAlsa.cpp alsa-lib's: they are not built here (G10 runs -fsyntax-only on them); the
# kernel's DRM uapi headers (drm.h, drm_mode.h) are found in PLAT_DRM_UAPI, after the system's own
# include directories.
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
# The kernel's DRM uapi headers (linux-libc-dev) for <drm.h> and <drm_mode.h>, as libdrm's include
# directory gives them on the image.
PLAT_DRM_UAPI ?= /usr/include/drm
PLAT_CPPFLAGS := $(COMPAT_TEST_CPPFLAGS) -I$(PLAT_DIR) -iquote $(PLAT_NANO_DIR) -idirafter $(PLAT_DRM_UAPI)

PLAT_TESTS := shellgate_test controlkeys_test linuxplatform_test display_test launcher_test audiopull_test volumes_test
PLAT_TESTS += power_test actions_test  # G7-power
# PLAT_SRCS_<test>: files of platform/linux; PLAT_NANO_SRCS_<test>: files of nano/.
PLAT_SRCS_shellgate_test := ShellGate.cpp
PLAT_SRCS_controlkeys_test := ControlKeys.cpp
PLAT_SRCS_linuxplatform_test := LinuxPlatform.cpp display/Display.cpp display/PanelOrientation.cpp Launcher.cpp ShellGate.cpp AudioPull.cpp Volumes.cpp
PLAT_SRCS_display_test := display/Display.cpp display/PanelOrientation.cpp
PLAT_SRCS_audiopull_test := AudioPull.cpp
PLAT_SRCS_volumes_test := Volumes.cpp
# The launcher (G7-launch-b) runs children: it links the shell gate for its execve (--wrap), and
# commitLaunch (NanoLaunchCommit.cpp) and the control keys for the path from a commit to a child.
PLAT_SRCS_launcher_test := Launcher.cpp ControlKeys.cpp ShellGate.cpp
PLAT_NANO_SRCS_linuxplatform_test := NanoPlatform.cpp NanoLaunchCommit.cpp
PLAT_NANO_SRCS_launcher_test := NanoPlatform.cpp NanoLaunchCommit.cpp
# G7-power: the power (with the control keys' request sequence) and the system actions.
PLAT_POWER_SRCS := Power.cpp ControlKeys.cpp actions/SystemActions.cpp actions/Run.cpp actions/WpaCtrl.cpp
PLAT_SRCS_linuxplatform_test += $(PLAT_POWER_SRCS)
PLAT_SRCS_power_test := Power.cpp ControlKeys.cpp
PLAT_SRCS_actions_test := actions/SystemActions.cpp actions/Run.cpp actions/WpaCtrl.cpp
# G7-input: the input policy (padd's intercept and tap).
PLAT_TESTS += inputpolicy_test
PLAT_SRCS_inputpolicy_test := InputPolicy.cpp
PLAT_SRCS_linuxplatform_test += InputPolicy.cpp
# G7-net: Wi-Fi over a fake wpa_supplicant socket, Bluetooth through canned bluetoothctl output.
PLAT_TESTS += network_test
PLAT_SRCS_network_test := actions/Network.cpp actions/SystemActions.cpp actions/Run.cpp actions/WpaCtrl.cpp
PLAT_SRCS_linuxplatform_test += actions/Network.cpp
# W8-EMU: the Emulators (a scratch core folder); it names cores as the launcher does (coreName).
PLAT_TESTS += emulators_test
PLAT_SRCS_emulators_test := Emulators.cpp Launcher.cpp ControlKeys.cpp ShellGate.cpp
PLAT_NANO_SRCS_emulators_test := NanoPlatform.cpp NanoLaunchCommit.cpp
PLAT_SRCS_linuxplatform_test += Emulators.cpp
PLAT_TEST_BINS := $(PLAT_TESTS:%=$(PLAT_O)/test/%)
# volumes_test reads thimsy-session's own volumes.json files from data/.
PLAT_TEST_DEFS := -DPLAT_TEST_DATA_DIR='"$(PLAT_TEST_DIR)/data"'

.PHONY: check check-platform-linux platform-linux-clean

$(PLAT_O)/obj/%.o: $(PLAT_DIR)/%.cpp | compat-guard
	@mkdir -p $(@D)
	$(COMPAT_CC_RUN) $(CXX) $(COMPAT_STD) $(COMPAT_WARN) $(COMPAT_ASAN_FLAGS) $(PLAT_CPPFLAGS) -MMD -MP -c $< -o $@

$(PLAT_O)/nano-obj/%.o: $(PLAT_NANO_DIR)/%.cpp | compat-guard
	@mkdir -p $(@D)
	$(COMPAT_CC_RUN) $(CXX) $(COMPAT_STD) $(COMPAT_WARN) $(COMPAT_ASAN_FLAGS) $(PLAT_CPPFLAGS) -MMD -MP -c $< -o $@

$(PLAT_O)/test-obj/%.o: $(PLAT_TEST_DIR)/%.cpp | compat-guard
	@mkdir -p $(@D)
	$(COMPAT_CC_RUN) $(CXX) $(COMPAT_STD) $(COMPAT_WARN) $(COMPAT_ASAN_FLAGS) $(PLAT_CPPFLAGS) $(PLAT_TEST_DEFS) -MMD -MP -c $< -o $@

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

-include $(wildcard $(PLAT_O)/obj/*.d $(PLAT_O)/obj/*/*.d $(PLAT_O)/nano-obj/*.d $(PLAT_O)/test-obj/*.d)
