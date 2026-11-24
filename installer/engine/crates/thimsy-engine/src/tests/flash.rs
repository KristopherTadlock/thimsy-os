//! The real RG Cube recipe against the bundled catalog (no firmware needed).
use crate::catalog::Catalog;
use crate::detect;
use crate::flash::{self, Mode};

#[test]
fn rg_cube_recipe_never_writes_device_unique_partitions() {
    let c = Catalog::bundled().unwrap();
    let d = c.device("anbernic-rg-cube").unwrap();
    let f = d.flash.as_ref().unwrap();
    for p in ["Persist", "calinv", "isedata", "UserData"] {
        assert!(
            f.never_write.iter().any(|x| x == p),
            "{p} must be in never_write"
        );
    }
    assert_eq!(
        f.update,
        [
            "Super",
            "VENDORBOOT",
            "VENDORBOOT_B",
            "DTBO",
            "DTBO_B",
            "BootLogo",
            "Fastboot_Logo"
        ]
    );
    assert_eq!(
        f.writes_update,
        [
            "logo",
            "fbootlogo",
            "vendor_boot_a",
            "vendor_boot_b",
            "dtbo_a",
            "dtbo_b",
            "super"
        ]
    );
    // An update writes the dtbo (the USB-C dock fix) instead of requiring
    // the stock one; the rest of the boot chain is still checked.
    assert!(!f.update_check.iter().any(|e| e.starts_with("DTBO")));
    for e in ["BOOT", "INITBOOT", "DTB", "DTB_B", "VBMETA"] {
        assert!(f.update_check.iter().any(|x| x == e), "{e} is checked");
    }
    assert_eq!(f.erase_full, ["userdata", "metadata"]);
    // The boot-area loaders aren't in the PAC's partition table.
    assert_eq!(f.unchecked_partitions, ["splloader", "splloader_bak"]);
    // Its sizes come in 8 KiB units (prodnv: 8192 for 64 MiB).
    assert!(f.partition_list_4k);
    assert!(
        Mode::parse("full").is_ok() && Mode::parse("update").is_ok() && Mode::parse("x").is_err()
    );
}

