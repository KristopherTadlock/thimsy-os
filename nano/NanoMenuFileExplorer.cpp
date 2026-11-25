// File Explorer (Settings > File Explorer).
//
// A controller-first file manager for GammaOS Nano. It reuses the existing folder-picker
// navigation idiom (opendir/readdir, storage roots, ".." up, rebuild-the-top-level in place)
// from NanoMenuPS3Folder.cpp's buildFolderBrowser, but lists files AND folders. The per-item
// actions (Copy / Move / Delete / Rename / Information) are surfaced through the SAME XMB
// X/Triangle side menu used everywhere else (openXmbOpt -> xmbOptAction routes "fe*" here),
// so the look and feel is 1:1 with the rest of the menu.
//
// Copy / Move / Delete run on a detached worker that touches only a heap result block + value-
// captured paths, never `this` - so a teardown mid-op cannot use-after-free, and a large copy
// never blocks the render thread (the watchdog hazard, see nano_render_thread_blocking).
//
// Zip and 7z archives open like read-only folders (NanoArchive does the reading). Extract and
// Compress to Zip run on the same kind of worker, with a progress dialog whose Cancel / Back
// stops them.

#include "NanoMenu.h"
#include "NanoI18n.h"      // trDyn() runtime translation of hardcoded UI strings
#include "NanoAppFiles.h"  // sdCanon
#include "NanoFileJob.h"

#include <stdarg.h>
#include <limits.h>
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <time.h>
#include <algorithm>
#include <vector>
#include <string>
#include <map>
#include <set>
#include <memory>
#include <thread>
#include <utils/Log.h>

