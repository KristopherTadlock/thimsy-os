# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2009-2016 Stephan Raue (stephan@openelec.tv)
# Copyright (C) 2017-present Team LibreELEC (https://libreelec.tv)
# Thimsy fixture: cut down from ROCKNIX projects/ROCKNIX/packages/linux/package.mk at
# 0f4806b59bfd1d3e6b17e3a632e95ced82a28719 (next); lines kept verbatim, blocks for other
# families and the perf/headers recipes removed.

PKG_NAME="linux"
PKG_LICENSE="GPL"
PKG_SITE="http://www.kernel.org"
PKG_DEPENDS_HOST="ccache:host openssl:host"
PKG_DEPENDS_TARGET="linux:host kmod:host xz:host keyutils ncurses openssl:host ${KERNEL_EXTRA_DEPENDS_TARGET}"
PKG_LONGDESC="This package contains a precompiled kernel image and the modules."
PKG_IS_KERNEL_PKG="yes"
PKG_STAMP="${KERNEL_TARGET} ${KERNEL_MAKE_EXTRACMD}"

PKG_PATCH_DIRS="${LINUX} mainline ${DEVICE} default"

[[ "${DEVICE}" =~ "RK3326|RK3399|S922X|H700" ]] && PKG_PATCH_DIRS+=" rocknix-joypad"
[[ "${DEVICE}" == RK* ]] && PKG_PATCH_DIRS+=" mainline-rockchip"

case ${DEVICE} in
  RK3588)
    PKG_VERSION="b8e62bed74766b6c8c423a767b35495e78b64caf"
    PKG_SHA256="0e0fe5a2f108d525a044e190c3f23a4603cdbf8d54bc1cc488d6ee04b9fc5b3a"
    PKG_URL="https://github.com/armbian/linux-rockchip/archive/${PKG_VERSION}.tar.gz"
    PKG_GIT_CLONE_BRANCH="rk-6.1-rkr3"
    PKG_PATCH_DIRS="${LINUX} ${DEVICE} default"
    ;;
  SM6115|SM8550|RK3566|RK3576)
    PKG_VERSION="7.3.5"
    PKG_SHA256="8f293f83d54f0b7b9be56863ebc5e4ea79d73e530b4ae55c572be97ef39f0990"
    PKG_URL="https://www.kernel.org/pub/linux/kernel/v${PKG_VERSION/.*/}.x/${PKG_NAME}-${PKG_VERSION}.tar.xz"
    PKG_PATCH_DIRS+=" 7.3"
    ;;
  AMD64|H700|SM4450|SM6450|SM8250|SM8650|SM8750)
    PKG_VERSION="7.2"
    PKG_SHA256="f9fef3d14c0df53819026f4be74459835c2a0b0dcbf5b5bbd9ea19f0829402b3"
    PKG_URL="https://www.kernel.org/pub/linux/kernel/v${PKG_VERSION/.*/}.x/${PKG_NAME}-${PKG_VERSION}.tar.xz"
    # Note that the patch dir 7.2 is automatically added as it matches the
    # version. When we move past 7.2, remember to add 7.2 to patch dirs.
    ;;
esac

PKG_KERNEL_CFG_FILE=$(kernel_config_path) || die

if [ "${DEVICE}" = "RK3326" -o "${DEVICE}" = "RK3566" ]; then
  PKG_DEPENDS_UNPACK+=" generic-dsi"
elif [ "${DEVICE}" = "SM8250" -o "${DEVICE}" = "H700" -o "${DEVICE}" = "SM8650" -o "${DEVICE}" = "SM8750" -o "${DEVICE}" = "SDM845" ]; then
  PKG_DEPENDS_UNPACK+=" kernel-firmware"
fi

# A built-in cfg80211 needs regulatory.db inside the kernel image (see
# pre_make_target), so unpack wireless-regdb before the kernel is built.
if grep -q '^CONFIG_CFG80211=y' ${PKG_KERNEL_CFG_FILE}; then
  PKG_DEPENDS_UNPACK+=" wireless-regdb"
fi

post_patch() {
  # linux was already built and its build dir autoremoved - prepare it again for kernel packages
  if [ -d ${PKG_INSTALL}/.image ]; then
    cp -p ${PKG_INSTALL}/.image/.config ${PKG_BUILD}
    kernel_make -C ${PKG_BUILD} prepare

    # restore the required Module.symvers from an earlier build
    cp -p ${PKG_INSTALL}/.image/Module.symvers ${PKG_BUILD}
  fi

  if [ "${DEVICE}" = "RK3326" -o "${DEVICE}" = "RK3566" -o "${DEVICE}" = "RK3576" ]; then
    cp -v $(get_pkg_directory generic-dsi)/sources/panel-generic-dsi.c ${PKG_BUILD}/drivers/gpu/drm/panel/
    echo "obj-y" += panel-generic-dsi.o >> ${PKG_BUILD}/drivers/gpu/drm/panel/Makefile
  fi

  DTS_SOURCE_DIR="${PROJECT_DIR}/${PROJECT}/devices/${DEVICE}/linux/dts"
  if [ -d "${DTS_SOURCE_DIR}" ]; then
    rsync -av "${DTS_SOURCE_DIR}/" ${PKG_BUILD}/arch/arm64/boot/dts/
  fi
}

