//! Downloads with resume and sha256 checks, local imports, the cache, and
//! finding the Thimsy package among the user's files.

use crate::catalog::{Artifact, Package, Source};
use crate::error::{Code, EngineError};
use crate::events::Reporter;
use crate::util;
use anyhow::{Context, Result};
use serde::{Deserialize, Serialize};
use std::fs::{self, File, OpenOptions};
use std::io::{Read, Write};
use std::path::{Path, PathBuf};
use std::time::Duration;

/// Where verified files live: `<workspace>/cache/<file>`, with a
/// `<file>.ok` marker recording the hash it was verified against (or, for
/// a downloaded package, that its download finished).
/// `imports` are folders to look in, or files the user picked.
pub struct Cache {
    pub dir: PathBuf,
    pub imports: Vec<PathBuf>,
    /// Longest wait for the server's answer or for the next piece of a
    /// download before the attempt counts as stalled (and is resumed).
    pub read_timeout: Duration,
    /// Pause before retrying a download that made no progress (times the
    /// attempt number).
    pub retry_delay: Duration,
}

/// A file called `name` of `size` bytes among `imports`: picked directly,
/// or inside an import folder.
pub fn find_in(imports: &[PathBuf], name: &str, size: u64) -> Option<PathBuf> {
    imports.iter().find_map(|imp| {
        let cand = if imp.is_dir() {
            imp.join(name)
        } else if imp.file_name().is_some_and(|n| n == name) {
            imp.clone()
        } else {
            return None;
        };
        fs::metadata(&cand)
            .is_ok_and(|m| m.is_file() && m.len() == size)
            .then_some(cand)
    })
}

/// Files the user picked (not folders), in the order given.
pub fn picked_files(imports: &[PathBuf]) -> Vec<&PathBuf> {
    imports.iter().filter(|p| p.is_file()).collect()
}

fn file_len(p: &Path) -> Option<u64> {
    fs::metadata(p)
        .ok()
        .filter(|m| m.is_file())
        .map(|m| m.len())
}

/// The unpacked file `name` of `size` bytes: by name in an import folder or
/// among the picked files, or a picked file of exactly that size under any
/// name (checked by sha256 once used).
pub fn find_unpacked(imports: &[PathBuf], name: &str, size: u64) -> Option<PathBuf> {
    find_in(imports, name, size).or_else(|| {
        picked_files(imports)
            .into_iter()
            .find(|p| file_len(p) == Some(size))
            .cloned()
    })
}

/// Whether a picked file is the official firmware or one of its download
/// parts (by name, or by size under any name), rather than a Thimsy package.
pub fn is_source_file(src: Option<&Source>, p: &Path) -> bool {
    let Some(src) = src else {
        return false;
    };
    let name = p.file_name().map(|n| n.to_string_lossy().to_string());
    let len = file_len(p);
    name.as_deref() == Some(src.contains.as_str())
        || len == Some(src.container.size)
        || src
            .parts
            .iter()
            .any(|a| name.as_deref() == Some(a.file.as_str()) || len == Some(a.size))
}

/// Whether a file is the official firmware or one of its download parts by
/// name alone (see [`is_source_file`], which also goes by size).
fn is_source_name(src: Option<&Source>, p: &Path) -> bool {
    let Some(src) = src else {
        return false;
    };
    let name = file_name(p);
    name == src.contains || src.parts.iter().any(|a| name == a.file)
}

/// N of a `name.NNN` part (parts count from 001).
fn part_number(p: &Path) -> Option<u32> {
    let ext = p.extension()?.to_str()?;
    (ext.len() >= 3 && ext.bytes().all(|b| b.is_ascii_digit()))
        .then(|| ext.parse().ok())
        .flatten()
        .filter(|&n| n > 0)
}

/// A file's name without its `.NNN` part number (`x.tar` for `x.tar.001`).
fn part_stem(p: &Path) -> String {
    p.file_stem()
        .unwrap_or_default()
        .to_string_lossy()
        .to_string()
}

fn file_name(p: &Path) -> String {
    p.file_name()
        .unwrap_or_default()
        .to_string_lossy()
        .to_string()
}

/// The name of the part after the last of `parts` (`x.tar.004` after
/// `x.tar.003`) when they're all numbered parts: the one to name when they
/// turn out to be cut short.
pub fn next_part(parts: &[PathBuf]) -> Option<String> {
    let last = parts.last()?;
    if !parts.iter().all(|p| part_number(p).is_some()) {
        return None;
    }
    let n = part_number(last)? + 1;
    let width = last.extension().map_or(3, |e| e.len());
    Some(format!("{}.{n:0width$}", part_stem(last)))
}

/// How many missing part names an error lists before "and N more".
const MISSING_LISTED: usize = 5;

