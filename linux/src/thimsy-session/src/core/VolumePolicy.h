// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
//
// Drives and volumes (IMPLEMENTATION F.1; plan 7.3; decisions I39, I40, I44). Pure: the loop
// describes each block device (sysfs, its partition table entry and its filesystem, read by the
// io layer) and runs the actions this returns.
//
// Which block devices are drives, per partition (classify()):
//   - never virtual devices (loop, ram, zram, dm, md, nbd, mtdblock, sr), an eMMC's hardware
//     partitions (mmcblkNbootM, mmcblkNrpmb), or anything on an eMMC (device/type MMC);
//   - never a whole disk that has partitions, nor anything under 1 MiB (extended-partition stubs);
//   - on a GPT disk, only the types Android's vold mounts: Microsoft basic data and Linux
//     filesystem data. So Linux reserved (the games entry D60firstboot is still making), Linux
//     home (a "data" partition, on any card), root and the rest are never mounted;
//   - on the boot disk (from /run/thimsy/bootdisk, as S00base resolved it), only the partition
//     named "games" typed basic data; when its type can't be read, not even that;
//   - when the table can't be read, partitions named uboot, boot, root or data are not drives.
// A drive's id is its filesystem serial: FAT and exFAT "XXXX-XXXX" (upper case, as vold and blkid
// print it), ext4 its UUID (lower case, dashed). An all-zero serial is no id (plan 7.3). Two drives
// with the same id are both left unused ("duplicate", plan 7.3 and decision 17) until one goes.
// A drive is checked first (vfat: fsck.fat -a when a dirty flag is set; exFAT: fsck.exfat -p
// always, the plan's choice until Phase 0 times it; ext4: e2fsck -p), then mounted at
// /storage/<id>. Removing a mounted drive detaches its mount; "eject" unmounts it fully.
#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "thimsy/json.h"

namespace thimsy {
namespace session {

// GPT partition type GUIDs, upper case.
extern const char kTypeBasicData[];
extern const char kTypeLinuxData[];
extern const char kTypeLinuxReserved[];
extern const char kTypeLinuxHome[];

struct FsInfo {
    std::string type;    // "vfat", "exfat", "ext4"; empty when nothing known was found
    std::string serial;  // vfat/exfat: "XXXX-XXXX"; ext4: the UUID
    std::string label;
    bool dirty = false;  // a dirty flag is set (vfat, exfat)
};

struct BlockDev {
    std::string name;            // kernel name, e.g. "mmcblk1p5"
    std::string disk;            // the whole disk, e.g. "mmcblk1" (== name for a disk)
    bool partition = false;
    int partn = 0;               // the partition table entry, 1-based
    std::string partName;        // GPT name (PARTNAME)
    std::string table;           // "gpt", "mbr" or "" when unread
    std::string partType;        // GPT: type GUID upper case; MBR: two hex digits
    bool emmc = false;           // the disk's device/type is MMC
    bool diskHasPartitions = false;
    uint64_t sizeBytes = 0;
    bool readOnly = false;
    std::string kind;            // "sd", "usb", "virtio", "other"
    FsInfo fs;
};

struct BootDisk {
    std::string disk;  // empty when unknown
    bool emmc = false;
};

struct PolicyConfig {
    std::string gamesName = "games";
    std::string storageDir = "/storage";       // mount points: <storageDir>/<id>
    std::string mediaRwDir = "/mnt/media_rw";  // the advertised alias directory
    uint64_t minSizeBytes = 1024 * 1024;
};

enum class Disposition { Ignore, Drive };
struct Classification {
    Disposition disposition = Disposition::Ignore;
    std::string reason;  // why it is ignored
};

Classification classify(const BlockDev& dev, const BootDisk& boot, const PolicyConfig& cfg);

// The volume id for a filesystem, or empty (no filesystem, no serial, an all-zero serial).
std::string volumeIdFor(const FsInfo& fs);
// mount(8)-style options per plan 7.3.
std::string mountOptions(const std::string& fsType, bool readOnly);
// Whether a check runs before the mount.
bool fsckNeeded(const FsInfo& fs, bool readOnly);

enum class VolState { Checking, Mounted, Ejected, CheckFailed, MountFailed, Unsupported, NoId, Duplicate };
const char* toString(VolState s);

struct Volume {
    std::string dev;
    std::string disk;
    int partn = 0;
    std::string id;
    std::string fs;
    std::string label;
    std::string kind;
    uint64_t sizeBytes = 0;
    bool bootDisk = false;
    bool readOnly = false;
    VolState state = VolState::Checking;
    std::string error;
    int fsckJob = 0;  // the running check, while Checking
};

enum class VolActKind { Fsck, KillFsck, Mount, Unmount };
struct VolAction {
    explicit VolAction(VolActKind k) : kind(k) {}
    VolActKind kind;
    std::string dev;         // kernel name
    std::string fs;
    std::string id;
    std::string mountPoint;  // <storageDir>/<id>
    std::string options;
    int job = 0;             // Fsck, KillFsck
    bool lazy = false;       // Unmount: the device is gone, detach
};

class VolumeManager {
 public:
    explicit VolumeManager(PolicyConfig cfg = {});

    // A device appeared or changed ("add", "change"), already probed.
    std::vector<VolAction> onDevice(const BlockDev& dev, const BootDisk& boot);
    std::vector<VolAction> onRemoved(const std::string& dev);
    // exitCode: the check's exit status, or -1 when it was killed by a signal.
    std::vector<VolAction> onFsckDone(int job, int exitCode);
    std::vector<VolAction> onMountDone(const std::string& dev, bool ok, const std::string& err);
    std::vector<VolAction> onUnmountDone(const std::string& dev, bool ok, const std::string& err);
    // Unmounts a mounted drive by id. False (with *err) if there is no such mounted drive.
    bool eject(const std::string& id, std::vector<VolAction>* out, std::string* err);
    // A mount found at start (a respawned session): <storageDir>/<id> from <dev> is mounted.
    void adoptMounted(const std::string& dev, const std::string& id);

    // True once since the last call if what toJson() returns has changed.
    bool takeChanged();
    json::Value toJson() const;
    const std::map<std::string, Volume>& volumes() const { return vols_; }
    const Volume* byId(const std::string& id) const;
    const PolicyConfig& config() const { return cfg_; }

 private:
    std::vector<VolAction> start(Volume* v);  // check, then mount
    std::vector<VolAction> release(Volume* v, bool lazy);  // stop a check or unmount
    std::vector<VolAction> resolveDuplicates();
    std::string mountPointFor(const std::string& id) const;

    // Bookkeeping per device that toJson() does not show.
    struct Extra {
        bool started = false;  // checked or mounted, or on the way (not waiting as a duplicate)
        bool mounted = false;  // mounted at its mount point
        bool dirty = false;    // the filesystem's dirty flag, at the last probe
    };

    PolicyConfig cfg_;
    std::map<std::string, Volume> vols_;  // by kernel name
    std::map<std::string, Extra> extra_;
    std::map<std::string, std::string> adopted_;  // dev -> id
    int nextJob_ = 1;
    bool changed_ = true;
};

}  // namespace session
}  // namespace thimsy
