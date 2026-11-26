################################################################################
#
# libretro-flycast
#
################################################################################

# The flycast core (IMPLEMENTATION.md E.3), built by package/libretro/libretro.mk.
# The upstream core (not ROCKNIX's flycast2021), with GLES and no Vulkan.
# Medium profile (2 GB images).

# The commit of mods/retroarch-cores.src.lock (decision I10): pending until
# scripts/core_sources.py has read the Android build's core from the build
# cache. tools/lint_packages.py reports it as pending until then, and checks
# it against the lock afterwards (--sync-cores writes it).
LIBRETRO_FLYCAST_VERSION = TODO
LIBRETRO_FLYCAST_SITE = https://github.com/flyinghead/flycast.git
LIBRETRO_FLYCAST_SITE_METHOD = git
LIBRETRO_FLYCAST_GIT_SUBMODULES = YES
LIBRETRO_FLYCAST_LICENSE = GPL-2.0
LIBRETRO_FLYCAST_LICENSE_FILES = LICENSE
LIBRETRO_FLYCAST_DEPENDENCIES = host-pkgconf libegl libgles zlib
LIBRETRO_FLYCAST_SO = flycast_libretro.so
LIBRETRO_FLYCAST_CONF_OPTS = \
	-DLIBRETRO=ON \
	-DUSE_GLES=ON \
	-DUSE_VULKAN=OFF \
	-DUSE_OPENMP=OFF

$(eval $(libretro-cmake-core-package))
