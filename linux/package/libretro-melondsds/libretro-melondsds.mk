################################################################################
#
# libretro-melondsds
#
################################################################################

# The melondsds core (IMPLEMENTATION.md E.3), built by package/libretro/libretro.mk.
# TODO(G10): melonDS DS fetches melonDS and its other dependencies with
# CMake's FetchContent at configure time, which a Buildroot build cannot do
# (no network there). They must become _EXTRA_DOWNLOADS, pinned, with
# FETCHCONTENT_SOURCE_DIR_<NAME> pointing at them; the names come from its
# cmake/ directory at the pinned commit. On Panfrost it renders in software
# (its GL renderer asks for desktop GL; plan 6.2). Not in the Android build
# (DS is a standalone app there), so its commit is chosen for Linux alone.

# The commit of mods/retroarch-cores.src.lock (decision I10): pending until
# scripts/core_sources.py has read the Android build's core from the build
# cache. tools/lint_packages.py reports it as pending until then, and checks
# it against the lock afterwards (--sync-cores writes it).
LIBRETRO_MELONDSDS_VERSION = TODO
LIBRETRO_MELONDSDS_SITE = https://github.com/JesseTG/melonds-ds.git
LIBRETRO_MELONDSDS_SITE_METHOD = git
LIBRETRO_MELONDSDS_GIT_SUBMODULES = YES
LIBRETRO_MELONDSDS_LICENSE = GPL-3.0
LIBRETRO_MELONDSDS_LICENSE_FILES = LICENSE
LIBRETRO_MELONDSDS_DEPENDENCIES = host-pkgconf libegl libgles zlib
LIBRETRO_MELONDSDS_SO = src/libretro/melondsds_libretro.so
LIBRETRO_MELONDSDS_CONF_OPTS = \
	-DENABLE_OPENGL=OFF \
	-DENABLE_JIT=ON \
	-DBUILD_TESTING=OFF

$(eval $(libretro-cmake-core-package))
