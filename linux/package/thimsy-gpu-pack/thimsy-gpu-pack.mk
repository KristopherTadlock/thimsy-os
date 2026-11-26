################################################################################
#
# thimsy-gpu-pack
#
################################################################################

# Moves this tree's Mesa files out of the tier root into gpu-<driver>.erofs in
# the images directory, one per enabled driver set (decision D25), through
# src/gpu_pack.py as a target-finalize hook: it runs after every package has
# installed and before the root image is made. The "source" is that script, in
# this external tree (MIT), so a local site is enough.
# The version is the commit (build.sh passes THIMSY_GIT_SHA), so every commit
# gets its own build directory and the sources are synced again: Buildroot
# rsyncs a local site only once per build directory (build-integration-3).
THIMSY_GPU_PACK_VERSION = $(or $(THIMSY_GIT_SHA),dev)
THIMSY_GPU_PACK_SITE = $(BR2_EXTERNAL_THIMSY_PATH)/package/thimsy-gpu-pack/src
THIMSY_GPU_PACK_SITE_METHOD = local
THIMSY_GPU_PACK_LICENSE = MIT
THIMSY_GPU_PACK_LICENSE_FILES = gpu_pack.py
THIMSY_GPU_PACK_DEPENDENCIES = mesa3d host-erofs-utils host-python3

THIMSY_GPU_PACK_DRIVERS = \
	$(if $(BR2_PACKAGE_THIMSY_GPU_PACK_PANFROST),panfrost) \
	$(if $(BR2_PACKAGE_THIMSY_GPU_PACK_SWRAST),swrast)

# Nothing is installed: the hook below takes files out of the target instead.
define THIMSY_GPU_PACK_INSTALL_TARGET_CMDS
	@:
endef

# gpu_pack.py refuses to run with no --driver: select at least one driver set.
define THIMSY_GPU_PACK_FINALIZE
	$(HOST_DIR)/bin/python3 $(THIMSY_GPU_PACK_DIR)/gpu_pack.py \
		--target $(TARGET_DIR) \
		--work $(BUILD_DIR)/thimsy-gpu-pack-work \
		--out $(BINARIES_DIR) \
		--mkfs $(HOST_DIR)/bin/mkfs.erofs \
		--epoch "$(SOURCE_DATE_EPOCH)" \
		$(foreach d,$(strip $(THIMSY_GPU_PACK_DRIVERS)),--driver $(d))
endef

ifeq ($(BR2_PACKAGE_THIMSY_GPU_PACK),y)
TARGET_FINALIZE_HOOKS += THIMSY_GPU_PACK_FINALIZE
endif

$(eval $(generic-package))