/// The Thimsy package among the picked files: every picked file that isn't
/// the official firmware (see [`is_source_file`]; a numbered part with the
/// name of a picked package part is the firmware only by its name). One
/// file without a part number is the package, under any name; several such
/// files are its parts in the order given. When the picked files are all
/// numbered parts (`name.001`, `name.002`, ...), they're put in number
/// order, and the parts after the highest one, of its name in the picked
/// parts' folders, are added while the next number is there: picking only
/// the first part (as for a split 7-Zip archive), or the first few, finds
/// the rest next to them. Parts missing below the highest one picked are
/// looked for next to the picked ones too; the picked ones must then have
/// one name, and parts still missing fail with `bad_input` naming them.
/// Empty when no picked file is a package. Whether it really is one (and
/// isn't cut short) is for [`crate::package`] to say.
pub fn picked_package(src: Option<&Source>, imports: &[PathBuf]) -> Result<Vec<PathBuf>> {
    let picked = picked_files(imports);
    // Names of picked parts that are surely package parts: other parts of
    // those names are the firmware only by name, whatever their size.
    let package_stems: std::collections::BTreeSet<String> = picked
        .iter()
        .filter(|p| part_number(p).is_some() && !is_source_file(src, p))
        .map(|p| part_stem(p))
        .collect();
    let is_firmware = |p: &Path| {
        if part_number(p).is_some() && package_stems.contains(&part_stem(p)) {
            is_source_name(src, p)
        } else {
            is_source_file(src, p)
        }
    };
    let files: Vec<PathBuf> = picked
        .into_iter()
        .filter(|p| !is_firmware(p))
        .cloned()
        .collect();
    let Some(nums) = files
        .iter()
        .map(|p| part_number(p))
        .collect::<Option<Vec<u32>>>()
        .filter(|n| !n.is_empty())
    else {
        return Ok(files);
    };
    let bad = |what: String| -> EngineError {
        let names: Vec<String> = files.iter().map(|p| file_name(p)).collect();
        EngineError::new(
            Code::BadInput,
            format!(
                "the files you chose ({}) look like parts of a Thimsy package, but {what}",
                names.join(", ")
            ),
        )
    };
    let mut parts: std::collections::BTreeMap<u32, PathBuf> = Default::default();
    for (n, p) in nums.into_iter().zip(&files) {
        match parts.get(&n) {
            Some(q) if q != p => {
                return Err(bad("two of them have the same part number".into())
                    .hint("Choose every part of the Thimsy package together, then try again.")
                    .into())
            }
            _ => {
                parts.insert(n, p.clone());
            }
        }
    }
    // The parts of a name in the picked parts' folders, by number.
    let dirs: std::collections::BTreeSet<PathBuf> = files
        .iter()
        .map(|p| match p.parent() {
            Some(d) if !d.as_os_str().is_empty() => d.to_path_buf(),
            _ => PathBuf::from("."),
        })
        .collect();
    let next_to = |stem: &str| {
        let mut found = std::collections::BTreeMap::new();
        for dir in &dirs {
            for e in fs::read_dir(dir).into_iter().flatten().flatten() {
                let path = e.path();
                let Some(n) = part_number(&path) else {
                    continue;
                };
                if part_stem(&path) == stem && path.is_file() && !is_source_name(src, &path) {
                    found.entry(n).or_insert(path);
                }
            }
        }
        found
    };
    let last = parts.keys().next_back().copied().unwrap_or(0);
    // Parts missing below the highest one picked: found next to the picked
    // ones, or named.
    if parts.len() < last as usize {
        let stem = part_stem(&files[0]);
        if let Some(other) = files.iter().find(|p| part_stem(p) != stem) {
            return Err(bad(format!(
                "{} and {} are parts of different files",
                file_name(&files[0]),
                file_name(other)
            ))
            .hint("Choose every part of the Thimsy package together, then try again.")
            .into());
        }
        for (n, p) in next_to(&stem) {
            if n < last {
                parts.entry(n).or_insert(p);
            }
        }
        let gaps = last as usize - parts.len();
        if gaps > 0 {
            let missing: Vec<String> = (1..last)
                .filter(|n| !parts.contains_key(n))
                .take(MISSING_LISTED)
                .map(|n| format!("{stem}.{n:03}"))
                .collect();
            let more = gaps.saturating_sub(missing.len());
            let more = if more > 0 {
                format!(" and {more} more")
            } else {
                String::new()
            };
            return Err(bad(format!(
                "not all of them are there; missing: {}{more}",
                missing.join(", ")
            ))
            .hint(
                "Put every part of the Thimsy package in the same folder, then choose them again.",
            )
            .into());
        }
    }
    // The parts after the highest one picked, while the next one is there.
    // Anything past a gap isn't used: a package missing a part is cut short,
    // and the missing part is named then (see [`next_part`]).
    let found = next_to(&part_stem(&parts[&last]));
    for n in last.saturating_add(1)..=u32::MAX {
        let Some(p) = found.get(&n) else { break };
        parts.insert(n, p.clone());
    }
    Ok(parts.into_values().collect())
}

