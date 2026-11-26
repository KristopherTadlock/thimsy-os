################################################################################
#
# thimsy-fonts
#
################################################################################

# The fonts Nano opens under /system/fonts (NanoMenuRender.cpp's font list,
# NanoBootScreen.cpp, NanoMenuButtonMap.cpp), from upstream releases pinned by
# hash, installed to /usr/share/fonts/thimsy under Android's file names
# (plan 5.2; I14's /system/fonts link points there):
#   Roboto-Regular.ttf          Roboto 2.138 (Apache-2.0), as Android ships it
#   DroidSans.ttf               a link to Roboto-Regular.ttf, as on Android
#                               since 5.0 (DroidSans is not shipped)
#   NotoSansCJK-Regular.ttc     Noto Sans CJK 2.004 (OFL-1.1)
#   NotoNaskhArabic-Regular.ttf, NotoSansThai-Regular.ttf,
#   NotoSansHebrew-Regular.ttf  the notofonts releases below (OFL-1.1)
#   NotoColorEmoji.ttf          Noto Emoji 2.047 (OFL-1.1)
#
# Versions, URLs and archive layouts are from memory (guess): they and the
# .hash file are filled in and checked at the first download, which is not
# allowed now. Until then the .hash file holds no hash and lint_packages.py
# reports the package as pending.
THIMSY_FONTS_VERSION = 2.138
THIMSY_FONTS_SITE = https://github.com/googlefonts/roboto/releases/download/v$(THIMSY_FONTS_VERSION)
THIMSY_FONTS_SOURCE = roboto-unhinted.zip
THIMSY_FONTS_LICENSE = Apache-2.0 (Roboto), OFL-1.1 (Noto)
# Licence files inside the archives (guess at their paths, checked with the
# hashes).
THIMSY_FONTS_LICENSE_FILES = roboto/LICENSE noto-emoji/fonts/LICENSE

THIMSY_FONTS_NOTO_CJK_VERSION = 2.004
THIMSY_FONTS_NOTO_CJK = 03_NotoSansCJK-OTC.zip
THIMSY_FONTS_NOTO_EMOJI_VERSION = 2.047
THIMSY_FONTS_NOTO_NASKH_ARABIC_VERSION = 2.018
THIMSY_FONTS_NOTO_SANS_THAI_VERSION = 2.002
THIMSY_FONTS_NOTO_SANS_HEBREW_VERSION = 3.001

THIMSY_FONTS_EXTRA_DOWNLOADS = \
	https://github.com/notofonts/noto-cjk/releases/download/Sans$(THIMSY_FONTS_NOTO_CJK_VERSION)/$(THIMSY_FONTS_NOTO_CJK) \
	https://github.com/googlefonts/noto-emoji/archive/v$(THIMSY_FONTS_NOTO_EMOJI_VERSION)/noto-emoji-$(THIMSY_FONTS_NOTO_EMOJI_VERSION).tar.gz \
	https://github.com/notofonts/arabic/releases/download/NotoNaskhArabic-v$(THIMSY_FONTS_NOTO_NASKH_ARABIC_VERSION)/NotoNaskhArabic-v$(THIMSY_FONTS_NOTO_NASKH_ARABIC_VERSION).zip \
	https://github.com/notofonts/thai/releases/download/NotoSansThai-v$(THIMSY_FONTS_NOTO_SANS_THAI_VERSION)/NotoSansThai-v$(THIMSY_FONTS_NOTO_SANS_THAI_VERSION).zip \
	https://github.com/notofonts/hebrew/releases/download/NotoSansHebrew-v$(THIMSY_FONTS_NOTO_SANS_HEBREW_VERSION)/NotoSansHebrew-v$(THIMSY_FONTS_NOTO_SANS_HEBREW_VERSION).zip

THIMSY_FONTS_DIR = /usr/share/fonts/thimsy

# Every archive is a zip but the emoji one: unpack each into its own directory.
define THIMSY_FONTS_EXTRACT_CMDS
	$(UNZIP) -o $(THIMSY_FONTS_DL_DIR)/$(THIMSY_FONTS_SOURCE) -d $(@D)/roboto
	$(UNZIP) -o $(THIMSY_FONTS_DL_DIR)/$(THIMSY_FONTS_NOTO_CJK) -d $(@D)/noto-cjk
	mkdir -p $(@D)/noto-emoji
	$(call suitable-extractor,noto-emoji.tar.gz) \
		$(THIMSY_FONTS_DL_DIR)/noto-emoji-$(THIMSY_FONTS_NOTO_EMOJI_VERSION).tar.gz | \
		$(TAR) --strip-components=1 -C $(@D)/noto-emoji $(TAR_OPTIONS) -
	$(UNZIP) -o $(THIMSY_FONTS_DL_DIR)/NotoNaskhArabic-v$(THIMSY_FONTS_NOTO_NASKH_ARABIC_VERSION).zip -d $(@D)/arabic
	$(UNZIP) -o $(THIMSY_FONTS_DL_DIR)/NotoSansThai-v$(THIMSY_FONTS_NOTO_SANS_THAI_VERSION).zip -d $(@D)/thai
	$(UNZIP) -o $(THIMSY_FONTS_DL_DIR)/NotoSansHebrew-v$(THIMSY_FONTS_NOTO_SANS_HEBREW_VERSION).zip -d $(@D)/hebrew
endef

# THIMSY_FONTS_INSTALL_ONE DIR NAME: installs the one file called NAME found under
# DIR (archive layouts differ between releases), or fails.
define THIMSY_FONTS_INSTALL_ONE
	f=$$(find $(1) -name '$(2)' -type f | sort | head -n 1); \
	test -n "$$f" || { echo "thimsy-fonts: no $(2) under $(1)"; exit 1; }; \
	$(INSTALL) -D -m 0644 "$$f" $(TARGET_DIR)$(THIMSY_FONTS_DIR)/$(2)
endef

define THIMSY_FONTS_INSTALL_TARGET_CMDS
	$(call THIMSY_FONTS_INSTALL_ONE,$(@D)/roboto,Roboto-Regular.ttf)
	ln -sf Roboto-Regular.ttf $(TARGET_DIR)$(THIMSY_FONTS_DIR)/DroidSans.ttf
	$(call THIMSY_FONTS_INSTALL_ONE,$(@D)/noto-cjk,NotoSansCJK-Regular.ttc)
	$(call THIMSY_FONTS_INSTALL_ONE,$(@D)/noto-emoji/fonts,NotoColorEmoji.ttf)
	$(call THIMSY_FONTS_INSTALL_ONE,$(@D)/arabic,NotoNaskhArabic-Regular.ttf)
	$(call THIMSY_FONTS_INSTALL_ONE,$(@D)/thai,NotoSansThai-Regular.ttf)
	$(call THIMSY_FONTS_INSTALL_ONE,$(@D)/hebrew,NotoSansHebrew-Regular.ttf)
endef

$(eval $(generic-package))
