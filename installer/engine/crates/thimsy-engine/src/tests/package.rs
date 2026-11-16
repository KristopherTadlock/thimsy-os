//! Thimsy packages are accepted by their own manifest: old and new formats
//! build, and anything malformed, damaged or made for another device,
//! profile or firmware is refused with its code before anything is
//! downloaded.

use super::build::{fixture, good_fixture, opts, quiet, write_package, Fixture, PACKAGE};
use crate::events::Event;
use crate::{job, Code};
use serde_json::{json, Value};
use std::path::{Path, PathBuf};

/// Builds with `manifest` written into a picked package (bytes changed by
/// `tamper`), in a fresh workspace.
fn build_picked(
    fx: &Fixture,
    name: &str,
    manifest: &Value,
    tamper: impl Fn(&mut Vec<u8>),
) -> (anyhow::Result<job::JobRecord>, PathBuf) {
    let d = fx.dir.path();
    let picked = d.join(format!("{name}.tar"));
    write_package(d, manifest, &picked, &[]);
    let mut bytes = std::fs::read(&picked).unwrap();
    tamper(&mut bytes);
    std::fs::write(&picked, &bytes).unwrap();
    let ws = d.join(format!("ws-{name}"));
    let mut o = opts(&ws, false);
    o.imports = vec![picked];
    let (rep, _) = quiet();
    (job::build(&fx.catalog, &o, &rep), ws)
}

fn nothing_downloaded(ws: &Path) {
    for f in [
        "fw.7z.001".to_string(),
        PACKAGE.to_string(),
        format!("{PACKAGE}.partial"),
    ] {
        assert!(!ws.join("cache").join(&f).exists(), "{f} was downloaded");
    }
}

/// The build is refused with `code`, a hint containing `hint` and a message
/// containing `msg`, and nothing was downloaded.
fn refused(fx: &Fixture, name: &str, manifest: &Value, code: Code, hint: &str, msg: &str) {
    refused_tampered(fx, name, manifest, |_| {}, code, hint, msg);
}

fn refused_tampered(
    fx: &Fixture,
    name: &str,
    manifest: &Value,
    tamper: impl Fn(&mut Vec<u8>),
    code: Code,
    hint: &str,
    msg: &str,
) {
    let (res, ws) = build_picked(fx, name, manifest, tamper);
    let err = res.expect_err(name);
    let (got, h) = crate::error::code_of(&err);
    assert_eq!(got, code, "{name}: {err:#}");
    let h = h.unwrap_or_default();
    assert!(h.contains(hint), "{name}: hint {h:?}");
    assert!(format!("{err:#}").contains(msg), "{name}: {err:#}");
    nothing_downloaded(&ws);
}

fn without(m: &Value, keys: &[&str]) -> Value {
    let mut m = m.clone();
    for k in keys {
        m.as_object_mut().unwrap().remove(*k);
    }
    m
}

fn with(m: &Value, key: &str, v: Value) -> Value {
    let mut m = m.clone();
    m[key] = v;
    m
}

#[test]
fn old_and_new_format_packages_are_accepted() {
    let fx = good_fixture();
    // As `dev pack` wrote them before format, device, profile, release and
    // min_installer existed (Thimsy 1.0a test 30's package).
    let old = without(
        &fx.manifest,
        &["format", "device", "profile", "release", "min_installer"],
    );
    let (res, _) = build_picked(&fx, "old", &old, |_| {});
    let rec = res.unwrap();
    assert_eq!(rec.sha256, fx.expected_sha);
    assert_eq!(rec.release, None);

    let ws = fx.dir.path().join("ws-new");
    let mut o = opts(&ws, false);
    o.imports = vec![fx.package.clone()];
    let (rep, sink) = quiet();
    let rec = job::build(&fx.catalog, &o, &rep).unwrap();
    assert_eq!(rec.sha256, fx.expected_sha);
    assert!(
        !ws.join("cache").join(PACKAGE).exists(),
        "picked, not downloaded"
    );
    assert!(sink.0.lock().unwrap().iter().any(|e| matches!(e,
        Event::Log { message, .. } if message.contains("Thimsy Test, package format 1, 7 build steps"))));
    // The same release under another name and a newer installer: fine.
    let newer = with(
        &fx.manifest,
        "min_installer",
        json!(crate::INSTALLER_VERSION),
    );
    let (res, _) = build_picked(&fx, "renamed copy", &newer, |_| {});
    assert_eq!(res.unwrap().sha256, fx.expected_sha);
}

#[test]
fn packages_for_another_device_profile_or_firmware_are_refused() {
    let fx = fixture(None);
    let m = &fx.manifest;
    refused(
        &fx,
        "source",
        &with(m, "source_sha256", json!("ab".repeat(32))),
        Code::BadPackage,
        "different official firmware",
        "was made from official firmware with sha256 abab",
    );
    // An old-format package is tied to its device by the firmware alone.
    refused(
        &fx,
        "source-old",
        &with(
            &without(m, &["device", "profile"]),
            "source_sha256",
            json!("ab".repeat(32)),
        ),
        Code::BadPackage,
        "different official firmware",
        "firmware.pac",
    );
    refused(
        &fx,
        "device",
        &with(m, "device", json!("anbernic-rg556")),
        Code::BadPackage,
        "another device or profile",
        "is a package for anbernic-rg556, not testdev",
    );
    refused(
        &fx,
        "profile",
        &with(m, "profile", json!("light")),
        Code::BadPackage,
        "another device or profile",
        "for the light profile, not heavy",
    );
}

