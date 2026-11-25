// NanoJson: parse, and anything that parses must serialize to text that parses again to the same
// text.
#include <stdint.h>
#include <stdlib.h>

#include <string>

#include "NanoJson.h"

using android::njson::Value;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const std::string text(reinterpret_cast<const char*>(data), size);
    Value v;
    if (!android::njson::parse(text, &v)) return 0;
    for (bool pretty : {false, true}) {
        const std::string out = android::njson::serialize(v, pretty);
        Value w;
        if (!android::njson::parse(out, &w)) abort();
        if (android::njson::serialize(w, pretty) != out) abort();
    }
    v.asInt();
    v.asNumber();
    return 0;
}
