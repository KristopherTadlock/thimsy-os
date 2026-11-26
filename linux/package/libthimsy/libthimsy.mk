################################################################################
#
# libthimsy
#
################################################################################

# Built from the git archive of the repository's libthimsy/ at HEAD that
# linux/build.sh writes to $(THIMSY_SRC_DIR) as libthimsy-<sha>.tar.gz, with
# its hash file in the hash directory build.sh adds to BR2_GLOBAL_PATCH_DIR
# (IMPLEMENTATION.md E.1, decisions I41 and G2a-14). A file site, not a local
# one, so legal-info saves the source and nothing untracked reaches an image.
LIBTHIMSY_VERSION = $(THIMSY_GIT_SHA)
LIBTHIMSY_SITE = $(or $(THIMSY_SRC_DIR),$(DL_DIR)/thimsy-src)
LIBTHIMSY_SITE_METHOD = file
LIBTHIMSY_SOURCE = libthimsy-$(LIBTHIMSY_VERSION).tar.gz
LIBTHIMSY_LICENSE = MIT
LIBTHIMSY_LICENSE_FILES = LICENSE
# The static library and headers for thimsy-nano, thimsy-session and
# thimsy-padd; thimsy-prop in the target (staging keeps a copy, unused).
LIBTHIMSY_INSTALL_STAGING = YES
LIBTHIMSY_CONF_OPTS = -DTHIMSY_BUILD_TESTS=OFF

define LIBTHIMSY_CHECK_SOURCE
	@test -n "$(THIMSY_GIT_SHA)" || { \
		echo "libthimsy: THIMSY_GIT_SHA is not set; build through linux/build.sh," \
			"which writes the source tarball"; exit 1; }
endef
LIBTHIMSY_PRE_DOWNLOAD_HOOKS += LIBTHIMSY_CHECK_SOURCE

$(eval $(cmake-package))
