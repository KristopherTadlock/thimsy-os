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

/// N of a `name.NNN` part.
fn part_number(p: &Path) -> Option<u32> {
    let ext = p.extension()?.to_str()?;
    (ext.len() >= 3 && ext.bytes().all(|b| b.is_ascii_digit()))
        .then(|| ext.parse().ok())
        .flatten()
}

/// The Thimsy package among the picked files: every picked file that isn't
/// the official firmware (see [`is_source_file`]). One file is the package,
/// under any name; several are its parts, in the order of their `.NNN`
/// numbers when they all have one (which must then run from 001 with none
/// missing), otherwise in the order given. Empty when no picked file is a
/// package. Whether it really is one is for [`crate::package`] to say.
pub fn picked_package(src: Option<&Source>, imports: &[PathBuf]) -> Result<Vec<PathBuf>> {
    let files: Vec<PathBuf> = picked_files(imports)
        .into_iter()
        .filter(|p| !is_source_file(src, p))
        .cloned()
        .collect();
    if files.len() < 2 {
        return Ok(files);
    }
    let Some(nums) = files
        .iter()
        .map(|p| part_number(p))
        .collect::<Option<Vec<u32>>>()
    else {
        return Ok(files);
    };
    let mut parts: Vec<(u32, PathBuf)> = nums.into_iter().zip(files).collect();
    parts.sort_by_key(|(n, _)| *n);
    let stem = parts[0]
        .1
        .file_stem()
        .unwrap_or_default()
        .to_string_lossy()
        .to_string();
    let last = parts.last().map_or(0, |(n, _)| *n);
    let missing: Vec<String> = (1..=last)
        .filter(|n| !parts.iter().any(|(m, _)| m == n))
        .map(|n| format!("{stem}.{n:03}"))
        .collect();
    let repeated = parts.windows(2).any(|w| w[0].0 == w[1].0);
    if !missing.is_empty() || repeated {
        let names: Vec<String> = parts
            .iter()
            .map(|(_, p)| {
                p.file_name()
                    .unwrap_or_default()
                    .to_string_lossy()
                    .to_string()
            })
            .collect();
        let what = if repeated {
            "two of them have the same part number".to_string()
        } else {
            format!("missing: {}", missing.join(", "))
        };
        return Err(EngineError::new(
            Code::BadInput,
            format!(
                "the files you chose ({}) look like parts of a Thimsy package, but not all of them; {what}",
                names.join(", ")
            ),
        )
        .hint("Choose every part of the Thimsy package together, then try again.")
        .into());
    }
    Ok(parts.into_iter().map(|(_, p)| p).collect())
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
        let touch = |name: &str| {
            let p = d.path().join(name);
            std::fs::write(&p, name.as_bytes()).unwrap();
            p
        };
        let (p1, p2, p3) = (
            touch("pack.tar.001"),
            touch("pack.tar.002"),
            touch("pack.tar.003"),
        );
        let got = picked_package(None, &[p3.clone(), p1.clone(), p2.clone()]).unwrap();
        assert_eq!(got, [p1.clone(), p2.clone(), p3.clone()]);
        let err = picked_package(None, &[p1.clone(), p3.clone()]).unwrap_err();
        assert_eq!(crate::error::code_of(&err).0, Code::BadInput);
        assert!(err.to_string().contains("pack.tar.002"), "{err}");
        // Renamed copies: in the order chosen. One file: the package.
        let (a, b) = (touch("download (1).bin"), touch("download (2).bin"));
        assert_eq!(
            picked_package(None, &[a.clone(), b.clone()]).unwrap(),
            [a.clone(), b]
        );
        assert_eq!(picked_package(None, std::slice::from_ref(&a)).unwrap(), [a]);
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
