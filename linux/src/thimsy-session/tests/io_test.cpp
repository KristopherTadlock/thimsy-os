// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
//
// The io layer's parts that need no root: sysfs reads over a fake tree, input capabilities, the
// fake key FIFOs, the text uevent format, mount option splitting, and spawning.
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "io/Evdev.h"
#include "io/Mounter.h"
#include "io/Netlink.h"
#include "io/Proc.h"
#include "io/Sysfs.h"
#include "thimsy/atomic_file.h"

using namespace thimsy;
using namespace thimsy::session;

namespace {

void removeTree(const std::string& path) {
    struct stat st;
    if (lstat(path.c_str(), &st) != 0) return;
    if (S_ISDIR(st.st_mode)) {
        if (DIR* d = opendir(path.c_str())) {
            while (struct dirent* e = readdir(d)) {
                if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
                removeTree(path + "/" + e->d_name);
            }
            closedir(d);
        }
        rmdir(path.c_str());
    } else {
        unlink(path.c_str());
    }
}

class Tree {
 public:
    Tree() {
        const char* t = getenv("TMPDIR");
        std::string tmpl = std::string(t && *t ? t : "/tmp") + "/thimsy-session-test-XXXXXX";
        std::vector<char> buf(tmpl.begin(), tmpl.end());
        buf.push_back('\0');
        if (mkdtemp(buf.data())) root_ = buf.data();
    }
    ~Tree() {
        if (!root_.empty()) removeTree(root_);
    }
    const std::string& root() const { return root_; }
    void file(const std::string& rel, const std::string& text) {
        std::string p = root_ + rel;
        makeDirs(p.substr(0, p.rfind('/')));
        writeFileAtomic(p, text, {0644, false});
    }
    void link(const std::string& rel, const std::string& target) {
        std::string p = root_ + rel;
        makeDirs(p.substr(0, p.rfind('/')));
        EXPECT_EQ(symlink(target.c_str(), p.c_str()), 0);
    }
    // A disk the way sysfs shows it: /sys/devices/<parent>/block/<disk>/<parts>, class links, and
    // the device's type file when given.
    void disk(const std::string& parent, const std::string& disk, const std::string& type,
              const std::vector<std::pair<int, std::string>>& parts) {
        std::string dir = "/sys/devices/" + parent + "/block/" + disk;
        file(dir + "/dev", "179:0\n");
        file(dir + "/size", "62333952\n");
        file(dir + "/ro", "0\n");
        file(dir + "/uevent", "MAJOR=179\nMINOR=0\nDEVNAME=" + disk + "\nDEVTYPE=disk\n");
        if (!type.empty()) {
            file("/sys/devices/" + parent + "/type", type + "\n");
            link(dir + "/device", "../..");
        }
        link("/sys/class/block/" + disk, root_ + dir);
        for (const auto& p : parts) {
            std::string name = disk + (isdigit(static_cast<unsigned char>(disk.back())) ? "p" : "") + std::to_string(p.first);
            std::string pd = dir + "/" + name;
            file(pd + "/dev", "179:" + std::to_string(p.first) + "\n");
            file(pd + "/partition", std::to_string(p.first) + "\n");
            file(pd + "/size", "2048000\n");
            file(pd + "/ro", "0\n");
            file(pd + "/uevent", "DEVNAME=" + name + "\nDEVTYPE=partition\nPARTN=" + std::to_string(p.first) +
                                     "\nPARTNAME=" + p.second + "\n");
            link("/sys/class/block/" + name, root_ + pd);
        }
    }

 private:
    std::string root_;
};

}  // namespace

TEST(Sysfs, BlockDevicesAsTheKernelShowsThem) {
    Tree t;
    t.disk("platform/fe2b0000.mmc/mmc_host/mmc0/mmc0:0001", "mmcblk0", "MMC", {{1, "boot"}, {17, "userdata"}});
    t.disk("platform/fe2c0000.mmc/mmc_host/mmc1/mmc1:aaaa", "mmcblk1", "SD", {{4, "data"}, {5, "games"}});
    t.disk("platform/usb/1-1/1-1:1.0/host0/target0:0:0/0:0:0:0", "sda", "", {});
    Sysfs s(t.root());
    std::vector<std::string> names = s.blockNames();
    ASSERT_EQ(names.size(), 7u);
    EXPECT_EQ(s.diskOf("mmcblk1p5"), "mmcblk1");
    EXPECT_EQ(s.diskOf("mmcblk1"), "mmcblk1");
    EXPECT_TRUE(s.isEmmc("mmcblk0"));
    EXPECT_FALSE(s.isEmmc("mmcblk1"));
    EXPECT_EQ(s.emmcDisks(), std::vector<std::string>{"mmcblk0"});

    BlockDev d;
    ASSERT_TRUE(s.blockDev("mmcblk1p5", &d));
    EXPECT_TRUE(d.partition);
    EXPECT_EQ(d.disk, "mmcblk1");
    EXPECT_EQ(d.partn, 5);
    EXPECT_EQ(d.partName, "games");
    EXPECT_EQ(d.sizeBytes, 2048000ull * 512);
    EXPECT_FALSE(d.emmc);
    EXPECT_EQ(d.kind, "sd");
    ASSERT_TRUE(s.blockDev("mmcblk0p17", &d));
    EXPECT_TRUE(d.emmc);
    ASSERT_TRUE(s.blockDev("mmcblk1", &d));
    EXPECT_TRUE(d.diskHasPartitions);
    ASSERT_TRUE(s.blockDev("sda", &d));
    EXPECT_FALSE(d.diskHasPartitions);
    EXPECT_EQ(d.kind, "usb");
    EXPECT_FALSE(s.blockDev("nope", &d));
}

