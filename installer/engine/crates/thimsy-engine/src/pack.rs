//! Maintainer tooling: compare the official firmware with a reference Thimsy
//! build and produce the files of a Thimsy package (shared system image +
//! small deltas) and its `pack.json`, whose build steps rebuild the
//! reference exactly (see `package`).

use crate::events::Reporter;
use crate::lp::Super;
use crate::pac::{self, Pac};
use crate::package::{self, Manifest, Member, Op, Unpack};
use crate::{delta, util};
use anyhow::{bail, Context, Result};
use std::collections::HashMap;
use std::fs::{self, File};
use std::io::{BufWriter, Write};
use std::path::{Path, PathBuf};

/// What the package is for, written into `pack.json`.
#[derive(Debug, Clone)]
pub struct Target {
    /// Catalog device id.
    pub device: String,
    pub profile: String,
    /// e.g. "Thimsy 1.0a test 30".
    pub release: String,
}

fn art_for(path: &Path, id: &str) -> Result<Member> {
    let size = fs::metadata(path)?.len();
    Ok(Member {
        id: id.into(),
        file: path.file_name().unwrap().to_string_lossy().into(),
        size,
        sha256: util::sha256_file(path, |_, _| {})?,
        unpack: None,
    })
}

/// Writes the package's files and `pack.json` into `out`. `short` prefixes
/// the patches' names (e.g. `rgcube-odm-a-delta`).
pub fn make(
    source: &Path,
    reference: &Path,
    out: &Path,
    short: &str,
    system_name: &str,
    target: &Target,
    rep: &Reporter,
) -> Result<Manifest> {
    fs::create_dir_all(out)?;
    let a = Pac::open(source)?;
    let b = Pac::open(reference)?;
    if a.entries.len() != b.entries.len() {
        bail!("the two PACs have different layouts");
    }
    let mut fa = File::open(source)?;
    let mut fb = File::open(reference)?;
    let mut build = Vec::new();
    let mut artifacts = Vec::new();
    let mut by_hash: HashMap<String, String> = HashMap::new();
    let mut system = None;

    for (ea, eb) in a.entries.iter().zip(&b.entries) {
        if ea.id != eb.id || ea.size != eb.size || ea.offset != eb.offset {
            bail!("entry {} differs in layout", ea.id);
        }
        if ea.size == 0 {
            continue;
        }
        let ha = util::sha256_range(&mut fa, ea.offset, ea.size, &mut |_, _| {})?;
        let hb = util::sha256_range(&mut fb, eb.offset, eb.size, &mut |_, _| {})?;
        if ha == hb {
            continue;
        }
        rep.info(format!("{} changed", ea.id));
        let is_super = {
            let mut sa = Super::new(&mut fa, ea.offset, ea.size);
            sa.metadata().is_ok()
        };
        if is_super {
            let pa = Super::new(&mut fa, ea.offset, ea.size).partitions()?;
            let pb = Super::new(&mut fb, eb.offset, eb.size).partitions()?;
            for ((na, xa), (nb, xb)) in pa.iter().zip(pb.iter()) {
                if na != nb {
                    bail!("super partition order differs");
                }
                if xa.is_empty() && xb.is_empty() {
                    continue;
                }
                if xb.len() > xa.len() {
                    build.push(Op::LpGrow {
                        entry: ea.id.clone(),
                        partition: na.clone(),
                    });
                }
                let ca = Super::new(&mut fa, ea.offset, ea.size).read_partition(na)?;
                let cb = Super::new(&mut fb, eb.offset, eb.size).read_partition(nb)?;
                if ca == cb {
                    continue;
                }
                if na.starts_with("system") {
                    // The shared system image: zstd level 19 with long-distance
                    // matching (a 128 MiB window, which every decoder accepts by
                    // default). The 1.0a image is 2.08 GB this way, against 2.17 GB
                    // at level 12, which keeps a whole package under GitHub's
                    // 2 GiB asset limit. Decoding speed is the same.
                    let path = out.join(format!("{system_name}.img.zst"));
                    rep.info(format!(
                        "Compressing {na} ({}) as {}",
                        util::human_bytes(cb.len() as u64),
                        path.display()
                    ));
                    let f = BufWriter::new(File::create(&path)?);
                    let mut enc = zstd::stream::write::Encoder::new(f, 19)?;
                    enc.multithread(
                        std::thread::available_parallelism()
                            .map(|n| n.get() as u32)
                            .unwrap_or(4)
                            .min(32),
                    )?;
                    enc.long_distance_matching(true)?;
                    enc.window_log(27)?;
                    enc.write_all(&cb)?;
                    enc.finish()?.flush()?;
                    let mut art = art_for(&path, system_name)?;
                    art.unpack = Some(Unpack {
                        format: "zstd".into(),
                        size: cb.len() as u64,
                        sha256: util::sha256_hex(&cb),
                    });
                    build.push(Op::LpWrite {
                        entry: ea.id.clone(),
                        partition: na.clone(),
                        artifact: "@system".into(),
                    });
                    system = Some(art);
                } else {
                    let id = format!("{short}-{}-delta", na.replace('_', "-"));
                    let patch = delta::make(&ca, &cb)?;
                    let path = out.join(format!("{id}.bsdiff"));
                    fs::write(&path, &patch)?;
                    artifacts.push(art_for(&path, &id)?);
                    build.push(Op::LpDelta {
                        entry: ea.id.clone(),
                        partition: na.clone(),
                        artifact: id,
                        in_sha256: util::sha256_hex(&ca),
                        out_sha256: util::sha256_hex(&cb),
                    });
                }
            }
        } else {
            let ca = a.read_entry(ea)?;
            let cb = b.read_entry(eb)?;
            let key = format!("{}>{}", util::sha256_hex(&ca), util::sha256_hex(&cb));
            let id = match by_hash.get(&key) {
                Some(id) => id.clone(),
                None => {
                    let id = format!(
                        "{short}-{}-delta",
                        ea.id.to_ascii_lowercase().replace('_', "-")
                    );
                    let patch = delta::make(&ca, &cb)?;
                    let path = out.join(format!("{id}.bsdiff"));
                    fs::write(&path, &patch)?;
                    artifacts.push(art_for(&path, &id)?);
                    by_hash.insert(key, id.clone());
                    id
                }
            };
            build.push(Op::PacDelta {
                entry: ea.id.clone(),
                artifact: id,
                in_sha256: util::sha256_hex(&ca),
                out_sha256: util::sha256_hex(&cb),
            });
        }
    }
    if a.version() != b.version() || a.alias() != b.alias() {
        build.push(Op::PacSetInfo {
            version: b.version(),
            alias: b.alias(),
        });
    }
    build.push(Op::PacFixCrc);
    let (_, crc_b) = pac::stored_crcs(reference)?;
    rep.info(format!("Reference data CRC {crc_b:04x}"));
    let res = Manifest {
        format: package::FORMAT,
        device: Some(target.device.clone()),
        profile: Some(target.profile.clone()),
        release: Some(target.release.clone()),
        min_installer: Some(package::MIN_INSTALLER.into()),
        system: system.context("the system partition didn't change; nothing to pack")?,
        artifacts,
        build,
        reference_sha256: util::sha256_file(reference, |_, _| {})?,
        source_sha256: util::sha256_file(source, |_, _| {})?,
    };
    let json = serde_json::to_vec_pretty(&res)?;
    // What the installer will accept, checked before anything ships.
    package::parse(&json, "the new pack.json")?;
    fs::write(out.join(package::MANIFEST), json)?;
    Ok(res)
}

pub fn default_out(dir: &Path) -> PathBuf {
    dir.to_path_buf()
}
