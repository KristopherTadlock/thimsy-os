//! Thimsy packages. A package is an uncompressed tar (see `bundle`) holding
//! the shared system image, the device's patches against its official
//! firmware, and `pack.json`, the manifest that describes them: each member's
//! size and sha256, the build steps, the official firmware it applies to and
//! the sha256 of the firmware it must produce.
//!
//! The installer accepts any package whose manifest is well formed and
//! matches the selected device: the catalog pins no build. A package isn't
//! signed; what protects the device is that the build only takes typed steps,
//! every patch checks the firmware it is applied to, and the result must be
//! the reference firmware the manifest names before it can be flashed.

use crate::archive::{MultiVolume, Span};
use crate::bundle::Bundle;
use crate::error::{Code, EngineError};
use crate::events::Reporter;
use crate::util;
use anyhow::Result;
use serde::{Deserialize, Serialize};
use std::collections::HashSet;
use std::io::Read;

/// The manifest's name inside a package.
pub const MANIFEST: &str = "pack.json";

/// The manifest format this installer reads. A manifest without `format`
/// is format 1 (packages made before the field existed).
pub const FORMAT: u64 = 1;

/// What `dev pack` writes as `min_installer`: the first installer that
/// accepts packages by their manifest (see [`parse_version`]).
pub const MIN_INSTALLER: &str = "0.1.0-test18";

/// A manifest is a few KB; anything much larger isn't one.
const MAX_MANIFEST: u64 = 1 << 20;

/// One build step. Typed: a package can only combine these.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(tag = "op", rename_all = "snake_case", deny_unknown_fields)]
pub enum Op {
    /// Grow a logical partition of the `super` entry into free space.
    LpGrow { entry: String, partition: String },
    /// Write the system image (`@system`) or another member to a logical
    /// partition.
    LpWrite {
        entry: String,
        partition: String,
        artifact: String,
    },
    /// Patch a logical partition with a delta.
    LpDelta {
        entry: String,
        partition: String,
        artifact: String,
        in_sha256: String,
        out_sha256: String,
    },
    /// Patch a PAC entry with a delta.
    PacDelta {
        entry: String,
        artifact: String,
        in_sha256: String,
        out_sha256: String,
    },
    /// Set the product strings flash tools show ({profile}, {release} expand).
    PacSetInfo { version: String, alias: String },
    /// Recompute the PAC data CRC.
    PacFixCrc,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Unpack {
    pub format: String,
    pub size: u64,
    pub sha256: String,
}

/// A file in the package.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Member {
    pub id: String,
    pub file: String,
    pub size: u64,
    pub sha256: String,
    /// Set when the file is compressed (zstd) and written unpacked.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub unpack: Option<Unpack>,
}

fn format_one() -> u64 {
    FORMAT
}

/// `pack.json`. Unknown fields are refused, so a misspelt or newer field is
/// never silently ignored.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Manifest {
    #[serde(default = "format_one")]
    pub format: u64,
    /// Catalog device id. When absent (older packages), the official
    /// firmware's sha256 alone ties the package to its device.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub device: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub profile: Option<String>,
    /// The Thimsy release, e.g. "Thimsy 1.0a test 30".
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub release: Option<String>,
    /// The oldest installer that can use the package (see [`parse_version`]).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub min_installer: Option<String>,
    pub system: Member,
    pub artifacts: Vec<Member>,
    pub build: Vec<Op>,
    /// sha256 of the firmware the build steps must produce.
    pub reference_sha256: String,
    /// sha256 of the official firmware container the steps start from.
    pub source_sha256: String,
}

impl Manifest {
    /// The system image, then the patches.
    pub fn members(&self) -> impl Iterator<Item = &Member> {
        std::iter::once(&self.system).chain(&self.artifacts)
    }

    /// The member a build step names (`@system` for the system image).
    pub fn member(&self, id: &str) -> Option<&Member> {
        if id == "@system" {
            return Some(&self.system);
        }
        self.artifacts.iter().find(|a| a.id == id)
    }
}

