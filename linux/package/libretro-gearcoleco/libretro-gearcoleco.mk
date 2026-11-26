################################################################################
#
# libretro-gearcoleco
#
################################################################################

# The gearcoleco core (IMPLEMENTATION.md E.3), built by package/libretro/libretro.mk.

# The commit of mods/retroarch-cores.src.lock (decision I10): pending until
# scripts/core_sources.py has read the Android build's core from the build
# cache. tools/lint_packages.py reports it as pending until then, and checks
# it against the lock afterwards (--sync-cores writes it).
LIBRETRO_GEARCOLECO_VERSION = TODO
LIBRETRO_GEARCOLECO_SITE = $(call github,drhelius,Gearcoleco,$(LIBRETRO_GEARCOLECO_VERSION))
LIBRETRO_GEARCOLECO_LICENSE = GPL-3.0
LIBRETRO_GEARCOLECO_LICENSE_FILES = LICENSE
LIBRETRO_GEARCOLECO_SUBDIR = platforms/libretro

$(eval $(libretro-core-package))
