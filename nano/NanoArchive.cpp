#define LOG_TAG "NanoArchive"

#include "NanoArchive.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <linux/magic.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <algorithm>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <utility>

#include <android-base/unique_fd.h>
#include <log/log.h>
#include <ziparchive/zip_archive.h>
#include <ziparchive/zip_writer.h>

#include <7z.h>
#include <7zCrc.h>
#include <7zFile.h>

namespace nanoarc {
namespace {

using android::base::unique_fd;

constexpr uint64_t kSpaceMargin = 8ull << 20;   // the same spare room the folder copy keeps
constexpr UInt32 k7zNoFolder = 0xFFFFFFFFu;

bool endsWithNoCase(const std::string& s, const char* suffix) {
    const size_t n = strlen(suffix);
    return s.size() > n && strcasecmp(s.c_str() + s.size() - n, suffix) == 0;
}

std::string parentOf(const std::string& p) {
    const size_t sl = p.find_last_of('/');
    if (sl == std::string::npos) return ".";
    return sl == 0 ? std::string("/") : p.substr(0, sl);
}

std::string baseOf(const std::string& p) {
    const size_t sl = p.find_last_of('/');
    return sl == std::string::npos ? p : p.substr(sl + 1);
}

std::string joinPath(const std::string& dir, const std::string& name) {
    return (dir == "/" ? std::string("/") : dir + "/") + name;
}

bool step(Progress& p, uint64_t n) {
    p.done.fetch_add(n, std::memory_order_relaxed);
    if (p.notify) p.notify();
    return !p.cancel.load(std::memory_order_relaxed);
}

bool writeAll(int fd, const uint8_t* p, size_t n) {
    while (n > 0) {
        const ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (w == 0) { errno = EIO; return false; }
        p += w;
        n -= (size_t)w;
    }
    return true;
}

Err writeErr(int err) { return err == ENOSPC ? Err::NoSpace : Err::Write; }

// Sizes come from the archive's own header, so sums of them must not wrap.
uint64_t satAdd(uint64_t a, uint64_t b) { return a > UINT64_MAX - b ? UINT64_MAX : a + b; }

// The destination's filesystem will not take this name (exFAT / vfat refuse ':' '?' '"' ..., or it
// is too long): skip the entry rather than give up on the whole archive.
bool nameRefused(int err) { return err == EINVAL || err == ENAMETOOLONG || err == EILSEQ; }

// An entry name as a relative path: backslashes taken as separators (old Windows zippers), empty
// and "." components dropped. Absolute names and ".." components are refused outright rather than
// "fixed", since a name built to escape the destination says what the archive is.
enum class Name { Ok, Skip, Unsafe };
Name cleanName(std::string raw, std::string* out) {
    if (raw.find('\0') != std::string::npos) return Name::Unsafe;
    std::replace(raw.begin(), raw.end(), '\\', '/');
    if (!raw.empty() && raw[0] == '/') return Name::Unsafe;
    std::string res;
    for (size_t i = 0; i <= raw.size();) {
        size_t j = raw.find('/', i);
        if (j == std::string::npos) j = raw.size();
        const std::string c = raw.substr(i, j - i);
        if (c == "..") return Name::Unsafe;
        if (!c.empty() && c != ".") {
            if (!res.empty()) res += '/';
            res += c;
        }
        i = j + 1;
    }
    if (res.empty()) return Name::Skip;
    *out = std::move(res);
    return Name::Ok;
}

std::vector<std::string> splitPath(const std::string& p) {
    std::vector<std::string> out;
    for (size_t i = 0; i <= p.size();) {
        size_t j = p.find('/', i);
        if (j == std::string::npos) j = p.size();
        out.push_back(p.substr(i, j - i));
        i = j + 1;
    }
    return out;
}

std::string joinComps(const std::vector<std::string>& c, size_t n) {
    std::string s;
    for (size_t i = 0; i < n; i++) {
        if (i) s += '/';
        s += c[i];
    }
    return s;
}

// The directory comps[0..n) under rootFd, created if asked, opened one component at a time with
// O_NOFOLLOW so a link already in the destination is never followed out of it. Returns -1 with
// errno ELOOP (a link) or ENOTDIR (a file) when something other than a directory is in the way.
// Each directory it had to create is appended to *made (as "a/b"), when given.
int openSubdir(int rootFd, const std::vector<std::string>& comps, size_t n, bool create,
               std::vector<std::string>* made = nullptr) {
    unique_fd cur(fcntl(rootFd, F_DUPFD_CLOEXEC, 0));
    if (!cur.ok()) return -1;
    for (size_t i = 0; i < n; i++) {
        const char* c = comps[i].c_str();
        const int flags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;
        int next = openat(cur.get(), c, flags);
        if (next < 0 && errno == ENOENT && create) {
            if (mkdirat(cur.get(), c, 0775) == 0) {
                if (made) made->push_back(joinComps(comps, i + 1));
            } else if (errno != EEXIST) {
                return -1;
            }
            next = openat(cur.get(), c, flags);
        }
        if (next < 0) {
            // O_NOFOLLOW|O_DIRECTORY on a link fails with ENOTDIR, as for a file: tell them apart.
            const int err = errno;
            struct stat st;
            if (err == ENOTDIR || err == ELOOP)
                errno = (fstatat(cur.get(), c, &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISLNK(st.st_mode)) ? ELOOP : ENOTDIR;
            else
                errno = err;
            return -1;
        }
        cur.reset(next);
    }
    return cur.release();
}

// What is at comps under rootFd: 0 nothing (it would be created), 1 something (a file to keep, or a
// file where a folder is needed), 2 a link in the way.
int existsUnder(int rootFd, const std::vector<std::string>& comps) {
    unique_fd dir(openSubdir(rootFd, comps, comps.size() - 1, false));
    if (!dir.ok()) return errno == ELOOP ? 2 : errno == ENOTDIR ? 1 : 0;
    struct stat st;
    return fstatat(dir.get(), comps.back().c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0 ? 1 : 0;
}

bool validUtf8(const std::string& s) {
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = (unsigned char)s[i];
        size_t n;
        if (c == 0) return false;
        if (c < 0x80) n = 0;
        else if ((c & 0xE0) == 0xC0) n = 1;
        else if ((c & 0xF0) == 0xE0) n = 2;
        else if ((c & 0xF8) == 0xF0) n = 3;
        else return false;
        if (s.size() - i < n + 1) return false;
        for (size_t k = 1; k <= n; k++)
            if (((unsigned char)s[i + k] & 0xC0) != 0x80) return false;
        i += n + 1;
    }
    return true;
}

// ---- sources ------------------------------------------------------------------------------------

struct RawEntry {
    std::string name;
    uint64_t size = 0;
    bool isDir = false, isLink = false, encrypted = false, unsupported = false;
    uint32_t ref = 0;   // the format's own handle on the entry
};

class Source {
  public:
    virtual ~Source() = default;
    // prog (may be null) lets a long read be cancelled.
    virtual Err open(const std::string& path, Progress* prog) = 0;
    // Bytes held in memory at once to produce this entry (7z: its whole solid block).
    virtual uint64_t blockSize(const RawEntry&) const { return 0; }
    // Writes the entry to fd; never more than e.size bytes. Method = skip this one, carry on.
    virtual Err write(const RawEntry& e, int fd, Progress& prog, int* sysErr) = 0;
    const std::vector<RawEntry>& entries() const { return mEntries; }

  protected:
    std::vector<RawEntry> mEntries;
};

// libziparchive keeps inflating until the deflate stream ends and checks the length only then, so
// the cap has to be enforced here or a bomb would fill the disk first.
class CappedFdWriter final : public zip_archive::Writer {
  public:
    CappedFdWriter(int fd, uint64_t cap, Progress& prog) : mFd(fd), mCap(cap), mProg(prog) {}
    bool Append(uint8_t* buf, size_t n) override {
        if (n > mCap - mWritten) return false;
        if (!writeAll(mFd, buf, n)) { sysErr = errno; return false; }
        mWritten += n;
        if (!step(mProg, n)) { cancelled = true; return false; }
        return true;
    }
    int sysErr = 0;
    bool cancelled = false;

  private:
    int mFd;
    uint64_t mCap, mWritten = 0;
    Progress& mProg;
};

class ZipSource final : public Source {
  public:
    ~ZipSource() override { if (mHandle) CloseArchive(mHandle); }

    Err open(const std::string& path, Progress*) override {
        // The handle exists even when opening fails, and must still be closed.
        const int32_t r0 = OpenArchive(path.c_str(), &mHandle);
        if (r0 == -10) return Err::NameEncoding;   // kInvalidEntryName (zip_error.h, not exported)
        if (r0 != 0) return access(path.c_str(), R_OK) == 0 ? Err::Corrupt : Err::Open;
        void* cookie = nullptr;
        if (StartIteration(mHandle, &cookie) != 0) return Err::Corrupt;
        ZipEntry64 ze;
        std::string name;
        int32_t r;
        while ((r = Next(cookie, &ze, &name)) == 0) {
            RawEntry e;
            e.name = name;
            e.size = ze.uncompressed_length;
            const bool unixHost = (ze.version_made_by >> 8) == 3;
            e.isDir = (!name.empty() && (name.back() == '/' || name.back() == '\\'))
                      || (unixHost && S_ISDIR(ze.unix_mode));
            e.isLink = unixHost && S_ISLNK(ze.unix_mode);
            e.encrypted = (ze.gpbf & 1) != 0;
            e.unsupported = ze.method != kCompressStored && ze.method != kCompressDeflated;
            e.ref = (uint32_t)mZips.size();
            mZips.push_back(ze);
            mEntries.push_back(std::move(e));
        }
        EndIteration(cookie);
        return r == -1 ? Err::None : Err::Corrupt;
    }

    Err write(const RawEntry& e, int fd, Progress& prog, int* sysErr) override {
        CappedFdWriter w(fd, e.size, prog);
        if (ExtractToWriter(mHandle, &mZips[e.ref], &w) == 0) return Err::None;
        if (w.cancelled) return Err::Cancelled;
        if (w.sysErr) { *sysErr = w.sysErr; return writeErr(w.sysErr); }
        return Err::Corrupt;   // also an entry that inflated past its declared size
    }

  private:
    ZipArchiveHandle mHandle = nullptr;
    std::vector<ZipEntry64> mZips;
};

// Every allocation the 7z decoder makes goes through here, and the live total is capped, so a header
// that declares absurd sizes (or a BCJ2 folder's extra buffers on top of a big block) gets a clean
// SZ_ERROR_MEM instead of an out-of-memory kill. The cap sits above k7zBlockCap so a block that
// passes the up-front check still has room for the file list and the decoder state.
constexpr uint64_t kSzMemCap = k7zBlockCap + (64ull << 20);
constexpr size_t kSzHeader = 16;   // keeps malloc's 16-byte alignment for the caller
struct SzMem {
    ISzAlloc vt;
    mutable uint64_t live = 0;
};
void* szAlloc(ISzAllocPtr p, size_t size) {
    const SzMem* m = reinterpret_cast<const SzMem*>(p);
    if (size == 0 || size > kSzMemCap - m->live) return nullptr;
    uint8_t* b = static_cast<uint8_t*>(malloc(size + kSzHeader));
    if (!b) return nullptr;
    memcpy(b, &size, sizeof(size));
    m->live += size;
    return b + kSzHeader;
}
void szFree(ISzAllocPtr p, void* addr) {
    if (!addr) return;
    uint8_t* b = static_cast<uint8_t*>(addr) - kSzHeader;
    size_t size;
    memcpy(&size, b, sizeof(size));
    reinterpret_cast<const SzMem*>(p)->live -= size;
    free(b);
}

// Sits under the look-ahead buffer so Cancel also stops a solid block mid-decode, which for a
// large block is many seconds of work with no other point to check.
struct CancelStream {
    ISeekInStream vt;
    const ISeekInStream* inner;
    Progress* prog;
};
SRes cancelRead(const ISeekInStream* p, void* buf, size_t* size) {
    const CancelStream* s = reinterpret_cast<const CancelStream*>(p);
    if (s->prog && s->prog->cancel.load(std::memory_order_relaxed)) return SZ_ERROR_PROGRESS;
    return s->inner->Read(s->inner, buf, size);
}
SRes cancelSeek(const ISeekInStream* p, Int64* pos, ESzSeek origin) {
    const CancelStream* s = reinterpret_cast<const CancelStream*>(p);
    return s->inner->Seek(s->inner, pos, origin);
}

std::string utf16ToUtf8(const UInt16* s) {
    std::string out;
    for (size_t i = 0; s[i]; i++) {
        uint32_t c = s[i];
        if (c >= 0xD800 && c < 0xDC00 && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000) {
            c = 0x10000 + ((c - 0xD800) << 10) + (s[i + 1] - 0xDC00);
            i++;
        } else if (c >= 0xD800 && c < 0xE000) {
            c = 0xFFFD;
        }
        if (c < 0x80) {
            out += (char)c;
        } else if (c < 0x800) {
            out += (char)(0xC0 | (c >> 6));
            out += (char)(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += (char)(0xE0 | (c >> 12));
            out += (char)(0x80 | ((c >> 6) & 0x3F));
            out += (char)(0x80 | (c & 0x3F));
        } else {
            out += (char)(0xF0 | (c >> 18));
            out += (char)(0x80 | ((c >> 12) & 0x3F));
            out += (char)(0x80 | ((c >> 6) & 0x3F));
            out += (char)(0x80 | (c & 0x3F));
        }
    }
    return out;
}

class SevenZSource final : public Source {
  public:
    ~SevenZSource() override {
        szFree(&mMem.vt, mBlock);
        if (mDbOpen) SzArEx_Free(&mDb, &mMem.vt);
        if (mFileOpen) File_Close(&mFile.file);
    }

    Err open(const std::string& path, Progress* prog) override {
        static std::once_flag crcOnce;
        std::call_once(crcOnce, CrcGenerateTable);
        File_Construct(&mFile.file);
        if (InFile_Open(&mFile.file, path.c_str()) != 0) return Err::Open;
        mFileOpen = true;
        FileInStream_CreateVTable(&mFile);
        mCancel.vt.Read = cancelRead;
        mCancel.vt.Seek = cancelSeek;
        mCancel.inner = &mFile.vt;
        mCancel.prog = prog;
        LookToRead2_CreateVTable(&mLook, False);
        mLookBuf.resize(1 << 18);
        mLook.buf = mLookBuf.data();
        mLook.bufSize = mLookBuf.size();
        mLook.realStream = &mCancel.vt;
        LookToRead2_Init(&mLook);
        SzArEx_Init(&mDb);
        const SRes r = SzArEx_Open(&mDb, &mLook.vt, &mMem.vt, &mMem.vt);   // frees itself on failure
        if (r != SZ_OK) {
            ALOGW("7z: open %s failed: %d", path.c_str(), r);
            return r == SZ_ERROR_UNSUPPORTED ? Err::Unsupported
                 : r == SZ_ERROR_PROGRESS    ? Err::Cancelled
                 : r == SZ_ERROR_READ        ? Err::Open
                                             : Err::Corrupt;
        }
        mDbOpen = true;
        mFolderState.assign(mDb.db.NumFolders, -1);
        std::vector<UInt16> name;
        for (UInt32 i = 0; i < mDb.NumFiles; i++) {
            const size_t len = SzArEx_GetFileNameUtf16(&mDb, i, nullptr);
            name.assign(len + 1, 0);
            SzArEx_GetFileNameUtf16(&mDb, i, name.data());
            RawEntry e;
            e.name = utf16ToUtf8(name.data());
            e.isDir = SzArEx_IsDir(&mDb, i);
            e.size = e.isDir ? 0 : SzArEx_GetFileSize(&mDb, i);
            if (SzBitWithVals_Check(&mDb.Attribs, i)) {
                // 0x400 is a Windows reparse point; 0x8000 says the high half is a Unix st_mode.
                const UInt32 a = mDb.Attribs.Vals[i];
                e.isLink = (a & 0x400) || ((a & 0x8000) && S_ISLNK(a >> 16));
            }
            const UInt32 fo = mDb.FileToFolder[i];
            if (!e.isDir && fo != k7zNoFolder) {
                const int st = folderState(fo);
                e.encrypted = st == 1;
                e.unsupported = st == 2;
            }
            e.ref = i;
            mEntries.push_back(std::move(e));
        }
        return Err::None;
    }

    uint64_t blockSize(const RawEntry& e) const override {
        const UInt32 fo = mDb.FileToFolder[e.ref];
        return fo == k7zNoFolder ? 0 : SzAr_GetFolderUnpackSize(&mDb.db, fo);
    }

    Err write(const RawEntry& e, int fd, Progress& prog, int* sysErr) override {
        // An empty file has no folder to decode, and asking for one would drop the cached block.
        if (e.size == 0) return Err::None;
        const UInt32 fo = mDb.FileToFolder[e.ref];
        if (mFolderState[fo] == 2) return Err::Method;
        size_t offset = 0, got = 0;
        const SRes r = SzArEx_Extract(&mDb, &mLook.vt, e.ref, &mBlockIndex, &mBlock, &mBlockSize,
                                      &offset, &got, &mMem.vt, &mMem.vt);
        if (r != SZ_OK) {
            // A failed decode leaves a half-filled block marked as cached.
            szFree(&mMem.vt, mBlock);
            mBlock = nullptr;
            mBlockIndex = k7zNoFolder;
            if (r == SZ_ERROR_PROGRESS) return Err::Cancelled;
            if (r == SZ_ERROR_UNSUPPORTED) { mFolderState[fo] = 2; return Err::Method; }
            if (r == SZ_ERROR_MEM) return Err::NoMemory;
            ALOGW("7z: extracting %s failed: %d", e.name.c_str(), r);
            return Err::Corrupt;
        }
        if (got != e.size) return Err::Corrupt;
        const uint8_t* p = mBlock + offset;
        for (uint64_t left = got; left > 0;) {
            const size_t n = (size_t)std::min<uint64_t>(left, 1 << 20);
            if (!writeAll(fd, p, n)) { *sysErr = errno; return writeErr(errno); }
            p += n;
            left -= n;
            if (!step(prog, n)) return Err::Cancelled;
        }
        return Err::None;
    }

  private:
    // 0 decodable, 1 encrypted, 2 a method this decoder lacks. Mirrors 7zDec.c's method list; a
    // folder it still turns down is caught by SZ_ERROR_UNSUPPORTED at extraction.
    int folderState(UInt32 fo) {
        if (mFolderState[fo] >= 0) return mFolderState[fo];
        CSzFolder f;
        CSzData sd;
        sd.Data = mDb.db.CodersData + mDb.db.FoCodersOffsets[fo];
        sd.Size = mDb.db.FoCodersOffsets[fo + 1] - mDb.db.FoCodersOffsets[fo];
        int st = SzGetNextFolderItem(&f, &sd) == SZ_OK ? 0 : 2;
        for (UInt32 c = 0; st == 0 && c < f.NumCoders; c++) {
            switch (f.Coders[c].MethodID) {
                case 0x06F10701: st = 1; break;   // AES-256 + SHA-256
                case 0x0:        // Copy
                case 0x3:        // Delta
                case 0x21:       // LZMA2
                case 0x30101:    // LZMA
                case 0x3030103:  // BCJ
                case 0x303011B:  // BCJ2
                case 0x3030205:  // PPC
                case 0x3030401:  // IA64
                case 0x3030501:  // ARM
                case 0x3030701:  // ARMT
                case 0x3030805:  // SPARC
                    break;
                default: st = 2; break;
            }
        }
        return mFolderState[fo] = (int8_t)st;
    }

    SzMem mMem{{szAlloc, szFree}};
    CFileInStream mFile;
    CLookToRead2 mLook;
    CSzArEx mDb;
    CancelStream mCancel;
    std::vector<Byte> mLookBuf;
    bool mFileOpen = false, mDbOpen = false;
    std::vector<int8_t> mFolderState;
    UInt32 mBlockIndex = k7zNoFolder;
    Byte* mBlock = nullptr;
    size_t mBlockSize = 0;
};

Err makeSource(const std::string& archive, std::unique_ptr<Source>* out) {
    switch (formatFromName(archive)) {
        case Format::Zip:      out->reset(new ZipSource()); return Err::None;
        case Format::SevenZip: out->reset(new SevenZSource()); return Err::None;
        case Format::Rar:      return Err::Rar;
        case Format::None:     break;
    }
    return Err::Open;
}

// ---- zip creation -------------------------------------------------------------------------------

struct ZipItem {
    std::string path, name;
    bool isDir = false;
    uint64_t size = 0;
    time_t mtime = 0;
};

Err writeZip(FILE* f, const std::vector<ZipItem>& items, Progress& prog, std::string* detail) {
    ZipWriter w(f);
    std::vector<uint8_t> buf(1 << 16);
    // ZipWriter fails with errno still set by the fwrite that failed.
    auto ioErr = [&]() { const int e = errno; *detail = strerror(e); return writeErr(e); };
    for (const ZipItem& it : items) {
        if (prog.cancel.load(std::memory_order_relaxed)) return Err::Cancelled;
        if (it.isDir) {
            if (w.StartEntryWithTime(it.name + "/", 0, it.mtime) != 0 || w.FinishEntry() != 0) return ioErr();
            continue;
        }
        unique_fd in(open(it.path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
        if (!in.ok()) { *detail = it.path; return Err::Read; }
        // Level 6 rather than ZipWriter's default 9: far faster on these cores for a few percent.
        if (w.StartEntryWithTime(it.name, ZipWriter::kCompress | ZipWriter::kDefaultCompression, it.mtime) != 0)
            return ioErr();
        // Stops at the size the limits were checked against, should the file grow meanwhile.
        for (uint64_t left = it.size; left > 0;) {
            const ssize_t n = read(in.get(), buf.data(), (size_t)std::min<uint64_t>(buf.size(), left));
            if (n < 0 && errno == EINTR) continue;
            if (n < 0) { *detail = it.path; return Err::Read; }
            if (n == 0) break;
            if (w.WriteBytes(buf.data(), (size_t)n) != 0) return ioErr();
            left -= (uint64_t)n;
            if (!step(prog, (uint64_t)n)) return Err::Cancelled;
        }
        if (w.FinishEntry() != 0) return ioErr();
    }
    return w.Finish() == 0 ? Err::None : ioErr();
}

}  // namespace

Format formatFromName(const std::string& name) {
    if (endsWithNoCase(name, ".zip")) return Format::Zip;
    if (endsWithNoCase(name, ".7z")) return Format::SevenZip;
    if (endsWithNoCase(name, ".rar")) return Format::Rar;
    return Format::None;
}

Index readIndex(const std::string& archive) {
    Index idx;
    std::unique_ptr<Source> src;
    if ((idx.err = makeSource(archive, &src)) != Err::None) return idx;
    if ((idx.err = src->open(archive, nullptr)) != Err::None) return idx;
    for (const RawEntry& e : src->entries()) {
        Entry out;
        if (cleanName(e.name, &out.path) != Name::Ok) continue;
        out.isDir = e.isDir;
        out.size = e.isDir ? 0 : e.size;
        idx.entries.push_back(std::move(out));
    }
    return idx;
}

ExtractResult extract(const std::string& archive, const std::string& inner, const std::string& dest,
                      bool newFolder, Progress& prog) {
    ExtractResult res;
    std::unique_ptr<Source> src;
    if ((res.err = makeSource(archive, &src)) != Err::None) return res;
    if ((res.err = src->open(archive, &prog)) != Err::None) return res;

    // Pick what is extracted, and where each piece goes relative to the destination.
    struct Item { const RawEntry* e; std::vector<std::string> comps; };
    std::vector<Item> files, dirs;
    const size_t innerSlash = inner.find_last_of('/');
    const size_t strip = innerSlash == std::string::npos ? 0 : innerSlash + 1;
    for (const RawEntry& e : src->entries()) {
        std::string clean;
        const Name k = cleanName(e.name, &clean);
        if (k == Name::Skip) continue;
        if (k == Name::Unsafe) {
            if (inner.empty()) res.unsafe++;
            continue;
        }
        if (!inner.empty() && clean != inner && clean.compare(0, inner.size() + 1, inner + "/") != 0) continue;
        if (e.isLink) { res.links++; continue; }
        Item it{&e, splitPath(clean.substr(strip))};
        if (e.isDir) { dirs.push_back(std::move(it)); continue; }
        if (e.encrypted) { res.encrypted++; continue; }
        if (e.unsupported) { res.unsupported++; continue; }
        files.push_back(std::move(it));
    }
    if (files.empty() && (res.encrypted || res.unsupported)) {
        res.err = res.encrypted >= res.unsupported ? Err::Encrypted : Err::Method;
        return res;
    }
    for (const Item& it : files) res.needBytes = std::max(res.needBytes, src->blockSize(*it.e));
    if (res.needBytes > k7zBlockCap) { res.err = Err::BlockTooBig; return res; }
    res.needBytes = 0;

    unique_fd root;
    if (!newFolder) {
        root.reset(open(dest.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
        if (!root.ok()) { res.err = Err::Write; res.detail = dest + ": " + strerror(errno); return res; }
    }

    // Room for everything that will be written, before anything is: each file rounded up to the
    // drive's cluster, a cluster per folder, and the margin. Files that are already there are not
    // counted, since they are skipped (a folder being created has none).
    const std::string spaceDir = newFolder ? parentOf(dest) : dest;
    uint64_t cluster = 4096, avail = 0;
    struct statvfs vfs;
    const bool known = statvfs(spaceDir.c_str(), &vfs) == 0;
    if (known) {
        if (vfs.f_bsize) cluster = (uint64_t)vfs.f_bsize;
        avail = (uint64_t)vfs.f_bavail * (uint64_t)vfs.f_frsize;
    }
    uint64_t need = kSpaceMargin, total = 0;
    std::set<std::string> folders;
    std::vector<const Item*> todo;
    for (const Item& it : files) {
        if (!newFolder) {
            const int there = existsUnder(root.get(), it.comps);
            if (there == 1) { res.existing++; continue; }
            if (there == 2) { res.unsafe++; continue; }
        }
        // A single entry declaring more than the drive has free is refused here, before its
        // rounding or the running sum can wrap (a zip64 size field is attacker-chosen).
        if (known && it.e->size > avail) { need = it.e->size; break; }
        for (size_t i = 1; i < it.comps.size(); i++) folders.insert(joinComps(it.comps, i));
        need = satAdd(need, satAdd(it.e->size / cluster * cluster, it.e->size % cluster ? cluster : 0));
        total = satAdd(total, it.e->size);
        todo.push_back(&it);
    }
    for (const Item& it : dirs) folders.insert(joinComps(it.comps, it.comps.size()));
    need = satAdd(need, (uint64_t)folders.size() * cluster);
    ALOGI("extract %s: %llu bytes needed, %llu free", archive.c_str(), (unsigned long long)need,
          (unsigned long long)avail);
    if (known && need > avail) {
        res.err = Err::NoSpaceCheck;
        res.needBytes = need;
        res.freeBytes = avail;
        return res;
    }
    if (prog.cancel.load(std::memory_order_relaxed)) { res.err = Err::Cancelled; return res; }
    prog.total.store(total, std::memory_order_relaxed);
    if (prog.notify) prog.notify();

    unique_fd parent;
    std::string madeName;
    if (newFolder) {
        parent.reset(open(parentOf(dest).c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
        if (!parent.ok()) { res.err = Err::Write; res.detail = parentOf(dest) + ": " + strerror(errno); return res; }
        for (int n = 1; n < 1000 && madeName.empty(); n++) {
            const std::string cand = n == 1 ? baseOf(dest) : baseOf(dest) + " (" + std::to_string(n) + ")";
            if (mkdirat(parent.get(), cand.c_str(), 0775) == 0) madeName = cand;
            else if (errno != EEXIST) { res.err = writeErr(errno); res.detail = cand + ": " + strerror(errno); return res; }
        }
        if (madeName.empty()) { res.err = Err::NoFreeName; return res; }
        res.dest = joinPath(parentOf(dest), madeName);
        root.reset(openat(parent.get(), madeName.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (!root.ok()) { res.err = Err::Write; res.detail = res.dest + ": " + strerror(errno); return res; }
    } else {
        res.dest = dest;
    }

    // In a new folder, everything this run creates, so a failure or cancel can take it all back.
    std::vector<std::string> madeDirs, madeFiles;
    std::vector<std::string>* made = newFolder ? &madeDirs : nullptr;

    // A directory that cannot be made: a link in the way is skipped as unsafe, a file in the way as
    // something already there, a name the drive refuses as such; anything else stops the extraction.
    auto dirFailed = [&](const std::vector<std::string>& comps, size_t n) {
        const int e = errno;
        if (e == ELOOP) { res.unsafe++; return false; }
        if (e == ENOTDIR || e == EEXIST) { res.existing++; return false; }
        if (nameRefused(e)) { res.badNames++; return false; }
        res.err = writeErr(e);
        res.detail = joinComps(comps, n) + ": " + strerror(e);
        return true;
    };
    for (size_t i = 0; res.err == Err::None && i < todo.size(); i++) {
        const Item& it = *todo[i];
        if (prog.cancel.load(std::memory_order_relaxed)) { res.err = Err::Cancelled; break; }
        unique_fd dir(openSubdir(root.get(), it.comps, it.comps.size() - 1, true, made));
        if (!dir.ok()) {
            if (dirFailed(it.comps, it.comps.size() - 1)) break;
            continue;
        }
        const char* leaf = it.comps.back().c_str();
        unique_fd out(openat(dir.get(), leaf, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0664));
        if (!out.ok()) {
            if (errno == EEXIST) { res.existing++; continue; }
            if (nameRefused(errno)) { res.badNames++; continue; }
            res.err = writeErr(errno);
            res.detail = joinComps(it.comps, it.comps.size()) + ": " + strerror(errno);
            break;
        }
        int sysErr = 0;
        Err w = src->write(*it.e, out.get(), prog, &sysErr);
        if (close(out.release()) != 0 && w == Err::None) { sysErr = errno; w = writeErr(sysErr); }
        if (w == Err::None) {
            res.extracted++;
            if (newFolder) madeFiles.push_back(joinComps(it.comps, it.comps.size()));
            continue;
        }
        unlinkat(dir.get(), leaf, 0);   // never leave a partial file behind
        if (w == Err::Method) { res.unsupported++; continue; }
        res.err = w;
        if (w == Err::Write) res.detail = joinComps(it.comps, it.comps.size()) + ": " + strerror(sysErr);
    }
    // Folder entries last: files make their own folders, so this only adds the empty ones, and a
    // failure on the first file leaves no folder skeleton behind.
    for (size_t i = 0; res.err == Err::None && i < dirs.size(); i++) {
        unique_fd d(openSubdir(root.get(), dirs[i].comps, dirs[i].comps.size(), true, made));
        if (!d.ok() && dirFailed(dirs[i].comps, dirs[i].comps.size())) break;
    }
    // A failed or cancelled Extract Here takes back what it wrote: its files, then its folders
    // deepest first (each was made after its parent), then the folder it created. Only paths this
    // run created are touched, so nothing that was there before can be removed.
    if (newFolder && res.err != Err::None) {
        for (const std::string& f : madeFiles) unlinkat(root.get(), f.c_str(), 0);
        for (auto d = madeDirs.rbegin(); d != madeDirs.rend(); ++d) unlinkat(root.get(), d->c_str(), AT_REMOVEDIR);
        res.rolledBack = unlinkat(parent.get(), madeName.c_str(), AT_REMOVEDIR) == 0;
        if (res.rolledBack) res.extracted = 0;
    }
    return res;
}

CompressResult compressToZip(const std::string& srcPath, Progress& prog) {
    CompressResult res;
    std::vector<ZipItem> items;
    const std::string parent = parentOf(srcPath), base = baseOf(srcPath);
    struct stat st;
    if (lstat(srcPath.c_str(), &st) == 0 && S_ISLNK(st.st_mode)) { res.err = Err::Link; return res; }
    if (stat(srcPath.c_str(), &st) != 0 || !(S_ISDIR(st.st_mode) || S_ISREG(st.st_mode))) {
        res.err = Err::Read;
        res.detail = srcPath;
        return res;
    }
    // A storage root (/storage/emulated, a microSD) or a share sits in a tmpfs folder: a zip beside
    // it would live in RAM, take up to half of it, and be gone at the next boot.
    struct statfs fs;
    if (statfs(parent.c_str(), &fs) == 0 && fs.f_type == TMPFS_MAGIC) { res.err = Err::TmpfsDest; return res; }

    // ZipWriter has no zip64: every offset and size must fit in 32 bits, and the entry count in 16.
    // Checked as the tree is walked, so a huge folder (or a share whose server loops a link back
    // onto a parent) stops early. Deflate can grow incompressible data slightly, which the 1% covers
    // many times over.
    uint64_t total = 0, overhead = 22;
    auto add = [&](ZipItem it) {
        if (!validUtf8(it.name)) { res.err = Err::BadName; res.detail = it.name; return false; }
        total += it.size;
        overhead += 30 + 46 + 2 * (it.name.size() + 1) + 16;
        if (items.size() + 1 >= 65535 || total + total / 100 + overhead >= UINT32_MAX) { res.err = Err::TooBig; return false; }
        items.push_back(std::move(it));
        return true;
    };
    std::set<std::pair<dev_t, ino_t>> seenDirs;   // a folder reached twice is a loop: zip it once
    seenDirs.insert({st.st_dev, st.st_ino});
    std::function<bool(const std::string&, const std::string&)> walk =
        [&](const std::string& dir, const std::string& name) {
            std::vector<std::string> kids;
            DIR* d = opendir(dir.c_str());
            if (!d) { res.err = Err::Read; res.detail = dir; return false; }
            while (struct dirent* e = readdir(d)) {
                if (strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0) kids.push_back(e->d_name);
            }
            closedir(d);
            std::sort(kids.begin(), kids.end());
            for (const std::string& k : kids) {
                if (prog.cancel.load(std::memory_order_relaxed)) { res.err = Err::Cancelled; return false; }
                const std::string path = dir + "/" + k, nm = name + "/" + k;
                struct stat ks;
                if (lstat(path.c_str(), &ks) != 0) { res.err = Err::Read; res.detail = path; return false; }
                if (S_ISLNK(ks.st_mode)) {
                    res.links++;
                } else if (S_ISDIR(ks.st_mode)) {
                    if (ks.st_ino != 0 && !seenDirs.insert({ks.st_dev, ks.st_ino}).second) { res.links++; continue; }
                    if (!add({path, nm, true, 0, ks.st_mtime}) || !walk(path, nm)) return false;
                } else if (S_ISREG(ks.st_mode)) {
                    if (!add({path, nm, false, (uint64_t)ks.st_size, ks.st_mtime})) return false;
                }
            }
            return true;
        };
    const bool isDir = S_ISDIR(st.st_mode);
    if (!add({srcPath, base, isDir, isDir ? 0 : (uint64_t)st.st_size, st.st_mtime})) return res;
    if (isDir && !walk(srcPath, base)) return res;

    int fd = -1;
    for (int n = 1; n < 1000; n++) {
        res.zipPath = joinPath(parent, n == 1 ? base + ".zip" : base + " (" + std::to_string(n) + ").zip");
        fd = open(res.zipPath.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0664);
        if (fd >= 0 || errno != EEXIST) break;
    }
    if (fd < 0) {
        res.err = errno == EEXIST ? Err::NoFreeName : writeErr(errno);
        res.detail = strerror(errno);
        return res;
    }
    FILE* f = fdopen(fd, "wb");
    if (!f) {
        res.err = Err::Write;
        res.detail = strerror(errno);
        close(fd);
        unlink(res.zipPath.c_str());
        return res;
    }
    prog.total.store(total, std::memory_order_relaxed);
    if (prog.notify) prog.notify();
    res.err = writeZip(f, items, prog, &res.detail);
    if (fclose(f) != 0 && res.err == Err::None) {
        res.err = writeErr(errno);
        res.detail = strerror(errno);
    }
    if (res.err != Err::None) unlink(res.zipPath.c_str());
    return res;
}

}  // namespace nanoarc
