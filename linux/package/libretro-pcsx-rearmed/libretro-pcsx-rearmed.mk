################################################################################
#
# libretro-pcsx-rearmed
#
################################################################################

# The pcsx_rearmed core (IMPLEMENTATION.md E.3), built by package/libretro/libretro.mk.
# The arm64 dynarec is picked from the cross compiler's target (plan 6.2);
# lightrec, libchdr and others are git submodules.

# The commit of mods/retroarch-cores.src.lock (decision I10): pending until
# scripts/core_sources.py has read the Android build's core from the build
# cache. tools/lint_packages.py reports it as pending until then, and checks
# it against the lock afterwards (--sync-cores writes it).
LIBRETRO_PCSX_REARMED_VERSION = TODO
LIBRETRO_PCSX_REARMED_SITE = https://github.com/libretro/pcsx_rearmed.git
LIBRETRO_PCSX_REARMED_SITE_METHOD = git
LIBRETRO_PCSX_REARMED_GIT_SUBMODULES = YES
LIBRETRO_PCSX_REARMED_LICENSE = GPL-2.0
LIBRETRO_PCSX_REARMED_LICENSE_FILES = COPYING
LIBRETRO_PCSX_REARMED_MAKEFILE = Makefile.libretro

$(eval $(libretro-core-package))
