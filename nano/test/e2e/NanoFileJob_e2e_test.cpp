// The copy engine end to end on a scratch tree: folder copies and microSD moves (nanofilejob::run)
// and the File Explorer's copy / move / delete. `m gammaos_nano_filejob_tests`.

#include <gtest/gtest.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <map>
#include <set>
#include <string>

#include "NanoFileJob.h"
#include "nano_test_fs.h"

using namespace android::nanofilejob;
using nanotest::TempDir;

namespace {

std::string realDir(const TempDir& t) {
  char buf[PATH_MAX];
  return realpath(t.path().c_str(), buf) ? std::string(buf) : t.path();
}

void mkdirs(const std::string& p) {
  for (size_t i = 1; i <= p.size(); i++)
    if (i == p.size() || p[i] == '/') mkdir(p.substr(0, i).c_str(), 0755);
}

void put(const std::string& p, const std::string& text) {
  mkdirs(p.substr(0, p.find_last_of('/')));
  const int fd = open(p.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  ASSERT_GE(fd, 0) << p;
  ASSERT_EQ((ssize_t)text.size(), write(fd, text.data(), text.size()));
  close(fd);
}

std::string get(const std::string& p) {
  std::string out;
  const int fd = open(p.c_str(), O_RDONLY | O_NOFOLLOW);
  if (fd < 0) return "<none>";
  char buf[4096];
  for (ssize_t n; (n = read(fd, buf, sizeof(buf))) > 0;) out.append(buf, (size_t)n);
  close(fd);
  return out;
}

bool exists(const std::string& p) { struct stat st; return lstat(p.c_str(), &st) == 0; }
bool isDir(const std::string& p) { struct stat st; return lstat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode); }

// Files (not folders) anywhere under dir.
int countFiles(const std::string& dir) {
  int n = 0;
  if (DIR* d = opendir(dir.c_str())) {
    while (dirent* e = readdir(d)) {
      if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
      const std::string p = dir + "/" + e->d_name;
      n += isDir(p) ? countFiles(p) : 1;
    }
    closedir(d);
  }
  return n;
}

class FileJob : public ::testing::Test {
protected:
  void SetUp() override {
    root = realDir(tmp);
    src = root + "/internal/emu";
    card = root + "/card/emulator_config/emu";
    backup = root + "/internal/Nano Backup/emu";
    outside = root + "/system";
    put(src + "/a.sav", "AAAA");
    put(src + "/sub/b.sav", std::string(10000, 'b'));
    put(src + "/sub/deeper/c.cfg", "c=1\n");
    put(outside + "/b.sav", "system's own\n");
    put(outside + "/secret.conf", "psk=hunter2\n");
    mkdirs(root + "/card");
  }
  Pair movePair() const {
    Pair p{src, card};
    p.move = true;
    p.backup = backup;
    p.group = 7;
    return p;
  }
  TempDir tmp{"filejob"};
  std::string root, src, card, backup, outside;
};

}  // namespace

// ---- never overwrites ----

TEST_F(FileJob, CopyNeverOverwrites) {
  put(card + "/a.sav", "card's");
  const Result r = run({Pair{src, card}}, Hooks());
  EXPECT_EQ("card's", get(card + "/a.sav"));
  EXPECT_EQ(std::string(10000, 'b'), get(card + "/sub/b.sav"));
  EXPECT_EQ("c=1\n", get(card + "/sub/deeper/c.cfg"));
  EXPECT_EQ(3, r.counters.total);
  EXPECT_EQ(2, r.counters.hits);
  EXPECT_EQ(1, r.counters.skip);
  EXPECT_EQ(0, r.counters.fail);
  EXPECT_EQ("AAAA", get(src + "/a.sav"));   // a copy deletes nothing
  EXPECT_FALSE(exists(card + "/a.sav.nanocopy"));
}

TEST_F(FileJob, MoveKeepsTheCardsCopyAndBacksUpTheOriginal) {
  put(card + "/a.sav", "card's");
  int relocated = 0;
  Hooks h;
  h.relocate = [&](int group) { EXPECT_EQ(7, group); relocated++; return true; };
  const Result r = run({movePair()}, h);
  EXPECT_TRUE(r.committed);
  EXPECT_FALSE(r.incomplete);
  EXPECT_EQ(1, relocated);
  EXPECT_EQ("card's", get(card + "/a.sav"));
  EXPECT_EQ("AAAA", get(backup + "/a.sav"));
  ASSERT_EQ(1u, r.conflicts.size());
  EXPECT_EQ(backup + "/a.sav", r.conflicts[0].backup);
  EXPECT_EQ(card + "/a.sav", r.conflicts[0].dst);
  EXPECT_FALSE(r.conflicts[0].sameJob);
  EXPECT_EQ(6, r.conflicts[0].dstSize);
  EXPECT_EQ(std::string(10000, 'b'), get(card + "/sub/b.sav"));
  EXPECT_FALSE(exists(src));   // emptied and pruned
}

TEST_F(FileJob, ReplaceOlderOnlyReplacesWithANewerFile) {
  put(card + "/a.sav", "old");
  struct timespec ts[2] = {{1000, 0}, {1000, 0}};
  utimensat(AT_FDCWD, (card + "/a.sav").c_str(), ts, 0);
  Pair p{src, card};
  p.replaceOlder = true;
  run({p}, Hooks());
  EXPECT_EQ("AAAA", get(card + "/a.sav"));
}

