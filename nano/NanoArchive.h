// Archive reading, extraction and zip creation for the File Explorer.
//
// Zip goes through libziparchive (read) and ZipWriter (create, deflate); 7z through the LZMA SDK
// decoder in liblzma. Everything here is self-contained and touches no NanoMenu state, so it can
// run on a detached worker: the caller shares a Progress block with it and nothing else.
//
// Archives are untrusted input. Entry names that are absolute or contain ".." are skipped, links
// are skipped, files are only ever created (O_CREAT|O_EXCL|O_NOFOLLOW) through directories opened
// without following links, an entry is never written past its declared size, and a 7z solid block
// (which the SDK decodes whole into memory) larger than k7zBlockCap is refused up front, and the
// decoder's memory as a whole is capped just above that.

#pragma once

#include <stdint.h>
#include <atomic>
#include <string>
#include <vector>

namespace nanoarc {

enum class Format { None, Zip, SevenZip, Rar };
Format formatFromName(const std::string& name);   // by extension

enum class Err {
    None,
    Open,          // the archive or a source could not be opened
    Corrupt,       // damaged, not a valid archive, or an entry did not match its declared size
    NameEncoding,  // a zip whose names are not UTF-8 (CP437, Shift-JIS...), which libziparchive refuses
    Unsupported,   // a 7z whose file list needs a method the decoder lacks (usually encrypted names)
    Rar,
    Encrypted,     // every selected file is password-protected
    Method,        // every selected file uses an unsupported compression method
    BlockTooBig,   // a 7z solid block over k7zBlockCap; nothing was written
    NoMemory,      // the 7z decoder hit its overall memory cap part way
    NoSpaceCheck,  // the free-space pre-check failed; nothing was written
    NoSpace,       // the destination filled up part way
    Write,
    Read,
    NoFreeName,
    TooBig,        // over ZipWriter's limits (no zip64): 4 GB, 65,535 entries
    BadName,       // a name ZipWriter cannot store (not valid UTF-8)
    TmpfsDest,     // the zip would land on a RAM-backed folder (a storage root or a share's parent)
    Link,          // Compress was asked to zip a link
    Cancelled,
};

constexpr uint64_t k7zBlockCap = 512ull << 20;

// Shared between the worker and the UI. total stays 0 until the work is sized.
struct Progress {
    std::atomic<uint64_t> done{0};
    std::atomic<uint64_t> total{0};
    std::atomic<bool> cancel{false};
    void (*notify)() = nullptr;   // called from the worker whenever done moves
};

struct Entry { std::string path; uint64_t size = 0; bool isDir = false; bool isLink = false; };
struct Index { Err err = Err::None; std::vector<Entry> entries; };

// The archive's entries with cleaned names ("a/b", no trailing slash). Unsafe names are left out.
Index readIndex(const std::string& archive);
// The same for an archive whose name does not give its format (a comic book's .cbz is a zip).
Index readIndex(const std::string& archive, Format fmt);

// One file entry (a path as readIndex gives it) read into memory, for a caller that shows it rather
// than extracting it: a comic book manual's page. Nothing is read when the entry is over maxBytes
// (TooBig) or a 7z would decode a solid block over maxBlock to reach it (BlockTooBig; k7zBlockCap
// applies too). Link for a link, Encrypted or Method for an entry the decoder can't read, Open when
// there is no such file, Corrupt when it does not match its declared size; data is empty then.
struct EntryData { Err err = Err::None; std::vector<uint8_t> data; };
EntryData readEntry(const std::string& archive, Format fmt, const std::string& inner, uint64_t maxBytes,
                    uint64_t maxBlock);

struct ExtractResult {
    Err err = Err::None;
    std::string dest;     // where it went: the created folder when newFolder was set
    std::string detail;   // for Write: the file and the reason
    int extracted = 0, existing = 0, unsafe = 0, links = 0, encrypted = 0, unsupported = 0;
    int badNames = 0;          // names the destination's filesystem refuses (exFAT "a:b", too long)
    bool rolledBack = false;   // newFolder: a failure or cancel removed everything this run wrote
    uint64_t needBytes = 0, freeBytes = 0;   // NoSpaceCheck; BlockTooBig puts the block size in needBytes
};

// Extracts `inner` (an entry path from readIndex, file or folder; "" = everything) from the archive.
// A selected file or folder lands under its own name, so "a/b" extracts as dest/b. With newFolder,
// dest is created, taking the first free name of "dest", "dest (2)", ...; if that extraction then
// fails or is cancelled, the folder and everything this run put in it are removed again. Otherwise
// dest must exist and existing files in it are skipped, never overwritten.
ExtractResult extract(const std::string& archive, const std::string& inner, const std::string& dest,
                      bool newFolder, Progress& prog);

struct CompressResult {
    Err err = Err::None;
    std::string zipPath;
    std::string detail;   // the file a Read / BadName / Write error is about
    int links = 0;        // links inside a folder are skipped: ZipWriter cannot store them
};

// Zips a file or folder (deflate) to "<name>.zip" beside it, or "<name> (2).zip", ... if taken.
CompressResult compressToZip(const std::string& src, Progress& prog);

}  // namespace nanoarc
