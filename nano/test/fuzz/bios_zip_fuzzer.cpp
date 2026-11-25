// NanoBiosCatalog's zip reader and matchers, plus the PS2 ROMDIR and Vita PUP header probes.
#include <stdint.h>

#include <string>
#include <vector>

#include "NanoBiosCatalog.h"
#include "fuzz_util.h"

using namespace android::nanobios;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const std::string path = nanofuzz::scratchDir() + "/bios.bin";
    if (!nanofuzz::writeFile(path, data, size)) return 0;
    std::vector<ZipEntry> dir;
    if (zipDirectory(path, &dir))
        for (const ZipEntry& e : dir) zipMemberSha1(path, e);
    std::vector<std::string> found;
    zipMatchSet(path, &found);
    int region = 0;
    ps2RomverLabel(ps2Romver(path), &region);
    uint32_t fw = 0;
    isVitaPup(path, &fw);
    return 0;
}
