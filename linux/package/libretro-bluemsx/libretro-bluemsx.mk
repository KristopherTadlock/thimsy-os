################################################################################
#
# libretro-bluemsx
#
################################################################################

# The bluemsx core (IMPLEMENTATION.md E.3), built by package/libretro/libretro.mk.

# The commit of mods/retroarch-cores.src.lock (decision I10): pending until
# scripts/core_sources.py has read the Android build's core from the build
# cache. tools/lint_packages.py reports it as pending until then, and checks
# it against the lock afterwards (--sync-cores writes it).
LIBRETRO_BLUEMSX_VERSION = TODO
LIBRETRO_BLUEMSX_SITE = $(call github,libretro,blueMSX-libretro,$(LIBRETRO_BLUEMSX_VERSION))
LIBRETRO_BLUEMSX_LICENSE = GPL-2.0
LIBRETRO_BLUEMSX_LICENSE_FILES = license.txt
LIBRETRO_BLUEMSX_MAKEFILE = Makefile.libretro

$(eval $(libretro-core-package))