/// Whether `name` is a package for `device` and `profile` as Thimsy names
/// them (`Thimsy-<release>-<device>-<profile>.tar`), or the catalog's name.
fn package_name(name: &str, device: &str, profile: &str, pkg: Option<&Package>) -> bool {
    pkg.is_some_and(|p| p.file == name)
        || (name.starts_with("Thimsy-") && name.ends_with(&format!("-{device}-{profile}.tar")))
}

/// A package for `device` and `profile` in one of `folders` (import folders,
/// then the `thimsy-build` folder next to the app), by name: the `.tar`, or
/// its `.tar.001`, `.tar.002`, ... parts. The first folder that has one
/// wins; a folder with two different ones is an error (`bad_input`).
pub fn package_in_folders(
    folders: &[PathBuf],
    device: &str,
    profile: &str,
    pkg: Option<&Package>,
) -> Result<Option<Vec<PathBuf>>> {
    for dir in folders.iter().filter(|d| d.is_dir()) {
        let mut wholes: Vec<PathBuf> = Vec::new();
        let mut parts: std::collections::BTreeMap<String, Vec<(u32, PathBuf)>> = Default::default();
        for e in fs::read_dir(dir).into_iter().flatten().flatten() {
            let path = e.path();
            if !path.is_file() {
                continue;
            }
            let name = e.file_name().to_string_lossy().to_string();
            if package_name(&name, device, profile, pkg) {
                wholes.push(path);
            } else if let Some(n) = part_number(&path) {
                let stem = path
                    .file_stem()
                    .unwrap_or_default()
                    .to_string_lossy()
                    .to_string();
                if package_name(&stem, device, profile, pkg) {
                    parts.entry(stem).or_default().push((n, path));
                }
            }
        }
        let mut found: Vec<Vec<PathBuf>> = wholes.iter().map(|p| vec![p.clone()]).collect();
        for (stem, mut ps) in parts {
            if wholes
                .iter()
                .any(|w| w.file_name().is_some_and(|n| n.to_string_lossy() == stem))
            {
                continue;
            }
            ps.sort_by_key(|(n, _)| *n);
            // Parts 001.. with none missing; anything else isn't a package.
            if ps
                .iter()
                .enumerate()
                .all(|(i, (n, _))| *n as usize == i + 1)
            {
                found.push(ps.into_iter().map(|(_, p)| p).collect());
            }
        }
        match found.len() {
            0 => continue,
            1 => return Ok(found.pop()),
            _ => {
                found.sort();
                let names: Vec<String> = found
                    .iter()
                    .map(|f| {
                        f[0].file_name()
                            .unwrap_or_default()
                            .to_string_lossy()
                            .to_string()
                    })
                    .collect();
                return Err(EngineError::new(
                    Code::BadInput,
                    format!(
                        "{} holds more than one Thimsy package for {device} ({profile}): {}",
                        dir.display(),
                        names.join(", ")
                    ),
                )
                .hint("There is more than one Thimsy package in that folder. Choose the one to use with Use a file I already downloaded.")
                .into());
            }
        }
    }
    Ok(None)
}

/// The package `pkg` (or each of its parts) as an earlier run downloaded
/// it into `cache_dir`, if that download finished.
pub fn cached_package(cache_dir: &Path, pkg: &Package) -> Option<Vec<PathBuf>> {
    let files: Vec<&str> = if pkg.parts.is_empty() {
        vec![pkg.file.as_str()]
    } else {
        pkg.parts.iter().map(|p| p.file.as_str()).collect()
    };
    files
        .into_iter()
        .map(|f| {
            let path = cache_dir.join(f);
            let done = fs::read_to_string(cache_dir.join(format!("{f}.ok")))
                .is_ok_and(|m| m.trim() == DOWNLOADED);
            (done && path.is_file()).then_some(path)
        })
        .collect()
}

/// What a download must be: its name, where it is, and its size and sha256
/// when they're known in advance (a Thimsy package's aren't: its manifest
/// is checked once it's here).
struct Fetch {
    file: String,
    size: Option<u64>,
    sha256: Option<String>,
    urls: Vec<String>,
    note: Option<String>,
}

/// What a finished package download's marker holds.
const DOWNLOADED: &str = "downloaded";

/// What `<file>.partial.json` records about a download in progress, so a
/// resume only ever continues the same file from the same server.
#[derive(Debug, Clone, Default, PartialEq, Serialize, Deserialize)]
struct PartialInfo {
    url: String,
    sha256: String,
    size: u64,
    etag: Option<String>,
    last_modified: Option<String>,
}

impl Cache {
    pub fn new(dir: PathBuf, imports: Vec<PathBuf>) -> Result<Self> {
        fs::create_dir_all(&dir).with_context(|| format!("creating {}", dir.display()))?;
        Ok(Cache {
            dir,
            imports,
            read_timeout: Duration::from_secs(60),
            retry_delay: Duration::from_secs(2),
        })
    }

