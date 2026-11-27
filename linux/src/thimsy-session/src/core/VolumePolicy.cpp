// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
#include "core/VolumePolicy.h"

#include <cctype>

namespace thimsy {
namespace session {

const char kTypeBasicData[] = "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7";
const char kTypeLinuxData[] = "0FC63DAF-8483-4772-8E79-3D69D8477DE4";
const char kTypeLinuxReserved[] = "8DA63339-0007-60C0-C436-083AC8230908";
const char kTypeLinuxHome[] = "933AC7E1-2EB4-4F13-B844-0E14E2AEF915";

namespace {

// A drive's error while its ownership is FsError.
const char kFsErrorText[] = "the filesystem went read-only after an error; check it on a computer";

bool startsWith(const std::string& s, const char* p) { return s.compare(0, std::char_traits<char>::length(p), p) == 0; }

bool allDigits(const std::string& s, size_t from) {
    if (from >= s.size()) return false;
    for (size_t i = from; i < s.size(); ++i) {
        if (!isdigit(static_cast<unsigned char>(s[i]))) return false;
    }
    return true;
}

// mmcblkN followed by bootM or rpmb: an eMMC's hardware partitions.
bool isEmmcHardwarePart(const std::string& name) {
    if (!startsWith(name, "mmcblk")) return false;
    size_t i = 6;
    while (i < name.size() && isdigit(static_cast<unsigned char>(name[i]))) ++i;
    if (i == 6) return false;
    std::string rest = name.substr(i);
    if (rest == "rpmb") return true;
    return startsWith(rest, "boot") && allDigits(rest, 4);
}

// A never-booted Thimsy card's info partition is 1 MiB (OA-33); anything up to 2 MiB named info is
// taken for that stub (W8-SMALL-10).
const uint64_t kInfoStubMaxBytes = 2 * 1024 * 1024;

Classification ignore(std::string why) {
    Classification c;
    c.disposition = Disposition::Ignore;
    c.reason = std::move(why);
    return c;
}

bool isHex4Dash4(const std::string& s) {
    if (s.size() != 9 || s[4] != '-') return false;
    for (size_t i = 0; i < 9; ++i) {
        if (i == 4) continue;
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) return false;
    }
    return true;
}

bool allZeroHex(const std::string& s) {
    bool any = false;
    for (char c : s) {
        if (c == '-') continue;
        if (c != '0') return false;
        any = true;
    }
    return any;
}

// Splits at spaces; empty fields are dropped.
std::vector<std::string> fieldsOf(const std::string& line) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos < line.size()) {
        size_t sp = line.find(' ', pos);
        if (sp == std::string::npos) sp = line.size();
        if (sp > pos) out.push_back(line.substr(pos, sp - pos));
        pos = sp + 1;
    }
    return out;
}

}  // namespace

Classification classify(const BlockDev& dev, const BootDisk& boot, const PolicyConfig& cfg) {
    static const char* const kVirtual[] = {"loop", "ram", "zram", "dm-", "md", "nbd", "mtdblock", "sr", "fd"};
    for (const char* p : kVirtual) {
        if (startsWith(dev.name, p)) return ignore("a virtual device");
    }
    if (isEmmcHardwarePart(dev.name)) return ignore("an eMMC hardware partition");
    if (dev.emmc) return ignore("on internal storage (eMMC)");
    if (!dev.partition && dev.diskHasPartitions) return ignore("a disk with partitions");
    if (dev.sizeBytes < cfg.minSizeBytes) return ignore("under 1 MiB");
    if (dev.partition && dev.partName == "info" && dev.sizeBytes <= kInfoStubMaxBytes) {
        return ignore("a Thimsy card's info stub (named info, 2 MiB or less)");
    }
    if (dev.table == "gpt" && dev.partition) {
        if (dev.partType == kTypeLinuxReserved) return ignore("typed Linux reserved");
        if (dev.partType != kTypeBasicData && dev.partType != kTypeLinuxData) {
            return ignore("type " + (dev.partType.empty() ? std::string("unknown") : dev.partType) +
                          " is not a data type");
        }
    } else if (dev.table == "mbr" && dev.partition) {
        if (dev.partType == "05" || dev.partType == "0f" || dev.partType == "85") return ignore("an extended partition");
        if (dev.partType == "ee") return ignore("a protective MBR entry");
    }
    if (!boot.disk.empty() && dev.disk == boot.disk) {
        if (dev.partition && dev.partName == cfg.gamesName && dev.table == "gpt" && dev.partType == kTypeBasicData) {
            Classification c;
            c.disposition = Disposition::Drive;
            return c;
        }
        return ignore("on the boot disk (only its basic-data '" + cfg.gamesName + "' partition is a drive)");
    }
    if (dev.table.empty() && dev.partition) {
        static const char* const kSystem[] = {"uboot", "boot", "root", "data"};
        for (const char* n : kSystem) {
            if (dev.partName == n) return ignore("named like a system partition, with an unread table");
        }
    }
    Classification c;
    c.disposition = Disposition::Drive;
    return c;
}