// ---- rolls back ----

TEST_F(FileJob, FailedMoveRollsBackWhatItCreated) {
  Hooks h;
  std::atomic<int> writes{0};   // io's functions run on the copy threads
  h.io.write = [&](int fd, const void* b, size_t n) -> ssize_t {
    if (++writes == 2) { errno = EIO; return -1; }
    return write(fd, b, n);
  };
  const Result r = run({movePair()}, h);
  EXPECT_FALSE(r.committed);
  EXPECT_GE(r.counters.fail, 1);
  EXPECT_EQ(0, countFiles(root + "/card"));             // every file this job made there removed
  EXPECT_FALSE(exists(root + "/card/emulator_config"));  // and the folders it made
  EXPECT_EQ("AAAA", get(src + "/a.sav"));                // nothing deleted
  EXPECT_EQ(std::string(10000, 'b'), get(src + "/sub/b.sav"));
  EXPECT_EQ("c=1\n", get(src + "/sub/deeper/c.cfg"));
}

TEST_F(FileJob, CancelledMoveRollsBack) {
  put(card + "/keep.txt", "the card had this");
  Hooks h;
  int files = 0;
  h.progress = [&](const Counters& c) { files = c.done; };
  h.cancelled = [&]() { return files >= 1; };
  const Result r = run({movePair()}, h);
  EXPECT_FALSE(r.committed);
  EXPECT_EQ("the card had this", get(card + "/keep.txt"));
  EXPECT_EQ(1, countFiles(root + "/card"));
  EXPECT_EQ("AAAA", get(src + "/a.sav"));
  EXPECT_EQ(3, countFiles(src));
}

// ---- links ----

TEST_F(FileJob, LinkInsideTheSourceFailsTheMove) {
  ASSERT_EQ(0, symlink((outside + "/secret.conf").c_str(), (src + "/sub/evil.conf").c_str()));
  const Result r = run({movePair()}, Hooks());
  EXPECT_FALSE(r.committed);
  EXPECT_GE(r.counters.fail, 1);
  EXPECT_EQ("psk=hunter2\n", get(outside + "/secret.conf"));
  EXPECT_FALSE(exists(card + "/sub/evil.conf"));
  EXPECT_EQ(0, countFiles(root + "/card"));
  EXPECT_EQ(4, countFiles(src));   // the three files and the link, all still there
}

TEST_F(FileJob, LinkInsideACopyIsSkipped) {
  ASSERT_EQ(0, symlink((outside + "/secret.conf").c_str(), (src + "/evil.conf").c_str()));
  const Result r = run({Pair{src, card}}, Hooks());
  EXPECT_EQ(0, r.counters.fail);
  EXPECT_FALSE(exists(card + "/evil.conf"));
  EXPECT_EQ("AAAA", get(card + "/a.sav"));
}

TEST_F(FileJob, SourceThatIsALinkIsNeverMoved) {
  const std::string link = root + "/internal/linked";
  ASSERT_EQ(0, symlink(outside.c_str(), link.c_str()));
  Pair p = movePair();
  p.src = link;
  const Result r = run({p}, Hooks());
  EXPECT_FALSE(r.committed);
  EXPECT_GE(r.counters.fail, 1);
  EXPECT_EQ("psk=hunter2\n", get(outside + "/secret.conf"));
  EXPECT_EQ("system's own\n", get(outside + "/b.sav"));
  EXPECT_EQ(0, countFiles(root + "/card"));
}

TEST_F(FileJob, FolderSwappedForALinkBeforeDeletingDeletesNothingOutside) {
  // Between the copy and the deletes (while its program is pointed at the card) the app swaps
  // sub/ for a link to a system folder holding files of the same names and sizes, so only never
  // following a link keeps them.
  const std::string sys2 = root + "/system2";
  put(sys2 + "/b.sav", std::string(10000, 'z'));
  put(sys2 + "/deeper/c.cfg", "z=1\n");
  Hooks h;
  h.relocate = [&](int) {
    rename((src + "/sub").c_str(), (root + "/internal/sub.aside").c_str());
    return symlink(sys2.c_str(), (src + "/sub").c_str()) == 0;
  };
  const Result r = run({movePair()}, h);
  EXPECT_TRUE(r.committed);
  EXPECT_EQ(std::string(10000, 'z'), get(sys2 + "/b.sav"));
  EXPECT_EQ("z=1\n", get(sys2 + "/deeper/c.cfg"));
  EXPECT_TRUE(isDir(sys2 + "/deeper"));
  EXPECT_EQ(std::string(10000, 'b'), get(card + "/sub/b.sav"));
  EXPECT_FALSE(exists(src + "/a.sav"));   // the rest went as it should
}

TEST_F(FileJob, SourceOutsideItsRootIsRefused) {
  Pair p = movePair();
  p.srcRoot = root + "/card";   // the emulator's allowed folder, which src is not in
  const Result r = run({p}, Hooks());
  EXPECT_FALSE(r.committed);
  EXPECT_GE(r.counters.fail, 1);
  EXPECT_EQ(3, countFiles(src));
  p.srcRoot = root + "/internal";
  const Result ok = run({p}, Hooks());
  EXPECT_TRUE(ok.committed);
  EXPECT_FALSE(exists(src + "/a.sav"));
}

