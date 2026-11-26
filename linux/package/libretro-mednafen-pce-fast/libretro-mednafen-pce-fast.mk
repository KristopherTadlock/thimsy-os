################################################################################
#
# libretro-mednafen-pce-fast
#
################################################################################

# The mednafen_pce_fast core (IMPLEMENTATION.md E.3), built by package/libretro/libretro.mk.

# The commit of mods/retroarch-cores.src.lock (decision I10): pending until
# scripts/core_sources.py has read the Android build's core from the build
# cache. tools/lint_packages.py reports it as pending until then, and checks
# it against the lock afterwards (--sync-cores writes it).
LIBRETRO_MEDNAFEN_PCE_FAST_VERSION = TODO
LIBRETRO_MEDNAFEN_PCE_FAST_SITE = $(call github,libretro,beetle-pce-fast-libretro,$(LIBRETRO_MEDNAFEN_PCE_FAST_VERSION))
LIBRETRO_MEDNAFEN_PCE_FAST_LICENSE = GPL-2.0
LIBRETRO_MEDNAFEN_PCE_FAST_LICENSE_FILES = COPYING

$(eval $(libretro-core-package))