/// A version as `min_installer` and [`crate::INSTALLER_VERSION`] write it:
/// `MAJOR.MINOR.PATCH`, or `MAJOR.MINOR.PATCH-testN` for a test release.
/// Ordered by number; a test release comes before the release itself, and
/// test N before test N+1 (`0.1.0-test18` < `0.1.0-test19` < `0.1.0` <
/// `0.1.1-test1`).
pub fn parse_version(s: &str) -> Option<(u64, u64, u64, u64)> {
    let (core, test) = match s.split_once('-') {
        Some((core, t)) => (core, Some(t.strip_prefix("test")?)),
        None => (s, None),
    };
    let num = |p: &str| -> Option<u64> {
        if p.is_empty() || !p.bytes().all(|b| b.is_ascii_digit()) {
            return None;
        }
        p.parse().ok()
    };
    let mut it = core.split('.');
    let v = (num(it.next()?)?, num(it.next()?)?, num(it.next()?)?);
    if it.next().is_some() {
        return None;
    }
    let t = match test {
        Some(t) => num(t)?,
        None => u64::MAX,
    };
    Some((v.0, v.1, v.2, t))
}

/// What the selected device needs a package to be.
#[derive(Debug, Clone, Copy)]
pub struct Want<'a> {
    pub device: &'a str,
    pub profile: &'a str,
    /// The catalog's sha256 of the device's official firmware container.
    pub source_sha256: &'a str,
    /// The official firmware's name, for messages.
    pub source_name: &'a str,
}

fn not_a_package(what: &str, reason: String) -> anyhow::Error {
    EngineError::new(
        Code::BadPackage,
        format!("{what} isn't a Thimsy package the installer can use: {reason}"),
    )
    .hint("Choose the Thimsy package for your device: one .tar file, or all of its numbered parts. A renamed file works; a damaged or unfinished download doesn't.")
    .into()
}

fn for_another_device(what: &str, reason: String) -> anyhow::Error {
    EngineError::new(Code::BadPackage, format!("{what} {reason}"))
        .hint("This package is for another device or profile. Choose the Thimsy package made for the device and profile you selected.")
        .into()
}

fn for_other_firmware(what: &str, reason: String) -> anyhow::Error {
    EngineError::new(Code::BadPackage, format!("{what} {reason}"))
        .hint("This package was made from different official firmware than this installer uses. Choose a package made for this installer, or update Thimsy Installer.")
        .into()
}

fn needs_newer_installer(what: &str, reason: String) -> anyhow::Error {
    EngineError::new(Code::BadPackage, format!("{what} {reason}"))
        .hint("This package needs a newer Thimsy Installer. Download the latest installer, then try again.")
        .into()
}

fn sha256_ok(s: &str) -> bool {
    s.len() == 64 && s.bytes().all(|b| matches!(b, b'0'..=b'9' | b'a'..=b'f'))
}

/// A member's file name: a plain name a tar member can have.
fn name_ok(s: &str) -> bool {
    !s.is_empty()
        && s.len() <= 100
        && s != "."
        && s != ".."
        && s != MANIFEST
        && !s.contains(['/', '\\', '\0'])
}

/// The checks a format-1 manifest must pass beyond its schema. Returns what
/// is wrong.
fn validate(m: &Manifest) -> std::result::Result<(), String> {
    for (field, v) in [
        ("reference_sha256", &m.reference_sha256),
        ("source_sha256", &m.source_sha256),
    ] {
        if !sha256_ok(v) {
            return Err(format!("{field} isn't a sha256 (64 lowercase hex digits)"));
        }
    }
    for (field, v) in [
        ("device", &m.device),
        ("profile", &m.profile),
        ("release", &m.release),
    ] {
        if v.as_deref().is_some_and(|s| s.trim().is_empty()) {
            return Err(format!("{field} is empty"));
        }
    }
    let mut ids = HashSet::new();
    let mut files = HashSet::new();
    for a in m.members() {
        if a.id.is_empty() || a.id == "@system" || !ids.insert(a.id.as_str()) {
            return Err(format!(
                "member id {:?} is empty, reserved or repeated",
                a.id
            ));
        }
        if !name_ok(&a.file) || !files.insert(a.file.as_str()) {
            return Err(format!(
                "member file {:?} isn't a plain file name, or is repeated",
                a.file
            ));
        }
        if !sha256_ok(&a.sha256) {
            return Err(format!("{}: sha256 isn't 64 lowercase hex digits", a.file));
        }
        if let Some(u) = &a.unpack {
            if u.format != "zstd" {
                return Err(format!("{}: unknown compression {:?}", a.file, u.format));
            }
            if !sha256_ok(&u.sha256) {
                return Err(format!(
                    "{}: unpacked sha256 isn't 64 lowercase hex digits",
                    a.file
                ));
            }
        }
    }
    if m.build.is_empty() {
        return Err("it has no build steps".into());
    }
    let nonempty = |s: &str| !s.trim().is_empty();
    for (i, op) in m.build.iter().enumerate() {
        let n = i + 1;
        let ok = match op {
            Op::LpGrow { entry, partition } => nonempty(entry) && nonempty(partition),
            Op::LpWrite {
                entry,
                partition,
                artifact,
            } => {
                if m.member(artifact).is_none() {
                    return Err(format!("step {n} writes unknown member {artifact:?}"));
                }
                nonempty(entry) && nonempty(partition)
            }
            Op::LpDelta {
                entry,
                partition,
                artifact,
                in_sha256,
                out_sha256,
            } => {
                if artifact == "@system" || m.member(artifact).is_none() {
                    return Err(format!("step {n} applies unknown delta {artifact:?}"));
                }
                if !sha256_ok(in_sha256) || !sha256_ok(out_sha256) {
                    return Err(format!(
                        "step {n}: in_sha256 and out_sha256 must be sha256s"
                    ));
                }
                nonempty(entry) && nonempty(partition)
            }
            Op::PacDelta {
                entry,
                artifact,
                in_sha256,
                out_sha256,
            } => {
                if artifact == "@system" || m.member(artifact).is_none() {
                    return Err(format!("step {n} applies unknown delta {artifact:?}"));
                }
                if !sha256_ok(in_sha256) || !sha256_ok(out_sha256) {
                    return Err(format!(
                        "step {n}: in_sha256 and out_sha256 must be sha256s"
                    ));
                }
                nonempty(entry)
            }
            Op::PacSetInfo { version, alias } => nonempty(version) && nonempty(alias),
            Op::PacFixCrc => true,
        };
        if !ok {
            return Err(format!("step {n} has an empty entry, partition or name"));
        }
    }
    Ok(())
}

