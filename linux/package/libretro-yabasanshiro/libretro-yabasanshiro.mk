################################################################################
#
# libretro-yabasanshiro
#
################################################################################

# The yabasanshiro core (IMPLEMENTATION.md E.3), built by package/libretro/libretro.mk.
# The yabasanshiro branch of libretro/yabause. platform=unix would read the
# build host's uname -m, so a cross build from x86 would get neither the
# dynarec nor GLES; arm64_cortex_a53_gles3 selects both (plan 6.2). Medium
# profile (2 GB images).

# The commit of mods/retroarch-cores.src.lock (decision I10): pending until
# scripts/core_sources.py has read the Android build's core from the build
# cache. tools/lint_packages.py reports it as pending until then, and checks
# it against the lock afterwards (--sync-cores writes it).
LIBRETRO_YABASANSHIRO_VERSION = TODO
LIBRETRO_YABASANSHIRO_SITE = $(call github,libretro,yabause,$(LIBRETRO_YABASANSHIRO_VERSION))
LIBRETRO_YABASANSHIRO_LICENSE = GPL-2.0
LIBRETRO_YABASANSHIRO_LICENSE_FILES = yabause/COPYING
LIBRETRO_YABASANSHIRO_DEPENDENCIES = libegl libgles zlib
LIBRETRO_YABASANSHIRO_SUBDIR = yabause/src/libretro
LIBRETRO_YABASANSHIRO_PLATFORM = arm64_cortex_a53_gles3

$(eval $(libretro-core-package))