// ---- deleting only what is on the card ----

TEST_F(FileJob, MissingDestinationAtDeleteKeepsTheSource) {
  Hooks h;
  h.relocate = [&](int) { return unlink((card + "/sub/b.sav").c_str()) == 0; };
  const Result r = run({movePair()}, h);
  EXPECT_TRUE(r.committed);
  EXPECT_TRUE(r.incomplete);
  EXPECT_GE(r.counters.fail, 1);
  EXPECT_EQ(std::string(10000, 'b'), get(src + "/sub/b.sav"));
  EXPECT_FALSE(exists(src + "/a.sav"));
}

TEST_F(FileJob, TruncatedDestinationAtDeleteKeepsTheSource) {
  Hooks h;
  h.relocate = [&](int) { return truncate((card + "/sub/b.sav").c_str(), 100) == 0; };
  const Result r = run({movePair()}, h);
  EXPECT_TRUE(r.incomplete);
  EXPECT_EQ(std::string(10000, 'b'), get(src + "/sub/b.sav"));
}

TEST_F(FileJob, FailedRelocationKeepsEverythingAndRemovesTheCopies) {
  Hooks h;
  h.relocate = [&](int) { return false; };
  const Result r = run({movePair()}, h);
  ASSERT_EQ(1u, r.failedGroups.size());
  EXPECT_EQ(3, countFiles(src));
  EXPECT_EQ(0, countFiles(card));
}

// ---- room ----

TEST_F(FileJob, SpaceCheckStopsBeforeAnythingIsCopied) {
  Hooks h;
  bool stopped = false;
  h.statvfs = [](const char*, struct statvfs* v) {
    memset(v, 0, sizeof(*v));
    v->f_bsize = v->f_frsize = 4096;
    v->f_bavail = 100;   // 400 KB: less than the 8 MB kept free
    return 0;
  };
  h.stopPrograms = [&]() { stopped = true; return true; };
  const Result r = run({movePair()}, h);
  EXPECT_FALSE(r.committed);
  EXPECT_EQ(1, r.counters.fail);
  EXPECT_NE(std::string::npos, r.counters.error.find("Not enough space")) << r.counters.error;
  EXPECT_FALSE(stopped);   // no program was closed for nothing
  EXPECT_EQ(0, countFiles(root + "/card"));
  EXPECT_EQ(3, countFiles(src));
}

TEST_F(FileJob, SpaceCheckPassesWithRoom) {
  Hooks h;
  h.statvfs = [](const char*, struct statvfs* v) {
    memset(v, 0, sizeof(*v));
    v->f_bsize = v->f_frsize = 4096;
    v->f_bavail = 4096;   // 16 MB
    return 0;
  };
  const Result r = run({movePair()}, h);
  EXPECT_TRUE(r.committed);
  EXPECT_EQ(0, r.counters.fail);
}

// ---- H5: a short copy never deletes the source ----

TEST_F(FileJob, ErrorAtFlushKeepsTheSource) {
  Hooks h;
  int syncs = 0;
  h.io.syncfs = [&](int) { syncs++; errno = EIO; return -1; };   // the write error a card reports only on flush
  int relocated = 0;
  h.relocate = [&](int) { relocated++; return true; };
  const Result r = run({movePair()}, h);
  EXPECT_EQ(1, syncs);
  EXPECT_FALSE(r.committed);
  EXPECT_EQ(0, relocated);   // the program is never pointed at copies that may not be there
  EXPECT_GE(r.counters.fail, 1);
  EXPECT_EQ(3, countFiles(src));
  EXPECT_EQ(0, countFiles(root + "/card"));
}

TEST_F(FileJob, FullDestinationStopsTheMove) {
  Hooks h;
  h.io.write = [](int fd, const void* b, size_t n) -> ssize_t {
    if (n > 1000) {   // half of it, then the card is full
      const ssize_t w = write(fd, b, n / 2);
      (void)w;
      errno = ENOSPC;
      return -1;
    }
    return write(fd, b, n);
  };
  const Result r = run({movePair()}, h);
  EXPECT_FALSE(r.committed);
  EXPECT_NE(std::string::npos, r.counters.error.find("full")) << r.counters.error;
  EXPECT_EQ(3, countFiles(src));
  EXPECT_FALSE(exists(card + "/sub/b.sav"));
  EXPECT_FALSE(exists(card + "/sub/b.sav.nanocopy"));
}

TEST_F(FileJob, SilentShortWriteIsCaughtBySize) {
  Hooks h;
  h.io.write = [](int fd, const void* b, size_t n) -> ssize_t {
    const ssize_t w = write(fd, b, n > 100 ? 100 : n);   // claims all of it
    return w < 0 ? w : (ssize_t)n;
  };
  const Result r = run({movePair()}, h);
  EXPECT_FALSE(r.committed);
  EXPECT_EQ(std::string(10000, 'b'), get(src + "/sub/b.sav"));
  EXPECT_FALSE(exists(card + "/sub/b.sav"));
}

// ---- folders ----