std::string volumeIdFor(const FsInfo& fs) {
    if (fs.type == "vfat" || fs.type == "exfat") {
        if (!isHex4Dash4(fs.serial) || fs.serial == "0000-0000") return "";
        return fs.serial;
    }
    if (fs.type == "ext4") {
        if (fs.serial.size() != 36 || allZeroHex(fs.serial)) return "";
        return fs.serial;
    }
    return "";
}

std::string mountOptions(const std::string& fsType, bool readOnly) {
    std::string o;
    if (fsType == "vfat") {
        o = "utf8,shortname=mixed,dirsync,noatime,nodev,nosuid,noexec,flush";
    } else if (fsType == "exfat") {
        o = "dirsync,noatime,nodev,nosuid,noexec";
    } else {
        o = "noatime,nodev,nosuid,noexec";
    }
    return readOnly ? "ro," + o : o;
}

bool fsckNeeded(const FsInfo& fs, bool readOnly) {
    if (readOnly) return false;
    if (fs.type == "vfat") return fs.dirty;
    return fs.type == "exfat" || fs.type == "ext4";
}

bool mountsForNano(const std::string& fsType, bool readOnly) { return fsType == "ext4" && !readOnly; }

const char* toString(VolState s) {
    switch (s) {
        case VolState::Checking: return "checking";
        case VolState::Mounted: return "mounted";
        case VolState::Ejected: return "ejected";
        case VolState::CheckFailed: return "check_failed";
        case VolState::MountFailed: return "mount_failed";
        case VolState::Unsupported: return "unsupported";
        case VolState::NoId: return "no_id";
        case VolState::Duplicate: return "duplicate";
    }
    return "?";
}

const char* toString(Ownership o) {
    switch (o) {
        case Ownership::None: return "";
        case Ownership::Nano: return "nano";
        case Ownership::Idmapped: return "idmapped";
        case Ownership::NeedsDecision: return "needs_decision";
        case Ownership::Declined: return "declined";
        case Ownership::FsError: return "fs_error";
    }
    return "?";
}

bool hasMountOption(const std::string& options, const std::string& opt) {
    size_t pos = 0;
    while (pos <= options.size()) {
        size_t comma = options.find(',', pos);
        if (comma == std::string::npos) comma = options.size();
        if (options.compare(pos, comma - pos, opt) == 0) return true;
        pos = comma + 1;
    }
    return false;
}

Ownership ownershipOfMount(const std::string& fsType, const std::string& mountOptions,
                           const std::string& superOptions) {
    if (fsType != "ext4") return Ownership::None;
    if (hasMountOption(superOptions, "ro")) return Ownership::FsError;
    if (hasMountOption(mountOptions, "idmapped")) return Ownership::Idmapped;
    return hasMountOption(mountOptions, "ro") ? Ownership::NeedsDecision : Ownership::Nano;
}

std::vector<MountInfo> parseMountInfo(const std::string& text) {
    std::vector<MountInfo> out;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::vector<std::string> f = fieldsOf(text.substr(pos, nl - pos));
        pos = nl + 1;
        // id parent major:minor root mount-point options [optional fields] - type source super-options
        size_t dash = 6;
        while (dash < f.size() && f[dash] != "-") ++dash;
        if (dash + 3 >= f.size()) continue;
        MountInfo m;
        m.mountPoint = f[4];
        m.mountOptions = f[5];
        m.fsType = f[dash + 1];
        m.source = f[dash + 2];
        m.superOptions = f[dash + 3];
        out.push_back(m);
    }
    return out;
}

bool superblockReadOnlyAt(const std::vector<MountInfo>& mounts, const std::string& mountPoint) {
    const MountInfo* top = nullptr;
    for (const MountInfo& m : mounts) {
        if (m.mountPoint == mountPoint) top = &m;
    }
    return top && hasMountOption(top->superOptions, "ro");
}

std::set<std::string> parseKeptReadOnly(const std::string& text) {
    std::set<std::string> ids;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        const std::string id = text.substr(pos, nl - pos);
        pos = nl + 1;
        bool ok = !id.empty() && id.size() <= 64;
        for (char c : id) {
            ok = ok && ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '-');
        }
        if (ok) ids.insert(id);
    }
    return ids;
}

