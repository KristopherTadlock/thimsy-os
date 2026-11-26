################################################################################
#
# libretro-mame2003-plus
#
################################################################################

# The mame2003_plus core (IMPLEMENTATION.md E.3), built by package/libretro/libretro.mk.

# The commit of mods/retroarch-cores.src.lock (decision I10): pending until
# scripts/core_sources.py has read the Android build's core from the build
# cache. tools/lint_packages.py reports it as pending until then, and checks
# it against the lock afterwards (--sync-cores writes it).
LIBRETRO_MAME2003_PLUS_VERSION = TODO
LIBRETRO_MAME2003_PLUS_SITE = $(call github,libretro,mame2003-plus-libretro,$(LIBRETRO_MAME2003_PLUS_VERSION))
LIBRETRO_MAME2003_PLUS_LICENSE = MAME licence (non-commercial)
LIBRETRO_MAME2003_PLUS_LICENSE_FILES = LICENSE.md
LIBRETRO_MAME2003_PLUS_DEPENDENCIES = zlib

$(eval $(libretro-core-package))
