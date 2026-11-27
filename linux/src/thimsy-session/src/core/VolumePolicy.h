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
//   - a partition named "info" of at most 2 MiB is never a drive, whether or not the table was
//     read: a never-booted Thimsy card's README stub (OA-33, W8-SMALL-10), basic data until its
//     first boot retypes it. A larger partition that happens to be named info is not a stub;
//   - when the table can't be read, partitions named uboot, boot, root or data are not drives.
// A drive's id is its filesystem serial: FAT and exFAT "XXXX-XXXX" (upper case, as vold and blkid
// print it), ext4 its UUID (lower case, dashed). An all-zero serial is no id (plan 7.3). Two drives
// with the same id are both left unused ("duplicate", plan 7.3 and decision 17) until one goes.
// A drive is checked first (vfat: fsck.fat -a when a dirty flag is set; exFAT: fsck.exfat -p
// always, the plan's choice until Phase 0 times it; ext4: e2fsck -p), then mounted at
// /storage/<id>. Removing a mounted drive detaches its mount; "eject" unmounts it fully.
//
// ext2/3/4 drives that are not read-only are mounted for Nano (OA-29, io/Idmap.h): as they are
// when their top folder belongs to `nano` (1000), else through an idmapped mount that shows the
// owner as `nano`, else read-only with an ownership decision pending; the "take-ownership" request
// then chowns the top folder and mounts the drive writable (takeOwnership()), and "keep-read-only"
// records that the user declined (keepReadOnly(), which the session saves per drive id). A drive
// whose filesystem went read-only by itself (ext4 errors) is a filesystem error, not a decision.
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
    // Nano runs as `nano` (uid 1000, OA-27), so ext2/3/4 drives are mounted for it (mountsForNano,
    // OA-29). False while Nano runs as root: root needs no mapping, and could not create files
    // through one (uid 0 is not mapped in it), so the drives are mounted as they are.
    bool nanoRunsAsNano = true;
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
// Whether the drive is mounted through the owner check (OA-29): ext2/3/4 (all probed as "ext4")
// that is not read-only.
bool mountsForNano(const std::string& fsType, bool readOnly);

enum class VolState { Checking, Mounted, Ejected, CheckFailed, MountFailed, Unsupported, NoId, Duplicate };
const char* toString(VolState s);

// How Nano may use a mounted ext2/3/4 drive (OA-29). None: another filesystem, or not mounted.
enum class Ownership {
    None,
    Nano,           // its top folder belongs to nano: as it was, or after take-ownership
    Idmapped,       // the owner shows as nano through an idmapped mount
    NeedsDecision,  // no idmapped mount: read-only for Nano until the user lets Thimsy take it
    Declined,       // as NeedsDecision, but the user said no: Nano does not ask again (take-ownership
                    // still works when the user asks for it)
    FsError,        // the superblock is read-only by itself (ext4 found errors): read-only for Nano
};
const char* toString(Ownership o);
// The ownership of an ext4 mount a respawned session finds in /proc/self/mountinfo, from its type,
// its own options and its superblock's: a read-only superblock -> FsError (a drive mounted for Nano
// never has one on purpose); "idmapped" -> Idmapped; "ro" (that mount only) -> NeedsDecision; else
// Nano. Not ext4: None. A kept decline is applied later (VolumeManager).
Ownership ownershipOfMount(const std::string& fsType, const std::string& mountOptions,
                           const std::string& superOptions);
// Whether a comma-separated option list ("rw,nosuid,idmapped") holds opt.
bool hasMountOption(const std::string& options, const std::string& opt);

// One line of /proc/self/mountinfo (paths keep the kernel's octal escapes).
struct MountInfo {
    std::string mountPoint;
    std::string mountOptions;  // this mount's: "rw" or "ro", then nosuid, nodev, idmapped and so on
    std::string fsType;
    std::string source;
    std::string superOptions;  // the superblock's: "rw" or "ro", then the filesystem's own
};
// The lines of /proc/self/mountinfo, in order; a malformed line is skipped.
std::vector<MountInfo> parseMountInfo(const std::string& text);
// Whether the topmost mount at mountPoint has a read-only superblock (false if nothing is there).
bool superblockReadOnlyAt(const std::vector<MountInfo>& mounts, const std::string& mountPoint);

