# Video and music decoding on Linux (OA-24)

Date: 2026-11-26. Status: research only. Nothing was built or run, and no device was touched. Public pages were read on this date; where a page could not be read in full, the note says so. Claims not backed by a source or by the code are marked *(guess)*. Numbers in brackets are the sources at the end.

OA-24 makes video and music a requirement for the Linux alpha: game video snaps, theme background videos and the music player, on FFmpeg's libraries, an LGPL build, with the RK3566 hardware decoder where the kernel and Buildroot support it and software decode otherwise. This note works out how.

## Summary

- **Library:** link FFmpeg's libraries (libavformat, libavcodec, libswresample, libavutil) into Nano directly. Do not embed libmpv and do not launch ffplay. Nano draws video inside its own GL scene and taps the music's PCM for its visualiser; only direct use of the libraries keeps both.
- **Alpha:** software decode on the Cortex-A55s, sized for snaps and theme videos (SD), with the existing YUV texture path. Every format OA-24 lists works this way with stock FFmpeg and no out-of-tree patch.
- **Hardware, mainline:** on RK3566 only the Hantro-driven VDPU2 block is in mainline: H.264 and MPEG-2 up to 1080p, VP8. The RK3566's HEVC and VP9 block (VDPU346) has posted patches but is not in any released kernel. Upstream FFmpeg has no V4L2 request API hwaccel yet; ROCKNIX and LibreELEC carry it as a patch.
- **Hardware, plan:** add H.264 hardware decode after the alpha with Jonas Karlman's v4l2-request patches on FFmpeg, decoded frames passed to the GPU as DRM PRIME buffers (zero-copy into a `GL_TEXTURE_EXTERNAL_OES`, the same texture type Nano's Android path uses). Pick up HEVC and VP9 once VDPU346 reaches the imported ROCKNIX kernel.
- **Not rkmpp:** Rockchip's MPP needs the vendor BSP kernel, which Thimsy does not use (D1).
- **Licence:** a plain LGPL build (no `--enable-gpl`, no `--enable-nonfree`), linked dynamically. On Linux, FFmpeg's own AC-3 decoder (LGPL) replaces the GPL liba52, so AC-3 keeps working with `NANO_WITH_AC3=OFF`.

## 1. What Nano's media code does today

Read-only, in `nano/` on `step/w7-res-media` (based on `linux` at `e002d39`).

| File | What it does | Android APIs |
|---|---|---|
| `NanoVideo.{h,cpp}` | The one video engine. AMediaExtractor + AMediaCodec on a worker thread, output into a `GLConsumer` (a `GL_TEXTURE_EXTERNAL_OES` texture) that the render thread latches and draws into a rect (`NanoVideo.h:1-10`, `:24-31`). Has a YUV path: decode into an `AImageReader` (YUV_420_888) and upload three planes, for GPUs that can't import the decoder's buffer (`NanoVideo.cpp:40-61`). "Fed" mode takes access units from Nano's own demuxers. A software decoder is used only up to 720x576, because on the A53 devices Android's software codec "CANNOT do 720p/1080p at realtime" (`NanoVideo.cpp:587-591`) | NDK media, libgui |
| `NanoAudio.{h,cpp}` | The music engine and the video's sound: extractor + codec to int16 PCM in a ring, a 512-point FFT for the visualiser, output through AAudio (`NanoAudio.h:17-34`). Formats: mp3, flac, m4a, aac, ogg, opus, wav (`MUSIC_PLAYER.md:13`) | NDK media; output already goes through `nanoplat::Audio` |
| `NanoTsDemux`, `NanoAviDemux` | Nano's own MPEG-TS and AVI demuxers feeding `NanoVideo` fed mode and the audio ring; the AVI parser is "deliberately Android-free" (`NanoAviDemux.h`) | AMediaCodec (MP3 in AVI) |
| `NanoIcyDemux`, `NanoHls`, `NanoTsDescramble` | Internet radio (Ogg, FLAC, ADTS over HTTP), IPTV over HTTP/HLS through a custom `AMediaDataSource`, and a PMT rewrite for "scrambled" ATSC captures | AMediaDataSource, AMediaCodec |
| `NanoAacDec`, `NanoAc3` | ADTS AAC through AMediaCodec; AC-3 through the vendored liba52 (GPL-2) | AMediaCodec; liba52 |

The users of `NanoVideo` are the video wallpaper, the ES-DE theme background video and the focused game's video snap (they share one decoder, a game's video outranks the background), the picker preview, thumbnails and the video player (`NanoMenu.h:2905`, `:2922-2945`, `:2954`, `:4655`, `:5430`). So one engine covers every OA-24 case.

All of these files are `[core]` (shared) in `nano/build/sources.txt:79-98`. On Linux they compile against inert stubs (`nano/compat/stubs/src/ndk_media.cpp`: every create returns null) with `NANO_WITH_MEDIA=OFF` and `NANO_WITH_AC3=OFF` (IMPLEMENTATION.md C.2). The architecture audit counts 366 NDK media references in 11 files (`audit-2026-11-26/design.md:262`).