std::string keptReadOnlyText(const std::set<std::string>& ids) {
    std::string text;
    for (const std::string& id : ids) text += id + "\n";
    return text;
}

VolumeManager::VolumeManager(PolicyConfig cfg) : cfg_(std::move(cfg)) {}

std::string VolumeManager::mountPointFor(const std::string& id) const { return cfg_.storageDir + "/" + id; }

VolAction VolumeManager::mountAction(const Volume& v) const {
    VolAction a(VolActKind::Mount);
    a.dev = v.dev;
    a.fs = v.fs;
    a.id = v.id;
    a.mountPoint = mountPointFor(v.id);
    a.options = mountOptions(v.fs, v.readOnly);
    a.forNano = cfg_.nanoRunsAsNano && mountsForNano(v.fs, v.readOnly);
    return a;
}

Ownership VolumeManager::decided(const std::string& id, Ownership own) const {
    return own == Ownership::NeedsDecision && keptReadOnly_.count(id) ? Ownership::Declined : own;
}

const Volume* VolumeManager::byId(const std::string& id) const {
    for (const auto& kv : vols_) {
        if (kv.second.id == id) return &kv.second;
    }
    return nullptr;
}

bool VolumeManager::takeChanged() {
    bool c = changed_;
    changed_ = false;
    return c;
}

std::vector<VolAction> VolumeManager::start(Volume* v) {
    Extra& x = extra_[v->dev];
    std::vector<VolAction> out;
    x.started = true;
    v->error.clear();
    changed_ = true;
    if (x.mounted) {
        v->state = VolState::Mounted;
        if (v->ownership == Ownership::FsError) v->error = kFsErrorText;
        return out;
    }
    v->state = VolState::Checking;
    v->ownership = Ownership::None;
    FsInfo fs;
    fs.type = v->fs;
    fs.dirty = x.dirty;
    if (fsckNeeded(fs, v->readOnly)) {
        v->fsckJob = nextJob_++;
        VolAction a(VolActKind::Fsck);
        a.dev = v->dev;
        a.fs = v->fs;
        a.id = v->id;
        a.job = v->fsckJob;
        out.push_back(a);
        return out;
    }
    out.push_back(mountAction(*v));
    return out;
}

std::vector<VolAction> VolumeManager::release(Volume* v, bool lazy) {
    Extra& x = extra_[v->dev];
    std::vector<VolAction> out;
    if (v->fsckJob) {
        VolAction a(VolActKind::KillFsck);
        a.dev = v->dev;
        a.job = v->fsckJob;
        out.push_back(a);
        v->fsckJob = 0;
    }
    if (x.mounted) {
        VolAction a(VolActKind::Unmount);
        a.dev = v->dev;
        a.id = v->id;
        a.mountPoint = mountPointFor(v->id);
        a.lazy = lazy;
        out.push_back(a);
        if (lazy) x.mounted = false;
    }
    x.started = false;
    return out;
}

std::vector<VolAction> VolumeManager::resolveDuplicates() {
    std::vector<VolAction> out;
    std::map<std::string, std::vector<Volume*>> byId;
    for (auto& kv : vols_) {
        Volume& v = kv.second;
        if (v.id.empty() || v.state == VolState::Unsupported || v.state == VolState::NoId) continue;
        byId[v.id].push_back(&v);
    }
    for (auto& kv : byId) {
        std::vector<Volume*>& group = kv.second;
        if (group.size() > 1) {
            for (Volume* v : group) {
                if (v->state == VolState::Duplicate) continue;
                std::vector<VolAction> r = release(v, false);
                out.insert(out.end(), r.begin(), r.end());
                v->state = VolState::Duplicate;
                v->error = "another drive has the same id";
                changed_ = true;
            }
            continue;
        }
        Volume* v = group.front();
        if (v->state == VolState::Duplicate || !extra_[v->dev].started) {
            std::vector<VolAction> s = start(v);
            out.insert(out.end(), s.begin(), s.end());
        }
    }
    return out;
}