#[test]
fn unknown_formats_and_newer_installers_are_refused() {
    let fx = fixture(None);
    let m = &fx.manifest;
    refused(
        &fx,
        "format2",
        &with(m, "format", json!(2)),
        Code::BadPackage,
        "needs a newer Thimsy Installer",
        "is a format 2 package; this installer reads format 1",
    );
    for bad in [json!(0), json!("1"), json!(1.5)] {
        refused(
            &fx,
            "format-bad",
            &with(m, "format", bad),
            Code::BadPackage,
            "Choose the Thimsy package for your device",
            "has an unknown format",
        );
    }
    refused(
        &fx,
        "min",
        &with(m, "min_installer", json!("0.1.0-test999")),
        Code::BadPackage,
        "needs a newer Thimsy Installer",
        "needs Thimsy Installer 0.1.0-test999 or newer",
    );
    refused(
        &fx,
        "min-release",
        &with(m, "min_installer", json!("0.2.0")),
        Code::BadPackage,
        "needs a newer Thimsy Installer",
        "needs Thimsy Installer 0.2.0 or newer",
    );
    refused(
        &fx,
        "min-bad",
        &with(m, "min_installer", json!("soon")),
        Code::BadPackage,
        "Choose the Thimsy package for your device",
        "malformed min_installer",
    );
}

#[test]
fn malformed_manifests_are_refused() {
    let fx = fixture(None);
    let m = &fx.manifest;
    let not_pkg = "Choose the Thimsy package for your device";
    refused(
        &fx,
        "unknown-field",
        &with(m, "signature", json!("x")),
        Code::BadPackage,
        not_pkg,
        "unknown field `signature`",
    );
    refused(
        &fx,
        "no-build",
        &without(m, &["build"]),
        Code::BadPackage,
        not_pkg,
        "missing field `build`",
    );
    // Only the typed steps exist: nothing else can run.
    let mut evil = m.clone();
    evil["build"][0] = json!({"op": "run_command", "command": "rm -rf /"});
    refused(
        &fx,
        "op",
        &evil,
        Code::BadPackage,
        not_pkg,
        "unknown variant `run_command`",
    );
    let mut evil = m.clone();
    evil["build"][2]["path"] = json!("/etc");
    refused(
        &fx,
        "op-field",
        &evil,
        Code::BadPackage,
        not_pkg,
        "unknown field `path`",
    );
    // Patches keep their input and output checks.
    let mut no_check = m.clone();
    no_check["build"][0]
        .as_object_mut()
        .unwrap()
        .remove("in_sha256");
    refused(
        &fx,
        "no-in",
        &no_check,
        Code::BadPackage,
        not_pkg,
        "missing field `in_sha256`",
    );
    let mut weak = m.clone();
    weak["build"][0]["out_sha256"] = json!("x");
    refused(
        &fx,
        "weak-out",
        &weak,
        Code::BadPackage,
        not_pkg,
        "must be sha256s",
    );
    let mut unknown = m.clone();
    unknown["build"][4]["artifact"] = json!("nope");
    refused(
        &fx,
        "unknown-delta",
        &unknown,
        Code::BadPackage,
        not_pkg,
        "unknown delta \"nope\"",
    );
    let mut path = m.clone();
    path["artifacts"][0]["file"] = json!("../vb.bsdiff");
    refused(
        &fx,
        "path",
        &path,
        Code::BadPackage,
        not_pkg,
        "isn't a plain file name",
    );
    refused(
        &fx,
        "ref",
        &with(m, "reference_sha256", json!("ABC")),
        Code::BadPackage,
        not_pkg,
        "reference_sha256 isn't a sha256",
    );
    // A member the manifest lists that the tar doesn't have.
    let mut listed = m.clone();
    listed["artifacts"]
        .as_array_mut()
        .unwrap()
        .push(json!({"id": "x", "file": "missing.bsdiff", "size": 1, "sha256": "0".repeat(64)}));
    let d = fx.dir.path();
    let staging = d.join("only-listed");
    std::fs::create_dir_all(&staging).unwrap();
    std::fs::write(
        staging.join("pack.json"),
        serde_json::to_vec(&listed).unwrap(),
    )
    .unwrap();
    let files: Vec<PathBuf> = std::iter::once(staging.join("pack.json"))
        .chain(["thimsy-system.img.zst", "vb.bsdiff", "odm.bsdiff"].map(|f| d.join(f)))
        .collect();
    let refs: Vec<&Path> = files.iter().map(|p| p.as_path()).collect();
    let picked = d.join("missing.tar");
    crate::bundle::write(&picked, &refs, &mut |_, _| {}).unwrap();
    let ws = d.join("ws-missing");
    let mut o = opts(&ws, false);
    o.imports = vec![picked];
    let (rep, _) = quiet();
    let err = job::build(&fx.catalog, &o, &rep).unwrap_err();
    assert_eq!(crate::error::code_of(&err).0, Code::BadPackage, "{err:#}");
    assert!(
        err.to_string()
            .contains("missing.bsdiff that its pack.json lists is missing"),
        "{err}"
    );
    nothing_downloaded(&ws);
}