    /// A file called `name` of `size` bytes among the imports: picked
    /// directly, or inside an import folder.
    pub fn find_import(&self, name: &str, size: u64) -> Option<PathBuf> {
        find_in(&self.imports, name, size)
    }

    fn marker_for(&self, file: &str) -> PathBuf {
        self.dir.join(format!("{file}.ok"))
    }

    fn is_verified(&self, a: &Artifact, path: &Path) -> bool {
        let Ok(meta) = fs::metadata(path) else {
            return false;
        };
        if meta.len() != a.size {
            return false;
        }
        fs::read_to_string(self.marker_for(&a.file)).is_ok_and(|m| m.trim() == a.sha256)
    }

    fn verify(&self, a: &Artifact, path: &Path, rep: &Reporter) -> Result<()> {
        let got = util::sha256_file(path, |d, t| {
            rep.progress_for("Checking {file}", &a.file, d, t)
        })?;
        if got != a.sha256 {
            return Err(EngineError::new(
                Code::HashMismatch,
                format!("{} has sha256 {got}, expected {}", a.file, a.sha256),
            )
            .hint("The file is damaged or a different version. Delete it and download again.")
            .into());
        }
        util::write_atomic(&self.marker_for(&a.file), a.sha256.as_bytes())?;
        Ok(())
    }

    /// Returns the path of a verified copy of `a`, fetching it if needed.
    pub fn get(&self, a: &Artifact, rep: &Reporter) -> Result<PathBuf> {
        let dest = self.dir.join(&a.file);
        if self.is_verified(a, &dest) {
            rep.info(format!("{} already downloaded", a.file));
            return Ok(dest);
        }
        let tmp = self.dir.join(format!("{}.import", a.file));
        // A file the user picked or a folder of downloads.
        if let Some(cand) = self.find_import(&a.file, a.size) {
            rep.info(format!("Importing {} from {}", a.file, cand.display()));
            copy_with_progress(&cand, &tmp, &a.file, Some(a.size), rep)?;
            self.verify(a, &tmp, rep)?;
            fs::rename(&tmp, &dest)?;
            return Ok(dest);
        }
        if dest.exists() && fs::metadata(&dest)?.len() == a.size {
            // Present but never verified (e.g. copied in by hand).
            if self.verify(a, &dest, rep).is_ok() {
                return Ok(dest);
            }
            fs::remove_file(&dest)?;
        }
        let f = Fetch {
            file: a.file.clone(),
            size: Some(a.size),
            sha256: Some(a.sha256.clone()),
            urls: a.urls.clone(),
            note: a.note.clone(),
        };
        self.fetch(&f, rep, &|p| self.verify(a, p, rep))
    }

    /// Downloads the package `pkg` (or each of its parts) into the cache.
    /// Its size and contents aren't known in advance: the caller opens it
    /// and checks it against its own manifest.
    pub fn download_package(&self, pkg: &Package, rep: &Reporter) -> Result<Vec<PathBuf>> {
        let pieces: Vec<(&str, &[String])> = if pkg.parts.is_empty() {
            vec![(pkg.file.as_str(), pkg.urls.as_slice())]
        } else {
            pkg.parts
                .iter()
                .map(|p| (p.file.as_str(), p.urls.as_slice()))
                .collect()
        };
        let mut out = Vec::new();
        for (file, urls) in pieces {
            let marker = self.marker_for(file);
            let dest = self.dir.join(file);
            if fs::read_to_string(&marker).is_ok_and(|m| m.trim() == DOWNLOADED) && dest.is_file() {
                rep.info(format!("{file} already downloaded"));
                out.push(dest);
                continue;
            }
            let _ = fs::remove_file(&marker);
            let f = Fetch {
                file: file.to_string(),
                size: None,
                sha256: None,
                urls: urls.to_vec(),
                note: pkg.note.clone(),
            };
            let path = self.fetch(&f, rep, &|_| Ok(()))?;
            util::write_atomic(&marker, DOWNLOADED.as_bytes())?;
            out.push(path);
        }
        Ok(out)
    }

    /// Removes a downloaded package (or its parts) from the cache, so the
    /// next try downloads it again.
    pub fn forget_package(&self, pkg: &Package) {
        let files: Vec<&str> = std::iter::once(pkg.file.as_str())
            .chain(pkg.parts.iter().map(|p| p.file.as_str()))
            .collect();
        for f in files {
            let _ = fs::remove_file(self.dir.join(f));
            let _ = fs::remove_file(self.marker_for(f));
        }
    }