TEST_F(FileJob, MoveKeepsEmptyFolders) {
  mkdirs(src + "/states/empty");
  mkdirs(src + "/cheats");
  const Result r = run({movePair()}, Hooks());
  EXPECT_TRUE(r.committed);
  EXPECT_TRUE(isDir(card + "/states/empty"));
  EXPECT_TRUE(isDir(card + "/cheats"));
  EXPECT_FALSE(exists(src));
}

TEST_F(FileJob, SkipDirsAndMergedStay) {
  put(src + "/bios/scph.bin", "bios");
  Pair p = movePair();
  p.skipDirs = {"bios"};
  p.merged = {"sub/deeper/c.cfg"};
  const Result r = run({p}, Hooks());
  EXPECT_TRUE(r.committed);
  EXPECT_FALSE(exists(card + "/bios"));
  EXPECT_EQ("bios", get(src + "/bios/scph.bin"));
  EXPECT_FALSE(exists(card + "/sub/deeper/c.cfg"));
  EXPECT_FALSE(exists(src + "/sub/deeper/c.cfg"));   // its data is in another file of the job
}

// ---- File Explorer ----

TEST_F(FileJob, ExplorerMoveSkipsSpecialFilesAndKeepsThem) {
  ASSERT_EQ(0, mkfifo((src + "/sub/pipe").c_str(), 0644));
  ASSERT_TRUE(treeMove(src, root + "/moved"));
  EXPECT_EQ("AAAA", get(root + "/moved/a.sav"));
  EXPECT_FALSE(exists(root + "/moved/sub/pipe"));
  struct stat st;
  ASSERT_EQ(0, lstat((src + "/sub/pipe").c_str(), &st));   // not copied, so not deleted
  EXPECT_TRUE(S_ISFIFO(st.st_mode));
  EXPECT_FALSE(exists(src + "/a.sav"));
}

TEST_F(FileJob, ExplorerMoveKeepsTheSourceWhenTheFlushFails) {
  CopyIo io;
  io.syncfs = [](int) { errno = EIO; return -1; };
  EXPECT_FALSE(treeMove(src, root + "/moved", &io));
  EXPECT_EQ(3, countFiles(src));
}

// Many small files: no flush per file (each one flushes a card's cache), one per drive before the deletes.
TEST_F(FileJob, ManyFilesAreFlushedOncePerDrive) {
  for (int i = 0; i < 300; i++) put(src + "/media/" + std::to_string(i) + ".png", "img" + std::to_string(i));
  Hooks h;
  std::atomic<int> fsyncs{0};
  int syncs = 0;
  h.io.fsync = [&](int fd) { fsyncs++; return fsync(fd); };
  h.io.syncfs = [&](int fd) { syncs++; return syncfs(fd); };
  const Result r = run({movePair()}, h);
  EXPECT_TRUE(r.committed);
  EXPECT_EQ(0, fsyncs);
  EXPECT_EQ(1, syncs);
  EXPECT_EQ(303, countFiles(card));
  EXPECT_FALSE(exists(src));

  CopyIo io;
  int feFsyncs = 0, feSyncs = 0;
  io.fsync = [&](int fd) { feFsyncs++; return fsync(fd); };
  io.syncfs = [&](int fd) { feSyncs++; return syncfs(fd); };
  ASSERT_TRUE(treeMove(card, root + "/moved", &io));
  EXPECT_EQ(0, feFsyncs);
  EXPECT_EQ(1, feSyncs);
  EXPECT_EQ(303, countFiles(root + "/moved"));
}

TEST_F(FileJob, BackupToAnotherDriveIsFlushedBeforeTheOriginalGoes) {
  // A copy (not a rename) to the backup: flushed by itself, as the original goes right after.
  struct stat a, b;
  if (stat("/dev/shm", &b) != 0 || stat(root.c_str(), &a) != 0 || a.st_dev == b.st_dev)
    GTEST_SKIP() << "needs /dev/shm on another filesystem";
  char tmpl[] = "/dev/shm/nano_filejob_bak_XXXXXX";
  ASSERT_NE(nullptr, mkdtemp(tmpl));
  const std::string bak = tmpl;
  put(card + "/a.sav", "card's");
  Hooks h;
  int fsyncs = 0;
  h.io.fsync = [&](int) { fsyncs++; errno = EIO; return -1; };
  Pair p = movePair();
  p.backup = bak + "/emu";
  const Result r = run({p}, h);
  EXPECT_EQ(1, fsyncs);
  EXPECT_TRUE(r.incomplete);
  EXPECT_EQ("AAAA", get(src + "/a.sav"));   // the backup failed: nothing deleted
  EXPECT_EQ(3, countFiles(src));
  EXPECT_FALSE(exists(bak + "/emu/a.sav"));
  rmdir((bak + "/emu").c_str());
  rmdir(bak.c_str());
}

