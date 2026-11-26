################################################################################
#
# libretro-mupen64plus-next
#
################################################################################

# The mupen64plus_next core (IMPLEMENTATION.md E.3), built by package/libretro/libretro.mk.
# Built for OpenGL ES 3 (Panfrost), as plan 6.2 says. The Android build
# ships the same source as mupen64plus_next_gles3 (an alias, plan 6.2).

# The commit of mods/retroarch-cores.src.lock (decision I10): pending until
# scripts/core_sources.py has read the Android build's core from the build
# cache. tools/lint_packages.py reports it as pending until then, and checks
# it against the lock afterwards (--sync-cores writes it).
LIBRETRO_MUPEN64PLUS_NEXT_VERSION = TODO
LIBRETRO_MUPEN64PLUS_NEXT_SITE = $(call github,libretro,mupen64plus-libretro-nx,$(LIBRETRO_MUPEN64PLUS_NEXT_VERSION))
LIBRETRO_MUPEN64PLUS_NEXT_LICENSE = GPL-2.0
LIBRETRO_MUPEN64PLUS_NEXT_LICENSE_FILES = LICENSE
LIBRETRO_MUPEN64PLUS_NEXT_DEPENDENCIES = libegl libgles zlib
LIBRETRO_MUPEN64PLUS_NEXT_MAKE_OPTS = GLES3=1

$(eval $(libretro-core-package))