    /// Downloads `a` into the cache from the first of its addresses that
    /// works, resuming interrupted attempts, and returns its path once
    /// `check` accepts it.
    fn fetch(
        &self,
        a: &Fetch,
        rep: &Reporter,
        check: &dyn Fn(&Path) -> Result<()>,
    ) -> Result<PathBuf> {
        let dest = self.dir.join(&a.file);
        if a.urls.is_empty() {
            return Err(EngineError::new(
                Code::DownloadFailed,
                format!("{} has no download address", a.file),
            )
            .hint(a.note.as_deref().unwrap_or(
                "Download the file yourself, then choose it with Use a file I already downloaded.",
            ))
            .into());
        }
        let partial = self.dir.join(format!("{}.partial", a.file));
        let mut last_err = None;
        for url in &a.urls {
            // Attempts that got nothing; one that made progress doesn't
            // count (a long download may stall many times).
            let mut fruitless = 0u32;
            let mut tries = 0u32;
            let mut restarted = false;
            loop {
                tries += 1;
                let before = fs::metadata(&partial).map(|m| m.len()).unwrap_or(0);
                match self.download(url, &partial, a, rep) {
                    Ok(resumed) => match check(&partial) {
                        Ok(()) => {
                            fs::rename(&partial, &dest)?;
                            let _ = fs::remove_file(sidecar(&partial));
                            return Ok(dest);
                        }
                        Err(e) => {
                            discard(&partial);
                            if resumed && !restarted {
                                // The file may have changed on the server
                                // between the pieces: once more from zero.
                                restarted = true;
                                rep.warn(format!(
                                    "{}: the resumed download doesn't match; downloading it again from the start",
                                    a.file
                                ));
                                continue;
                            }
                            last_err = Some(e);
                            break;
                        }
                    },
                    Err(e) => {
                        let missing = e.downcast_ref::<NotPublished>().is_some();
                        let after = fs::metadata(&partial).map(|m| m.len()).unwrap_or(0);
                        if after <= before {
                            fruitless += 1;
                        }
                        rep.warn(format!(
                            "{url}: {e:#} (attempt {tries}; {fruitless} of 3 without progress)"
                        ));
                        last_err = Some(e);
                        if missing || fruitless >= 3 || tries >= 100 {
                            break; // asking again won't help
                        }
                        std::thread::sleep(self.retry_delay * fruitless.max(1));
                    }
                }
            }
        }
        let e = last_err.unwrap();
        if crate::error::code_of(&e).0 == Code::HashMismatch {
            return Err(e);
        }
        Err(EngineError::new(
            Code::DownloadFailed,
            format!("couldn't download {}: {e:#}", a.file),
        )
        .hint(a.note.as_deref().unwrap_or(
            "Check the internet connection, or download the file yourself and choose it with Use a file I already downloaded.",
        ))
        .into())
    }

    /// Downloads `url` to `partial`, resuming from its current length when
    /// its `.partial.json` shows it's the same file from the same server
    /// (else starting over). Returns whether it continued earlier bytes.
    fn download(&self, url: &str, partial: &Path, a: &Fetch, rep: &Reporter) -> Result<bool> {
        if let Some(local) = url.strip_prefix("file://") {
            copy_with_progress(Path::new(local), partial, &a.file, a.size, rep)?;
            return Ok(false);
        }
        let info_path = sidecar(partial);
        let mut info: PartialInfo = fs::read(&info_path)
            .ok()
            .and_then(|b| serde_json::from_slice(&b).ok())
            .unwrap_or_default();
        let mut have = fs::metadata(partial).map(|m| m.len()).unwrap_or(0);
        let sha = a.sha256.clone().unwrap_or_default();
        let same = info.url == url && info.sha256 == sha && a.size.is_none_or(|s| info.size == s);
        if have > 0 && (!same || (info.size > 0 && have > info.size)) {
            rep.info(format!(
                "Starting {} again: the partial download doesn't belong to this file",
                a.file
            ));
            discard(partial);
            have = 0;
        }
        if have == 0 || !same {
            info = PartialInfo {
                url: url.to_string(),
                sha256: sha,
                size: a.size.unwrap_or(0),
                etag: None,
                last_modified: None,
            };
        }
        if info.size > 0 && have == info.size {
            return Ok(true);
        }
        let agent: ureq::Agent = ureq::Agent::config_builder()
            .timeout_connect(Some(Duration::from_secs(30)))
            .timeout_recv_response(Some(self.read_timeout))
            .timeout_per_read(Some(self.read_timeout))
            .http_status_as_error(false)
            .user_agent(format!("thimsy-installer/{}", crate::VERSION))
            .build()
            .into();
        // The bytes as stored: a compressed transfer can't be resumed by
        // byte offset.
        let mut req = agent.get(url).header("Accept-Encoding", "identity");
        if have > 0 {
            req = req.header("Range", &format!("bytes={have}-"));
            // Only continue if the server still has the same file.
            if let Some(v) = info.etag.as_ref().or(info.last_modified.as_ref()) {
                req = req.header("If-Range", v);
            }
        }
        let resp = req.call().with_context(|| format!("connecting to {url}"))?;
        let status = resp.status().as_u16();
        let header = |name: &str| {
            resp.headers()
                .get(name)
                .and_then(|v| v.to_str().ok())
                .map(str::to_string)
        };
        let etag = header("etag");
        let last_modified = header("last-modified");
        let resumed = match status {
            206 => {
                let range = header("content-range").unwrap_or_default();
                let changed = info.etag.is_some() && etag.is_some() && info.etag != etag;
                match content_range_start(&range) {
                    Some((start, total))
                        if start == have && (info.size == 0 || total == info.size) && !changed =>
                    {
                        info.size = total;
                    }
                    // Not the bytes asked for, or a different file now.
                    _ => {
                        discard(partial);
                        anyhow::bail!(
                            "the server sent {range:?} for a resume at byte {have}; starting over"
                        );
                    }
                }
                true
            }
            200 => {
                if a.size.is_none() {
                    info.size = header("content-length")
                        .and_then(|v| v.trim().parse().ok())
                        .unwrap_or(0);
                }
                false
            }
            416 => {
                discard(partial);
                anyhow::bail!("the server can't resume at byte {have} (HTTP 416); starting over");
            }
            404 | 410 => return Err(NotPublished.into()),
            _ => anyhow::bail!("server answered HTTP {status}"),
        };
        if etag.is_some() || last_modified.is_some() || !resumed {
            info.etag = etag;
            info.last_modified = last_modified;
        }
        util::write_atomic(&info_path, &serde_json::to_vec(&info)?)?;
        let (mut file, mut done) = if resumed {
            (OpenOptions::new().append(true).open(partial)?, have)
        } else {
            (File::create(partial)?, 0)
        };
        // 0 when neither the catalog nor the server said how large it is.
        let total = info.size;
        let mut body = resp.into_body().into_reader();
        let mut buf = vec![0u8; 1 << 20];
        loop {
            let n = body.read(&mut buf).context("download interrupted")?;
            if n == 0 {
                break;
            }
            if total > 0 && done + n as u64 > total {
                drop(file);
                discard(partial);
                anyhow::bail!("{} is larger than expected", a.file);
            }
            file.write_all(&buf[..n])?;
            done += n as u64;
            rep.progress_for("Downloading {file}", &a.file, done, total);
        }
        file.sync_all()?;
        if total > 0 && done != total {
            anyhow::bail!("download stopped at {done} of {total} bytes");
        }
        Ok(resumed)
    }
}

