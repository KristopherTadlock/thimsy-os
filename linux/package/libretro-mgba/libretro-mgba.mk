################################################################################
#
# libretro-mgba
#
################################################################################

# The mgba core (IMPLEMENTATION.md E.3), built by package/libretro/libretro.mk.

# The commit of mods/retroarch-cores.src.lock (decision I10): pending until
# scripts/core_sources.py has read the Android build's core from the build
# cache. tools/lint_packages.py reports it as pending until then, and checks
# it against the lock afterwards (--sync-cores writes it).
LIBRETRO_MGBA_VERSION = TODO
LIBRETRO_MGBA_SITE = $(call github,libretro,mgba,$(LIBRETRO_MGBA_VERSION))
LIBRETRO_MGBA_LICENSE = MPL-2.0
LIBRETRO_MGBA_LICENSE_FILES = LICENSE
LIBRETRO_MGBA_MAKEFILE = Makefile.libretro

$(eval $(libretro-core-package))
