# Build environment for hakuX (tools/hakux/build.sh): the toolchain hakuX's
# android/README.md names, and a host C compiler for its host tests. Used by
# compose.yaml's hakux service.
FROM --platform=linux/amd64 ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive
SHELL ["/bin/bash", "-o", "pipefail", "-c"]
ENV PYTHONDONTWRITEBYTECODE=1

RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential \
      ca-certificates \
      cmake \
      curl \
      file \
      git \
      ninja-build \
      openjdk-21-jdk-headless \
      python3 \
      unzip \
      zip \
    && rm -rf /var/lib/apt/lists/*

# Google's command-line tools, pinned by version and checksum (from
# dl.google.com/android/repository/repository2-3.xml).
ENV ANDROID_HOME=/opt/android-sdk
ARG CMDLINE_TOOLS=commandlinetools-linux-16111833_latest.zip
ARG CMDLINE_TOOLS_SHA1=e025545c62a8e64c7559119566a569fb1dec5f60
RUN curl -fsSLo /tmp/tools.zip "https://dl.google.com/android/repository/$CMDLINE_TOOLS" \
    && echo "$CMDLINE_TOOLS_SHA1  /tmp/tools.zip" | sha1sum -c - \
    && mkdir -p "$ANDROID_HOME/cmdline-tools" \
    && unzip -q /tmp/tools.zip -d "$ANDROID_HOME/cmdline-tools" \
    && mv "$ANDROID_HOME/cmdline-tools/cmdline-tools" "$ANDROID_HOME/cmdline-tools/latest" \
    && rm /tmp/tools.zip
ENV PATH=$ANDROID_HOME/cmdline-tools/latest/bin:$PATH

# hakuX's SDK, build tools, platform tools (Gradle installs them otherwise,
# on every run) and NDK. Installing them accepts Google's Android
# SDK License Agreement (sdkmanager --licenses); sdkmanager checks each
# package's checksum.
ARG NDK_VERSION=29.0.14206865
RUN (yes || true) | sdkmanager --licenses > /dev/null \
    && sdkmanager --install "platforms;android-36" "build-tools;36.1.0" "platform-tools" "ndk;$NDK_VERSION" > /dev/null \
    && test -f "$ANDROID_HOME/ndk/$NDK_VERSION/build/cmake/android.toolchain.cmake"
ENV ANDROID_NDK=$ANDROID_HOME/ndk/$NDK_VERSION

# The checkout is bind-mounted from the host, where its files can have another
# owner; without this git refuses to work in it.
RUN git config --system --add safe.directory '*'
WORKDIR /workspaces/thimsy-os
