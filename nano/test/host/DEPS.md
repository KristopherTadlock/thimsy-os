# Nano host build: what it needs

`nano/test/host/Makefile` (docs/linux/IMPLEMENTATION.md C.3) builds with g++ and make alone for
`check-compat` and `check-unit-0`. The other targets need development packages that the WSL
`Ubuntu` distro (20.04) does not have yet; they print `SKIP <target>: missing <header>` until the
packages are installed. Installing them is step G10's job, once downloads are allowed:

```sh
sudo apt-get install zlib1g-dev libssl-dev libtinyxml2-dev libfreetype-dev libpng-dev \
  libjpeg-turbo8-dev libwebp-dev libgif-dev libcurl4-openssl-dev libegl-dev libgles-dev \
  libdrm-dev libgbm-dev libasound2-dev libgtest-dev pkg-config
```

(On 20.04 the FreeType package is `libfreetype6-dev`; `libfreetype-dev` is its newer name.)

| Target | Needs | Package |
|---|---|---|
| `check-compat` | g++, make, `prlimit`, `setarch`, `timeout` | already there |
| `check-unit-0` | the same | already there |
| `check-unit` | `zlib.h` (NdsBanner, BiosCatalog and every test that reaches it) | `zlib1g-dev` |
| | `openssl/md5.h`, `openssl/sha.h` (BiosCatalog, ButtonMapReaders) | `libssl-dev` |
| | `tinyxml2.h` (EsdeTheme) | `libtinyxml2-dev` |
| | `ziparchive/zip_archive.h` (EsdeSystemName and EsdeTheme tests) | none: Android's libziparchive. TODO(G10): Debian's `android-libziparchive-dev` (headers under `/usr/include/android`), or a small zip reader behind the same API |
| | GoogleTest (optional: `GTEST=system`) | `libgtest-dev` |
| `syntax-core` | all of the above, plus `ft2build.h`, `png.h`, `curl/curl.h`, `EGL/*.h`, `GLES2/*.h`, `GLES3/gl3.h`, libdrm's `drm.h` and `drm_fourcc.h`, and G4's `nano/compat/stubs/include` | `libfreetype-dev libpng-dev libcurl4-openssl-dev libegl-dev libgles-dev libdrm-dev` |
| `headless` | `syntax-core`'s, plus G5's `test/host/headless/main_headless.cpp` and `[host]` sources | as `syntax-core` |
| the image decoder's tests (`check-compat`) | `png.h`, `jpeglib.h`, `webp/decode.h`, `webp/demux.h`, `gif_lib.h` (they skip without) | `libpng-dev libjpeg-turbo8-dev libwebp-dev libgif-dev` |
| the Linux image's back ends (G9, built by CMake) | `gbm.h`, `alsa/asoundlib.h` | `libgbm-dev libasound2-dev` |

Besides headers, some test programs read the asset folders (`ps3xmb/`, `nano_esde_themes/`):
`nano/` holds only the asset files the patch changes, so they need the base's too. Make them once
with `py -3.12 tools/nano-tree/nano_tree.py fetch --from-cache <cache>` and
`py -3.12 tools/nano-tree/nano_tree.py compose work/nano-composed` (the Makefile's default
`NANO_DATA`); without them those programs print `SKIP <program>: needs the base's <folder>`.

Which test program needs which header is generated, not written here:
`nano/build/unit-closure.txt` (from `tools/nano-tree/include_census.py`), and
`make -C nano/test/host list-unit` prints it with each program's target.

`NanoArchive` (libziparchive and the LZMA SDK) is not in the host suite until a portable zip back end
exists (C.3).
