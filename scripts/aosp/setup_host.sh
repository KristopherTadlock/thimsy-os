#!/bin/bash
# Prepares an Ubuntu 22.04 build host for scripts/aosp/system.sh, as root:
# the packages LineageOS 21 builds need, and a user to build as (AOSP refuses
# to build as root) owning the folder the tree goes in.
#
#   sudo scripts/aosp/setup_host.sh [USER [DIR]]     (defaults: builder, /aosp)
#
# On Windows, in a WSL distro kept for the build (README "1.4.4 base").
set -euo pipefail

user=${1:-builder}
dir=${2:-/aosp}
[[ $(id -u) -eq 0 ]] || { echo "run as root" >&2; exit 1; }

export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq \
  bc bison build-essential ccache curl file flex fontconfig g++-multilib gcc-multilib \
  git git-lfs gnupg gperf imagemagick lib32ncurses5-dev lib32readline-dev lib32z1-dev \
  libelf-dev liblz4-tool libncurses5 libsdl1.2-dev libssl-dev libxml2 libxml2-utils \
  locales lz4 lzop p7zip-full pngcrush python-is-python3 python3 python3-pip rsync \
  schedtool squashfs-tools sudo unzip xsltproc xz-utils zip zlib1g-dev zstd
locale-gen en_US.UTF-8 >/dev/null

id "$user" >/dev/null 2>&1 || useradd -m -s /bin/bash "$user"
mkdir -p "$dir"
chown "$user:$user" "$dir"
su - "$user" -c '
  git config --global user.name >/dev/null || git config --global user.name thimsy-builder
  git config --global user.email >/dev/null || git config --global user.email builder@localhost
  git lfs install --skip-repo >/dev/null
'
echo "ready: build as $user in $dir (scripts/aosp/system.sh $dir/gammaos ...)"