The Linux audio sink already exists: `platform/linux/AudioAlsa.cpp` (ALSA `default`, a dmix the image sets up, S16 interleaved) behind `nanoplat::Audio` (`NanoPlatform.h:139-169`).

## 2. Hardware video decode on RK3566 with a mainline kernel

### 2.1 The blocks

| Block | Kernel driver | Codecs on RK3566 | Mainline status |
|---|---|---|---|
| VDPU2 (Hantro family), `rockchip,rk3568-vpu` | `hantro_vpu` (`drivers/media/platform/verisilicon/`) | MPEG-2, H.264, VP8. H.264 and MPEG-2 to Full HD, VP8 to UHD [5]. PINE64: H.264 "only Hantro, not rkvdec2", 1080p max [8] | In mainline. The node is in `rk356x-base.dtsi` with no `status`, so it is on for every RK356x board [6]; the RGxx3 dtsi does not touch it [7] |
| VDPU346 (rkvdec2 family), `rockchip,rk3568-vdec` | `rkvdec` | H.264 and HEVC in the posted series, VP9 "active work" [2]; OpenWrt's backport adds VP9 from Christian Hewitt's tree, "not posted upstream" [3] | **Not in mainline.** Upstream `rkvdec.c` matches rk3288, rk3328, rk3399, rk3588 and rk3576 only [4], and `rk356x-base.dtsi` has no vdec node [6]. Hewitt's v3 (January 2026, on 6.19-rc2) had 2 of 3 patches reviewed, with reports of 4K and large-frame problems [2]; OpenWrt's backport (opened 2026-11-14, shown as merged) marks a v4 "to be posted upstream" and states "Linux 6.18 does not drive the VDPU346" [3] |
| VEPU (encoder) | `hantro_vpu` | JPEG encode only [5] | Not needed |
| JPEG decoder | none | none | PINE64: no mainline driver [8] |

The RK3588 and RK3576 decoders (VDPU381, VDPU383) were merged in Linux 7.0 for H.264 and HEVC; VDPU346 was described as the next step [1].

Both drivers are V4L2 *stateless* decoders: the kernel keeps no decoding state, and userspace parses the bitstream and passes each frame's headers as controls attached to a media request [34]. That is why a player needs request-API support in its decoder library (section 3); the older V4L2 mem2mem support in FFmpeg (`v4l2m2m`) is for stateful decoders and does not drive these.

### 2.2 What Thimsy's kernel has

- Thimsy's RK3566 kernel is ROCKNIX `next`'s 7.3.5 with ROCKNIX's config (`tests/fixtures/expected/generated/rk3566/defconfig.fragment`, `hw-rk3566.fragment`). That config has `CONFIG_VIDEO_HANTRO=m`, `CONFIG_VIDEO_HANTRO_ROCKCHIP=y`, `CONFIG_VIDEO_ROCKCHIP_VDEC=y`, `CONFIG_MEDIA_SUPPORT=y`, `CONFIG_V4L2_MEM2MEM_DEV=y` and `CONFIG_DMABUF_HEAPS=y` [39]. `CONFIG_MEDIA_CONTROLLER_REQUEST_API` was not found in the part of the file read *(check with grep on the generated config)*.
- ROCKNIX's RK3566 and common Rockchip kernel patch folders, as GitHub lists them, have no patch named for rkvdec, vdpu or vdec [40]. So the imported kernel very likely has no VDPU346 support *(guess: GitHub's listing may be truncated; grep the generated patch set)*. With `VIDEO_ROCKCHIP_VDEC=y` but no rk3568 match, rkvdec simply never binds on RK3566.
- `hantro_vpu` is a module, and the image has no uevent-driven module loading: a module loads only if `device.json`'s `modules` names it (`linux/board/rk3566/common/README.md:91-93`, `S03modules`). The VPU needs either `CONFIG_VIDEO_HANTRO=y` in `board/rk3566/linux.fragment` or `hantro_vpu` in the module list. S03modules runs before Nano's first frame, so the boot cost has to be measured, or the module loaded later in the deferred stage *(guess: the load is small)*.
- Device nodes: the root has devtmpfs and libudev-zero, no udev (`linux/configs/fragments/common.fragment:40-43`). Under the non-root Nano of OA-27 (`video` group), `/dev/video*` and `/dev/media*` need group `video` access set by the session or the device table *(guess: devtmpfs creates them root-only)*.

### 2.3 The vendor path: rkmpp

Rockchip's MPP library drives all of the SoC's decoders (including HEVC, VP9 and the JPEG block), and the `nyanmisaka/ffmpeg-rockchip` fork wires it into FFmpeg. Its README says the "Rockchip BSP/vendor kernel is necessary, 5.10 and 6.1 are two tested versions", with RK3588 as the typical target [16]. Forum reports say the MPP device (`/dev/mpp_service`) is missing on mainline kernels [17]. ROCKNIX uses rkmpp only on RK3588 (its RK3588 kernel is the vendor BSP) and v4l2-request on its other Rockchip families [14]. Thimsy is mainline-only by D1, so rkmpp is out on RK3566.