// The audit's case: a file-size limit standing in for a full card. The old copy (stdio, close
// unchecked) reported success with 1000 of 3000 bytes, and a Move then deleted the source.
TEST_F(FileJob, ExplorerMoveKeepsTheSourceWhenTheDriveFills) {
  put(root + "/src.sav", std::string(3000, 'x'));
  const pid_t pid = fork();
  ASSERT_GE(pid, 0);
  if (pid == 0) {
    signal(SIGXFSZ, SIG_IGN);
    struct rlimit rl = {1000, 1000};
    setrlimit(RLIMIT_FSIZE, &rl);
    _exit(treeMove(root + "/src.sav", root + "/dst.sav") ? 1 : 0);
  }
  int status = 0;
  ASSERT_EQ(pid, waitpid(pid, &status, 0));
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(0, WEXITSTATUS(status));   // reported as failed
  EXPECT_EQ(std::string(3000, 'x'), get(root + "/src.sav"));
  EXPECT_FALSE(exists(root + "/dst.sav"));
}

TEST_F(FileJob, ExplorerMoveCopiesLinksAsLinks) {
  ASSERT_EQ(0, symlink((outside + "/secret.conf").c_str(), (src + "/link").c_str()));
  ASSERT_TRUE(treeMove(src, root + "/moved"));
  char buf[PATH_MAX] = {};
  ASSERT_GT(readlink((root + "/moved/link").c_str(), buf, sizeof(buf) - 1), 0);
  EXPECT_EQ(outside + "/secret.conf", std::string(buf));
  EXPECT_EQ("psk=hunter2\n", get(outside + "/secret.conf"));
}

TEST_F(FileJob, ExplorerCopyNeverOverwrites) {
  put(root + "/dst/a.sav", "mine");
  EXPECT_FALSE(copyFile(src + "/a.sav", root + "/dst/a.sav", COPY_EXCL_DIRECT));
  EXPECT_EQ("mine", get(root + "/dst/a.sav"));
}

TEST_F(FileJob, ExplorerDeleteNeverFollowsALink) {
  ASSERT_EQ(0, symlink(outside.c_str(), (src + "/sub/sys").c_str()));
  ASSERT_TRUE(treeRemove(src));
  EXPECT_FALSE(exists(src));
  EXPECT_EQ("psk=hunter2\n", get(outside + "/secret.conf"));
}

TEST_F(FileJob, RenameNeverReplaces) {
  put(root + "/x", "x");
  put(root + "/y", "y");
  EXPECT_EQ(EEXIST, renameNoReplace(root + "/x", root + "/y"));
  EXPECT_EQ("y", get(root + "/y"));
  EXPECT_EQ(0, renameNoReplace(root + "/x", root + "/z"));
  EXPECT_EQ("x", get(root + "/z"));
}

TEST_F(FileJob, OpenNoFollowRefusesALinkAnywhere) {
  ASSERT_EQ(0, symlink(outside.c_str(), (root + "/lnk").c_str()));
  EXPECT_LT(openNoFollow(root + "/lnk/secret.conf", O_RDONLY), 0);
  EXPECT_LT(openNoFollow(root + "/lnk", O_RDONLY), 0);
  const int fd = openNoFollow(outside + "/secret.conf", O_RDONLY);
  EXPECT_GE(fd, 0);
  if (fd >= 0) close(fd);
  EXPECT_LT(openNoFollow(outside + "/../system/secret.conf", O_RDONLY), 0);
}

// ---- several files at once ----

namespace {

// The path an open file was opened as.
std::string fdPath(int fd) {
  char buf[PATH_MAX];
  const ssize_t n = readlink(("/proc/self/fd/" + std::to_string(fd)).c_str(), buf, sizeof(buf) - 1);
  return n > 0 ? std::string(buf, (size_t)n) : std::string();
}

bool endsWith(const std::string& s, const std::string& tail) {
  return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}

// Every file under dir (relative path -> contents).
void listFiles(const std::string& dir, const std::string& rel, std::map<std::string, std::string>* out) {
  if (DIR* d = opendir(dir.c_str())) {
    while (dirent* e = readdir(d)) {
      if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
      const std::string p = dir + "/" + e->d_name, r = rel.empty() ? e->d_name : rel + "/" + e->d_name;
      if (isDir(p)) listFiles(p, r, out);
      else (*out)[r] = get(p);
    }
    closedir(d);
  }
}
std::map<std::string, std::string> listFiles(const std::string& dir) {
  std::map<std::string, std::string> out;
  listFiles(dir, std::string(), &out);
  return out;
}

std::string bigText(size_t n, char seed) {
  std::string s(n, '\0');
  for (size_t i = 0; i < n; i++) s[i] = (char)(seed + (i * 7919) % 251);
  return s;
}

// RetroArch's shaders in small: many small files over many folders, and a few big ones.
void putMany(const std::string& dir, int files) {
  for (int i = 0; i < files; i++)
    put(dir + "/shaders/set" + std::to_string(i % 23) + "/p" + std::to_string(i % 5) + "/s" + std::to_string(i) + ".slang",
        "shader " + std::to_string(i) + std::string((size_t)(i % 97), 'x'));
  put(dir + "/big/one.bin", bigText(3 << 20, 'a'));
  put(dir + "/big/two.bin", bigText((1 << 20) + 17, 'b'));
  put(dir + "/shaders/set0/huge.png", bigText(600 << 10, 'c'));
}

}  // namespace