pre_make_target() {
  cp ${PKG_KERNEL_CFG_FILE} ${PKG_BUILD}/.config

  # set initramfs source
  ${PKG_BUILD}/scripts/config --set-str CONFIG_INITRAMFS_SOURCE "$(kernel_initramfs_confs) ${BUILD}/initramfs"

  if [[ "${TARGET_ARCH}" =~ i*86|x86_64 ]]; then
    # copy some extra firmware to linux tree
    mkdir -p ${PKG_BUILD}/external-firmware
      cp -a $(get_build_dir kernel-firmware)/.copied-firmware/{amdgpu,amd-ucode,i915,radeon,e100,rtl_nic} ${PKG_BUILD}/external-firmware

    ${PKG_BUILD}/scripts/config --set-str CONFIG_EXTRA_FIRMWARE "${FW_LIST}"
    ${PKG_BUILD}/scripts/config --set-str CONFIG_EXTRA_FIRMWARE_DIR "external-firmware"
  elif [ "${TARGET_ARCH}" = "aarch64" -a "${DEVICE}" = "H700" ]; then
    mkdir -p ${PKG_BUILD}/external-firmware/rtl_bt
    mkdir -p ${PKG_BUILD}/external-firmware/rtw88
    mkdir -p ${PKG_BUILD}/external-firmware/panels
      cp -Lv $(get_build_dir kernel-firmware)/.copied-firmware/rtl_bt/rtl8821cs_config.bin ${PKG_BUILD}/external-firmware/rtl_bt
      cp -Lv $(get_build_dir kernel-firmware)/.copied-firmware/rtl_bt/rtl8821cs_fw.bin ${PKG_BUILD}/external-firmware/rtl_bt
      cp -Lv $(get_build_dir kernel-firmware)/.copied-firmware/rtw88/rtw8821c_fw.bin ${PKG_BUILD}/external-firmware/rtw88
      cp -Lv $(get_build_dir kernel-firmware)/.copied-firmware/panels/anbernic,rg40xx-panel.panel ${PKG_BUILD}/external-firmware/panels
      cp -Lv $(get_build_dir kernel-firmware)/.copied-firmware/panels/anbernic,rg40xx-v2-panel.panel ${PKG_BUILD}/external-firmware/panels

    FW_LIST="$(find ${PKG_BUILD}/external-firmware -type f | sed 's|.*external-firmware/||' | sort | xargs)"

    ${PKG_BUILD}/scripts/config --set-str CONFIG_EXTRA_FIRMWARE "${FW_LIST}"
    ${PKG_BUILD}/scripts/config --set-str CONFIG_EXTRA_FIRMWARE_DIR "external-firmware"
  fi

  # cfg80211 requests regulatory.db as soon as it initialises.
  if grep -q '^CONFIG_CFG80211=y' ${PKG_BUILD}/.config; then
    mkdir -p ${PKG_BUILD}/external-firmware
      cp -Lv $(get_build_dir wireless-regdb)/regulatory.db ${PKG_BUILD}/external-firmware
      cp -Lv $(get_build_dir wireless-regdb)/regulatory.db.p7s ${PKG_BUILD}/external-firmware

    FW_LIST="$(${PKG_BUILD}/scripts/config --state CONFIG_EXTRA_FIRMWARE)"
    [ "${FW_LIST}" = "undef" ] && FW_LIST=""

    ${PKG_BUILD}/scripts/config --set-str CONFIG_EXTRA_FIRMWARE \
      "$(echo ${FW_LIST} regulatory.db regulatory.db.p7s | xargs)"
    ${PKG_BUILD}/scripts/config --set-str CONFIG_EXTRA_FIRMWARE_DIR "external-firmware"
  fi
}

makeinstall_target() {
  mkdir -p ${INSTALL}/.image
  cp -p arch/${TARGET_KERNEL_ARCH}/boot/${KERNEL_TARGET} System.map .config Module.symvers ${INSTALL}/.image/

  if [ "${BOOTLOADER}" = "u-boot" ]; then
    mkdir -p ${INSTALL}/usr/share/bootloader
    for dtb in arch/${TARGET_KERNEL_ARCH}/boot/dts/**/*.dtb; do
      if [ -f ${dtb} ]; then
        if [ "${DEVICE}" = "H700" -o "${DEVICE}" = "RK3326" -o "${DEVICE}" = "RK3399" -o "${DEVICE}" = "RK3566" -o "${DEVICE}" = "RK3576" -o "${DEVICE}" = "RK3588" ]; then
          mkdir -p ${INSTALL}/usr/share/bootloader/device_trees
          cp -v ${dtb} ${INSTALL}/usr/share/bootloader/device_trees
        else
          cp -v ${dtb} ${INSTALL}/usr/share/bootloader
        fi
      fi
    done
  fi
}