/// An update plan for the real RG Cube recipe, on a synthetic PAC with the
/// RG Cube's entries for it: the spd_dump command writes both dtbo slots and
/// never reads a dtbo back, but still reads back the rest of the boot chain
/// (e2e.sh checks the same on the real firmware, when it is given it).
#[test]
fn rg_cube_update_writes_dtbo_and_reads_back_the_rest() {
    use crate::pac::{self, Pac};
    use crate::testutil::*;
    let c = Catalog::bundled().unwrap();
    let d = c.device("anbernic-rg-cube").unwrap();
    let f = d.flash.as_ref().unwrap();
    let written: &[(&str, &str)] = &[
        ("VENDORBOOT", "vendor_boot_a"),
        ("VENDORBOOT_B", "vendor_boot_b"),
        ("DTBO", "dtbo_a"),
        ("DTBO_B", "dtbo_b"),
        ("BootLogo", "logo"),
        ("Fastboot_Logo", "fbootlogo"),
        ("Super", "super"),
    ];
    // update_check's entries and their partitions (BOOT_B: boot_b, INITBOOT: init_boot_a).
    let checked: Vec<(String, String)> = f
        .update_check
        .iter()
        .map(|id| {
            let (stem, slot) = match id.strip_suffix("_B") {
                Some(s) => (s, "b"),
                None => (id.as_str(), "a"),
            };
            let block = match stem {
                "INITBOOT" => "init_boot".to_string(),
                s => s.to_ascii_lowercase(),
            };
            (id.clone(), format!("{block}_{slot}"))
        })
        .collect();
    let never: Vec<(String, String)> = f
        .never_write
        .iter()
        .map(|n| (n.clone(), n.to_ascii_lowercase()))
        .collect();
    let mut files: Vec<(&str, &str, Option<&str>, u64, bool)> = vec![
        ("FDL", "FDL", None, 0x65000800, false),
        ("FDL2", "NAND_FDL", None, 0xb4fffe00, false),
        ("SPLLoader", "UBOOT_LOADER2", Some("splloader"), 0, false),
        (
            "SPLLoader_bak",
            "UBOOT_LOADER2",
            Some("splloader_bak"),
            0,
            false,
        ),
        ("ProdNV", "CODE2", Some("prodnv"), 0, true),
    ];
    let mut table: Vec<(&str, u64)> = vec![("prodnv", 64)];
    for &(id, block) in written {
        files.push((id, "CODE2", Some(block), 0, false));
        table.push((block, if id == "Super" { 12 } else { 1 }));
    }
    for (id, block) in &checked {
        files.push((id.as_str(), "CODE2", Some(block.as_str()), 0, false));
        table.push((block.as_str(), 1));
    }
    for (id, block) in &never {
        files.push((id.as_str(), "CODE2", Some(block.as_str()), 0, false));
    }
    table.push(("userdata", pac::REST_OF_DISK));
    // never_write entries carry no data, as UserData doesn't in the real PAC.
    let mut entries: Vec<E> = files
        .iter()
        .enumerate()
        .map(|(i, file)| {
            let empty = never.iter().any(|(n, _)| n == file.0);
            let data = if empty {
                Vec::new()
            } else {
                pattern(4096, i as u32 + 1)
            };
            E(file.0, "img", data)
        })
        .collect();
    entries.push(E("", "cfg.xml", config_xml_with_table(&files, &table)));
    let dir = tempfile::tempdir().unwrap();
    let path = dir.path().join("cube.pac");
    std::fs::write(&path, build_pac(&entries, "v", "a")).unwrap();
    let p = Pac::open(&path).unwrap();

    let plan = flash::spd_plan(d, &p, Mode::Update).unwrap();
    let mut got: Vec<&str> = plan.writes.iter().map(|w| w.partition.as_str()).collect();
    got.sort();
    let mut want: Vec<&str> = f.writes_update.iter().map(String::as_str).collect();
    want.sort();
    assert_eq!(got, want);
    assert!(got.contains(&"dtbo_a") && got.contains(&"dtbo_b"));
    assert!(plan.erases.is_empty());
    let base: Vec<&str> = plan.base.iter().map(|b| b.partition.as_str()).collect();
    assert_eq!(base.len(), f.update_check.len());
    assert!(!base.iter().any(|b| b.starts_with("dtbo")), "{base:?}");
    for b in [
        "boot_a",
        "boot_b",
        "init_boot_a",
        "dtb_a",
        "dtb_b",
        "vbmeta_a",
        "vbmeta_system_b",
    ] {
        assert!(base.contains(&b), "{b} is read back");
    }
    let s = flash::spd_invocation(d, &plan, "backup", false).display();
    assert!(s.contains("write_part dtbo_a dtbo_a.img"), "{s}");
    assert!(s.contains("write_part dtbo_b dtbo_b.img"), "{s}");
    assert!(
        s.contains("read_part dtb_b 0 0x1000 backup/check-dtb_b.img"),
        "{s}"
    );
    assert!(!s.contains("read_part dtbo"), "{s}");
    assert!(!s.contains("erase_part"), "{s}");
}

#[test]
fn unlock_routes() {
    let c = Catalog::bundled().unwrap();
    let rep =
        crate::events::Reporter::new(std::sync::Arc::new(crate::events::MemorySink::default()));
    let err = flash::unlock(&c, "anbernic-rg-cube", &rep).unwrap_err();
    assert_eq!(crate::error::code_of(&err).0, crate::Code::UnlockGuided);
    flash::unlock(&c, "anbernic-rg557", &rep).unwrap(); // route none
}

#[test]
fn detect_names_generic_modes() {
    let c = Catalog::bundled().unwrap();
    assert_eq!(
        detect::classify(&c, 0x18d1, 0x4ee0, None).unwrap().mode,
        "fastboot"
    );
    assert_eq!(
        detect::classify(&c, 0x0e8d, 0x2000, None).unwrap().mode,
        "preloader"
    );
    let adb = detect::classify(&c, 0x1234, 0x0001, Some("RG Cube")).unwrap();
    assert_eq!(
        (adb.mode.as_str(), adb.device.as_deref()),
        ("adb", Some("anbernic-rg-cube"))
    );
}