TEST_F(FileJob, ManySmallAndAFewBigFilesMoveWhole) {
  putMany(src, 1200);
  const auto before = listFiles(src);
  ASSERT_EQ(1206u, before.size());
  Hooks h;
  h.copyThreads = 4;
  int lastDone = 0, total = -1;
  bool monotonic = true;
  h.progress = [&](const Counters& c) {
    if (c.done < lastDone || c.done > c.total || (total >= 0 && c.total != total)) monotonic = false;
    lastDone = c.done;
    total = c.total;
  };
  const Result r = run({movePair()}, h);
  EXPECT_TRUE(r.committed);
  EXPECT_FALSE(r.incomplete);
  EXPECT_TRUE(monotonic);
  EXPECT_EQ(1206, r.counters.total);
  EXPECT_EQ(1206, r.counters.done);
  EXPECT_EQ(1206, r.counters.hits);
  EXPECT_EQ(0, r.counters.fail);
  EXPECT_TRUE(listFiles(card) == before);   // every file, whole, and no .nanocopy left
  EXPECT_FALSE(exists(src));
}

// An interrupted copy's temporary files in the source: never copied (a copy of x.nanocopy and x's
// own temporary file are one file), and a move deletes them with the folder.
TEST_F(FileJob, LeftoverTempFilesInTheSourceAreJunk) {
  for (int i = 0; i < 24; i++) {
    put(src + "/s/f" + std::to_string(i), "file " + std::to_string(i));
    put(src + "/s/f" + std::to_string(i) + ".nanocopy", "partial");
  }
  put(src + "/a.sav.nanocopy", "AA");
  put(src + "/only/x.nanocopy", "partial");
  for (int threads : {1, 4, 8}) {
    TempDir t{"filejob_junk"};
    const std::string c = realDir(t) + "/card";
    Hooks h;
    h.copyThreads = threads;
    const Result rc = run({Pair{src, c}}, h);
    EXPECT_EQ(27, rc.counters.total) << threads;
    EXPECT_EQ(0, rc.counters.fail) << threads;
    EXPECT_EQ("AAAA", get(c + "/a.sav")) << threads;
    EXPECT_EQ("file 5", get(c + "/s/f5")) << threads;
    for (const auto& f : listFiles(c)) EXPECT_FALSE(endsWith(f.first, ".nanocopy")) << threads << " " << f.first;
    EXPECT_TRUE(isDir(c + "/only")) << threads;
    EXPECT_EQ("partial", get(src + "/s/f5.nanocopy")) << threads;   // a copy deletes nothing
  }
  for (int threads : {1, 4}) {
    TempDir t{"filejob_junk_move"};
    const std::string s = realDir(t) + "/emu", c = realDir(t) + "/card";
    for (const auto& f : listFiles(src)) put(s + "/" + f.first, f.second);
    Pair p{s, c};
    p.move = true;
    p.backup = realDir(t) + "/backup";
    Hooks h;
    h.copyThreads = threads;
    const Result r = run({p}, h);
    EXPECT_TRUE(r.committed) << threads;
    EXPECT_FALSE(r.incomplete) << threads;
    EXPECT_EQ(0, r.counters.fail) << threads << " " << r.counters.error;
    EXPECT_EQ(27, r.counters.hits) << threads;
    for (const auto& f : listFiles(c)) EXPECT_FALSE(endsWith(f.first, ".nanocopy")) << threads << " " << f.first;
    EXPECT_EQ("file 23", get(c + "/s/f23")) << threads;
    EXPECT_TRUE(isDir(c + "/only")) << threads;
    EXPECT_FALSE(exists(s)) << threads;
  }
}

// Only a regular file ending exactly in ".nanocopy" is junk, and only a move that goes through
// deletes it.
TEST_F(FileJob, TempNamesAreJunkOnlyWhenTheMoveGoesThrough) {
  put(src + "/a.sav.nanocopy", "partial");
  {
    Hooks h;
    h.relocate = [](int) { return false; };
    run({movePair()}, h);
    EXPECT_EQ("partial", get(src + "/a.sav.nanocopy"));
  }
  {
    Hooks h;
    h.io.write = [](int fd, const void* b, size_t n) -> ssize_t {
      if (endsWith(fdPath(fd), "/c.cfg.nanocopy")) { errno = EIO; return -1; }
      return write(fd, b, n);
    };
    const Result r = run({movePair()}, h);
    EXPECT_FALSE(r.committed);
    EXPECT_EQ("partial", get(src + "/a.sav.nanocopy"));
  }
  {
    TempDir t{"filejob_junk_link"};
    const std::string s = realDir(t) + "/emu";
    put(s + "/a.sav", "AAAA");
    put(s + "/a.sav.nanocopy", "partial");
    ASSERT_EQ(0, symlink((outside + "/secret.conf").c_str(), (s + "/l.nanocopy").c_str()));
    Pair p{s, realDir(t) + "/card"};
    p.move = true;
    p.backup = realDir(t) + "/backup";
    const Result r = run({p}, Hooks());
    EXPECT_FALSE(r.committed);
    EXPECT_EQ("partial", get(s + "/a.sav.nanocopy"));
    EXPECT_EQ("psk=hunter2\n", get(outside + "/secret.conf"));
    struct stat st;
    EXPECT_EQ(0, lstat((s + "/l.nanocopy").c_str(), &st));
  }
  put(src + "/d.nanocopy/f", "in a folder");
  put(src + "/nanocopy", "1");
  put(src + "/.nanocopy", "2");
  put(src + "/x.nanocopy.bak", "3");
  put(src + "/x.NANOCOPY", "4");
  put(card + "/sub/b.sav.nanocopy", "the card's stale temporary file");
  const Result r = run({movePair()}, Hooks());
  EXPECT_TRUE(r.committed);
  EXPECT_FALSE(r.incomplete);
  EXPECT_EQ(0, r.counters.fail) << r.counters.error;
  EXPECT_EQ("in a folder", get(card + "/d.nanocopy/f"));
  EXPECT_EQ("1", get(card + "/nanocopy"));
  EXPECT_EQ("2", get(card + "/.nanocopy"));
  EXPECT_EQ("3", get(card + "/x.nanocopy.bak"));
  EXPECT_EQ("4", get(card + "/x.NANOCOPY"));
  EXPECT_EQ(std::string(10000, 'b'), get(card + "/sub/b.sav"));
  EXPECT_FALSE(exists(card + "/sub/b.sav.nanocopy"));
  EXPECT_FALSE(exists(card + "/a.sav.nanocopy"));
  EXPECT_FALSE(exists(src));
}