/// Reads `pack.json` from `raw`: its format and `min_installer` first (a
/// newer manifest may have fields this installer doesn't know), then the
/// strict format-1 schema and its checks.
pub fn parse(raw: &[u8], what: &str) -> Result<Manifest> {
    let v: serde_json::Value = serde_json::from_slice(raw)
        .map_err(|e| not_a_package(what, format!("its {MANIFEST} isn't valid JSON ({e})")))?;
    let Some(obj) = v.as_object() else {
        return Err(not_a_package(
            what,
            format!("its {MANIFEST} isn't a JSON object"),
        ));
    };
    if let Some(f) = obj.get("format") {
        match f.as_u64() {
            Some(FORMAT) => {}
            Some(n) if n > FORMAT => {
                return Err(needs_newer_installer(
                    what,
                    format!("is a format {n} package; this installer reads format {FORMAT}"),
                ))
            }
            _ => {
                return Err(not_a_package(
                    what,
                    format!("its {MANIFEST} has an unknown format {f}"),
                ))
            }
        }
    }
    if let Some(mi) = obj.get("min_installer") {
        let need = mi.as_str().and_then(|s| parse_version(s).map(|v| (s, v)));
        let Some((s, need)) = need else {
            return Err(not_a_package(
                what,
                format!("its {MANIFEST} has a malformed min_installer {mi}"),
            ));
        };
        let have = parse_version(crate::INSTALLER_VERSION).expect("INSTALLER_VERSION parses");
        if need > have {
            return Err(needs_newer_installer(
                what,
                format!(
                    "needs Thimsy Installer {s} or newer; this is {}",
                    crate::INSTALLER_VERSION
                ),
            ));
        }
    }
    let m: Manifest = serde_json::from_value(v).map_err(|e| {
        not_a_package(
            what,
            format!("its {MANIFEST} doesn't follow format {FORMAT} ({e})"),
        )
    })?;
    validate(&m).map_err(|e| not_a_package(what, format!("its {MANIFEST} is malformed: {e}")))?;
    Ok(m)
}

/// An opened package whose manifest passed [`parse`] and matches the
/// selected device. Its members' contents are checked by [`Package::verify`].
#[derive(Debug, Clone)]
pub struct Package {
    pub manifest: Manifest,
    bundle: Bundle,
}