## 3. FFmpeg and the request API

- **Upstream:** FFmpeg's Changelog, through 9.1 and `next`, has no V4L2 request API entry [10]. Karlman's series went to ffmpeg-devel in August 2024 (MPEG-2, H.264, HEVC; a dedicated `v4l2request` hwdevice instead of the old DRM one; frames out as `drm_prime`) [9]. Its successor, PR 20847 ("WIP: Add V4L2 Request API hwaccels for MPEG2, H.264, HEVC, VP8, VP9 and AV1"), opened 2025-11-05, was still an open draft, not merged, when read again on 2026-11-27 [11].
- **Out of tree:** Karlman keeps v4l2-request branches per FFmpeg release: `v4l2-request-n8.1` and `-n8.0.1` (March 2026), `-n7.1.3`, and older `-n6.1.1` (December 2023) [12]. ROCKNIX builds FFmpeg 9.0 with a single `0001-v4l2-request.patch`, plus `detlev` and `vf-deinterlace-v4l2m2m` patch sets on most Rockchip families, configured with `--enable-v4l2-request --enable-libudev --enable-libdrm --enable-v4l2_m2m`, LGPL (`--enable-version3`, no `--enable-gpl`) [14][15]. ROCKNIX's FFmpeg packaging is LibreELEC's [14].
- **Players:** mpv's `--hwdec=drm` works today against a patched FFmpeg that uses the old DRM hwdevice [8]; mpv's PR for the new `v4l2request` device is open and "on ice" until FFmpeg merges [13]. A November 2026 tester reported a stall above 32 CAPTURE buffers in the FFmpeg series [13].
- **Alternative:** GStreamer's `v4l2codecs` plugin (gst-plugins-bad) has upstream stateless decoders for H.264, HEVC, MPEG-2, VP8, VP9 and AV1 [18]. In Buildroot that option depends on `BR2_PACKAGE_HAS_UDEV` [19], and libudev-zero provides only `HAS_LIBUDEV` [20], so it does not fit Thimsy's udev-less root without a recipe change. GStreamer would also replace the whole demux and decode stack, not just the hardware part.

What this means for Thimsy: hardware decode needs an out-of-tree FFmpeg patch until PR 20847, or a successor, is merged. The patch is LGPL code in libavcodec, so it belongs in `linux/gpl/patches/ffmpeg/` with its own SPDX line (D6 as refined by I5, I9 and I41). Which branch fits depends on the FFmpeg version (section 8).

## 4. Getting frames on screen with KMS/GBM

Nano is the KMS master and draws everything with GLES on GBM (D4; `platform/linux/display/`). A snap or theme video is one textured quad inside a theme layout, under and over other elements, so the video must reach Nano's GL scene as a texture. It cannot go to a separate player's plane or window.

- **Hardware frames:** the v4l2-request hwaccel outputs `AV_PIX_FMT_DRM_PRIME` frames [9], each an `AVDRMFrameDescriptor` of dma-buf fds, planes, offsets, pitches and modifier [37]. Nano imports them with `EGL_EXT_image_dma_buf_import` [35] as an EGLImage and binds it to a `GL_TEXTURE_EXTERNAL_OES` texture (`OES_EGL_image_external`) [36]. No copy is made. This is the same texture type `NanoVideo` samples today through `GLConsumer`, so its `samplerExternalOES` draw program carries over. The decoder's buffers must stay referenced until GL is done with the frame (hold the `AVFrame` until the next latch).
- **Risk:** NV12 import on Panfrost has had colour bugs on RK3399 through the external-image path (a Weston issue; only its title could be read) [38]. Phase 0 checks Hantro NV12 output into Panfrost on the ARC-D. Nano already has the fallback: the YUV path (`NanoVideo.cpp:40-61`) uploads planes to plain textures and converts in a shader. On Linux it is fed from mapped DRM PRIME frames (`av_hwframe_transfer_data`) or from software frames. Nano's YUV path takes three planes, so NV12's two planes need a second shader variant, or `swscale` to yuv420p *(guess: the shader is cheaper)*.
- **Software frames:** decoded yuv420p `AVFrame`s go through the same YUV path. This is the alpha's only path.
- **What mpv and ffplay do instead:** mpv's `drm` VO "Does not support hardware acceleration" [21]; `--hwdec=drm` needs `--vo=gpu` or `gpu-next` [22], which takes the KMS device itself, or the libmpv render API inside a host's GL context [21]. ffplay draws through SDL2 [27]. Neither can share Nano's KMS master and scene except libmpv's render API (section 6).

## 5. Audio decode and ALSA output

