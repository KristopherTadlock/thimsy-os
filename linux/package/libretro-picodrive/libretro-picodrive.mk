################################################################################
#
# libretro-picodrive
#
################################################################################

# The picodrive core (IMPLEMENTATION.md E.3), built by package/libretro/libretro.mk.
# Its CPU cores and sound chips are git submodules.

# The commit of mods/retroarch-cores.src.lock (decision I10): pending until
# scripts/core_sources.py has read the Android build's core from the build
# cache. tools/lint_packages.py reports it as pending until then, and checks
# it against the lock afterwards (--sync-cores writes it).
LIBRETRO_PICODRIVE_VERSION = TODO
LIBRETRO_PICODRIVE_SITE = https://github.com/libretro/picodrive.git
LIBRETRO_PICODRIVE_SITE_METHOD = git
LIBRETRO_PICODRIVE_GIT_SUBMODULES = YES
LIBRETRO_PICODRIVE_LICENSE = MAME-style licence (non-commercial)
LIBRETRO_PICODRIVE_LICENSE_FILES = COPYING
LIBRETRO_PICODRIVE_MAKEFILE = Makefile.libretro

$(eval $(libretro-core-package))