std::vector<VolAction> VolumeManager::onDevice(const BlockDev& dev, const BootDisk& boot) {
    std::vector<VolAction> out;
    Classification c = classify(dev, boot, cfg_);
    auto it = vols_.find(dev.name);
    if (c.disposition == Disposition::Ignore) {
        if (it != vols_.end()) {
            out = release(&it->second, true);
            extra_.erase(dev.name);
            vols_.erase(it);
            changed_ = true;
            std::vector<VolAction> d = resolveDuplicates();
            out.insert(out.end(), d.begin(), d.end());
        }
        return out;
    }

    Volume nv;
    nv.dev = dev.name;
    nv.disk = dev.disk;
    nv.partn = dev.partn;
    nv.fs = dev.fs.type;
    nv.label = dev.fs.label;
    nv.kind = dev.kind;
    nv.sizeBytes = dev.sizeBytes;
    nv.bootDisk = !boot.disk.empty() && dev.disk == boot.disk;
    nv.readOnly = dev.readOnly;
    nv.id = volumeIdFor(dev.fs);
    if (dev.fs.type.empty()) {
        nv.state = VolState::Unsupported;
        nv.error = "no FAT, exFAT or ext4 filesystem";
    } else if (nv.id.empty()) {
        nv.state = VolState::NoId;
        nv.error = "the filesystem has no usable serial";
    }

    if (it != vols_.end()) {
        Volume& old = it->second;
        if (!nv.id.empty() && old.id == nv.id && old.fs == nv.fs) {
            if (old.label != nv.label || old.sizeBytes != nv.sizeBytes) {
                old.label = nv.label;
                old.sizeBytes = nv.sizeBytes;
                changed_ = true;
            }
            return out;  // the same drive again (D10cards' coldplug, a change event)
        }
        out = release(&old, true);
        extra_.erase(dev.name);
        vols_.erase(it);
    }

    Extra& x = extra_[dev.name];
    x = Extra();
    x.dirty = dev.fs.dirty;
    auto ad = adopted_.find(dev.name);
    if (ad != adopted_.end()) {
        if (ad->second.id == nv.id) {
            x.mounted = true;
            // A read-only device's drive is never mounted for Nano, so its "ro" decides nothing.
            nv.ownership = cfg_.nanoRunsAsNano && mountsForNano(nv.fs, nv.readOnly)
                               ? decided(nv.id, ad->second.ownership)
                               : Ownership::None;
        }
        adopted_.erase(ad);
    }
    vols_[dev.name] = nv;
    changed_ = true;
    std::vector<VolAction> d = resolveDuplicates();
    out.insert(out.end(), d.begin(), d.end());
    return out;
}

std::vector<VolAction> VolumeManager::onRemoved(const std::string& dev) {
    std::vector<VolAction> out;
    adopted_.erase(dev);
    auto it = vols_.find(dev);
    if (it == vols_.end()) return out;
    out = release(&it->second, true);
    extra_.erase(dev);
    vols_.erase(it);
    changed_ = true;
    std::vector<VolAction> d = resolveDuplicates();
    out.insert(out.end(), d.begin(), d.end());
    return out;
}

std::vector<VolAction> VolumeManager::onFsckDone(int job, int exitCode) {
    std::vector<VolAction> out;
    if (job <= 0) return out;
    for (auto& kv : vols_) {
        Volume& v = kv.second;
        if (v.fsckJob != job) continue;
        v.fsckJob = 0;
        if (v.state != VolState::Checking) return out;
        int okMax = v.fs == "ext4" ? 3 : 1;  // e2fsck 2: fixed, reboot advised (only matters for root)
        if (exitCode < 0 || exitCode > okMax) {
            v.state = VolState::CheckFailed;
            v.error = exitCode < 0 ? "the check was killed" : "the check failed (exit " + std::to_string(exitCode) + ")";
            changed_ = true;
            return out;
        }
        out.push_back(mountAction(v));
        return out;
    }
    return out;  // a check whose drive went away meanwhile
}

std::vector<VolAction> VolumeManager::onMountDone(const std::string& dev, bool ok, const std::string& err,
                                                  Ownership own) {
    auto it = vols_.find(dev);
    if (it == vols_.end()) return {};
    Volume& v = it->second;
    if (v.state != VolState::Checking) return {};
    if (ok) {
        extra_[dev].mounted = true;
        v.state = VolState::Mounted;
        v.ownership = decided(v.id, own);
        v.error = own == Ownership::FsError ? kFsErrorText : "";
    } else {
        v.state = VolState::MountFailed;
        v.ownership = Ownership::None;
        v.error = err.empty() ? "mount failed" : err;
    }
    changed_ = true;
    return {};
}

std::vector<VolAction> VolumeManager::onUnmountDone(const std::string& dev, bool ok, const std::string& err) {
    auto it = vols_.find(dev);
    if (it == vols_.end()) return {};
    Volume& v = it->second;
    if (ok) {
        extra_[dev].mounted = false;
        if (v.state == VolState::Mounted) v.state = VolState::Ejected;
        v.ownership = Ownership::None;
        v.error.clear();
    } else {
        v.error = "still mounted: " + (err.empty() ? std::string("busy") : err);
    }
    changed_ = true;
    return {};
}

