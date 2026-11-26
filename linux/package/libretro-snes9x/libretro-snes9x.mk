################################################################################
#
# libretro-snes9x
#
################################################################################

# The snes9x core (IMPLEMENTATION.md E.3), built by package/libretro/libretro.mk.

# The commit of mods/retroarch-cores.src.lock (decision I10): pending until
# scripts/core_sources.py has read the Android build's core from the build
# cache. tools/lint_packages.py reports it as pending until then, and checks
# it against the lock afterwards (--sync-cores writes it).
LIBRETRO_SNES9X_VERSION = TODO
LIBRETRO_SNES9X_SITE = $(call github,libretro,snes9x,$(LIBRETRO_SNES9X_VERSION))
LIBRETRO_SNES9X_LICENSE = Snes9x licence (non-commercial)
LIBRETRO_SNES9X_LICENSE_FILES = LICENSE
LIBRETRO_SNES9X_SUBDIR = libretro

$(eval $(libretro-core-package))