#[test]
fn damaged_or_foreign_files_are_refused_without_downloading() {
    let fx = fixture(None);
    let m = &fx.manifest;
    // A flipped byte in the system image: its sha256 doesn't match.
    let sys_size = m["system"]["size"].as_u64().unwrap() as usize;
    refused_tampered(
        &fx,
        "corrupt",
        m,
        |b| {
            let i = 2048 + sys_size / 2; // inside the second member
            b[i] ^= 0xFF;
        },
        Code::HashMismatch,
        "The package file is damaged",
        "thimsy-system.img.zst in corrupt.tar (the file you chose) has sha256",
    );
    // Cut short (an unfinished download): the wrong size.
    refused_tampered(
        &fx,
        "short",
        m,
        |b| b.truncate(b.len() / 2),
        Code::BadPackage,
        "a damaged or unfinished download",
        "cut short",
    );
    // Not a tar at all.
    refused_tampered(
        &fx,
        "video",
        m,
        |b| *b = b"\x00\x00\x00\x18ftypmp42 not firmware".repeat(64),
        Code::BadPackage,
        "Choose the Thimsy package for your device",
        "video.tar (the file you chose) isn't a Thimsy package",
    );
    // A tar without pack.json.
    let d = fx.dir.path();
    let picked = d.join("no-manifest.tar");
    let files = [d.join("vb.bsdiff")];
    let refs: Vec<&Path> = files.iter().map(|p| p.as_path()).collect();
    crate::bundle::write(&picked, &refs, &mut |_, _| {}).unwrap();
    let ws = d.join("ws-no-manifest");
    let mut o = opts(&ws, false);
    o.imports = vec![picked];
    let (rep, _) = quiet();
    let err = job::build(&fx.catalog, &o, &rep).unwrap_err();
    assert_eq!(crate::error::code_of(&err).0, Code::BadPackage);
    assert!(err.to_string().contains("it has no pack.json"), "{err}");
    nothing_downloaded(&ws);
}

#[test]
fn patches_still_check_the_firmware_they_apply_to() {
    let fx = fixture(None);
    // The right official firmware, but a patch made for another copy of
    // VENDORBOOT: the build stops at that step.
    let mut m = fx.manifest.clone();
    m["build"][0]["in_sha256"] = json!("cd".repeat(32));
    let (res, _) = build_picked(&fx, "wrong-input", &m, |_| {});
    let err = res.unwrap_err();
    assert_eq!(crate::error::code_of(&err).0, Code::HashMismatch, "{err:#}");
    assert!(
        format!("{err:#}").contains("isn't the one this delta was made for"),
        "{err:#}"
    );
}

#[test]
fn a_damaged_download_is_refused_and_downloaded_again_next_time() {
    let fx = good_fixture();
    let good = std::fs::read(&fx.package).unwrap();
    let mut bad = good.clone();
    let i = good.len() / 2;
    bad[i] ^= 0xFF;
    std::fs::write(&fx.package, &bad).unwrap();
    let ws = fx.dir.path().join("ws");
    let (rep, _) = quiet();
    let err = job::build(&fx.catalog, &opts(&ws, false), &rep).unwrap_err();
    let (code, hint) = crate::error::code_of(&err);
    assert_eq!(code, Code::HashMismatch, "{err:#}");
    assert!(hint
        .unwrap()
        .contains("The download was damaged. Try again"));
    assert!(err.to_string().contains("the download is damaged"), "{err}");
    assert!(
        !ws.join("cache").join(PACKAGE).exists(),
        "the damaged copy is gone"
    );
    assert!(
        !ws.join("cache/fw.7z.001").exists(),
        "the firmware isn't downloaded for it"
    );

    // Fixed on the server: the next try downloads it again and builds.
    std::fs::write(&fx.package, &good).unwrap();
    let (rep, _) = quiet();
    let rec = job::build(&fx.catalog, &opts(&ws, false), &rep).unwrap();
    assert_eq!(rec.sha256, fx.expected_sha);

    // A download that's a package for another device is refused the same
    // way, and not kept.
    let other = with(&fx.manifest, "device", json!("anbernic-rg556"));
    write_package(fx.dir.path(), &other, &fx.package, &[]);
    let ws2 = fx.dir.path().join("ws2");
    let err = job::build(&fx.catalog, &opts(&ws2, false), &rep).unwrap_err();
    assert_eq!(crate::error::code_of(&err).0, Code::BadPackage, "{err:#}");
    assert!(
        err.to_string().starts_with(
            "the downloaded Thimsy-test-testdev-heavy.tar is a package for anbernic-rg556"
        ),
        "{err}"
    );
    assert!(!ws2.join("cache").join(PACKAGE).exists());
}