/// `<file>.partial.json` next to `<file>.partial`.
fn sidecar(partial: &Path) -> PathBuf {
    let mut s = partial.as_os_str().to_owned();
    s.push(".json");
    PathBuf::from(s)
}

/// Removes a partial download and what's recorded about it.
fn discard(partial: &Path) {
    let _ = fs::remove_file(partial);
    let _ = fs::remove_file(sidecar(partial));
}

/// (start, total) of a `Content-Range: bytes START-END/TOTAL` header.
fn content_range_start(v: &str) -> Option<(u64, u64)> {
    let rest = v.trim().strip_prefix("bytes ")?;
    let (range, total) = rest.split_once('/')?;
    let (start, _) = range.split_once('-')?;
    Some((start.trim().parse().ok()?, total.trim().parse().ok()?))
}

/// Copies `src` to `dst`: exactly `size` bytes when that's known, else the
/// whole file.
fn copy_with_progress(
    src: &Path,
    dst: &Path,
    file: &str,
    size: Option<u64>,
    rep: &Reporter,
) -> Result<()> {
    let mut i = File::open(src).with_context(|| format!("opening {}", src.display()))?;
    let n = match size {
        Some(n) => n,
        None => i.metadata()?.len(),
    };
    let mut o = File::create(dst)?;
    util::copy_exact(&mut i, &mut o, n, &mut |d| {
        rep.progress_for("Importing {file}", file, d, n)
    })?;
    o.sync_all()?;
    Ok(())
}

/// The server has no file at the address (HTTP 404): typically a release
/// that is still a draft. Not retried.
#[derive(Debug)]
struct NotPublished;

impl std::fmt::Display for NotPublished {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(
            f,
            "the file isn't public at this address (HTTP 404); its release may still be a draft"
        )
    }
}

