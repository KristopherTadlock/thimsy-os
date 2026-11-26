################################################################################
#
# libretro-mamearcade
#
################################################################################

# The mamearcade core (IMPLEMENTATION.md E.3), built by package/libretro/libretro.mk.
# Current MAME's arcade drivers only; 2 GB images (decision 12). A long
# build: hours on one core.

# The commit of mods/retroarch-cores.src.lock (decision I10): pending until
# scripts/core_sources.py has read the Android build's core from the build
# cache. tools/lint_packages.py reports it as pending until then, and checks
# it against the lock afterwards (--sync-cores writes it).
LIBRETRO_MAMEARCADE_VERSION = TODO
LIBRETRO_MAMEARCADE_SITE = $(call github,libretro,mame,$(LIBRETRO_MAMEARCADE_VERSION))
LIBRETRO_MAMEARCADE_LICENSE = GPL-2.0+
LIBRETRO_MAMEARCADE_LICENSE_FILES = COPYING
LIBRETRO_MAMEARCADE_DEPENDENCIES = zlib
LIBRETRO_MAMEARCADE_MAKEFILE = Makefile.libretro
LIBRETRO_MAMEARCADE_MAKE_OPTS = SUBTARGET=arcade

$(eval $(libretro-core-package))