// The drives the user chose to keep read-only, as the session saves them (W8-IDMAP-15): one id per
// line. A line that is not shaped like a volume id (letters, digits and '-') is skipped.
std::set<std::string> parseKeptReadOnly(const std::string& text);
std::string keptReadOnlyText(const std::set<std::string>& ids);

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
    Ownership ownership = Ownership::None;  // while Mounted
};

enum class VolActKind { Fsck, KillFsck, Mount, Unmount, TakeOwnership };
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
    bool forNano = false;    // Mount: through the owner check (mountsForNano); TakeOwnership: true
};

class VolumeManager {
 public:
    explicit VolumeManager(PolicyConfig cfg = {});

    // A device appeared or changed ("add", "change"), already probed.
    std::vector<VolAction> onDevice(const BlockDev& dev, const BootDisk& boot);
    std::vector<VolAction> onRemoved(const std::string& dev);
    // exitCode: the check's exit status, or -1 when it was killed by a signal.
    std::vector<VolAction> onFsckDone(int job, int exitCode);
    // own: how the mount came out, for a Mount with forNano (else None).
    std::vector<VolAction> onMountDone(const std::string& dev, bool ok, const std::string& err,
                                       Ownership own = Ownership::None);
    std::vector<VolAction> onUnmountDone(const std::string& dev, bool ok, const std::string& err);
    // Unmounts a mounted drive by id. False (with *err) if there is no such mounted drive.
    bool eject(const std::string& id, std::vector<VolAction>* out, std::string* err);
    // The user agreed that Thimsy takes the drive's top folder (OA-29). False (with *err) if there
    // is no such mounted drive or it has no ownership decision pending (NeedsDecision or Declined).
    bool takeOwnership(const std::string& id, std::vector<VolAction>* out, std::string* err);
    // stillMounted: false when the drive lost its mount on the way. fsError: it failed because the
    // drive's superblock is read-only (the session read that from mountinfo).
    std::vector<VolAction> onTakeOwnershipDone(const std::string& dev, bool ok, bool stillMounted,
                                               const std::string& err, bool fsError = false);
    // The user declined that Thimsy takes the drive's top folder: it stays read-only and shows as
    // Declined, now and at every later mount of that id. False (with *err) if there is no such
    // mounted drive or it has no decision pending. The caller saves keptReadOnly().
    bool keepReadOnly(const std::string& id, std::string* err);
    // The ids the user declined for earlier (the session's saved list, at start), and now.
    void setKeptReadOnly(std::set<std::string> ids);
    const std::set<std::string>& keptReadOnly() const { return keptReadOnly_; }
    // A mount found at start (a respawned session): <storageDir>/<id> from <dev> is mounted, as own
    // says (read from its mount options).
    void adoptMounted(const std::string& dev, const std::string& id, Ownership own = Ownership::None);

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
    VolAction mountAction(const Volume& v) const;
    Ownership decided(const std::string& id, Ownership own) const;  // NeedsDecision -> Declined if kept

    // Bookkeeping per device that toJson() does not show.
    struct Extra {
        bool started = false;  // checked or mounted, or on the way (not waiting as a duplicate)
        bool mounted = false;  // mounted at its mount point
        bool dirty = false;    // the filesystem's dirty flag, at the last probe
    };

    PolicyConfig cfg_;
    std::map<std::string, Volume> vols_;  // by kernel name
    std::map<std::string, Extra> extra_;
    struct Adopted {
        std::string id;
        Ownership ownership = Ownership::None;
    };
    std::map<std::string, Adopted> adopted_;  // by dev
    std::set<std::string> keptReadOnly_;      // ids the user declined to let Thimsy take
    int nextJob_ = 1;
    bool changed_ = true;
};

}  // namespace session
}  // namespace thimsy
