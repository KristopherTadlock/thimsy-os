################################################################################
#
# libretro-genesis-plus-gx
#
################################################################################

# The genesis_plus_gx core (IMPLEMENTATION.md E.3), built by package/libretro/libretro.mk.

# The commit of mods/retroarch-cores.src.lock (decision I10): pending until
# scripts/core_sources.py has read the Android build's core from the build
# cache. tools/lint_packages.py reports it as pending until then, and checks
# it against the lock afterwards (--sync-cores writes it).
LIBRETRO_GENESIS_PLUS_GX_VERSION = TODO
LIBRETRO_GENESIS_PLUS_GX_SITE = $(call github,libretro,Genesis-Plus-GX,$(LIBRETRO_GENESIS_PLUS_GX_VERSION))
LIBRETRO_GENESIS_PLUS_GX_LICENSE = Genesis Plus GX licence (non-commercial)
LIBRETRO_GENESIS_PLUS_GX_LICENSE_FILES = LICENSE.txt
LIBRETRO_GENESIS_PLUS_GX_MAKEFILE = Makefile.libretro

$(eval $(libretro-core-package))