TEST_F(FileJob, CancelledPartWayMoveGivesEverythingBack) {
  putMany(src, 600);
  put(card + "/keep.txt", "the card had this");
  const auto before = listFiles(src);
  Hooks h;
  h.copyThreads = 4;
  int done = 0;
  h.progress = [&](const Counters& c) { done = c.done; };
  h.cancelled = [&]() { return done >= 50; };
  const Result r = run({movePair()}, h);
  EXPECT_FALSE(r.committed);
  EXPECT_GE(r.counters.done, 50);
  EXPECT_LT(r.counters.done, r.counters.total);
  EXPECT_TRUE(listFiles(src) == before);
  const auto left = listFiles(root + "/card");
  ASSERT_EQ(1u, left.size());
  EXPECT_EQ("the card had this", left.begin()->second);
}

TEST_F(FileJob, CancelledPartWayCopyLeavesOnlyWholeFiles) {
  putMany(src, 600);
  const auto before = listFiles(src);
  Hooks h;
  h.copyThreads = 8;
  int done = 0;
  h.progress = [&](const Counters& c) { done = c.done; };
  h.cancelled = [&]() { return done >= 100; };
  const Result r = run({Pair{src, card}}, h);
  EXPECT_LT(r.counters.done, r.counters.total);
  const auto copied = listFiles(card);
  EXPECT_GE((int)copied.size(), r.counters.hits);   // every counted copy is there
  EXPECT_LT(copied.size(), before.size());
  for (const auto& f : copied) {
    ASSERT_TRUE(before.count(f.first)) << f.first;   // no .nanocopy
    EXPECT_TRUE(before.at(f.first) == f.second) << f.first;
  }
}

TEST_F(FileJob, OneFailingFileKeepsEveryOriginal) {
  putMany(src, 800);
  put(src + "/shaders/set7/bad.slang", "unlucky");
  const auto before = listFiles(src);
  for (int threads : {1, 4, 8}) {
    Hooks h;
    h.copyThreads = threads;
    h.io.write = [](int fd, const void* b, size_t n) -> ssize_t {
      if (endsWith(fdPath(fd), "/bad.slang.nanocopy")) { errno = EIO; return -1; }
      return write(fd, b, n);
    };
    int relocated = 0;
    h.relocate = [&](int) { relocated++; return true; };
    const Result r = run({movePair()}, h);
    EXPECT_FALSE(r.committed) << threads;
    EXPECT_EQ(0, relocated) << threads;
    EXPECT_EQ(1, r.counters.fail) << threads;
    EXPECT_EQ(r.counters.total, r.counters.done) << threads;
    EXPECT_EQ(r.counters.total - 1, r.counters.hits) << threads;
    EXPECT_EQ("Some files could not be copied.", r.counters.error) << threads;
    EXPECT_TRUE(listFiles(src) == before) << threads;
    EXPECT_EQ(0, countFiles(root + "/card")) << threads;
    EXPECT_FALSE(exists(root + "/card/emulator_config")) << threads;
  }
}

TEST_F(FileJob, FullDestinationStopsAtTheSameFileAtAnyThreadCount) {
  putMany(src, 500);
  const auto before = listFiles(src);
  Counters first;
  for (int threads : {1, 2, 4, 8, 8, 8}) {
    Hooks h;
    h.copyThreads = threads;
    h.io.write = [](int fd, const void* b, size_t n) -> ssize_t {
      if (endsWith(fdPath(fd), "/s250.slang.nanocopy")) { errno = ENOSPC; return -1; }
      return write(fd, b, n);
    };
    const Result r = run({movePair()}, h);
    EXPECT_FALSE(r.committed);
    EXPECT_NE(std::string::npos, r.counters.error.find("full")) << r.counters.error;
    EXPECT_LT(r.counters.done, r.counters.total);
    EXPECT_EQ(1, r.counters.fail);
    EXPECT_TRUE(listFiles(src) == before) << threads;
    EXPECT_EQ(0, countFiles(root + "/card")) << threads;
    if (threads == 1) { first = r.counters; continue; }
    EXPECT_EQ(first.done, r.counters.done) << threads;
    EXPECT_EQ(first.hits, r.counters.hits) << threads;
    EXPECT_EQ(first.skip, r.counters.skip) << threads;
  }
}

