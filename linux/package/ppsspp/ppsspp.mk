################################################################################
#
# ppsspp
#
################################################################################

# PPSSPP at the Android app's version: mods/10-update-emulators.sh ships
# PPSSPP 1.20.4 (IMPLEMENTATION.md E.3). A git site, because the release
# tarballs GitHub makes leave out PPSSPP's submodules (FFmpeg, glslang,
# SPIRV-Cross and others).
PPSSPP_VERSION = v1.20.4
PPSSPP_SITE = https://github.com/hrydgard/ppsspp.git
PPSSPP_SITE_METHOD = git
PPSSPP_GIT_SUBMODULES = YES
PPSSPP_LICENSE = GPL-2.0+
PPSSPP_LICENSE_FILES = LICENSE.TXT
# SDL is reached through its sdl2 pkg-config module, so swapping Buildroot's
# SDL2 for sdl2-compat on SDL3 (decision I43) changes ppsspp's Config.in and
# apps-v1.fragment, not this file.
PPSSPP_DEPENDENCIES = host-pkgconf sdl2 libegl libgles zlib libpng
# GLES through EGL, no Vulkan on Mali (decision D11), the bundled FFmpeg, no
# X11 or Wayland; option names from PPSSPP's CMakeLists.txt as remembered
# (guess, checked at the first build).
PPSSPP_CONF_OPTS = \
	-DARM64=ON \
	-DUSING_EGL=ON \
	-DUSING_GLES2=ON \
	-DUSING_X11_VULKAN=OFF \
	-DUSE_WAYLAND_WSI=OFF \
	-DVULKAN=OFF \
	-DUSE_DISCORD=OFF \
	-DUSE_FFMPEG=ON \
	-DUSE_SYSTEM_FFMPEG=OFF \
	-DUSE_SYSTEM_LIBPNG=ON \
	-DUSE_SYSTEM_ZLIB=ON \
	-DHEADLESS=OFF \
	-DUNITTEST=OFF \
	-DSIMULATOR=OFF

# PPSSPP's CMake has no install rule for the SDL build: the binary and its
# assets go to /usr/share/ppsspp, where it looks for assets/ beside itself.
define PPSSPP_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(PPSSPP_BUILDDIR)/PPSSPPSDL $(TARGET_DIR)/usr/share/ppsspp/PPSSPPSDL
	mkdir -p $(TARGET_DIR)/usr/share/ppsspp/assets
	cp -R $(PPSSPP_BUILDDIR)/assets/. $(TARGET_DIR)/usr/share/ppsspp/assets/
	ln -sf ../share/ppsspp/PPSSPPSDL $(TARGET_DIR)/usr/bin/ppsspp
endef

$(eval $(cmake-package))