TEST(Sysfs, PartitionsNestedInTheDiskDirectory) {
    // The layout G2e's shell tests use: no links, partitions inside the disk's class directory.
    Tree t;
    t.file("/sys/class/block/mmcblk1/dev", "179:0\n");
    t.file("/sys/class/block/mmcblk1/mmcblk1p5/partition", "5\n");
    t.file("/sys/class/block/mmcblk1p5/partition", "5\n");
    Sysfs s(t.root());
    EXPECT_EQ(s.diskOf("mmcblk1p5"), "mmcblk1");
}

TEST(Sysfs, FilesAndBacklight) {
    Tree t;
    t.file("/sys/class/backlight/backlight/brightness", "120\n");
    t.file("/sys/class/backlight/backlight/max_brightness", "255\n");
    Sysfs s(t.root());
    EXPECT_EQ(s.backlight(), "/sys/class/backlight/backlight");
    ASSERT_TRUE(s.write("/sys/class/backlight/backlight/brightness", "7"));
    std::string v;
    ASSERT_TRUE(s.readLine("/sys/class/backlight/backlight/brightness", &v));
    EXPECT_EQ(v, "7");
    EXPECT_FALSE(s.write("/sys/nope/file", "1"));
    int64_t n = 0;
    EXPECT_TRUE(parseInt64("-42", &n));
    EXPECT_EQ(n, -42);
    EXPECT_FALSE(parseInt64("4x", &n));
}

TEST(Evdev, CapabilityBitmaps) {
    // A gpio-keys power button: KEY_POWER (116) in the lowest word.
    EXPECT_TRUE(capabilityHas("10000000000000 0", KEY_POWER));
    EXPECT_FALSE(capabilityHas("10000000000000 0", KEY_VOLUMEUP));
    EXPECT_TRUE(capabilityHas("1", SW_LID));
    EXPECT_FALSE(capabilityHas("", SW_LID));
    // BTN_SOUTH (0x130 = 304) is in the fifth 64-bit word from the end.
    EXPECT_TRUE(capabilityHas("1000000000000 0 0 0 0", BTN_SOUTH));
    EXPECT_FALSE(capabilityHas("10000 0 0 0 0", BTN_SOUTH));
}

TEST(Evdev, FakeNodesCarryTextEvents) {
    Tree t;
    t.file("/sys/class/input/event0/device/name", "rk805 pwrkey\n");
    t.file("/sys/class/input/event0/device/capabilities/key", "10000000000000 0\n");
    t.file("/sys/class/input/event1/device/name", "Thimsy Gamepad\n");
    t.file("/sys/class/input/event1/device/capabilities/key", "7fff000000000000 0 0 0 0\n");
    t.file("/sys/class/input/event2/device/name", "Goodix Capacitive TouchScreen\n");
    t.file("/sys/class/input/event2/device/capabilities/key", "400 0 0 0 0 0\n");
    t.file("/sys/class/input/event3/device/name", "gpio-keys-control\n");
    t.file("/sys/class/input/event3/device/capabilities/key", "ffff000000000000 0 0 1c000000000000 0\n");
    makeDirs(t.root() + "/dev/input");
    for (const char* n : {"event0", "event1", "event2", "event3"}) {
        ASSERT_EQ(mkfifo((t.root() + "/dev/input/" + n).c_str(), 0600), 0);
    }
    Sysfs s(t.root());
    Evdev ev(s, true);
    EXPECT_TRUE(ev.scan());
    ASSERT_EQ(ev.fds().size(), 2u);  // the power key and the Thimsy pad; not touch, not the pad's source
    EXPECT_FALSE(ev.scan());

    int w = open((t.root() + "/dev/input/event0").c_str(), O_WRONLY | O_NONBLOCK);
    ASSERT_GE(w, 0);
    std::string text = "KEY_POWER 1 0\nKEY_POWER 0 3100\nbogus line\nKEY_VOLUMEUP 1\n";
    ASSERT_EQ(write(w, text.data(), text.size()), static_cast<ssize_t>(text.size()));
    close(w);
    std::vector<KeyEvent> out;
    int powerFd = -1;
    for (int fd : ev.fds()) {
        if (ev.nameOf(fd).find("pwrkey") != std::string::npos) powerFd = fd;
    }
    ASSERT_GE(powerFd, 0);
    ASSERT_TRUE(ev.read(powerFd, 50000, &out));
    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(out[0].code, KEY_POWER);
    EXPECT_EQ(out[0].value, 1);
    EXPECT_EQ(out[0].time, 50000);
    EXPECT_EQ(out[1].time, 53100);
    EXPECT_EQ(out[2].code, KEY_VOLUMEUP);
    EXPECT_EQ(out[2].time, 50000);
    EXPECT_EQ(out[0].source, KeyEvent::Source::Keys);

    unlink((t.root() + "/dev/input/event0").c_str());
    EXPECT_TRUE(ev.scan());
    EXPECT_EQ(ev.fds().size(), 1u);
}

