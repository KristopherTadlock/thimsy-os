# Build and development environment for Thimsy OS: the firmware build
# (README "Building", scripts/build.sh) and the universal installer
# (installer/: Rust engine, spd_dump, Flutter app). Used by VS Code's Dev
# Containers (devcontainer.json) and by docker compose (compose.yaml).
# Google's Android build tools (and the Flutter Linux SDK) are x86-64 only, so
# the image is too; Docker runs it under emulation on Arm hosts (Apple silicon).
FROM --platform=linux/amd64 ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive
# A failed download in a `curl | ...` step must fail the build, not cache an
# image without the tool.
SHELL ["/bin/bash", "-o", "pipefail", "-c"]
# The checkout is bind-mounted; keep Python's caches out of it.
ENV PYTHONDONTWRITEBYTECODE=1

# Firmware build tools. e2fsprogs provides e2fsck, resize2fs and debugfs;
# attr provides setfattr; util-linux (in the base image) provides flock,
# losetup and unshare; procps provides pgrep and pkill (installer/tests/e2e.sh).
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
      openssl \
      patch \
      procps \
      python3 \
      python3-pil \
      secilc \
      sudo \
      unzip \
      xz-utils \
      zip \
      zstd \
    && rm -rf /var/lib/apt/lists/*

# Android Games helper (scripts/build_android_games.sh): JDK 17, and Google's
# build tools 34.0.0
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
# AOSP's mkbootimg and unpack_bootimg, from the same tag. Ubuntu's mkbootimg
# package leaves out the gki module its mkbootimg imports, so it can't run.
RUN mkdir -p /opt/aosp-mkbootimg/gki \
    && base=https://android.googlesource.com/platform/system/tools/mkbootimg/+/refs/tags/$AVB_TAG \
    && for f in mkbootimg.py unpack_bootimg.py gki/generate_gki_certificate.py; do \
         curl -fsSL "$base/$f?format=TEXT" | base64 -d > "/opt/aosp-mkbootimg/$f" || exit 1; \
       done \
    && chmod +x /opt/aosp-mkbootimg/mkbootimg.py /opt/aosp-mkbootimg/unpack_bootimg.py \
    && ln -s /opt/aosp-mkbootimg/mkbootimg.py /usr/local/bin/mkbootimg \
    && ln -s /opt/aosp-mkbootimg/unpack_bootimg.py /usr/local/bin/unpack_bootimg \
    && mkbootimg --help > /dev/null && unpack_bootimg --help > /dev/null
ENV MKBOOTIMG=/opt/aosp-mkbootimg/mkbootimg.py
ENV AVB_KEY=/opt/aosp-avb/testkey_rsa4096.pem

# Installer: Rust (engine), libusb (spd_dump) and the Flutter Linux desktop
# dependencies, as .github/workflows/installer.yml installs them.
RUN apt-get update && apt-get install -y --no-install-recommends \
      clang \
      cmake \
      libgtk-3-dev \
      liblzma-dev \
      libusb-1.0-0-dev \
      ninja-build \
      pkg-config \
    && rm -rf /var/lib/apt/lists/*

ENV RUSTUP_HOME=/opt/rustup
ENV CARGO_HOME=/opt/cargo
RUN curl -fsSL https://sh.rustup.rs | sh -s -- -y --no-modify-path --profile minimal \
      --default-toolchain stable --component clippy,rustfmt
ENV PATH=$CARGO_HOME/bin:$PATH
RUN cargo --version && cargo clippy --version && cargo fmt --version

# The Flutter version installer.yml pins.
ARG FLUTTER_VERSION=3.50.0
RUN curl -fsSL "https://storage.googleapis.com/flutter_infra_release/releases/stable/linux/flutter_linux_${FLUTTER_VERSION}-stable.tar.xz" \
      | tar -xJ -C /opt \
    && git config --system --add safe.directory /opt/flutter
ENV PATH=/opt/flutter/bin:$PATH
RUN flutter config --no-analytics --no-cli-animations \
    && flutter precache --linux \
    && flutter --version

# The checkout is bind-mounted from the host, where its files can have another
# owner; without this git refuses to work in it.
RUN git config --system --add safe.directory '*'

# The build loop-mounts the system image, so the container runs privileged
# and as root; build.sh's sudo calls are then no-ops.
WORKDIR /workspaces/thimsy-os