// A mixed job (kept, merged, failing, and two pairs writing one file) ends the same at every
// thread count.
TEST(FileJobThreads, ResultsAreTheSameAtAnyThreadCount) {
  auto shader = [](int k) {
    return "shaders/set" + std::to_string(k % 23) + "/p" + std::to_string(k % 5) + "/s" + std::to_string(k) + ".slang";
  };
  std::map<std::string, std::string> firstFiles;
  Counters first;
  for (int threads : {1, 2, 4, 8, 8, 8}) {
    TempDir t{"filejob_threads"};   // the same tree, made the same way, each round
    const std::string root = realDir(t), src = root + "/internal/emu", other = root + "/internal/other",
                      card = root + "/card/emu";
    putMany(src, 400);
    put(src + "/shaders/set3/bad.slang", "unlucky");
    for (int i = 0; i < 60; i++) put(other + "/" + shader(i * 5), "other's");
    for (int i = 0; i < 40; i++) put(card + "/" + shader(i * 23 + 1), "card's");
    Pair a{src, card};
    a.merged = {shader(2)};
    Pair b{other, card};
    Hooks h;
    h.copyThreads = threads;
    h.io.write = [](int fd, const void* buf, size_t n) -> ssize_t {
      if (endsWith(fdPath(fd), "/bad.slang.nanocopy")) { errno = EIO; return -1; }
      return write(fd, buf, n);
    };
    const Result r = run({a, b}, h);
    const auto files = listFiles(card);
    if (threads == 1) {
      first = r.counters;
      firstFiles = files;
      EXPECT_EQ(1, first.fail);
      EXPECT_EQ("Some files could not be copied.", first.error);
      EXPECT_GE(first.skip, 60 + 18);   // every file of b (a's came first), and the card's
      EXPECT_EQ("card's", get(card + "/" + shader(24)));
      EXPECT_EQ("shader 10", get(card + "/" + shader(10)).substr(0, 9));
      EXPECT_FALSE(exists(card + "/" + shader(2)));
      EXPECT_FALSE(exists(card + "/shaders/set3/bad.slang"));
      continue;
    }
    EXPECT_EQ(first.total, r.counters.total) << threads;
    EXPECT_EQ(first.done, r.counters.done) << threads;
    EXPECT_EQ(first.hits, r.counters.hits) << threads;
    EXPECT_EQ(first.skip, r.counters.skip) << threads;
    EXPECT_EQ(first.fail, r.counters.fail) << threads;
    EXPECT_EQ(first.error, r.counters.error) << threads;
    EXPECT_TRUE(firstFiles == files) << threads;
  }
}

// Two moved folders into one (SeedlessDS's backup/ and savestates/): a name both have is copied once,
// from the first, and the second's goes to the backup unless it is the same file.
TEST(FileJobThreads, TwoMovesIntoOneFolderAtAnyThreadCount) {
  for (int threads : {1, 4, 8}) {
    TempDir t{"filejob_twomoves"};
    const std::string root = realDir(t), a = root + "/user/backup", b = root + "/user/savestates",
                      card = root + "/card/seedlessds", bak = root + "/Nano Backup/seedlessds";
    for (int i = 0; i < 120; i++) put(a + "/g" + std::to_string(i) + ".dsv", "save " + std::to_string(i));
    for (int i = 0; i < 120; i++) put(b + "/g" + std::to_string(i) + "_0.dss", "state " + std::to_string(i));
    put(a + "/clash.dsv", "a's");
    put(b + "/clash.dsv", "b's");
    put(a + "/same.dsv", "one");
    put(b + "/same.dsv", "one");
    std::vector<Pair> pairs;
    for (const std::string& s : {a, b}) {
      Pair p{s, card};
      p.move = true;
      p.backup = bak;
      p.group = 13;
      pairs.push_back(p);
    }
    Hooks h;
    h.copyThreads = threads;
    const Result r = run(pairs, h);
    EXPECT_TRUE(r.committed) << threads;
    EXPECT_FALSE(r.incomplete) << threads;
    EXPECT_EQ(0, r.counters.fail) << threads;
    EXPECT_EQ(244, r.counters.total) << threads;
    EXPECT_EQ(243, r.counters.hits) << threads;
    EXPECT_EQ(1, r.counters.skip) << threads;
    EXPECT_EQ("a's", get(card + "/clash.dsv")) << threads;
    EXPECT_EQ("one", get(card + "/same.dsv")) << threads;
    EXPECT_EQ("save 7", get(card + "/g7.dsv")) << threads;
    EXPECT_EQ("state 7", get(card + "/g7_0.dss")) << threads;
    ASSERT_EQ(1u, r.conflicts.size()) << threads;
    EXPECT_EQ(bak + "/clash.dsv", r.conflicts[0].backup) << threads;
    EXPECT_TRUE(r.conflicts[0].sameJob) << threads;
    EXPECT_EQ("b's", get(bak + "/clash.dsv")) << threads;
    EXPECT_EQ(242, countFiles(card)) << threads;
    EXPECT_FALSE(exists(a)) << threads;
    EXPECT_FALSE(exists(b)) << threads;
  }
}
