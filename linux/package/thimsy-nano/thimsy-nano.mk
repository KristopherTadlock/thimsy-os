################################################################################
#
# thimsy-nano
#
################################################################################

# Nano, built with CMake (nano/CMakeLists.txt, step G6) from the git archive of
# the repository's nano/ at HEAD that linux/build.sh writes to $(THIMSY_SRC_DIR)
# as nano-<sha>.tar.gz, with its hash file from build.sh (IMPLEMENTATION.md
# E.1 and E.3, decisions I41 and G2a-14).
THIMSY_NANO_VERSION = $(THIMSY_GIT_SHA)
THIMSY_NANO_SITE = $(or $(THIMSY_SRC_DIR),$(DL_DIR)/thimsy-src)
THIMSY_NANO_SITE_METHOD = file
THIMSY_NANO_SOURCE = nano-$(THIMSY_NANO_VERSION).tar.gz
# The Kanji table (NanoOskKanji.h, from SKK-JISYO) makes the binary a
# GPL-compatible combination, published with its full source (decision I41;
# plan 3.3's question stays open for the owner). liba52 (GPL-2.0) is built only
# with NANO_WITH_AC3=ON, which Linux leaves off.
THIMSY_NANO_LICENSE = Apache-2.0, GPL-2.0+ (NanoOskKanji.h, from SKK-JISYO), Zlib (nanosvg), MIT (rcheevos)
# nano/LICENSE is the Apache-2.0 text, and nano/NOTICE lists the parts under
# other licences (NanoOskKanji.h, liba52/ with NanoAc3.*, nanosvg/, rcheevos/);
# both are fork-only (tools/nano-tree/upstream.json), so the Android patch does
# not carry them.
THIMSY_NANO_LICENSE_FILES = LICENSE NOTICE NanoOskKanji.h nanosvg/nanosvg.h rcheevos/LICENSE
THIMSY_NANO_DEPENDENCIES = \
	host-pkgconf \
	libthimsy \
	libegl \
	libgles \
	libgbm \
	libdrm \
	freetype \
	libpng \
	jpeg \
	libwebp \
	giflib \
	zlib \
	tinyxml2 \
	libcurl \
	openssl \
	alsa-lib
# nano/CMakeLists.txt's options (G6; tools/nano-tree/tests/test_nano_cmake.py
# checks that each one passed here exists). The feature switches stay OFF:
# AC-3 (liba52, GPL-2.0), PDF (no pdfium package), zip/7z (Android's
# libziparchive) and a media back end (none yet); what they leave out links as
# generated inert stubs. NANO_REQUIRE_IMAGE_LIBS makes a missing image library
# an error, not a format the photo viewer quietly lacks. The build runs two
# Python 3.8+ generators (standard library only) with the build host's python3,
# which Buildroot requires anyway, and the toolchain's nm.
THIMSY_NANO_CONF_OPTS = \
	-DNANO_PLATFORM=linux \
	-DNANO_WITH_AC3=OFF \
	-DNANO_WITH_PDF=OFF \
	-DNANO_WITH_ARCHIVE=OFF \
	-DNANO_WITH_MEDIA=OFF \
	-DNANO_REQUIRE_IMAGE_LIBS=ON \
	-DNANO_BUILD_TESTS=OFF

ifeq ($(BR2_PACKAGE_THIMSY_NANO_SCRAPE),y)
THIMSY_NANO_CONF_OPTS += -DNANO_BUILD_SCRAPE=ON
else
THIMSY_NANO_CONF_OPTS += -DNANO_BUILD_SCRAPE=OFF
endif

define THIMSY_NANO_CHECK_SOURCE
	@test -n "$(THIMSY_GIT_SHA)" || { \
		echo "thimsy-nano: THIMSY_GIT_SHA is not set; build through linux/build.sh," \
			"which writes the source tarball"; exit 1; }
endef
THIMSY_NANO_PRE_DOWNLOAD_HOOKS += THIMSY_NANO_CHECK_SOURCE

$(eval $(cmake-package))