TEST(Netlink, TextUevents) {
    std::string buf =
        "add@/devices/platform/mmc1/block/mmcblk1/mmcblk1p5\nACTION=add\nSUBSYSTEM=block\nDEVNAME=mmcblk1p5\n"
        "DEVTYPE=partition\nPARTN=5\nPARTNAME=games\n\n"
        "change@/devices/x\nSUBSYSTEM=block\n\n"
        "garbage without header\n\n"
        "remove@/devices/platform/mmc1/block/mmcblk1/mmcblk1p5\nSUBSYSTEM=block\nDEVNAME=mmcblk1p5";
    std::vector<Uevent> out;
    parseTextUevents(&buf, &out);
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0].action, "add");
    EXPECT_EQ(out[0].devname, "mmcblk1p5");
    EXPECT_EQ(out[0].get("PARTNAME"), "games");
    EXPECT_EQ(out[1].action, "change");
    EXPECT_FALSE(buf.empty());  // the unfinished remove waits for its blank line
    parseTextUevents(&buf, &out, true);
    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(out[2].action, "remove");
    EXPECT_TRUE(buf.empty());
}

TEST(Mounter, OptionsSplitIntoFlagsAndData) {
    unsigned long flags = 0;
    std::string data;
    splitMountOptions("utf8,shortname=mixed,dirsync,noatime,nodev,nosuid,noexec,flush", &flags, &data);
    EXPECT_EQ(flags, static_cast<unsigned long>(MS_DIRSYNC | MS_NOATIME | MS_NODEV | MS_NOSUID | MS_NOEXEC));
    EXPECT_EQ(data, "utf8,shortname=mixed,flush");
    splitMountOptions("ro,rw,dirsync", &flags, &data);
    EXPECT_EQ(flags, static_cast<unsigned long>(MS_RDONLY | MS_DIRSYNC));
    EXPECT_EQ(data, "");
}

TEST(Proc, SpawnWaitAndPidFiles) {
    Tree t;
    SpawnSpec s;
    s.argv = {"sh", "-c", "echo \"$THIMSY_X\"; exit 3"};
    s.env = {"THIMSY_X=hello"};
    s.logFile = t.root() + "/out.log";
    EXPECT_EQ(runWait(s, 5000), 3);
    std::string text;
    ASSERT_TRUE(readFile(t.root() + "/out.log", &text));
    EXPECT_EQ(text, "hello\n");
    SpawnSpec missing;
    missing.argv = {t.root() + "/no-such-program"};
    EXPECT_EQ(runWait(missing, 5000), 127);
    SpawnSpec refused;
    refused.argv = {"true"};
    refused.preExec = [](std::string* err) {
        *err = "no";
        return false;
    };
    EXPECT_EQ(runWait(refused, 5000), 126);
    SpawnSpec slow;
    slow.argv = {"sleep", "10"};
    EXPECT_EQ(runWait(slow, 100), 128 + 9);

    t.file("/p.pid", "1234\n");
    EXPECT_EQ(readPidFile(t.root() + "/p.pid"), 1234);
    EXPECT_EQ(readPidFile(t.root() + "/none.pid"), 0);
    EXPECT_TRUE(alive(getpid()));
    EXPECT_FALSE(alive(0));
    EXPECT_NE(cmdline(getpid()).find("io_test"), std::string::npos);
}
