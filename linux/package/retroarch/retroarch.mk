################################################################################
#
# retroarch
#
################################################################################

# RetroArch at the Android build's version: mods/10-update-emulators.sh ships
# RetroArch 1.22.2 (the stable APK), so saves, states and core options match
# (IMPLEMENTATION.md E.3). Thimsy's patches, such as drm_ctx.c's rotation, go
# in linux/gpl/patches/retroarch/ (decision I9), read through
# BR2_GLOBAL_PATCH_DIR.
RETROARCH_VERSION = 1.22.2
RETROARCH_SITE = $(call github,libretro,RetroArch,v$(RETROARCH_VERSION))
RETROARCH_LICENSE = GPL-3.0+
RETROARCH_LICENSE_FILES = COPYING
RETROARCH_DEPENDENCIES = \
	host-pkgconf \
	libegl \
	libgles \
	libgbm \
	libdrm \
	alsa-lib \
	udev \
	zlib

# The core info files of the same release (libretro-core-info is tagged with
# RetroArch's versions). Only the enabled cores' files are installed
# (LIBRETRO_CORE_NAMES, from package/libretro/libretro.mk).
RETROARCH_CORE_INFO = libretro-core-info-$(RETROARCH_VERSION).tar.gz
RETROARCH_EXTRA_DOWNLOADS = \
	https://github.com/libretro/libretro-core-info/archive/v$(RETROARCH_VERSION)/$(RETROARCH_CORE_INFO)

# RetroArch's configure is its own (qb/), not autoconf. KMS/DRM with EGL and
# OpenGL ES 3.x, ALSA and libudev; no X11, Wayland, Qt, PulseAudio, SDL or
# Vulkan (no Vulkan on Mali, decision D11). Flag names from qb/config.params.sh
# as remembered (guess, checked at the first build).
RETROARCH_CONF_OPTS = \
	--prefix=/usr \
	--enable-kms \
	--enable-egl \
	--enable-opengles \
	--enable-opengles3 \
	--enable-opengles3_1 \
	--disable-opengl1 \
	--disable-vulkan \
	--enable-alsa \
	--enable-udev \
	--enable-zlib \
	--enable-networking \
	--disable-x11 \
	--disable-wayland \
	--disable-qt \
	--disable-pulse \
	--disable-jack \
	--disable-oss \
	--disable-sdl \
	--disable-sdl2 \
	--disable-ffmpeg \
	--disable-discord \
	--disable-update_assets \
	--disable-update_cores

define RETROARCH_CONFIGURE_CMDS
	cd $(@D) && $(TARGET_CONFIGURE_OPTS) \
		CROSS_COMPILE="$(TARGET_CROSS)" \
		PKG_CONF_PATH="$(PKG_CONFIG_HOST_BINARY)" \
		./configure $(RETROARCH_CONF_OPTS)
endef

define RETROARCH_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D)
endef

define RETROARCH_INSTALL_TARGET_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) DESTDIR=$(TARGET_DIR) install
	mkdir -p $(@D)/core-info $(TARGET_DIR)/usr/share/libretro/info
	$(call suitable-extractor,$(RETROARCH_CORE_INFO)) $(RETROARCH_DL_DIR)/$(RETROARCH_CORE_INFO) | \
		$(TAR) --strip-components=1 -C $(@D)/core-info $(TAR_OPTIONS) -
	for c in $(sort $(LIBRETRO_CORE_NAMES)); do \
		test -f $(@D)/core-info/$${c}_libretro.info || { \
			echo "retroarch: no core info for $$c in $(RETROARCH_CORE_INFO)"; exit 1; }; \
		$(INSTALL) -m 0644 $(@D)/core-info/$${c}_libretro.info \
			$(TARGET_DIR)/usr/share/libretro/info/; \
	done
endef

$(eval $(generic-package))