impl Package {
    /// Opens the package formed by `spans` (one file or its parts, end to
    /// end). `what` names it in messages.
    pub fn open(spans: &[Span], what: &str, want: &Want) -> Result<Package> {
        let bundle = Bundle::index(spans).map_err(|e| not_a_package(what, format!("{e:#}")))?;
        let Some(m) = bundle.get(MANIFEST) else {
            return Err(not_a_package(what, format!("it has no {MANIFEST}")));
        };
        if m.size > MAX_MANIFEST {
            return Err(not_a_package(
                what,
                format!("its {MANIFEST} is {} bytes", m.size),
            ));
        }
        let mut raw = Vec::new();
        MultiVolume::open_spans(&m.spans)?.read_to_end(&mut raw)?;
        let manifest = parse(&raw, what)?;
        if let Some(dev) = manifest.device.as_deref().filter(|d| *d != want.device) {
            return Err(for_another_device(
                what,
                format!("is a package for {dev}, not {}", want.device),
            ));
        }
        if let Some(p) = manifest.profile.as_deref().filter(|p| *p != want.profile) {
            return Err(for_another_device(
                what,
                format!("is a package for the {p} profile, not {}", want.profile),
            ));
        }
        if manifest.source_sha256 != want.source_sha256 {
            return Err(for_other_firmware(
                what,
                format!(
                    "was made from official firmware with sha256 {}; the installer builds {} from {} (sha256 {})",
                    manifest.source_sha256, want.device, want.source_name, want.source_sha256
                ),
            ));
        }
        for a in manifest.members() {
            if bundle.get(&a.file).is_none() {
                return Err(not_a_package(
                    what,
                    format!("{} that its {MANIFEST} lists is missing", a.file),
                ));
            }
        }
        Ok(Package { manifest, bundle })
    }

    /// Where a member's bytes are in the package.
    pub fn spans(&self, a: &Member) -> &[Span] {
        &self
            .bundle
            .get(&a.file)
            .expect("Package::open checked every member is there")
            .spans
    }

    /// Checks every member's size and sha256 against the manifest.
    /// `downloaded`: the package came from the network this run (the
    /// message says to try again rather than to fetch a new copy).
    pub fn verify(&self, what: &str, downloaded: bool, rep: &Reporter) -> Result<()> {
        for a in self.manifest.members() {
            let spans = self.spans(a);
            let have: u64 = spans.iter().map(|s| s.len).sum();
            let got = if have == a.size {
                let mut mv = MultiVolume::open_spans(spans)?;
                let mut h = <sha2::Sha256 as sha2::Digest>::new();
                let mut buf = vec![0u8; util::CHUNK];
                let mut done = 0u64;
                loop {
                    let n = mv.read(&mut buf)?;
                    if n == 0 {
                        break;
                    }
                    sha2::Digest::update(&mut h, &buf[..n]);
                    done += n as u64;
                    rep.progress_for("Checking {file}", &a.file, done, a.size);
                }
                Some(hex::encode(sha2::Digest::finalize(h)))
            } else {
                None
            };
            if got.as_deref() == Some(a.sha256.as_str()) {
                continue;
            }
            let found = match &got {
                Some(sha) => format!("sha256 {sha}"),
                None => format!("{have} bytes"),
            };
            let expected = match &got {
                Some(_) => a.sha256.clone(),
                None => format!("{} bytes", a.size),
            };
            let msg = format!(
                "{} in {what} has {found}; its {MANIFEST} says {expected}",
                a.file
            );
            return Err(if downloaded {
                EngineError::new(Code::HashMismatch, format!("the download is damaged: {msg}"))
                    .hint("The download was damaged. Try again: it will be downloaded again from the start.")
            } else {
                EngineError::new(Code::HashMismatch, format!("the package is damaged: {msg}"))
                    .hint("The package file is damaged. Download it again, then choose the new copy.")
            }
            .into());
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn versions_order_tests_before_their_release() {
        let v = |s| parse_version(s).unwrap();
        assert!(v("0.1.0-test17") < v("0.1.0-test18"));
        assert!(v("0.1.0-test9") < v("0.1.0-test10"));
        assert!(v("0.1.0-test99") < v("0.1.0"));
        assert!(v("0.1.0") < v("0.1.1-test1"));
        assert!(v("0.9.9") < v("0.10.0"));
        assert!(v("1.0.0") < v("2.0.0-test1"));
        for bad in [
            "",
            "0.1",
            "0.1.0.0",
            "0.1.0-beta1",
            "0.1.0-test",
            "v0.1.0",
            "0.1.x",
            "0.1.0-test1a",
            "0.1.0 ",
            "+1.0.0",
            "0.1.-1",
        ] {
            assert!(parse_version(bad).is_none(), "{bad:?}");
        }
    }

    #[test]
    fn the_installer_version_is_the_engine_version() {
        let (a, b, c, _) = parse_version(crate::INSTALLER_VERSION).unwrap();
        assert_eq!(format!("{a}.{b}.{c}"), crate::VERSION);
        assert!(
            parse_version(MIN_INSTALLER).unwrap()
                <= parse_version(crate::INSTALLER_VERSION).unwrap()
        );
    }
}