- FFmpeg decodes every audio format on OA-24's list natively, with no external library: AAC, AC-3, FLAC, MP3, Opus, Vorbis [31]; WAV/PCM is native too. `libswresample` converts to interleaved S16 at the stream's rate.
- AC-3: FFmpeg's `ac3dec.c` is LGPL 2.1 or later [30]. On Linux it replaces liba52 (`NanoAc3`, GPL-2), so the GPL code stays out (plan 3.3, `NANO_WITH_AC3=OFF`) and AC-3 audio in .ts and .avi still plays.
- No hardware audio decode is needed or available.
- Output stays on `nanoplat::Audio`: `NanoAudioPlayer`'s ring feeds `AudioAlsa.cpp`'s pull thread, mixed by dmix with UI sounds and the game. The FFT tap is unchanged, because the decoded PCM still passes through Nano's ring.

## 6. Which player or library

| Option | Fit | Verdict |
|---|---|---|
| **FFmpeg libraries directly** | Same shape as today's extractor + codec: `avformat_open_input`/`av_read_frame` for AMediaExtractor, `avcodec_send_packet`/`avcodec_receive_frame` for AMediaCodec, a custom `AVIOContext` for `AMediaDataSource` (NanoHls, ICY). Nano keeps its threads, A/V clock, async open and release, the PCM ring and FFT, and its GL draw. ES-DE's own video player is built the same way, on libavcodec, libavformat, libavfilter and libavutil, with a hardware path (`VideoFFmpegComponent`, MIT) [33] | **Recommended** |
| libmpv (render API) | Robust playback and A/V sync, renders into the host's GL context [21]. But it is GPL-2.0+ unless built with `-Dgpl=false`, a mode meant for libmpv [23]; Buildroot's mpv package is GPL-2.0+, always selects libplacebo and libass, and has no `gpl` switch [24][25]. It owns audio output, so the music visualiser loses its PCM tap, and Nano's TS, AVI, IPTV and radio paths would have to be rebuilt around it | No: licence work, size, loses the FFT tap |
| ffplay, launched | A separate SDL2 process [27]; it would need the KMS master and full screen, so no snap in a theme rect, no theme background | No |
| GStreamer | Upstream stateless decode [18], but needs udev in Buildroot [19][20] and replaces the whole stack | Not now; reconsider only if FFmpeg's request-API work stalls for good |

The Android side does not change: it keeps AMediaExtractor and AMediaCodec, moved behind the platform layer (section 9).

## 7. Licensing

- FFmpeg is LGPL 2.1 or later by default; GPL parts are off unless `--enable-gpl` is passed, and using them makes "the GPL applies to all of FFmpeg" [28][29]. FFmpeg's checklist asks for no `--enable-gpl`, no `--enable-nonfree`, and dynamic linking [28]. `--enable-nonfree` makes the result "unredistributable" [29], so it is never used.
- Nothing Nano needs is GPL-only: the GPL list is x86 assembly, test tools, about 30 libavfilter filters and external libraries such as x264, x265 and xvid [29]. `libpostproc` needs GPL in Buildroot [27]; leave it off.
- Nano is Apache-2.0. FFmpeg says Apache 2.0 "is incompatible with the LGPL v2.1 and the GPL v2, but not with version 3" [29]; that sentence is about libraries combined *into* FFmpeg. Nano linking a shared LGPL library is the case LGPL allows. So the recommendation is plain LGPL 2.1 or later, without `--enable-version3` *(our reading; for the owner's licence check, not legal advice)*. ROCKNIX builds with `--enable-version3` (LGPLv3) [14]. Thimsy can do the same if the owner's check prefers LGPLv3, and it costs no feature Nano uses.
- Nothing in this note's recommendation needs version3. In FFmpeg 6.1's `configure`, the external libraries that need it are gmp, libaribb24, liblensfun, libopencore_amrnb, libopencore_amrwb, libvo_amrwbenc, mbedtls and rkmpp; libsmbclient needs GPLv3 [41]. `configure` stops with an error if one of them is enabled without `--enable-version3` [41]. rkmpp is out (section 2.3), the protocol list is file only, so mbedtls is not needed, and the rest (AMR codecs, ARIB subtitles, the lensfun filter, gmp for RTMP encryption) are things Nano does not use. Karlman's v4l2-request hwaccel is in no licence list: its `configure` puts `v4l2_request` in the autodetected hwaccel libraries only [42].
- The v4l2-request patches are libavcodec code under FFmpeg's LGPL *(guess: check each patch's header when importing)*.
- Patents: FFmpeg warns that implementations "might be subject to some patent rights", and that MPEG LA "is vigilant and diligent" about MPEG technologies [28]. H.264, HEVC and AAC are in that class; MP3's licensing programme ended on 2017-04-23 [32]. This is a distribution question for the owner, the same one every handheld firmware that ships FFmpeg faces (ROCKNIX does [14]); this note does not settle it.
- Release duties: Buildroot's `ffmpeg.mk` sets `FFMPEG_LICENSE = LGPL-2.1+, libjpeg license`, with `LICENSE.md` and `COPYING.LGPLv2.1` as licence files. Only `BR2_PACKAGE_FFMPEG_GPL` adds `GPL-2.0+` and `COPYING.GPLv2` [26]. So `legal-info` lists FFmpeg as LGPL-2.1+ and the libjpeg licence, with no GPL. The libjpeg part is three DCT files taken from libjpeg (`jfdctfst.c`, `jfdctint_template.c`, `jrevdct.c`); its licence asks for the IJG to be credited in the documentation when only binaries are shipped [29]. The `.mk` changes the licence string only for the GPL option, so `--enable-version3` through `_EXTRACONF` would not show in `legal-info`; the release notes would have to say it. The `.mk` also puts `--disable-version3` in its base `FFMPEG_CONF_OPTS`, outside any condition, so every build passes it, and no Buildroot option changes it [26]. `_EXTRACONF` is the last thing appended to `FFMPEG_CONF_OPTS` [26], and FFmpeg's `configure` applies its options in order, so the last of the two wins [41]. A version3 build through `_EXTRACONF` therefore depends on `_EXTRACONF` staying after `--disable-version3` on the configure line. That holds in 2026.08.2, but is not a documented promise; Thimsy's own FFmpeg recipe (section 8) would set the flag directly. With the recommended LGPL 2.1+ build none of this matters. The release's source publication must include FFmpeg's source and the patches applied.

