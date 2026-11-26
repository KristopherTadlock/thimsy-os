################################################################################
#
# scummvm
#
################################################################################

# ScummVM at the Android app's version: mods/added-apps.txt ships
# scummvm-2026.3.0.apk, built from the v2026.3.0 tag (IMPLEMENTATION.md E.3).
# ScummVM publishes its own source tarballs.
#
# Buildroot may package ScummVM itself (guess, unchecked offline); if the
# pinned Buildroot has package/scummvm, the two recipes clash and this one is
# renamed (tools/defconfig.py --check --buildroot DIR reports the clash).
SCUMMVM_VERSION = 2026.3.0
SCUMMVM_SOURCE = scummvm-$(SCUMMVM_VERSION).tar.xz
SCUMMVM_SITE = https://downloads.scummvm.org/frs/scummvm/$(SCUMMVM_VERSION)
SCUMMVM_LICENSE = GPL-3.0+
SCUMMVM_LICENSE_FILES = COPYING
# SDL through sdl2-config / pkg-config, as for ppsspp (decision I43).
SCUMMVM_DEPENDENCIES = host-pkgconf sdl2 libgles zlib libpng freetype

# ScummVM's configure is its own, not autoconf. The SDL back end with OpenGL
# ES 2; option names as remembered (guess, checked at the first build).
SCUMMVM_CONF_OPTS = \
	--host=$(GNU_TARGET_NAME) \
	--backend=sdl \
	--prefix=/usr \
	--enable-release \
	--disable-debug \
	--enable-opengl \
	--opengl-mode=gles2 \
	--with-sdl-prefix=$(STAGING_DIR)/usr \
	--disable-sparkle \
	--disable-updates \
	--disable-eventrecorder

define SCUMMVM_CONFIGURE_CMDS
	cd $(@D) && $(TARGET_CONFIGURE_OPTS) ./configure $(SCUMMVM_CONF_OPTS)
endef

define SCUMMVM_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D)
endef

define SCUMMVM_INSTALL_TARGET_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) DESTDIR=$(TARGET_DIR) install
endef

$(eval $(generic-package))
