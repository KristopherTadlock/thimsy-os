// The ES-DE theme parser: the input is the set's theme.xml, beside a fixed capabilities.xml. A
// parse must finish quickly whatever the input (includes and variables are capped).
#include <stdint.h>

#include <map>
#include <string>

#include "NanoEsdeTheme.h"
#include "fuzz_util.h"

static const char kCaps[] =
    "<themeCapabilities><variant name=\"default\"><selectable>true</selectable></variant>"
    "<colorScheme name=\"dark\"/><aspectRatio>16:9</aspectRatio><aspectRatio>4:3</aspectRatio>"
    "<fontSize>medium</fontSize><language>en_US</language></themeCapabilities>";

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size > 256 * 1024) return 0;
    static const std::string dir = [] {
        const std::string d = nanofuzz::scratchDir();
        nanofuzz::writeFile(d + "/capabilities.xml", reinterpret_cast<const uint8_t*>(kCaps),
                            sizeof(kCaps) - 1);
        return d;
    }();
    if (!nanofuzz::writeFile(dir + "/theme.xml", data, size)) return 0;
    nanoesde::Theme theme;
    const std::map<std::string, std::string> vars = {{"system.theme", "snes"}, {"system.name", "snes"}};
    theme.load(dir, "", "default", "dark", "automatic", "medium", 16.0f / 9.0f, vars);
    return 0;
}