## 8. Buildroot

- Buildroot 2026.08.2 (`linux/build.sh:56`) ships FFmpeg **6.1.5**, licence `LGPL-2.1+, libjpeg license`, plus `GPL-2.0+` only with `BR2_PACKAGE_FFMPEG_GPL` [26]. Options: `_GPL`, `_NONFREE`, `_FFMPEG`, `_FFPLAY` (selects SDL2), `_FFPROBE`, `_SWSCALE`, `_POSTPROC` (GPL), lists of decoders, demuxers, parsers, protocols, filters, `_INDEVS`, `_OUTDEVS`, and `_EXTRACONF` [27]. `--enable-libdrm` follows `BR2_PACKAGE_LIBDRM`, `--enable-hwaccels` is always on, and dav1d is used when selected [26]. There is no v4l2-request patch and no option for it [26].
- Release config for the alpha, in a Thimsy fragment: GPL and nonfree off; the `ffmpeg`, `ffprobe` and `ffplay` programs off (dev builds may keep `ffmpeg` and `ffprobe` for Phase 0 measurements); decoders limited to h264, hevc, mpeg4, msmpeg4*, mpeg1/2video, mjpeg, vp8, vp9, aac, ac3, eac3, mp3, flac, vorbis, opus, pcm_*; demuxers to mov, matroska (webm), avi, mpegts, ogg, flac, mp3, aac, wav, hls; protocols to file only (Nano fetches HTTP itself with curl, NanoHls); filters off. The exact lists come from the formats ES-DE media and the music player use *(guess at the lists; the trimmed size is to be measured)*.
- Hardware decode needs, in a later stage: the v4l2-request patch in `linux/gpl/patches/ffmpeg/` (the image already sets `BR2_GLOBAL_PATCH_DIR` to `gpl/patches`, `common.fragment:19`), `--enable-v4l2-request --enable-libudev` through `_EXTRACONF`, and libudev-zero built before FFmpeg. `_EXTRACONF` adds no build dependency, so the order is not guaranteed *(guess)*: either a one-line patch to `ffmpeg.mk` or Thimsy's own FFmpeg package. Karlman's newest branch is for 8.1 [12]; the 6.1 branch is from 2023 and uses the old DRM hwdevice. So Thimsy probably moves to its own FFmpeg recipe at 8.1 or 9.x, as ROCKNIX does with 9.0 [14] *(guess; whether a BR2_EXTERNAL package may shadow Buildroot's `ffmpeg` name was not checked, so a new name such as `thimsy-ffmpeg` is the safe choice)*.
- mpv 0.40.0 and GStreamer's `v4l2codecs` are in Buildroot [24][19], but this note does not recommend them.
- PPSSPP builds its own bundled FFmpeg (`linux/package/ppsspp/ppsspp.mk:32-33`, `USE_SYSTEM_FFMPEG=OFF`). That is separate and unaffected; using the system FFmpeg there later would save space *(guess)*.
- Size and boot time: unknown until built. A trimmed LGPL libavcodec, libavformat, libavutil and libswresample is a few MB on arm64 *(guess)*. Linking them into Nano adds dynamic-loader work before the first frame. If Phase 0's measurement shows it, Nano can `dlopen` the media back end on the first video or track *(guess)*.

## 9. Mapping Nano's Android media code behind the platform layer (OA-4)

OA-4 puts every Android-only API in `platform/firmware`, and OA-24 asks for "a media decode interface ... with the Android implementation moved verbatim". Two shapes are possible:

1. **Player level (recommended first).** `nanoplat` declares interfaces with today's public APIs: a video player like `NanoVideo` (probe, async open by path, URL or fed stream, feed, transport, `updateFrame`, texture or planes, draw) and an audio decoder source like `NanoAudioPlayer`'s decode side (open, fed PCM, metadata probe). `platform/firmware` gets today's `NanoVideo.cpp`, `NanoAudio.cpp` decode code, `NanoAacDec`, `NanoIcyDemux`'s codec part, `NanoHls`'s data source and `NanoTsDescramble` as moves, so the Android change is a verified move (H.3). `platform/linux` implements the same interfaces on FFmpeg. The cost is that pacing, clock and GL draw code exists twice.
2. **Component level (later).** Shared code keeps the threads, clock, ring, FFT and GL draw. The platform provides only `Demuxer` (tracks, packets, seek, custom reader), `VideoDecoder` (packets in, frames out as an external-texture image or CPU planes) and `AudioDecoder` (packets in, S16 out). This is OA-24's "demux, audio and video decode" literally, but it rewrites the Android engines rather than moving them.

The mapping, per Android piece:

| Android today | Linux implementation |
|---|---|
| `AMediaExtractor` (setDataSourceFd, tracks, readSampleData, seekTo) | `AVFormatContext`: `avformat_open_input`, `avformat_find_stream_info`, `av_read_frame`, `av_seek_frame` |
| `AMediaDataSource` (NanoHls, NanoTsDescramble) | `AVIOContext` with a read callback over NanoHls's temp file; the descramble rewrite may be unneeded if libavformat's mpegts ignores the CA descriptor *(guess; test with an ATSC capture)* |
| `AMediaFormat` (mime, csd-0/1, width, height, rate) | `AVCodecParameters` (codec id, extradata, size, sample rate, channels) |
| `AMediaCodec` video, hardware | `AVCodecContext` with the v4l2-request hwaccel (stage 3), DRM PRIME frames |
| `AMediaCodec` video, software (`c2.android.*`) | `AVCodecContext`, software, with frame threads on 4 A55 cores |
| `AMediaCodec` audio (AAC, MP3, FLAC, Opus, Vorbis), `NanoAacDec` | `AVCodecContext` + `libswresample` to S16 |
| `NanoAc3` (liba52) | FFmpeg `ac3` decoder |
| `GLConsumer` + `Surface` (OES texture) | EGLImage from the DRM PRIME descriptor, bound to `GL_TEXTURE_EXTERNAL_OES` |
| `AImageReader` YUV_420_888 path | software `AVFrame` planes (or mapped DRM PRIME) uploaded to the YUV textures |
| AAudio | already `nanoplat::Audio` (`AudioAlsa.cpp`) |
| `nanoVideoUseYuvPath()` reading `ro.board.platform` | a Display or Media capability of the platform; no property on Linux |
| "one hardware decoder" rules (the wallpaper, background and preview share one) | kept: true of Hantro in practice for UI load, and software decode is CPU-bound anyway *(guess)* |

Nano's own demuxers (`NanoTsDemux`, `NanoAviDemux`, `NanoIcyDemux`'s framing) are plain C++ and can stay shared; libavformat could replace them on Linux later.

