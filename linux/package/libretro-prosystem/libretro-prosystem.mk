################################################################################
#
# libretro-prosystem
#
################################################################################

# The prosystem core (IMPLEMENTATION.md E.3), built by package/libretro/libretro.mk.

# The commit of mods/retroarch-cores.src.lock (decision I10): pending until
# scripts/core_sources.py has read the Android build's core from the build
# cache. tools/lint_packages.py reports it as pending until then, and checks
# it against the lock afterwards (--sync-cores writes it).
LIBRETRO_PROSYSTEM_VERSION = TODO
LIBRETRO_PROSYSTEM_SITE = $(call github,libretro,prosystem-libretro,$(LIBRETRO_PROSYSTEM_VERSION))
LIBRETRO_PROSYSTEM_LICENSE = GPL-2.0
LIBRETRO_PROSYSTEM_LICENSE_FILES = License.txt

$(eval $(libretro-core-package))
