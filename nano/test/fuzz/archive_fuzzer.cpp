// NanoArchive: list and extract a zip or a 7z (by its signature) into a scratch folder.
#include <dirent.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>

#include "NanoArchive.h"
#include "fuzz_util.h"

static void removeTree(const std::string& p) {
    struct stat st;
    if (lstat(p.c_str(), &st) != 0) return;
    if (S_ISDIR(st.st_mode)) {
        if (DIR* d = opendir(p.c_str())) {
            while (dirent* e = readdir(d))
                if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) removeTree(p + "/" + e->d_name);
            closedir(d);
        }
        rmdir(p.c_str());
    } else {
        unlink(p.c_str());
    }
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const bool sevenZip = size >= 2 && data[0] == '7' && data[1] == 'z';
    const std::string dir = nanofuzz::scratchDir();
    const std::string arc = dir + (sevenZip ? "/in.7z" : "/in.zip");
    if (!nanofuzz::writeFile(arc, data, size)) return 0;
    nanoarc::Index idx = nanoarc::readIndex(arc);
    if (idx.err == nanoarc::Err::None) {
        const std::string out = dir + "/out";
        nanoarc::Progress prog;
        nanoarc::extract(arc, "", out, /*newFolder=*/true, prog);
        removeTree(out);
    }
    unlink(arc.c_str());
    return 0;
}
