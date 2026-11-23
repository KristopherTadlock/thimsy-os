# Build and development environment for Thimsy OS: the firmware build
# (README "Building", scripts/build.sh, tests/run.sh). Used by VS Code's Dev
# Containers (devcontainer.json) and by docker compose (compose.yaml).
# Google's Android build tools are x86-64 only, so the image is too; Docker
# runs it under emulation on Arm hosts (Apple silicon).
FROM --platform=linux/amd64 ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive

# Firmware build tools. mkbootimg also provides unpack_bootimg; e2fsprogs
# provides e2fsck, resize2fs and debugfs; attr provides setfattr; util-linux
# (in the base image) provides flock, losetup and unshare.
RUN apt-get update && apt-get install -y --no-install-recommends \
      7zip \
      attr \
      bash \
      build-essential \
      ca-certificates \
      cpio \
      curl \
      e2fsprogs \
      erofs-utils \
      file \
      git \
      lz4 \
      mkbootimg \
      openssl \
      patch \
      python3 \
      python3-pil \
      secilc \
      sudo \
      unzip \
      xz-utils \
      zip \
      zstd \
    && rm -rf /var/lib/apt/lists/*

# Android Games helper and PanVK driver package (scripts/build_android_games.sh,
# scripts/build_panvk_driver.sh): JDK 17, and Google's build tools 34.0.0
# (aapt, aapt2, d8, zipalign, apksigner) and API 34 android.jar, which
# Ubuntu's installer packages download from Google.
RUN apt-get update && apt-get install -y --no-install-recommends \
      openjdk-17-jdk-headless \
      google-android-build-tools-34.0.0-installer \
      google-android-platform-34-installer \
    && rm -rf /var/lib/apt/lists/*
ENV ANDROID_HOME=/usr/lib/android-sdk
ENV ANDROID_JAR=$ANDROID_HOME/platforms/android-34/android.jar
ENV PATH=$ANDROID_HOME/build-tools/34.0.0:$PATH

# AOSP's avbtool and AVB test key (Ubuntu has no package for them), from a
# release tag of platform/external/avb.
ARG AVB_TAG=android-14.0.0_r1
RUN mkdir -p /opt/aosp-avb \
    && base=https://android.googlesource.com/platform/external/avb/+/refs/tags/$AVB_TAG \
    && curl -fsSL "$base/avbtool.py?format=TEXT" | base64 -d > /opt/aosp-avb/avbtool.py \
    && curl -fsSL "$base/test/data/testkey_rsa4096.pem?format=TEXT" | base64 -d > /opt/aosp-avb/testkey_rsa4096.pem \
    && chmod +x /opt/aosp-avb/avbtool.py \
    && ln -s /opt/aosp-avb/avbtool.py /usr/local/bin/avbtool \
    && avbtool version \
    && grep -q 'BEGIN.*PRIVATE KEY' /opt/aosp-avb/testkey_rsa4096.pem
ENV MKBOOTIMG=mkbootimg
ENV AVB_KEY=/opt/aosp-avb/testkey_rsa4096.pem

# The checkout is bind-mounted from the host, where its files can have another
# owner; without this git refuses to work in it.
RUN git config --system --add safe.directory '*'

# The build loop-mounts the system image, so the container runs privileged
# and as root; build.sh's sudo calls are then no-ops.
WORKDIR /workspaces/thimsy-os
