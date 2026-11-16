use crate::catalog::{Catalog, InstallSupport};
use crate::host::Host;

const ROOT: &str = "version: t\nrelease: R\n";
const PROFILE: &str = "id: heavy\nname: Heavy\nsummary: s\n";
const DEV: &str = r#"id: d
name: D
maker: M
soc: S
display: "1x1"
status: candidate
hardware: { android_api: 34, armv82: true, vulkan: true, ram_gb: 8, gpu: g }
support: { build: candidate, boot: candidate, install: { windows: untested } }
guides: { unlock: { route: none } }
"#;

fn files(extra: &[(&str, &str)]) -> Vec<(String, String)> {
    let mut v = vec![
        ("catalog.yaml".into(), ROOT.into()),
        ("profiles/heavy.yaml".into(), PROFILE.into()),
    ];
    v.extend(extra.iter().map(|(a, b)| (a.to_string(), b.to_string())));
    v
}

#[test]
fn bundled_catalog_is_consistent() {
    let c = Catalog::bundled().unwrap();
    for d in &c.devices {
        // Every device names every host explicitly or defaults to none.
        for h in [Host::Windows, Host::Macos, Host::Linux, Host::Android] {
            let _ = d.install_on(h);
        }
        // Android hosts can't flash in this version.
        assert_eq!(
            d.install_on(Host::Android),
            InstallSupport::None,
            "{}",
            d.id
        );
        // Buildable devices have a flash recipe and guides.
        if d.buildable() {
            assert!(d.flash.is_some(), "{} builds but can't flash", d.id);
            assert!(
                !d.guides.download_mode.is_empty(),
                "{} lacks download-mode steps",
                d.id
            );
            assert!(!d.guides.backup.is_empty(), "{} lacks backup steps", d.id);
        }
        // Devices this version can't flash somewhere must explain why on that host.
        if d.install_on(Host::Macos) == InstallSupport::None {
            assert!(
                crate::resolve::host_text(d, Host::Macos).contains("Windows"),
                "{}",
                d.id
            );
        }
    }
    // Heavy is published (the RG Cube has a package for it); light isn't.
    assert!(c.profile_published("heavy"));
    assert!(!c.profile_published("light"));
}

#[test]
fn rejects_malformed_catalogs() {
    // Missing root.
    assert!(Catalog::from_files(vec![("profiles/heavy.yaml".into(), PROFILE.into())]).is_err());
    // Unknown field (typo protection).
    assert!(
        Catalog::from_files(files(&[("devices/d.yaml", &format!("{DEV}colour: red\n"))])).is_err()
    );
    // Duplicate device ids.
    assert!(
        Catalog::from_files(files(&[("devices/a.yaml", DEV), ("devices/b.yaml", DEV)])).is_err()
    );
    let src = "source: { id: s, name: S, kind: file, parts: [], contains: f.pac, container: { format: pac, size: 1, sha256: \"0000000000000000000000000000000000000000000000000000000000000000\" } }\n";
    let pkg = "packages: { heavy: { file: Thimsy-1.0a-d-heavy.tar, approx_size: 1 } }\n";
    Catalog::from_files(files(&[("devices/d.yaml", &format!("{DEV}{src}{pkg}"))])).unwrap();
    // Packages need the official firmware they're built from.
    let err = Catalog::from_files(files(&[("devices/d.yaml", &format!("{DEV}{pkg}"))]))
        .unwrap_err()
        .to_string();
    assert!(err.contains("no source firmware"), "{err}");
    // A package for an unknown profile, or with a path for a name.
    let light = "packages: { light: { file: x.tar, approx_size: 1 } }\n";
    assert!(
        Catalog::from_files(files(&[("devices/d.yaml", &format!("{DEV}{src}{light}"))])).is_err()
    );
    let path = "packages: { heavy: { file: ../x.tar, approx_size: 1 } }\n";
    assert!(
        Catalog::from_files(files(&[("devices/d.yaml", &format!("{DEV}{src}{path}"))])).is_err()
    );
    // Malformed sha256 of the official firmware.
    let bad_src = src.replace(&"0".repeat(64), "xyz");
    assert!(Catalog::from_files(files(&[(
        "devices/d.yaml",
        &format!("{DEV}{bad_src}{pkg}")
    )]))
    .is_err());
    // Builds aren't pinned in the catalog any more: the old fields are
    // refused like any unknown field.
    for old in [
        "build: [{ op: pac_fix_crc }]\n",
        "expect: { heavy: abc }\n",
        "artifacts: []\n",
        "bundles: {}\n",
    ] {
        assert!(
            Catalog::from_files(files(&[("devices/d.yaml", &format!("{DEV}{src}{old}"))])).is_err(),
            "{old}"
        );
    }
    let old_root = "version: t\nrelease: R\nsystems: { heavy: x }\n";
    assert!(Catalog::from_files(vec![("catalog.yaml".into(), old_root.into())]).is_err());
}

#[test]
fn loads_from_a_directory() {
    let dir = tempfile::tempdir().unwrap();
    for (name, text) in files(&[("devices/d.yaml", DEV)]) {
        let p = dir.path().join(&name);
        std::fs::create_dir_all(p.parent().unwrap()).unwrap();
        std::fs::write(p, text).unwrap();
    }
    let c = Catalog::from_dir(dir.path()).unwrap();
    assert_eq!(
        c.device("d").unwrap().install_on(Host::Linux),
        InstallSupport::None,
        "unlisted host = none"
    );
    assert!(c.device("zz").is_err());
}
