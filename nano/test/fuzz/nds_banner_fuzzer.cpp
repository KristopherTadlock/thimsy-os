// NanoNdsBanner: a raw .nds, or a .zip holding one when the input starts with a zip signature.
#include <stdint.h>

#include <string>

#include "NanoNdsBanner.h"
#include "fuzz_util.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const bool zip = size >= 2 && data[0] == 'P' && data[1] == 'K';
    const std::string path = nanofuzz::scratchDir() + (zip ? "/in.zip" : "/in.nds");
    if (!nanofuzz::writeFile(path, data, size)) return 0;
    NdsBannerInfo info;
    if (ndsReadBanner(path, info) && info.rgba.size() != 32 * 32 * 4) __builtin_trap();
    return 0;
}
