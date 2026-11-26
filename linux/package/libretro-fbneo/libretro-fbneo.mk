################################################################################
#
# libretro-fbneo
#
################################################################################

# The fbneo core (IMPLEMENTATION.md E.3), built by package/libretro/libretro.mk.

# The commit of mods/retroarch-cores.src.lock (decision I10): pending until
# scripts/core_sources.py has read the Android build's core from the build
# cache. tools/lint_packages.py reports it as pending until then, and checks
# it against the lock afterwards (--sync-cores writes it).
LIBRETRO_FBNEO_VERSION = TODO
LIBRETRO_FBNEO_SITE = $(call github,libretro,FBNeo,$(LIBRETRO_FBNEO_VERSION))
LIBRETRO_FBNEO_LICENSE = FBNeo licence (non-commercial)
LIBRETRO_FBNEO_LICENSE_FILES = src/license.txt
LIBRETRO_FBNEO_DEPENDENCIES = zlib
LIBRETRO_FBNEO_SUBDIR = src/burner/libretro

$(eval $(libretro-core-package))