bool VolumeManager::eject(const std::string& id, std::vector<VolAction>* out, std::string* err) {
    for (auto& kv : vols_) {
        Volume& v = kv.second;
        if (v.id != id || v.state != VolState::Mounted) continue;
        VolAction a(VolActKind::Unmount);
        a.dev = v.dev;
        a.id = v.id;
        a.mountPoint = mountPointFor(v.id);
        out->push_back(a);
        return true;
    }
    if (err) *err = "no mounted drive " + id;
    return false;
}

bool VolumeManager::takeOwnership(const std::string& id, std::vector<VolAction>* out, std::string* err) {
    for (auto& kv : vols_) {
        Volume& v = kv.second;
        if (v.id != id || v.state != VolState::Mounted) continue;
        if (v.ownership == Ownership::FsError) {
            if (err) *err = "drive " + id + "'s filesystem went read-only after an error";
            return false;
        }
        if (v.ownership != Ownership::NeedsDecision && v.ownership != Ownership::Declined) {
            if (err) *err = "drive " + id + " has no ownership decision pending";
            return false;
        }
        VolAction a(VolActKind::TakeOwnership);
        a.dev = v.dev;
        a.fs = v.fs;
        a.id = v.id;
        a.mountPoint = mountPointFor(v.id);
        a.options = mountOptions(v.fs, false);
        a.forNano = true;
        out->push_back(a);
        return true;
    }
    if (err) *err = "no mounted drive " + id;
    return false;
}

std::vector<VolAction> VolumeManager::onTakeOwnershipDone(const std::string& dev, bool ok, bool stillMounted,
                                                          const std::string& err, bool fsError) {
    auto it = vols_.find(dev);
    if (it == vols_.end()) return {};
    Volume& v = it->second;
    if (v.state != VolState::Mounted) return {};
    if (ok) {
        v.ownership = Ownership::Nano;
        v.error.clear();
        keptReadOnly_.erase(v.id);
    } else if (!stillMounted) {
        v.error = "taking ownership failed: " + (err.empty() ? std::string("unknown error") : err);
        extra_[dev].mounted = false;
        v.state = VolState::MountFailed;
        v.ownership = Ownership::None;
    } else if (fsError) {
        v.ownership = Ownership::FsError;
        v.error = kFsErrorText;
    } else {
        v.error = "taking ownership failed: " + (err.empty() ? std::string("unknown error") : err);
    }
    changed_ = true;
    return {};
}

bool VolumeManager::keepReadOnly(const std::string& id, std::string* err) {
    for (auto& kv : vols_) {
        Volume& v = kv.second;
        if (v.id != id || v.state != VolState::Mounted) continue;
        if (v.ownership != Ownership::NeedsDecision && v.ownership != Ownership::Declined) {
            if (err) *err = "drive " + id + " has no ownership decision pending";
            return false;
        }
        keptReadOnly_.insert(id);
        if (v.ownership != Ownership::Declined) {
            v.ownership = Ownership::Declined;
            changed_ = true;
        }
        return true;
    }
    if (err) *err = "no mounted drive " + id;
    return false;
}

void VolumeManager::setKeptReadOnly(std::set<std::string> ids) { keptReadOnly_ = std::move(ids); }

void VolumeManager::adoptMounted(const std::string& dev, const std::string& id, Ownership own) {
    Adopted a;
    a.id = id;
    a.ownership = own;
    adopted_[dev] = a;
}

json::Value VolumeManager::toJson() const {
    json::Value root = json::Value::object();
    root.set("version", 1);
    json::Value list = json::Value::array();
    for (const auto& kv : vols_) {
        const Volume& v = kv.second;
        json::Value o = json::Value::object();
        o.set("id", v.id);
        o.set("state", toString(v.state));
        o.set("path", v.id.empty() ? std::string() : mountPointFor(v.id));
        o.set("media_rw", v.id.empty() ? std::string() : cfg_.mediaRwDir + "/" + v.id);
        o.set("dev", "/dev/" + v.dev);
        o.set("disk", v.disk);
        o.set("partition", v.partn);
        o.set("fs", v.fs);
        o.set("label", v.label);
        o.set("kind", v.kind);
        o.set("boot_disk", v.bootDisk);
        o.set("read_only", v.readOnly);
        o.set("size_bytes", static_cast<int64_t>(v.sizeBytes));
        if (v.ownership != Ownership::None) o.set("ownership", toString(v.ownership));
        if (!v.error.empty()) o.set("error", v.error);
        list.push(o);
    }
    root.set("volumes", list);
    return root;
}

}  // namespace session
}  // namespace thimsy