## 10. Recommendation and staged plan

**Recommendation:** FFmpeg's libraries linked into Nano, LGPL build, behind a player-level `nanoplat` media interface. Software decode for the alpha, H.264 hardware decode on the Hantro block after it, HEVC and VP9 when VDPU346 reaches the imported kernel. No libmpv, no ffplay, no rkmpp, no GStreamer.

| Stage | Work | Done when |
|---|---|---|
| 0, Phase 0 on the ARC-D (dev build) | With `ffmpeg`/`ffprobe` in the dev image: decode real ES-DE snaps, theme background videos and music with `-benchmark` (software, 1 to 4 threads); `modprobe hantro_vpu` and list `/dev/video*` and `/dev/media*`; once a patched FFmpeg exists, `-hwaccel v4l2request` (or `drm` on old branches) on the same files; import an NV12 dma-buf into Panfrost as an external texture and check the colours | numbers for CPU load per resolution, and a yes or no on Hantro and on NV12 import |
| 1, the interface | `nanoplat` media interfaces (player level); Android code moved into `platform/firmware` verbatim, an Android-surface commit cherry-picked after the AOSP build (OA-4, H.3); the media capability reports unsupported on Linux, so rows hide; the NDK media stubs and their budget shrink | Android builds and passes the owner's smoke test; host tests use a fake media back end |
| 2, the alpha | Linux back end on FFmpeg, software only: video into the YUV path, audio into `NanoAudioPlayer`'s ring, AC-3 by FFmpeg; Buildroot FFmpeg 6.1.5, LGPL, trimmed lists; `NANO_WITH_MEDIA=ON` for Linux; capability supported; image size and boot time measured (OA-24) | snaps, theme videos and music play on the ARC-D; `legal-info` lists FFmpeg as LGPL-2.1+ and the libjpeg licence, with no GPL |
| 3, hardware H.264 | Thimsy's FFmpeg recipe at the version Karlman's newest branch targets, the v4l2-request patch in `gpl/patches/ffmpeg/`, libudev-zero as a dependency; Hantro built in or in the module list; `/dev/video*` and `/dev/media*` in group `video`; the DRM PRIME to EGLImage path with the YUV path as fallback | 720p and 1080p H.264 plays at full rate with low CPU; software stays the fallback per file |
| 4, follow upstream | Drop the patch when PR 20847 lands; add HEVC and VP9 when VDPU346 is in the imported ROCKNIX kernel (or carry Hewitt's series in `gpl/` if the owner wants it sooner); consider the component-level interface | - |

**Not decided here, for the coordinator:**

- Player level against component level for the first interface (this note recommends player level, for a verbatim Android move).
- `--enable-version3` (LGPLv3) or plain LGPL 2.1+ for the FFmpeg build. This note recommends plain LGPL 2.1+: nothing recommended here needs version3 (section 7).
- Whether stage 3 belongs before the alpha. OA-24 allows software decode "at the snap sizes", so it does not have to.

## Sources

Read on 2026-11-26.

1. CNX Software, "Rockchip RK3588 and RK3576 H.264 and H.265 video decoders gain mainline Linux support", 2026-02-27. https://www.cnx-software.com/2026/02/27/rockchip-rk3588-rk3576-h-264-and-h-265-video-decoders-mainline-linux/
2. C. Hewitt, "[PATCH v3 0/3] media: rockchip: rkvdec: add support for the VDPU346 variant", linux-arm-kernel, 2026-01-10, with replies. https://ratatoskr.run/linux-arm-kernel/2026/01/3361486/t
3. OpenWrt PR #26704, "rockchip: add the RK356x VDPU346 video decoder", opened 2026-11-14; the page showed it merged, with a date (2026-11-27) later than this note's, so read it again before relying on the merge. https://github.com/openwrt/openwrt/pull/26704
4. Linux `drivers/media/platform/rockchip/rkvdec/rkvdec.c`, master. https://raw.githubusercontent.com/torvalds/linux/master/drivers/media/platform/rockchip/rkvdec/rkvdec.c
5. Linux `drivers/media/platform/verisilicon/rockchip_vpu_hw.c` (`rk3568_vpu_variant`, `rk3568_vepu_variant`), master. https://raw.githubusercontent.com/torvalds/linux/master/drivers/media/platform/verisilicon/rockchip_vpu_hw.c
6. Linux `arch/arm64/boot/dts/rockchip/rk356x-base.dtsi`, master. https://raw.githubusercontent.com/torvalds/linux/master/arch/arm64/boot/dts/rockchip/rk356x-base.dtsi
7. Linux `arch/arm64/boot/dts/rockchip/rk3566-anbernic-rgxx3.dtsi`, master. https://raw.githubusercontent.com/torvalds/linux/master/arch/arm64/boot/dts/rockchip/rk3566-anbernic-rgxx3.dtsi
8. PINE64, "Mainline Hardware Decoding" (undated). https://pine64.org/documentation/General/Mainline_Hardware_Decoding/
9. J. Karlman, "[PATCH v2 0/8] Add V4L2 Request API hwaccels for MPEG2, H.264 and HEVC", ffmpeg-devel, August 2024. https://ffmpeg.org/pipermail/ffmpeg-devel/2024-August/332034.html
10. FFmpeg `Changelog`, master. https://raw.githubusercontent.com/FFmpeg/FFmpeg/master/Changelog
11. FFmpeg PR #20847, "WIP: Add V4L2 Request API hwaccels for MPEG2, H.264, HEVC, VP8, VP9 and AV1", a draft; read again on 2026-11-27 (first 100,000 characters of the page). https://code.ffmpeg.org/FFmpeg/FFmpeg/pulls/20847
12. Kwiboo/FFmpeg branches matching "v4l2-request". https://github.com/Kwiboo/FFmpeg/branches/all?query=v4l2-request
13. mpv PR #14690 (v4l2request hwdec), and #14511 (closed RFC). https://github.com/mpv-player/mpv/pull/14690 , https://github.com/mpv-player/mpv/pull/14511
14. ROCKNIX `projects/ROCKNIX/packages/multimedia/ffmpeg/package.mk`, `next`. https://raw.githubusercontent.com/ROCKNIX/distribution/next/projects/ROCKNIX/packages/multimedia/ffmpeg/package.mk
15. ROCKNIX `packages/multimedia/ffmpeg/patches/` and `patches/v4l2-request/`, `next`. https://github.com/ROCKNIX/distribution/tree/next/packages/multimedia/ffmpeg/patches
16. nyanmisaka/ffmpeg-rockchip README. https://github.com/nyanmisaka/ffmpeg-rockchip
17. Radxa forum, "Armbian can't use ffmpeg and mpp" (forum report). https://forum.radxa.com/t/armbian-cant-use-ffmpeg-and-mpp/13100
18. GStreamer, v4l2codecs plugin documentation. https://gstreamer.freedesktop.org/documentation/v4l2codecs/index.html
19. Buildroot 2026.08.2 `package/gstreamer1/gst1-plugins-bad/Config.in`. https://raw.githubusercontent.com/buildroot/buildroot/2026.08.2/package/gstreamer1/gst1-plugins-bad/Config.in
20. Buildroot 2026.08.2 `package/libudev-zero/Config.in`. https://raw.githubusercontent.com/buildroot/buildroot/2026.08.2/package/libudev-zero/Config.in
21. mpv `DOCS/man/vo.rst`, master. https://raw.githubusercontent.com/mpv-player/mpv/master/DOCS/man/vo.rst
22. mpv `DOCS/man/options.rst` (`--hwdec`), master; only the first part of the file was read. https://raw.githubusercontent.com/mpv-player/mpv/master/DOCS/man/options.rst
23. mpv `Copyright`, master. https://raw.githubusercontent.com/mpv-player/mpv/master/Copyright
24. Buildroot 2026.08.2 `package/mpv/mpv.mk`. https://raw.githubusercontent.com/buildroot/buildroot/2026.08.2/package/mpv/mpv.mk
25. Buildroot 2026.08.2 `package/mpv/Config.in`. https://raw.githubusercontent.com/buildroot/buildroot/2026.08.2/package/mpv/Config.in
26. Buildroot 2026.08.2 `package/ffmpeg/ffmpeg.mk`. https://raw.githubusercontent.com/buildroot/buildroot/2026.08.2/package/ffmpeg/ffmpeg.mk
27. Buildroot 2026.08.2 `package/ffmpeg/Config.in`. https://raw.githubusercontent.com/buildroot/buildroot/2026.08.2/package/ffmpeg/Config.in
28. FFmpeg, "Legal". https://ffmpeg.org/legal.html
29. FFmpeg `LICENSE.md`, master; the libjpeg sentence also checked at tag `n6.1.2`. https://raw.githubusercontent.com/FFmpeg/FFmpeg/master/LICENSE.md
30. FFmpeg `libavcodec/ac3dec.c` licence header, master. https://raw.githubusercontent.com/FFmpeg/FFmpeg/master/libavcodec/ac3dec.c
31. FFmpeg, "General Documentation" (supported codecs). https://ffmpeg.org/general.html
32. Fraunhofer IIS, mp3 page (licensing programme terminated 2017-04-23). https://www.iis.fraunhofer.de/en/ff/amm/consumer-electronics/mp3.html
33. ES-DE `es-core/src/components/VideoFFmpegComponent.h`, master. https://gitlab.com/es-de/emulationstation-de/-/raw/master/es-core/src/components/VideoFFmpegComponent.h
34. Linux kernel documentation, "Memory-to-memory Stateless Video Decoder Interface". https://docs.kernel.org/userspace-api/media/v4l/dev-stateless-decoder.html
35. Khronos, `EGL_EXT_image_dma_buf_import`. https://registry.khronos.org/EGL/extensions/EXT/EGL_EXT_image_dma_buf_import.txt
36. Khronos, `OES_EGL_image_external`. https://registry.khronos.org/OpenGL/extensions/OES/OES_EGL_image_external.txt
37. FFmpeg `libavutil/hwcontext_drm.h` (`AVDRMFrameDescriptor`). https://raw.githubusercontent.com/FFmpeg/FFmpeg/master/libavutil/hwcontext_drm.h
38. Weston issue #839, "Panfrost (rk3399) NV12 sampling broken" (title only; the page was behind a bot check). https://gitlab.freedesktop.org/wayland/weston/-/issues/839
39. ROCKNIX `projects/ROCKNIX/devices/RK3566/linux/linux.aarch64.conf`, `next` ("Linux/arm64 7.3.5"). https://raw.githubusercontent.com/ROCKNIX/distribution/next/projects/ROCKNIX/devices/RK3566/linux/linux.aarch64.conf
40. ROCKNIX kernel patch folders, `next`: `projects/ROCKNIX/devices/RK3566/patches/linux/`, `projects/ROCKNIX/packages/linux/patches/{mainline,mainline-rockchip,7.3}/` (GitHub listings, possibly truncated). https://github.com/ROCKNIX/distribution/tree/next/projects/ROCKNIX/devices/RK3566/patches/linux
41. FFmpeg `configure`: `EXTERNAL_LIBRARY_VERSION3_LIST` and `EXTERNAL_LIBRARY_GPLV3_LIST` read at tag `n6.1.5` (Buildroot 2026.08.2's version); the `for opt do` option loop, `die_license_disabled version3` and the licence strings read at tag `n6.1.2`, read 2026-11-27. https://raw.githubusercontent.com/FFmpeg/FFmpeg/n6.1.5/configure , https://raw.githubusercontent.com/FFmpeg/FFmpeg/n6.1.2/configure
42. Kwiboo/FFmpeg `configure`, branch `v4l2-request-n8.1` (`v4l2_request` in `HWACCEL_AUTODETECT_LIBRARY_LIST`, not in a licence list), read 2026-11-27; only the first 100,000 characters were read. https://raw.githubusercontent.com/Kwiboo/FFmpeg/v4l2-request-n8.1/configure
