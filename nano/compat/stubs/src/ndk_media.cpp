/*
 * Copyright (C) 2026 The Thimsy OS Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// Inert definitions of the NDK media stubs (media/Ndk*.h): nothing can be created, so nothing
// else ever has an object to work on; each call logs once and fails.
#include <media/NdkImage.h>
#include <media/NdkImageReader.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaDataSource.h>
#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaFormat.h>

#include <nanostubs/stub.h>

extern "C" {

// The key names, with Android's values.
const char* AMEDIAFORMAT_KEY_ALBUM = "album";
const char* AMEDIAFORMAT_KEY_ARTIST = "artist";
const char* AMEDIAFORMAT_KEY_BIT_RATE = "bitrate";
const char* AMEDIAFORMAT_KEY_CDTRACKNUMBER = "cdtracknum";
const char* AMEDIAFORMAT_KEY_CHANNEL_COUNT = "channel-count";
const char* AMEDIAFORMAT_KEY_CSD_0 = "csd-0";
const char* AMEDIAFORMAT_KEY_CSD_1 = "csd-1";
const char* AMEDIAFORMAT_KEY_CSD_2 = "csd-2";
const char* AMEDIAFORMAT_KEY_DURATION = "durationUs";
const char* AMEDIAFORMAT_KEY_GENRE = "genre";
const char* AMEDIAFORMAT_KEY_HEIGHT = "height";
const char* AMEDIAFORMAT_KEY_LANGUAGE = "language";
const char* AMEDIAFORMAT_KEY_MIME = "mime";
const char* AMEDIAFORMAT_KEY_PCM_ENCODING = "pcm-encoding";
const char* AMEDIAFORMAT_KEY_SAMPLE_RATE = "sample-rate";
const char* AMEDIAFORMAT_KEY_TITLE = "title";
const char* AMEDIAFORMAT_KEY_WIDTH = "width";
const char* AMEDIAFORMAT_KEY_YEAR = "year";

AMediaFormat* AMediaFormat_new(void) { NANOSTUB_ONCE(); return nullptr; }
media_status_t AMediaFormat_delete(AMediaFormat*) { return AMEDIA_OK; }
bool AMediaFormat_getInt32(AMediaFormat*, const char*, int32_t*) { NANOSTUB_ONCE(); return false; }
bool AMediaFormat_getInt64(AMediaFormat*, const char*, int64_t*) { NANOSTUB_ONCE(); return false; }
bool AMediaFormat_getString(AMediaFormat*, const char*, const char** out) {
    NANOSTUB_ONCE();
    if (out) *out = nullptr;
    return false;
}
void AMediaFormat_setInt32(AMediaFormat*, const char*, int32_t) { NANOSTUB_ONCE(); }
void AMediaFormat_setString(AMediaFormat*, const char*, const char*) { NANOSTUB_ONCE(); }
void AMediaFormat_setBuffer(AMediaFormat*, const char*, const void*, size_t) { NANOSTUB_ONCE(); }

AMediaCodec* AMediaCodec_createCodecByName(const char*) { NANOSTUB_ONCE(); return nullptr; }
AMediaCodec* AMediaCodec_createDecoderByType(const char*) { NANOSTUB_ONCE(); return nullptr; }
media_status_t AMediaCodec_delete(AMediaCodec*) { return AMEDIA_OK; }
media_status_t AMediaCodec_configure(AMediaCodec*, const AMediaFormat*, ANativeWindow*, AMediaCrypto*, uint32_t) {
    NANOSTUB_ONCE();
    return AMEDIA_ERROR_UNKNOWN;
}
media_status_t AMediaCodec_start(AMediaCodec*) { NANOSTUB_ONCE(); return AMEDIA_ERROR_UNKNOWN; }
media_status_t AMediaCodec_stop(AMediaCodec*) { NANOSTUB_ONCE(); return AMEDIA_ERROR_UNKNOWN; }
media_status_t AMediaCodec_flush(AMediaCodec*) { NANOSTUB_ONCE(); return AMEDIA_ERROR_UNKNOWN; }
uint8_t* AMediaCodec_getInputBuffer(AMediaCodec*, size_t, size_t* outSize) {
    NANOSTUB_ONCE();
    if (outSize) *outSize = 0;
    return nullptr;
}
uint8_t* AMediaCodec_getOutputBuffer(AMediaCodec*, size_t, size_t* outSize) {
    NANOSTUB_ONCE();
    if (outSize) *outSize = 0;
    return nullptr;
}
ssize_t AMediaCodec_dequeueInputBuffer(AMediaCodec*, int64_t) { NANOSTUB_ONCE(); return AMEDIA_ERROR_UNKNOWN; }
media_status_t AMediaCodec_queueInputBuffer(AMediaCodec*, size_t, off_t, size_t, uint64_t, uint32_t) {
    NANOSTUB_ONCE();
    return AMEDIA_ERROR_UNKNOWN;
}
ssize_t AMediaCodec_dequeueOutputBuffer(AMediaCodec*, AMediaCodecBufferInfo*, int64_t) {
    NANOSTUB_ONCE();
    return AMEDIA_ERROR_UNKNOWN;
}
AMediaFormat* AMediaCodec_getOutputFormat(AMediaCodec*) { NANOSTUB_ONCE(); return nullptr; }
media_status_t AMediaCodec_releaseOutputBuffer(AMediaCodec*, size_t, bool) {
    NANOSTUB_ONCE();
    return AMEDIA_ERROR_UNKNOWN;
}

AMediaDataSource* AMediaDataSource_new(void) { NANOSTUB_ONCE(); return nullptr; }
void AMediaDataSource_delete(AMediaDataSource*) {}
void AMediaDataSource_setUserdata(AMediaDataSource*, void*) { NANOSTUB_ONCE(); }
void AMediaDataSource_setReadAt(AMediaDataSource*, AMediaDataSourceReadAt) { NANOSTUB_ONCE(); }
void AMediaDataSource_setGetSize(AMediaDataSource*, AMediaDataSourceGetSize) { NANOSTUB_ONCE(); }
void AMediaDataSource_setClose(AMediaDataSource*, AMediaDataSourceClose) { NANOSTUB_ONCE(); }

AMediaExtractor* AMediaExtractor_new(void) { NANOSTUB_ONCE(); return nullptr; }
media_status_t AMediaExtractor_delete(AMediaExtractor*) { return AMEDIA_OK; }
media_status_t AMediaExtractor_setDataSourceFd(AMediaExtractor*, int, off64_t, off64_t) {
    NANOSTUB_ONCE();
    return AMEDIA_ERROR_UNKNOWN;
}
media_status_t AMediaExtractor_setDataSource(AMediaExtractor*, const char*) {
    NANOSTUB_ONCE();
    return AMEDIA_ERROR_UNKNOWN;
}
media_status_t AMediaExtractor_setDataSourceCustom(AMediaExtractor*, AMediaDataSource*) {
    NANOSTUB_ONCE();
    return AMEDIA_ERROR_UNKNOWN;
}
size_t AMediaExtractor_getTrackCount(AMediaExtractor*) { NANOSTUB_ONCE(); return 0; }
AMediaFormat* AMediaExtractor_getTrackFormat(AMediaExtractor*, size_t) { NANOSTUB_ONCE(); return nullptr; }
AMediaFormat* AMediaExtractor_getFileFormat(AMediaExtractor*) { NANOSTUB_ONCE(); return nullptr; }
media_status_t AMediaExtractor_selectTrack(AMediaExtractor*, size_t) { NANOSTUB_ONCE(); return AMEDIA_ERROR_UNKNOWN; }
ssize_t AMediaExtractor_readSampleData(AMediaExtractor*, uint8_t*, size_t) { NANOSTUB_ONCE(); return -1; }
int64_t AMediaExtractor_getSampleTime(AMediaExtractor*) { NANOSTUB_ONCE(); return -1; }
bool AMediaExtractor_advance(AMediaExtractor*) { NANOSTUB_ONCE(); return false; }
media_status_t AMediaExtractor_seekTo(AMediaExtractor*, int64_t, SeekMode) {
    NANOSTUB_ONCE();
    return AMEDIA_ERROR_UNKNOWN;
}

void AImage_delete(AImage*) {}
media_status_t AImage_getWidth(const AImage*, int32_t*) { NANOSTUB_ONCE(); return AMEDIA_ERROR_UNKNOWN; }
media_status_t AImage_getHeight(const AImage*, int32_t*) { NANOSTUB_ONCE(); return AMEDIA_ERROR_UNKNOWN; }
media_status_t AImage_getCropRect(const AImage*, AImageCropRect*) { NANOSTUB_ONCE(); return AMEDIA_ERROR_UNKNOWN; }
media_status_t AImage_getPlanePixelStride(const AImage*, int, int32_t*) {
    NANOSTUB_ONCE();
    return AMEDIA_ERROR_UNKNOWN;
}
media_status_t AImage_getPlaneRowStride(const AImage*, int, int32_t*) {
    NANOSTUB_ONCE();
    return AMEDIA_ERROR_UNKNOWN;
}
media_status_t AImage_getPlaneData(const AImage*, int, uint8_t** data, int* dataLength) {
    NANOSTUB_ONCE();
    if (data) *data = nullptr;
    if (dataLength) *dataLength = 0;
    return AMEDIA_ERROR_UNKNOWN;
}

media_status_t AImageReader_new(int32_t, int32_t, int32_t, int32_t, AImageReader** reader) {
    NANOSTUB_ONCE();
    if (reader) *reader = nullptr;
    return AMEDIA_ERROR_UNKNOWN;
}
void AImageReader_delete(AImageReader*) {}
media_status_t AImageReader_getWindow(AImageReader*, ANativeWindow** window) {
    NANOSTUB_ONCE();
    if (window) *window = nullptr;
    return AMEDIA_ERROR_UNKNOWN;
}
media_status_t AImageReader_acquireLatestImage(AImageReader*, AImage** image) {
    NANOSTUB_ONCE();
    if (image) *image = nullptr;
    return AMEDIA_ERROR_UNKNOWN;
}

}  // extern "C"
