// The Quick Resume handoff's wait for RetroArch's config (with RetroArch's folder on a card, a RetroArch
// launch waits for the card's mount as well as the ROM's volume) and the root-written path files.

#include <gtest/gtest.h>

#include <cstdio>
#include <string>

#include <sys/stat.h>
#include <unistd.h>

#include "NanoMenuUtils.h"

using android::raConfigVolumeReady;

namespace {

const std::string kInternalOnly =
    "/dev/block/dm-5 /data f2fs rw 0 0\n"
    "/dev/fuse /storage/emulated fuse rw 0 0\n";
const std::string kWithCard = kInternalOnly +
    "/dev/block/vold/public:179,1 /mnt/media_rw/1234-ABCD vfat rw 0 0\n"
    "/dev/fuse /storage/1234-ABCD fuse rw 0 0\n";
const std::string kRawCardOnly = kInternalOnly +
    "/dev/block/vold/public:179,1 /mnt/media_rw/1234-ABCD vfat rw 0 0\n";

}  // namespace

TEST(RaConfigVolumeReady, FolderOnTheCardWaitsForItsMount) {
    const std::string dir = "/storage/1234-ABCD/emulator_config/retroarch";
    EXPECT_FALSE(raConfigVolumeReady(dir, kInternalOnly));
    // The raw mount comes first; launches resolve the folder through /storage.
    EXPECT_FALSE(raConfigVolumeReady(dir, kRawCardOnly));
    EXPECT_TRUE(raConfigVolumeReady(dir, kWithCard));
    EXPECT_FALSE(raConfigVolumeReady("/storage/5678-EF01/retroarch", kWithCard));
    EXPECT_FALSE(raConfigVolumeReady(dir, ""));
}

TEST(RaConfigVolumeReady, DefaultOrInternalFolderNeedsNoCard) {
    EXPECT_TRUE(raConfigVolumeReady("", kInternalOnly));
    EXPECT_TRUE(raConfigVolumeReady("", ""));
    EXPECT_TRUE(raConfigVolumeReady("/storage/emulated/0/RetroArch", kInternalOnly));
    EXPECT_TRUE(raConfigVolumeReady("/storage/self/primary/RetroArch", kInternalOnly));
    EXPECT_FALSE(raConfigVolumeReady("/storage/emulated/0/RetroArch", ""));
}

namespace {

std::string scratchDir() {
    char tmpl[] = "/tmp/nano_pathfile_XXXXXX";
    const char* d = mkdtemp(tmpl);
    return d ? std::string(d) : std::string();
}

std::string slurpFile(const std::string& p) {
    std::string s;
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) return s;
    char b[256];
    size_t n;
    while ((n = fread(b, 1, sizeof(b), f)) > 0) s.append(b, n);
    fclose(f);
    return s;
}

}  // namespace

TEST(WritePathFile, PlantedTempLinkIsNotFollowed) {
    const std::string dir = scratchDir();
    ASSERT_FALSE(dir.empty());
    const std::string victim = dir + "/victim", target = dir + "/nano_launch_game.txt";
    {
        FILE* f = fopen(victim.c_str(), "w");
        ASSERT_NE(nullptr, f);
        fputs("keep", f);
        fclose(f);
        chmod(victim.c_str(), 0600);
    }
    ASSERT_EQ(0, symlink(victim.c_str(), (target + ".tmp").c_str()));
    android::writePathFile(target.c_str(), "123\n/storage/1234-ABCD/roms/game.cue");
    EXPECT_EQ("keep", slurpFile(victim));
    struct stat st {};
    ASSERT_EQ(0, stat(victim.c_str(), &st));
    EXPECT_EQ(0600u, st.st_mode & 0777);
    EXPECT_EQ("123\n/storage/1234-ABCD/roms/game.cue", android::readPathFile(target.c_str()));
    ASSERT_EQ(0, lstat(target.c_str(), &st));
    EXPECT_TRUE(S_ISREG(st.st_mode));
    android::writePathFile(target.c_str(), "");
    EXPECT_NE(0, access(target.c_str(), F_OK));
    unlink((target + ".tmp").c_str());
    unlink(victim.c_str());
    rmdir(dir.c_str());
}

TEST(ReadPathFile, FifoReadsAsEmptyWithoutBlocking) {
    const std::string dir = scratchDir();
    ASSERT_FALSE(dir.empty());
    const std::string fifo = dir + "/nano_launch_game.txt";
    ASSERT_EQ(0, mkfifo(fifo.c_str(), 0600));
    EXPECT_EQ("", android::readPathFile(fifo.c_str()));
    unlink(fifo.c_str());
    rmdir(dir.c_str());
}
