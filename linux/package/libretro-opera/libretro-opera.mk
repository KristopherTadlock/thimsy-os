################################################################################
#
# libretro-opera
#
################################################################################

# The opera core (IMPLEMENTATION.md E.3), built by package/libretro/libretro.mk.
# Medium profile (2 GB images).

# The commit of mods/retroarch-cores.src.lock (decision I10): pending until
# scripts/core_sources.py has read the Android build's core from the build
# cache. tools/lint_packages.py reports it as pending until then, and checks
# it against the lock afterwards (--sync-cores writes it).
LIBRETRO_OPERA_VERSION = TODO
LIBRETRO_OPERA_SITE = $(call github,libretro,opera-libretro,$(LIBRETRO_OPERA_VERSION))
LIBRETRO_OPERA_LICENSE = LGPL-2.1, non-commercial (parts)
LIBRETRO_OPERA_LICENSE_FILES = LICENSE

$(eval $(libretro-core-package))
