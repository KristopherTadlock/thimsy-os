################################################################################
#
# libretro-a5200
#
################################################################################

# The a5200 core (IMPLEMENTATION.md E.3), built by package/libretro/libretro.mk.

# The commit of mods/retroarch-cores.src.lock (decision I10): pending until
# scripts/core_sources.py has read the Android build's core from the build
# cache. tools/lint_packages.py reports it as pending until then, and checks
# it against the lock afterwards (--sync-cores writes it).
LIBRETRO_A5200_VERSION = TODO
LIBRETRO_A5200_SITE = $(call github,libretro,a5200,$(LIBRETRO_A5200_VERSION))
LIBRETRO_A5200_LICENSE = GPL-2.0
LIBRETRO_A5200_LICENSE_FILES = License.txt

$(eval $(libretro-core-package))