impl std::error::Error for NotPublished {}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::events::MemorySink;
    use std::sync::Arc;

    fn art(file: &str, data: &[u8], urls: Vec<String>) -> Artifact {
        Artifact {
            id: file.into(),
            file: file.into(),
            size: data.len() as u64,
            sha256: util::sha256_hex(data),
            urls,
            note: None,
        }
    }

    #[test]
    fn picked_package_parts_are_ordered_and_must_be_complete() {
        let d = tempfile::tempdir().unwrap();
        let touch_in = |dir: &Path, name: &str| {
            std::fs::create_dir_all(dir).unwrap();
            let p = dir.join(name);
            std::fs::write(&p, name.as_bytes()).unwrap();
            p
        };
        let touch = |name: &str| touch_in(d.path(), name);
        let (p1, p2, p3) = (
            touch("pack.tar.001"),
            touch("pack.tar.002"),
            touch("pack.tar.003"),
        );
        // Other numbered files next to them aren't parts of this package.
        touch("fw.7z.001");
        touch("pack.tar.000");
        let all = [p1.clone(), p2.clone(), p3.clone()];
        // Chosen in any order: put in order.
        let got = picked_package(None, &[p3.clone(), p1.clone(), p2.clone()]).unwrap();
        assert_eq!(got, all);
        // Only some chosen, or the same one twice: the rest are found next
        // to them.
        for pick in [
            vec![p1.clone()],
            vec![p2.clone()],
            vec![p3.clone()],
            vec![p1.clone(), p3.clone()],
            vec![p1.clone(), p1.clone()],
        ] {
            assert_eq!(picked_package(None, &pick).unwrap(), all, "{pick:?}");
        }

        // A gap below the highest part chosen: bad_input naming what's
        // missing.
        let gap = d.path().join("gap");
        let g1 = touch_in(&gap, "pack.tar.001");
        let g3 = touch_in(&gap, "pack.tar.003");
        let err = picked_package(None, &[g1.clone(), g3]).unwrap_err();
        assert_eq!(crate::error::code_of(&err).0, Code::BadInput);
        let msg = err.to_string();
        assert!(
            msg.contains("missing: pack.tar.002") && !msg.contains("more"),
            "{msg}"
        );
        // A gap after the highest part chosen: the parts up to it, and the
        // part after them is the one to name if they're cut short (so are
        // a part still downloading, and a stray part past a gap).
        touch_in(&gap, "pack.tar.004.crdownload");
        touch_in(&gap, "pack.tar.005");
        let got = picked_package(None, std::slice::from_ref(&g1)).unwrap();
        assert_eq!(got, std::slice::from_ref(&g1));
        assert_eq!(next_part(&got).as_deref(), Some("pack.tar.002"));
        // The first part missing.
        let late = d.path().join("late");
        let l2 = touch_in(&late, "pack.tar.002");
        let err = picked_package(None, std::slice::from_ref(&l2)).unwrap_err();
        assert!(err.to_string().contains("missing: pack.tar.001"), "{err}");
        // Many missing: the first few are named.
        let far = d.path().join("far");
        let f1 = touch_in(&far, "pack.tar.001");
        let f10 = touch_in(&far, "pack.tar.010");
        let msg = picked_package(None, &[f1, f10]).unwrap_err().to_string();
        assert!(
            msg.contains(
                "pack.tar.002, pack.tar.003, pack.tar.004, pack.tar.005, pack.tar.006 and 3 more"
            ),
            "{msg}"
        );
        // A part number that doesn't fit is no part number.
        let huge = d.path().join("huge");
        let h1 = touch_in(&huge, "pack.tar.001");
        touch_in(&huge, "pack.tar.99999999999");
        assert_eq!(
            picked_package(None, std::slice::from_ref(&h1)).unwrap(),
            [h1]
        );

        // Parts in two folders: each folder's parts count.
        let x = d.path().join("x");
        let y = d.path().join("y");
        let x1 = touch_in(&x, "pack.tar.001");
        let y2 = touch_in(&y, "pack.tar.002");
        let y3 = touch_in(&y, "pack.tar.003");
        assert_eq!(
            picked_package(None, &[x1.clone(), y2.clone()]).unwrap(),
            [x1.clone(), y2, y3]
        );
        // The same part twice from different folders: bad_input.
        let y1 = touch_in(&y, "pack.tar.001");
        let err = picked_package(None, &[x1.clone(), y1]).unwrap_err();
        assert_eq!(crate::error::code_of(&err).0, Code::BadInput);
        assert!(err.to_string().contains("same part number"), "{err}");
        // Parts of two different names with one missing: bad_input.
        let other = touch_in(&x, "other.tar.003");
        let err = picked_package(None, &[x1, other]).unwrap_err();
        assert_eq!(crate::error::code_of(&err).0, Code::BadInput);
        assert!(err.to_string().contains("different files"), "{err}");

        // Every part chosen, renamed by a browser downloading them again
        // (so of two names), next to a stray part past a gap: those parts,
        // in order.
        let again = d.path().join("again");
        let r1 = touch_in(&again, "pack.tar (1).001");
        let r2 = touch_in(&again, "pack.tar (1).002");
        let r3 = touch_in(&again, "pack.tar.003");
        touch_in(&again, "pack.tar.005");
        touch_in(&again, "pack.tar (1).004.part");
        let mixed = [r1.clone(), r2.clone(), r3.clone()];
        assert_eq!(
            picked_package(None, &[r3.clone(), r2.clone(), r1.clone()]).unwrap(),
            mixed
        );
        assert_eq!(next_part(&mixed).as_deref(), Some("pack.tar.004"));
        // The first two chosen: the rest of their name found next to them.
        assert_eq!(
            picked_package(None, std::slice::from_ref(&r1)).unwrap(),
            [r1, r2]
        );

        // Package parts the size of the official firmware's parts: the
        // firmware only by name once a part of their name is a package's.
        let src: Source = serde_json::from_value(serde_json::json!({
            "id": "fw", "name": "fw", "kind": "archive7z",
            "parts": [{"id": "fw-001", "file": "fw.7z.001", "size": 4, "sha256": "x"}],
            "contains": "fw.pac",
            "container": {"format": "pac", "size": 99, "sha256": "x"},
        }))
        .unwrap();
        let sized = d.path().join("sized");
        let write = |name: &str, len: usize| {
            let p = sized.join(name);
            std::fs::create_dir_all(&sized).unwrap();
            std::fs::write(&p, vec![0u8; len]).unwrap();
            p
        };
        let s1 = write("big.tar.001", 4);
        let s2 = write("big.tar.002", 4);
        let s3 = write("big.tar.003", 2);
        let fw = write("fw.7z.001", 4);
        let fw_renamed = write("fw (1).7z.001", 4);
        let whole = [s1.clone(), s2.clone(), s3.clone()];
        for pick in [
            vec![s1.clone(), s2.clone(), s3.clone(), fw.clone()],
            vec![s3.clone(), fw.clone()],
            vec![s2.clone(), s3.clone()],
        ] {
            assert_eq!(
                picked_package(Some(&src), &pick).unwrap(),
                whole,
                "{pick:?}"
            );
        }
        // Firmware parts alone, under their name or another: no package.
        assert!(picked_package(Some(&src), &[fw, fw_renamed])
            .unwrap()
            .is_empty());

        // Renamed copies: in the order chosen. One file: the package.
        let (a, b) = (touch("download (1).bin"), touch("download (2).bin"));
        assert_eq!(
            picked_package(None, &[a.clone(), b.clone()]).unwrap(),
            [a.clone(), b]
        );
        assert_eq!(picked_package(None, std::slice::from_ref(&a)).unwrap(), [a]);
        // A lone numbered file with nothing next to it: the package.
        let lone = touch_in(&d.path().join("lone"), "renamed.001");
        assert_eq!(
            picked_package(None, std::slice::from_ref(&lone)).unwrap(),
            [lone]
        );
        // Folders aren't picked files.
        assert!(picked_package(None, &[d.path().to_path_buf()])
            .unwrap()
            .is_empty());
    }

    #[test]
    fn packages_in_folders_are_found_by_name() {
        let d = tempfile::tempdir().unwrap();
        let dir = d.path().to_path_buf();
        let find = || package_in_folders(std::slice::from_ref(&dir), "dev-x", "heavy", None);
        std::fs::write(dir.join("Thimsy-1.0a-dev-x-light.tar"), b"x").unwrap();
        std::fs::write(dir.join("notes.txt"), b"x").unwrap();
        assert_eq!(find().unwrap(), None);
        for n in [
            "Thimsy-1.0b-dev-x-heavy.tar.002",
            "Thimsy-1.0b-dev-x-heavy.tar.001",
        ] {
            std::fs::write(dir.join(n), b"x").unwrap();
        }
        assert_eq!(
            find().unwrap().unwrap(),
            [
                dir.join("Thimsy-1.0b-dev-x-heavy.tar.001"),
                dir.join("Thimsy-1.0b-dev-x-heavy.tar.002")
            ]
        );
        std::fs::write(dir.join("Thimsy-1.0a-dev-x-heavy.tar"), b"x").unwrap();
        let err = find().unwrap_err();
        assert_eq!(crate::error::code_of(&err).0, Code::BadInput);
    }

    #[test]
    fn import_then_cache_hit_and_bad_hash() {
        let ws = tempfile::tempdir().unwrap();
        let imp = tempfile::tempdir().unwrap();
        let data = b"hello thimsy".repeat(1000);
        std::fs::write(imp.path().join("a.bin"), &data).unwrap();
        let rep = Reporter::new(Arc::new(MemorySink::default()));
        let cache = Cache::new(ws.path().join("cache"), vec![imp.path().to_path_buf()]).unwrap();
        let a = art("a.bin", &data, vec![]);
        let p = cache.get(&a, &rep).unwrap();
        assert_eq!(std::fs::read(&p).unwrap(), data);
        // second call: cache hit even after the import folder is gone
        drop(imp);
        assert_eq!(cache.get(&a, &rep).unwrap(), p);
        // wrong hash from a file:// URL
        let src = ws.path().join("src.bin");
        std::fs::write(&src, b"other").unwrap();
        let mut bad = art("b.bin", b"other", vec![format!("file://{}", src.display())]);
        bad.sha256 = "0".repeat(64);
        let err = cache.get(&bad, &rep).unwrap_err();
        assert_eq!(crate::error::code_of(&err).0, Code::HashMismatch);
        // no URL and no import → download_failed with a hint
        let none = art("c.bin", b"x", vec![]);
        let err = cache.get(&none, &rep).unwrap_err();
        assert_eq!(crate::error::code_of(&err).0, Code::DownloadFailed);
    }
}