namespace android {

// ---- path helpers ---------------------------------------------------------
static std::string feBaseName(const std::string& p) {
    size_t sl = p.find_last_of('/');
    return (sl == std::string::npos) ? p : p.substr(sl + 1);
}
static std::string feParentDir(const std::string& p) {
    size_t sl = p.find_last_of('/');
    if (sl == std::string::npos) return "";
    if (sl == 0) return "/";
    return p.substr(0, sl);
}
// Join a directory and a child name without doubling the slash at root ("/" + "x" -> "/x").
static std::string feJoin(const std::string& dir, const std::string& name) {
    return (dir == "/" ? std::string("/") : dir + "/") + name;
}
static std::string feHumanSize(long long bytes) {
    char b[40];
    double v = (double)bytes;
    if (v >= 1024.0 * 1024.0 * 1024.0) snprintf(b, sizeof(b), "%.2f GB", v / (1024.0 * 1024.0 * 1024.0));
    else if (v >= 1024.0 * 1024.0)     snprintf(b, sizeof(b), "%.1f MB", v / (1024.0 * 1024.0));
    else if (v >= 1024.0)              snprintf(b, sizeof(b), "%.1f KB", v / 1024.0);
    else                               snprintf(b, sizeof(b), "%lld B", bytes);
    return b;
}
// Archive sizes come from untrusted headers and may not fit a long long.
static std::string feSize(uint64_t bytes) {
    return feHumanSize((long long)std::min<uint64_t>(bytes, (uint64_t)LLONG_MAX));
}
static std::string feFmt(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static std::string feFmt(const char* fmt, ...) {
    char b[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    return b;
}

// ---- archives -------------------------------------------------------------
enum { FE_OP_EXTRACT = 4, FE_OP_COMPRESS = 5 };   // FeOp kinds after copy (1) / move (2) / delete (3)

// p is root or inside it ("" contains nothing).
static bool feUnder(const std::string& p, const std::string& root) {
    return !root.empty() && (p == root || (p.size() > root.size() && p.compare(0, root.size(), root) == 0
                                           && p[root.size()] == '/'));
}
// The name without its extension: where Extract Here puts an archive's contents.
static std::string feStem(const std::string& name) {
    size_t dot = name.find_last_of('.');
    return (dot == std::string::npos || dot == 0) ? name : name.substr(0, dot);
}
static std::string feArcErrorText(nanoarc::Err e) {
    switch (e) {
        case nanoarc::Err::Rar:         return trDyn("RAR archives aren't supported.");
        case nanoarc::Err::Unsupported: return trDyn("This archive is encrypted or uses a method that isn't supported.");
        case nanoarc::Err::Open:        return trDyn("The archive could not be opened.");
        case nanoarc::Err::NameEncoding: return trDyn("This archive's file names use an encoding that isn't supported.");
        default:                        return trDyn("The archive is damaged and could not be read.");
    }
}
static std::string feProgressText(const std::string& name, uint64_t done, uint64_t total) {
    if (total == 0) return name + "\n" + trDyn("Preparing...");
    const int pct = (int)std::min(100.0, (double)done * 100.0 / (double)total);
    return name + "\n" + feFmt(trDyn("%s of %s (%d%%)"), feSize(done).c_str(),
                               feSize(total).c_str(), pct);
}
static std::string feExtractMessage(const nanoarc::ExtractResult& r) {
    using nanoarc::Err;
    std::string msg;
    switch (r.err) {
        case Err::None:      msg = feFmt(trDyn("Extracted to %s."), r.dest.c_str()); break;
        case Err::Cancelled: msg = trDyn("Extraction was cancelled."); break;
        case Err::NoSpace:   msg = trDyn("The destination ran out of space. The incomplete file was deleted."); break;
        case Err::Write:     msg = feFmt(trDyn("Could not write %s"), r.detail.c_str()); break;
        case Err::NoSpaceCheck:
            return feFmt(trDyn("Not enough space: %s needed, %s free. Nothing was extracted."),
                         feSize(r.needBytes).c_str(), feSize(r.freeBytes).c_str());
        case Err::BlockTooBig:
            return feFmt(trDyn("This 7z archive packs %s into one solid block, more than the %s that can be unpacked safely. Nothing was extracted."),
                         feSize(r.needBytes).c_str(), feSize(nanoarc::k7zBlockCap).c_str());
        case Err::Encrypted:  return trDyn("This archive is password-protected, which isn't supported.");
        case Err::Method:     return trDyn("This archive uses a compression method that isn't supported.");
        case Err::NoMemory:   msg = trDyn("This 7z archive needs more memory to unpack than can be used safely."); break;
        case Err::NoFreeName: return trDyn("Could not find a free name in this folder.");
        default:              msg = feArcErrorText(r.err); break;
    }
    auto line = [&](const char* fmt, int n) { if (n > 0) { msg += "\n"; msg += feFmt(fmt, n); } };
    if (r.err == Err::None || r.extracted > 0) msg += "\n" + feFmt(trDyn("Files extracted: %d"), r.extracted);
    line(trDyn("Existing files skipped: %d"), r.existing);
    line(trDyn("Unsafe names skipped: %d"), r.unsafe);
    line(trDyn("Symbolic links skipped: %d"), r.links);
    line(trDyn("Encrypted files skipped: %d"), r.encrypted);
    line(trDyn("Files with an unsupported compression method skipped: %d"), r.unsupported);
    line(trDyn("Names this drive can't store skipped: %d"), r.badNames);
    if (r.rolledBack) msg += std::string("\n") + trDyn("The new folder and everything extracted into it were removed.");
    return msg;
}
static std::string feCompressMessage(const nanoarc::CompressResult& r) {
    using nanoarc::Err;
    switch (r.err) {
        case Err::None: {
            std::string msg = feFmt(trDyn("Created %s."), feBaseName(r.zipPath).c_str());
            if (r.links > 0) msg += "\n" + feFmt(trDyn("Symbolic links skipped: %d"), r.links);
            return msg;
        }
        case Err::TmpfsDest:  return trDyn("Can't create a zip here. Open the folder and compress what's inside instead.");
        case Err::Link:       return trDyn("Links can't be compressed. Compress the file or folder it points to instead.");
        case Err::TooBig:     return trDyn("Too big for a zip: Compress to Zip is limited to 4 GB and 65,535 items. No zip was created.");
        case Err::BadName:    return feFmt(trDyn("%s has a name a zip can't store. No zip was created."), r.detail.c_str());
        case Err::Read:       return feFmt(trDyn("Could not read %s. No zip was created."), r.detail.c_str());
        case Err::NoFreeName: return trDyn("Could not find a free name in this folder.");
        case Err::Cancelled:  return trDyn("Compression was cancelled. No zip was created.");
        case Err::NoSpace:    return trDyn("The destination ran out of space. No zip was created.");
        default:              return feFmt(trDyn("Could not write the zip: %s"), r.detail.c_str());
    }
}

// ---- filesystem ops (worker-thread / non-render only) ---------------------
// Copy, Move and Delete are nanofilejob's treeCopy / treeMove / treeRemove (NanoFileJob.cpp): every
// write checked and flushed before a Move deletes anything, and nothing deleted through a link.
// If `dst` already exists, append " copy" / " copy N" (before the extension) so a paste into the
// same directory never clobbers the source or an existing file.
static std::string feUniqueDest(const std::string& dst) {
    struct stat st;   // lstat: a dangling link holds its name too
    if (lstat(dst.c_str(), &st) != 0) return dst;
    std::string dir = feParentDir(dst), base = feBaseName(dst), stem = base, ext;
    size_t dot = base.find_last_of('.');
    if (dot != std::string::npos && dot != 0) { stem = base.substr(0, dot); ext = base.substr(dot); }
    for (int i = 1; i < 1000; i++) {
        char suf[24];
        if (i == 1) snprintf(suf, sizeof(suf), " copy");
        else        snprintf(suf, sizeof(suf), " copy %d", i);
        std::string cand = dir + "/" + stem + suf + ext;
        if (lstat(cand.c_str(), &st) != 0) return cand;
    }
    return std::string();   // exhausted: caller must NOT proceed (never clobber the original dst)
}

// ---- browser screen -------------------------------------------------------
void NanoMenu::buildFileBrowser(const std::string& path, Ps3Level& out) {
    out.items.clear(); out.sel = 0; out.screenKind = FE_BROWSE;
    // Real-filesystem browser rooted at "/". Open defaults to /storage; ".." climbs the real tree up
    // to "/" (so the user can reach /data etc.), and "/" is the top (no "..", Back exits). nano is
    // root, so any directory is browsable/writable - the delete confirm is the safety net.
    std::string cur = path.empty() ? std::string("/storage") : path;
    mFeBrowsePath = cur;
    // Anywhere outside the archive's own subtree (".." from its root, Back, a jump) leaves it.
    if (!mFeArcPath.empty() && !feUnder(cur, mFeArcPath)) {
        mFeArcPath.clear(); mFeArcIndex.reset(); mFeArcLoad.reset();
    }
    if (!mFeArcPath.empty()) { feBuildArchiveListing(cur, out); return; }
    GLuint folderNmap = nmapForIcon(62);   // folder glyph
    GLuint fileNmap   = nmapForIcon(25);   // document/page glyph
    out.title = cur;
    // base avoids a doubled slash when cur == "/" (child = "/foo", not "//foo").
    std::string base = (cur == "/") ? std::string() : cur;

    // ".." up one real level, except at "/" (the top: there is nowhere higher, Back exits there).
    if (cur != "/") {
        Ps3Item it; it.label = ".."; it.kind = PS3_FE_DIR; it.payloadStr = feParentDir(cur);
        it.iconTex = iconTexForIcon(62); it.nmapTex = folderNmap; it.iconR = it.iconG = it.iconB = 1.0f;
        out.items.push_back(it);
    }

    // On /storage, leave the shares out of the plain listing: they are bind-mounted there so apps
    // can open them by path, so they really are in this directory, but the block below adds them
    // again with a "Share:" label that says what they are. Without this each one appears twice, and
    // the unlabelled copy is indistinguishable from a memory card.
    const std::vector<std::string> shareNames =
        (cur == "/storage") ? mountedShareNames() : std::vector<std::string>();

    // The listing comes from the worker (see fbRequestListing), not from an inline opendir + a stat
    // per entry. This browser exists to move files between local storage and a share, so it is
    // pointed at a NAS by design - and doing that synchronously meant one round trip per entry with
    // the render thread held, which nano's watchdog turns into an abort at 8s.
    std::vector<std::string> dirs, files;
    std::map<std::string, long long> sizes;
    bool listingReady = (mFbCacheValid && mFbCachePath == cur);
    if (listingReady) {
        for (const auto& e : mFbCacheEntries) {
            if (std::find(shareNames.begin(), shareNames.end(), e.name) != shareNames.end())
                continue;   // shown below with a "Share:" label instead
            if (e.isDir) dirs.push_back(e.name);
            else { files.push_back(e.name); sizes[e.name] = e.size; }
        }
    } else {
        fbRequestListing(cur);
    }

    for (const auto& n : dirs) {
        Ps3Item it; it.label = n; it.kind = PS3_FE_DIR; it.payloadStr = base + "/" + n;
        it.iconTex = iconTexForIcon(62); it.nmapTex = folderNmap; it.iconR = it.iconG = it.iconB = 1.0f;
        out.items.push_back(it);
    }
    for (const auto& n : files) {
        Ps3Item it; it.label = n; it.kind = PS3_FE_FILE; it.payloadStr = base + "/" + n;
        // An archive is a PS3_FE_DIR row so A / a tap opens it like a folder (feNavigate); .a tells
        // feNavigate and the side menu what it is.
        const nanoarc::Format fmt = nanoarc::formatFromName(n);
        if (fmt != nanoarc::Format::None) {
            it.kind = PS3_FE_DIR;
            it.a = (fmt == nanoarc::Format::Rar) ? FE_ROW_RAR : FE_ROW_ARCHIVE;
        }
        // Size comes from the worker's listing. Re-stat-ing here would put back exactly the
        // per-entry round trip the worker exists to avoid.
        auto sit = sizes.find(n);
        if (sit != sizes.end()) it.value = feHumanSize(sit->second);
        it.iconTex = iconTexForIcon(25); it.nmapTex = fileNmap; it.iconR = it.iconG = it.iconB = 1.0f;
        out.items.push_back(it);
    }
    if (!listingReady) {
        Ps3Item it; it.kind = PS3_FE_FILE; it.payloadStr = "";   // inert: dispatch guards on a path
        it.label = trDyn("Loading...");
        it.iconTex = 0; it.nmapTex = 0; it.iconR = it.iconG = it.iconB = 0.55f;
        out.items.push_back(it);
    }
    // Shortcut the mounted network shares onto the opening screen. They live at /mnt/shares and are
    // reachable by climbing to "/" like anything else, but the whole point of the explorer here is
    // copying between local and remote storage, so both ends should be one screen apart. Only on
    // /storage: elsewhere the listing stays a faithful view of the directory.
    if (cur == "/storage") {
        for (const std::string& sn : shareNames) {
            char shareLbl[256];
            snprintf(shareLbl, sizeof(shareLbl), trDyn("Share: %s"), sn.c_str());
            Ps3Item it; it.label = shareLbl;
            it.kind = PS3_FE_DIR; it.payloadStr = "/mnt/shares/" + sn;
            it.iconTex = iconTexForIcon(62); it.nmapTex = folderNmap; it.iconR = it.iconG = it.iconB = 1.0f;
            out.items.push_back(it);
        }
    }
    if (dirs.empty() && files.empty() && out.items.size() <= 1) {
        // Inert placeholder so an empty directory is not a blank screen. PS3_FE_FILE with an empty
        // payload is a no-op on Cross (the dispatch guards on a non-empty path) - do NOT use
        // PS3_GS_FIELD here, whose Cross handler calls gsEditField() (wrong context).
        Ps3Item it; it.label = "(empty folder)"; it.kind = PS3_FE_FILE; it.payloadStr = "";
        it.iconTex = 0; it.nmapTex = 0; it.iconR = it.iconG = it.iconB = 0.55f;
        out.items.push_back(it);
    }
    // Show the current directory as every row's subtitle, so the path is always visible.
    for (auto& it : out.items) it.desc = cur;
}

// One directory of the archive being browsed. cur is "<archive>/<dir inside>"; the entries list
// every path in full, so a folder that only appears as part of a file's path is still a folder.
void NanoMenu::feBuildArchiveListing(const std::string& cur, Ps3Level& out) {
    out.title = cur;
    GLuint folderNmap = nmapForIcon(62), fileNmap = nmapForIcon(25);
    {
        // ".." from the archive's root is its real folder, which leaves the archive.
        Ps3Item it; it.label = ".."; it.kind = PS3_FE_DIR; it.payloadStr = feParentDir(cur);
        it.iconTex = iconTexForIcon(62); it.nmapTex = folderNmap; it.iconR = it.iconG = it.iconB = 1.0f;
        out.items.push_back(it);
    }
    if (!mFeArcIndex) {
        Ps3Item it; it.kind = PS3_FE_FILE; it.payloadStr = "";   // inert, as in buildFileBrowser
        it.label = trDyn("Loading...");
        it.iconTex = 0; it.nmapTex = 0; it.iconR = it.iconG = it.iconB = 0.55f;
        out.items.push_back(it);
    } else {
        const std::string prefix = (cur == mFeArcPath) ? std::string() : cur.substr(mFeArcPath.size() + 1) + "/";
        std::set<std::string> dirSet;
        std::vector<std::pair<std::string, uint64_t>> files;
        for (const nanoarc::Entry& e : mFeArcIndex->entries) {
            if (e.path.size() <= prefix.size() || e.path.compare(0, prefix.size(), prefix) != 0) continue;
            const std::string rest = e.path.substr(prefix.size());
            const size_t sl = rest.find('/');
            if (sl != std::string::npos) dirSet.insert(rest.substr(0, sl));
            else if (e.isDir) dirSet.insert(rest);
            else files.emplace_back(rest, e.size);
        }
        std::vector<std::string> dirs(dirSet.begin(), dirSet.end());
        auto byName = [](const std::string& a, const std::string& b) { return strcasecmp(a.c_str(), b.c_str()) < 0; };
        std::sort(dirs.begin(), dirs.end(), byName);
        std::sort(files.begin(), files.end(), [&](const auto& a, const auto& b) { return byName(a.first, b.first); });
        for (const auto& n : dirs) {
            Ps3Item it; it.label = n; it.kind = PS3_FE_DIR; it.payloadStr = cur + "/" + n;
            it.iconTex = iconTexForIcon(62); it.nmapTex = folderNmap; it.iconR = it.iconG = it.iconB = 1.0f;
            out.items.push_back(it);
        }
        for (const auto& f : files) {
            Ps3Item it; it.label = f.first; it.kind = PS3_FE_FILE; it.payloadStr = cur + "/" + f.first;
            it.value = feSize(f.second);
            it.iconTex = iconTexForIcon(25); it.nmapTex = fileNmap; it.iconR = it.iconG = it.iconB = 1.0f;
            out.items.push_back(it);
        }
        if (dirs.empty() && files.empty()) {
            Ps3Item it; it.label = "(empty folder)"; it.kind = PS3_FE_FILE; it.payloadStr = "";
            it.iconTex = 0; it.nmapTex = 0; it.iconR = it.iconG = it.iconB = 0.55f;
            out.items.push_back(it);
        }
    }
    for (auto& it : out.items) it.desc = cur;
}

// The rows for storage roots and shares live in tmpfs folders, where compressToZip refuses to
// write; offering Compress there would only lead to that refusal.
bool NanoMenu::feCanCompress(const std::string& path) {
    const std::string parent = feParentDir(path);
    return !path.empty() && parent != "/storage" && parent != "/mnt/shares" && parent != "/mnt/media_rw"
           && parent != "/mnt";
}

void NanoMenu::feEnterArchive(const std::string& path) {
    if (nanoarc::formatFromName(path) == nanoarc::Format::Rar) {
        feInfoDialog(feBaseName(path), trDyn("RAR archives aren't supported."));
        return;
    }
    // Read afresh on every way in: the archive may have changed since it was last open. Reading
    // the index of an archive on a network share is many round trips, so it is a worker's job.
    mFeArcPath = path;
    mFeArcIndex.reset();
    auto job = std::make_shared<FeArcLoad>();
    job->path = path;
    mFeArcLoad = job;
    std::thread([job]() {
        job->index = nanoarc::readIndex(job->path);
        job->done.store(true, std::memory_order_release);
        FrameFlag::wake();   // feArcLoadTick adopts it on the next frame
    }).detach();
    buildFileBrowser(path, mPs3Stack.back());
}

void NanoMenu::feArcLoadTick() {
    if (!mFeArcLoad || !mFeArcLoad->done.load(std::memory_order_acquire)) return;
    const bool failed = mFeArcLoad->index.err != nanoarc::Err::None;
    if (failed && (mPs3DlgActive || mOskActive)) return;   // say why once the user's modal is gone
    std::shared_ptr<FeArcLoad> job = std::move(mFeArcLoad);
    if (job->path != mFeArcPath) return;
    const bool showing = ps3TopScreenKind() == FE_BROWSE && feUnder(mFeBrowsePath, mFeArcPath);
    if (failed) {
        mFeArcPath.clear();
        if (!showing) return;
        buildFileBrowser(feParentDir(job->path), mPs3Stack.back());
        feInfoDialog(feBaseName(job->path), feArcErrorText(job->index.err));
        return;
    }
    mFeArcIndex = std::make_shared<const nanoarc::Index>(std::move(job->index));
    if (!showing) return;
    int keep = mPs3Stack.back().sel;
    buildFileBrowser(mFeBrowsePath, mPs3Stack.back());
    int n = (int)mPs3Stack.back().items.size();
    mPs3Stack.back().sel = std::max(0, std::min(keep, n - 1));
    mDisplayDirty = true;
}

// Public wrapper so the media option menus (video/photo/music Delete) can perform a real
// delete through the exact recursive remove the file manager uses - handles a single file or
// a directory tree, on the FUSE /storage mount or a raw /data/media path (nano runs as root).
bool NanoMenu::nanoRemovePath(const std::string& p) {
    if (p.empty()) return false;
    return nanofilejob::treeRemove(p);
}

void NanoMenu::feOpen() {
    // Open at /storage, reusing the standard submenu slide-in animation. ".." from there climbs to "/".
    std::vector<Ps3Item> ps = ps3CurItems(); int pSel = ps3CurSel();
    Ps3Level lvl; buildFileBrowser("/storage", lvl); mPs3Stack.push_back(lvl);
    mPs3SubParentItems = ps; mPs3SubParentIdx = pSel; mPs3SubChildItems = mPs3Stack.back().items;
    mPs3SubDir = 1; mPs3SubAnimStart = mEffectTime; mPs3SubAnim = 0.0f;
    mPs3AnimItem = 0.0f; mPs3ItemAnimStart = -1.0f;
}

void NanoMenu::feNavigate(const std::string& path) {
    if (mPs3Stack.empty() || mPs3Stack.back().screenKind != FE_BROWSE) return;
    // The row says whether this is an archive; looking at the file instead would be a stat on the
    // render thread, which on a network share is a round trip.
    if (!feUnder(path, mFeArcPath)) {
        for (const Ps3Item& it : mPs3Stack.back().items) {
            if (it.payloadStr == path && it.kind == PS3_FE_DIR && (it.a == FE_ROW_ARCHIVE || it.a == FE_ROW_RAR)) {
                feEnterArchive(path);
                return;
            }
        }
    }
    buildFileBrowser(path, mPs3Stack.back());   // rebuild the top level in place (no push)
}

void NanoMenu::feRefresh() {
    if (mPs3Stack.empty() || mPs3Stack.back().screenKind != FE_BROWSE) return;
    // The cached listing predates whatever just changed the folder; drop it so the rebuild reads
    // the folder again instead of showing the old contents.
    if (!feInArchive() && mFbCachePath == mFeBrowsePath) mFbCacheValid = false;
    int keep = mPs3Stack.back().sel;
    buildFileBrowser(mFeBrowsePath, mPs3Stack.back());
    int n = (int)mPs3Stack.back().items.size();
    if (keep >= n) keep = n - 1;
    if (keep < 0) keep = 0;
    mPs3Stack.back().sel = keep;
}

bool NanoMenu::feBack() {
    if (mPs3Stack.empty() || mPs3Stack.back().screenKind != FE_BROWSE) return false;
    if (mFeBrowsePath == "/" || mFeBrowsePath.empty()) return false;   // at "/": exit the explorer
    buildFileBrowser(feParentDir(mFeBrowsePath), mPs3Stack.back());     // climb one real level
    return true;
}

void NanoMenu::feShowInfo(const std::string& path) {
    if (path.empty()) return;
    std::string body;
    auto row = [&](const char* label, const std::string& v) {
        if (v.empty()) return;
        std::string l = label;
        l.append(l.size() < 12 ? 12 - l.size() : 1, ' ');
        body += l; body += v; body += "\n";
    };
    if (path != mFeArcPath && feUnder(path, mFeArcPath)) {   // an entry inside the open archive
        const std::string inner = path.substr(mFeArcPath.size() + 1);
        const nanoarc::Entry* file = nullptr;
        if (mFeArcIndex) {
            for (const nanoarc::Entry& e : mFeArcIndex->entries)
                if (e.path == inner && !e.isDir) { file = &e; break; }
        }
        row(trDyn("Name"), feBaseName(path));
        row(trDyn("Type"), file ? trDyn("File") : trDyn("Folder"));
        if (file) row(trDyn("Size"), feSize(file->size));
        row(trDyn("Path"), path);
        feInfoDialog(feBaseName(path), body);
        return;
    }
    struct stat st;
    bool haveStat = (stat(path.c_str(), &st) == 0);
    bool isDir = haveStat && S_ISDIR(st.st_mode);
    row(trDyn("Name"), feBaseName(path));
    row(trDyn("Type"), isDir ? trDyn("Folder") : trDyn("File"));
    if (haveStat) {
        if (isDir) {
            // Counting the contents means a full listing, which on a network share is a request to
            // the server for something that is only a nicety in an info dialog - and this runs on
            // the render thread, so a big or slow directory would stall it. If the worker happens
            // to have this directory cached (the user just browsed it) the count is free; otherwise
            // it is left out rather than paid for.
            if (path.rfind("/mnt/shares/", 0) == 0 && !(mFbCacheValid && mFbCachePath == path)) {
                row(trDyn("Contents"), trDyn("(on a network share)"));
            } else if (mFbCacheValid && mFbCachePath == path) {
                const int count = (int)mFbCacheEntries.size();
                char c[64]; snprintf(c, sizeof(c), count == 1 ? trDyn("%d item") : trDyn("%d items"), count);
                row(trDyn("Contents"), c);
            } else {
                int count = 0;
                DIR* d = opendir(path.c_str());
                if (d) { struct dirent* e; while ((e = readdir(d)) != nullptr) {
                    if (e->d_name[0] == '.') continue; count++;
                } closedir(d); }
                char c[64]; snprintf(c, sizeof(c), count == 1 ? trDyn("%d item") : trDyn("%d items"), count);
                row(trDyn("Contents"), c);
            }
        } else {
            row(trDyn("Size"), feHumanSize((long long)st.st_size));
        }
        char when[40]; struct tm tmv;
        time_t mt = (time_t)st.st_mtime;
        if (localtime_r(&mt, &tmv)) { strftime(when, sizeof(when), "%Y-%m-%d %H:%M", &tmv); row(trDyn("Modified"), when); }
    }
    row(trDyn("Path"), path);
    if (body.empty()) body = "No information is available.";
    feInfoDialog(feBaseName(path), body);
}

void NanoMenu::feStartOp(int kind, const std::string& src, const std::string& dst) {
    if (mFeOp) return;   // one op at a time
    auto op = std::make_shared<FeOp>();
    op->kind = kind; op->name = feBaseName(src);
    mFeOp = op;
    bool move = (kind == 2);
    std::string s = src, dt = dst;
    // Worker captures only the shared result block + value strings (no `this`).
    std::thread([op, s, dt, kind, move]() {
        bool ok;
        if (kind == 3) ok = nanofilejob::treeRemove(s);
        else if (move) ok = nanofilejob::treeMove(s, dt);
        else ok = nanofilejob::treeCopy(s, dt);
        op->ok.store(ok, std::memory_order_release);
        op->done.store(true, std::memory_order_release);
        FrameFlag::wake();   // feTick reaps it on the next frame
    }).detach();
}

// Extract / Compress to Zip: same worker pattern as feStartOp, plus a progress dialog.
void NanoMenu::feStartArchiveOp(int kind, const std::string& src, const std::string& inner,
                                const std::string& dest, bool newFolder) {
    if (mFeOp) return;
    auto op = std::make_shared<FeOp>();
    op->kind = kind; op->name = feBaseName(inner.empty() ? src : inner);
    op->prog.notify = []() { FrameFlag::wakeThrottled(); };   // feTick redraws the progress, ~4 a second
    mFeOp = op;
    std::thread([op, kind, src, inner, dest, newFolder]() {
        if (kind == FE_OP_COMPRESS) {
            const nanoarc::CompressResult r = nanoarc::compressToZip(src, op->prog);
            op->result = feCompressMessage(r);
            op->ok.store(r.err == nanoarc::Err::None, std::memory_order_release);
        } else {
            const nanoarc::ExtractResult r = nanoarc::extract(src, inner, dest, newFolder, op->prog);
            op->result = feExtractMessage(r);
            op->ok.store(r.err == nanoarc::Err::None, std::memory_order_release);
        }
        op->done.store(true, std::memory_order_release);
        FrameFlag::wake();
    }).detach();

    // A plain one-button dialog: Cancel and Back both just close it, which feTick takes as the
    // request to stop. themeKey 0, so closing it runs no setting handler.
    mPs3DlgOptions.clear(); mPs3DlgSwatch.clear();
    mPs3DlgSlider = false; mPs3DlgKind = 0; mPs3DlgType = 1; mPs3DlgThemeKey = 0; mPs3DlgBinding = nullptr;
    mPs3DlgIllust = 0; mPs3DlgNotice.clear(); mPs3DlgRomInfo = false; mPs3DlgAppInfo = false;
    mPs3DlgTitle = (kind == FE_OP_COMPRESS) ? "Compressing" : "Extracting";
    mPs3DlgBody = mFeProgBody = feProgressText(op->name, 0, 0);
    mPs3DlgOptions.push_back("Cancel"); mPs3DlgSwatch.push_back(-1);
    mPs3DlgSel = 0; mPs3DlgOrigSel = 0;
    mPs3DlgIconTex = 0; mPs3DlgIconNmap = 0;
    mPs3DlgIconR = mPs3DlgIconG = mPs3DlgIconB = 1.0f;
    mPs3DlgActive = true; mPs3DlgAnim = 0.0f; mPs3DlgClosing = false; mPs3DlgBlurValid = false;
    mFeProgDlg = true;
}

void NanoMenu::feExtractToSelect(const std::string& dir) {
    if (!mPs3Stack.empty() && mPs3Stack.back().screenKind == GS_FOLDERBROWSE) mPs3Stack.pop_back();
    mDisplayDirty = true;
    if (mFeExtractArc.empty() || dir.empty()) return;
    feStartArchiveOp(FE_OP_EXTRACT, mFeExtractArc, mFeExtractInner, dir, false);
    mFeExtractArc.clear(); mFeExtractInner.clear();
}

void NanoMenu::feTick() {
    feArcLoadTick();
    if (!mFeOp) return;
    // Our progress dialog is the active one only while it still shows what we last put in it.
    const bool progUp = mFeProgDlg && mPs3DlgActive && mPs3DlgBody == mFeProgBody;
    if (mFeProgDlg && !progUp) {
        mFeProgDlg = false;
        // Closed (Cancel, Back, a tap outside) = stop. Replaced by some other dialog = not asked.
        if (!mPs3DlgActive) mFeOp->prog.cancel.store(true, std::memory_order_relaxed);
    }
    if (!mFeOp->done.load(std::memory_order_acquire)) {
        if (progUp) {
            mFeProgBody = feProgressText(mFeOp->name, mFeOp->prog.done.load(std::memory_order_relaxed),
                                         mFeOp->prog.total.load(std::memory_order_relaxed));
            mPs3DlgBody = mFeProgBody;
        }
        return;
    }
    // The op finished, but if the user is in a dialog or the OSK, defer reaping a frame so the
    // result dialog never overwrites their active modal (and never clobbers a pending confirm).
    // Our own progress dialog is simply replaced by the result.
    if (!progUp && (mPs3DlgActive || mOskActive)) return;
    mFeProgDlg = false;
    std::shared_ptr<FeOp> op = mFeOp;
    mFeOp.reset();
    bool ok = op->ok.load(std::memory_order_acquire);
    if (op->kind == 2 && ok) mFeClipPath.clear();   // a Move consumed the clipboard
    // An extract can write into a folder other than the one on screen (beside the archive being
    // browsed, or an Extract To... destination), so no cached listing is current any more.
    if (op->kind == FE_OP_EXTRACT || op->kind == FE_OP_COMPRESS) mFbCacheValid = false;
    feRefresh();
    if (op->kind == FE_OP_EXTRACT || op->kind == FE_OP_COMPRESS) {
        feInfoDialog(op->kind == FE_OP_EXTRACT ? "Extract" : "Compress to Zip", op->result);
        return;
    }
    const char* verb = (op->kind == 1) ? "Copy" : (op->kind == 2) ? "Move" : "Delete";
    const char* msg = (op->kind == 1) ? (ok ? trDyn("Copy completed.") : trDyn("Copy failed."))
                    : (op->kind == 2) ? (ok ? trDyn("Move completed.") : trDyn("Move failed."))
                                      : (ok ? trDyn("Delete completed.") : trDyn("Delete failed."));
    feInfoDialog(verb, msg);
}

void NanoMenu::feAction(const std::string& act) {
    if (mFeOp) return;   // busy with a copy/move/delete; ignore further ops until it finishes
    std::string path = mPs3OptCtxPayload;
    std::string name = feBaseName(path);

    if (act == "feopen") { if (!path.empty()) feNavigate(path); return; }

    // Extract To...: the standard folder picker, starting beside the archive. Its Select This Folder
    // comes back through feExtractToSelect.
    auto openExtractTo = [this](const std::string& arc, const std::string& inner) {
        mFeExtractArc = arc; mFeExtractInner = inner;
        mFolderPickTarget = 13;
        std::vector<Ps3Item> ps = ps3CurItems(); int pSel = ps3CurSel();
        Ps3Level lvl; buildFolderBrowser(feParentDir(arc), lvl); mPs3Stack.push_back(lvl);
        mPs3SubParentItems = ps; mPs3SubParentIdx = pSel; mPs3SubChildItems = mPs3Stack.back().items;
        mPs3SubDir = 1; mPs3SubAnimStart = mEffectTime; mPs3SubAnim = 0.0f;
        mPs3AnimItem = 0.0f; mPs3ItemAnimStart = -1.0f;
    };
    // Extracting the whole archive makes a folder named after it, beside it.
    auto extractHere = [this](const std::string& arc) {
        feStartArchiveOp(FE_OP_EXTRACT, arc, "", feJoin(feParentDir(arc), feStem(feBaseName(arc))), true);
    };

    if (feInArchive()) {   // the archive view is read-only: open, extract and inspect only
        if (path.empty() || !feUnder(path, mFeArcPath)) return;   // ".." at the root is outside it
        const std::string inner = (path == mFeArcPath) ? std::string() : path.substr(mFeArcPath.size() + 1);
        if (act == "feinfo") feShowInfo(path);
        else if (act == "feextractto") openExtractTo(mFeArcPath, inner);
        else if (act == "feextract" && inner.empty()) extractHere(mFeArcPath);
        else if (act == "feextract") feStartArchiveOp(FE_OP_EXTRACT, mFeArcPath, inner, feParentDir(mFeArcPath), false);
        return;
    }
    if (act == "feextracthere" || act == "feextractto") {
        if (mPs3OptCtxA != FE_ROW_ARCHIVE || path.empty()) return;
        if (act == "feextractto") openExtractTo(path, "");
        else extractHere(path);
        return;
    }

    if (act == "fecompress") { if (!path.empty()) feStartArchiveOp(FE_OP_COMPRESS, path, "", "", false); return; }

    if (act == "fecopy") { if (!path.empty()) { mFeClipPath = path; mFeClipMove = false; } return; }
    if (act == "femove") { if (!path.empty()) { mFeClipPath = path; mFeClipMove = true;  } return; }

    if (act == "fepaste") {
        if (mFeClipPath.empty() || mFeBrowsePath.empty()) return;
        std::string dst = feJoin(mFeBrowsePath, feBaseName(mFeClipPath));
        // Refuse to paste a folder into itself or its own subtree (would recurse forever). Compared
        // as real paths: /storage/emulated/0, /sdcard and /data/media/0 are one folder.
        const std::string here = sdCanon(mFeBrowsePath), clip = sdCanon(mFeClipPath);
        if (mFeClipMove && here == sdCanon(feParentDir(mFeClipPath))) { mFeClipPath.clear(); return; }
        if (sdUnder(here, clip)) {
            feInfoDialog("Paste", "Cannot paste a folder into itself.");
            return;
        }
        dst = feUniqueDest(dst);
        if (dst.empty()) { feInfoDialog("Paste", "Could not find a free name in this folder."); return; }
        feStartOp(mFeClipMove ? 2 : 1, mFeClipPath, dst);
        return;
    }

    if (act == "ferename") {
        if (path.empty()) return;
        std::string dir = feParentDir(path);
        std::string cur = name;
        openOskForPassword("Rename", [this, dir, path](const std::string& typed) {
            std::string nn = typed;
            for (char& c : nn) if (c == '/') c = '_';   // never allow a path separator
            // trim spaces
            while (!nn.empty() && nn.front() == ' ') nn.erase(nn.begin());
            while (!nn.empty() && nn.back() == ' ') nn.pop_back();
            if (nn.empty() || nn == feBaseName(path)) return;
            std::string ndst = feJoin(dir, nn);
            const int err = nanofilejob::renameNoReplace(path, ndst);   // never over a file of that name
            if (err == 0) feRefresh();
            else if (err == EEXIST) feInfoDialog("Rename", "A file with that name already exists.");
            else feInfoDialog("Rename", "Rename failed.");
        });
        mOskPasswordMode = false; mOskPlaintext = true;   // plain text, not masked
        mOskQuery = cur; mOsk.caret = (int)mOskQuery.size();
        return;
    }

    if (act == "fedelete") {
        if (path.empty()) return;
        mFeDeleteTarget = path;
        mPs3DlgOptions.clear(); mPs3DlgSwatch.clear();
        mPs3DlgSlider = false; mPs3DlgKind = 1; mPs3DlgThemeKey = 30; mPs3DlgBody.clear();
        { char t[320]; snprintf(t, sizeof(t), trDyn("Delete %s"), name.c_str()); mPs3DlgTitle = t; }
        mPs3DlgOptions.push_back("Cancel");  mPs3DlgSwatch.push_back(-1);
        mPs3DlgOptions.push_back("Delete");  mPs3DlgSwatch.push_back(-1);
        mPs3DlgSel = 0; mPs3DlgOrigSel = 0;
        mPs3DlgIconTex = 0; mPs3DlgIconNmap = nmapForIcon(22);
        mPs3DlgIconR = mPs3DlgIconG = mPs3DlgIconB = 1.0f;
        mPs3DlgActive = true; mPs3DlgAnim = 0.0f; mPs3DlgBlurValid = false;
        return;
    }

    if (act == "feinfo") { feShowInfo(path); return; }
}

// Generic XMB info dialog (kind 0), mirrors the "info" action in xmbOptAction.
void NanoMenu::feInfoDialog(const std::string& title, const std::string& body) {
    mPs3DlgOptions.clear(); mPs3DlgSwatch.clear();
    mPs3DlgSlider = false; mPs3DlgKind = 0; mPs3DlgType = 0; mPs3DlgThemeKey = 0; mPs3DlgBinding = nullptr;
    mPs3DlgIllust = 0; mPs3DlgNotice.clear(); mPs3DlgRomInfo = false;
    mPs3DlgTitle = title;
    mPs3DlgBody  = body;
    mPs3DlgSel = 0; mPs3DlgOrigSel = 0;
    mPs3DlgIconTex = 0; mPs3DlgIconNmap = 0;
    mPs3DlgIconR = mPs3DlgIconG = mPs3DlgIconB = 1.0f;
    mPs3DlgActive = true; mPs3DlgAnim = 0.0f; mPs3DlgBlurValid = false;
}

} // namespace android
