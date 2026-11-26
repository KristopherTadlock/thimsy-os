################################################################################
#
# libretro-stella
#
################################################################################

# The stella core (IMPLEMENTATION.md E.3), built by package/libretro/libretro.mk.

# The commit of mods/retroarch-cores.src.lock (decision I10): pending until
# scripts/core_sources.py has read the Android build's core from the build
# cache. tools/lint_packages.py reports it as pending until then, and checks
# it against the lock afterwards (--sync-cores writes it).
LIBRETRO_STELLA_VERSION = TODO
LIBRETRO_STELLA_SITE = $(call github,stella-emu,stella,$(LIBRETRO_STELLA_VERSION))
LIBRETRO_STELLA_LICENSE = GPL-2.0
LIBRETRO_STELLA_LICENSE_FILES = License.txt
LIBRETRO_STELLA_SUBDIR = src/os/libretro

$(eval $(libretro-core-package))
