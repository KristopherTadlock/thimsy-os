//! USB access for the flashing tools. Windows needs a driver for the
//! device's download mode that `spd_dump` can use: Unisoc's own ("SPRD U2S
//! Diag", sprdvcom; unlocking needs it too) or WinUSB. When neither is
//! installed, the bundled Unisoc driver package is, behind one UAC prompt.
//! Linux needs a udev rule that lets the logged-in user open the device,
//! written once through `pkexec`. macOS needs nothing.

use crate::catalog::Device;
use crate::error::{Code, EngineError};
use crate::events::Reporter;
use crate::host::Host;
use crate::i18n::tr;
use anyhow::Result;
use serde::Serialize;
use std::path::{Path, PathBuf};

#[derive(Debug, Serialize)]
pub struct DriverStatus {
    pub status: &'static str, // ok | missing | not_required
    pub detail: String,
}

/// Where udev reads rules from (Linux).
pub const UDEV_RULE_DIRS: &[&str] = &[
    "/etc/udev/rules.d",
    "/usr/lib/udev/rules.d",
    "/lib/udev/rules.d",
];

/// The rule file `drivers --install` writes on Linux.
pub const UDEV_RULE_FILE: &str = "/etc/udev/rules.d/70-thimsy-installer.rules";

pub fn status(d: &Device, host: Host) -> DriverStatus {
    match host {
        Host::Windows => windows_status(d),
        Host::Linux => linux_status(d),
        _ => DriverStatus {
            status: "not_required",
            detail: tr("{host} needs no driver").replace("{host}", host.display()),
        },
    }
}

fn windows_status(d: &Device) -> DriverStatus {
    if !d.identify.usb.is_empty() && covered_all(&usb_ids(d)) {
        return DriverStatus {
            status: "ok",
            detail: tr("A USB driver the installer can use is installed for {ids}")
                .replace("{ids}", &ids(d)),
        };
    }
    match crate::tools::locate_dir(UNISOC_DRIVER_DIR) {
        Ok(_) => DriverStatus {
            status: "missing",
            detail: tr("Unisoc's USB driver will be installed for {ids} the first time you flash")
                .replace("{ids}", &ids(d)),
        },
        Err(_) => DriverStatus {
            status: "missing",
            detail: tr("Unisoc's USB driver isn't bundled with this build. Install it from the UnisocTools of GammaOS Next's installation guide."),
        },
    }
}

fn linux_status(d: &Device) -> DriverStatus {
    if is_root() {
        return DriverStatus {
            status: "ok",
            detail: tr("The installer runs as root, so it can open the device"),
        };
    }
    let dirs: Vec<PathBuf> = UDEV_RULE_DIRS.iter().map(PathBuf::from).collect();
    if rules_cover(&dirs, d) {
        return DriverStatus {
            status: "ok",
            detail: tr("USB access is set up for {ids}").replace("{ids}", &ids(d)),
        };
    }
    DriverStatus {
        status: "missing",
        detail: tr(
            "USB access for {ids} isn't set up yet. Setting it up asks for your password once.",
        )
        .replace("{ids}", &ids(d)),
    }
}

#[cfg(unix)]
fn is_root() -> bool {
    // SAFETY: geteuid can't fail.
    unsafe { libc::geteuid() == 0 }
}

#[cfg(not(unix))]
fn is_root() -> bool {
    false
}

/// Whether the udev rules in `dirs` mention every USB ID of the device on
/// one (uncommented) line, case-insensitively: a rule matching it.
pub fn rules_cover(dirs: &[PathBuf], d: &Device) -> bool {
    if d.identify.usb.is_empty() {
        return false;
    }
    let mut lines = Vec::new();
    for dir in dirs {
        for e in std::fs::read_dir(dir).into_iter().flatten().flatten() {
            let p = e.path();
            if p.extension().is_some_and(|x| x == "rules") {
                if let Ok(text) = std::fs::read_to_string(&p) {
                    lines.extend(
                        text.lines()
                            .map(|l| l.trim().to_ascii_lowercase())
                            .filter(|l| !l.starts_with('#')),
                    );
                }
            }
        }
    }
    d.identify.usb.iter().all(|u| {
        let (v, p) = (format!("{:04x}", u.vid), format!("{:04x}", u.pid));
        lines.iter().any(|l| l.contains(&v) && l.contains(&p))
    })
}

/// The udev rules giving the logged-in user access to the device's USB
/// IDs (one line per ID).
pub fn udev_rules(d: &Device) -> String {
    let mut out =
        String::from("# Written by Thimsy Installer: lets the logged-in user flash the device.\n");
    for u in &d.identify.usb {
        out.push_str(&format!(
            "SUBSYSTEM==\"usb\", ATTR{{idVendor}}==\"{:04x}\", ATTR{{idProduct}}==\"{:04x}\", TAG+=\"uaccess\"\n",
            u.vid, u.pid
        ));
    }
    out
}

/// Text of an .inf file: UTF-16 (with a byte-order mark) or 8-bit.
pub fn decode_inf(bytes: &[u8]) -> String {
    if bytes.starts_with(&[0xFF, 0xFE]) {
        let units: Vec<u16> = bytes[2..]
            .as_chunks::<2>()
            .0
            .iter()
            .map(|c| u16::from_le_bytes(*c))
            .collect();
        return String::from_utf16_lossy(&units);
    }
    String::from_utf8_lossy(bytes).into_owned()
}

/// An .inf's text without whitespace, in capitals.
fn inf_flat(text: &str) -> String {
    text.chars()
        .filter(|c| !c.is_whitespace())
        .collect::<String>()
        .to_ascii_uppercase()
}

/// Whether an .inf installs WinUSB, and only WinUSB, for VID:PID: it names
/// the ID, includes `winusb.inf` and adds no service but WinUSB (libwdi's
/// and Zadig's packages, or any other built the same way). A vendor package
/// that merely mentions WinUSB somewhere doesn't count.
pub fn inf_is_winusb_for(text: &str, vid: u16, pid: u16) -> bool {
    inf_flat_is_winusb_for(&inf_flat(text), vid, pid)
}

fn inf_flat_is_winusb_for(t: &str, vid: u16, pid: u16) -> bool {
    let services = t.matches("ADDSERVICE=").count();
    t.contains(&format!("VID_{vid:04X}&PID_{pid:04X}"))
        && t.contains("INCLUDE=WINUSB.INF")
        && services > 0
        && t.matches("ADDSERVICE=WINUSB,").count() == services
}

/// Whether an .inf installs Unisoc's serial driver (sprdvcom, "SPRD U2S
/// Diag") for VID:PID.
pub fn inf_is_unisoc_for(text: &str, vid: u16, pid: u16) -> bool {
    inf_flat_is_unisoc_for(&inf_flat(text), vid, pid)
}

fn inf_flat_is_unisoc_for(flat: &str, vid: u16, pid: u16) -> bool {
    flat.contains(&format!("VID_{vid:04X}&PID_{pid:04X}")) && flat.contains("ADDSERVICE=SPRDVCOM,")
}

/// Whether an .inf gives VID:PID a driver `spd_dump` can use.
#[cfg_attr(not(any(windows, test)), allow(dead_code))]
fn inf_flat_covers(flat: &str, vid: u16, pid: u16) -> bool {
    inf_flat_is_unisoc_for(flat, vid, pid) || inf_flat_is_winusb_for(flat, vid, pid)
}

/// In one pass over the published packages: whether each of `ids` has a
/// driver `spd_dump` can use. Readable without admin rights; a package's
/// original name doesn't matter, so earlier installs (Unisoc's own
/// installer, Zadig) count too.
#[cfg(windows)]
fn store_covers(ids: &[(u16, u16)]) -> Vec<bool> {
    let mut covered = vec![false; ids.len()];
    for_each_published(|t| {
        let flat = inf_flat(t);
        for (i, &(v, p)) in ids.iter().enumerate() {
            covered[i] |= inf_flat_covers(&flat, v, p);
        }
    });
    covered
}

/// Calls `f` with the text of each published third-party package
/// (`<Windows>\INF\oem*.inf`).
#[cfg(windows)]
fn for_each_published(mut f: impl FnMut(&str)) {
    let Ok(files) = std::fs::read_dir(windows_dir().join("INF")) else {
        return;
    };
    for e in files.flatten() {
        let name = e.file_name().to_string_lossy().to_ascii_lowercase();
        if name.starts_with("oem") && name.ends_with(".inf") {
            if let Ok(b) = std::fs::read(e.path()) {
                f(&decode_inf(&b));
            }
        }
    }
}

/// The Windows folder, from the system (not the environment).
#[cfg(windows)]
pub(crate) fn windows_dir() -> PathBuf {
    use std::os::windows::ffi::OsStringExt;
    use windows_sys::Win32::System::SystemInformation::GetSystemWindowsDirectoryW;
    let mut buf = [0u16; 512];
    // SAFETY: the buffer is as long as passed.
    let n = unsafe { GetSystemWindowsDirectoryW(buf.as_mut_ptr(), buf.len() as u32) } as usize;
    if n == 0 || n >= buf.len() {
        return PathBuf::from(r"C:\Windows");
    }
    PathBuf::from(std::ffi::OsString::from_wide(&buf[..n]))
}

/// Whether every one of `ids` has a driver `spd_dump` can use.
#[cfg(windows)]
fn covered_all(ids: &[(u16, u16)]) -> bool {
    store_covers(ids).iter().all(|&c| c)
}

#[cfg(not(windows))]
fn covered_all(_ids: &[(u16, u16)]) -> bool {
    test_driver()
}

fn usb_ids(d: &Device) -> Vec<(u16, u16)> {
    d.identify.usb.iter().map(|u| (u.vid, u.pid)).collect()
}

fn ids(d: &Device) -> String {
    d.identify
        .usb
        .iter()
        .map(|u| format!("{:04x}:{:04x}", u.vid, u.pid))
        .collect::<Vec<_>>()
        .join(", ")
}

/// `drivers --install`: Windows installs Unisoc's driver (one UAC prompt),
/// Linux writes the udev rule (one password prompt); other hosts need
/// nothing.
pub fn install_for(d: &Device, host: Host, rep: &Reporter) -> Result<()> {
    match host {
        Host::Windows => install(d, rep),
        Host::Linux => linux_install(d, rep),
        _ => {
            rep.info("No driver needed on this computer");
            Ok(())
        }
    }
}

/// The flash's driver step: Windows installs Unisoc's driver when no
/// driver `spd_dump` can use is there; Linux only warns when no udev rule
/// covers the device (the app offers `drivers --install`; asking for a
/// password in the middle of a flash would be a surprise); other hosts need
/// nothing.
pub fn ensure_for_flash(d: &Device, host: Host, rep: &Reporter) -> Result<()> {
    match host {
        Host::Windows => install(d, rep),
        Host::Linux => {
            if linux_status(d).status != "ok" {
                rep.warn(format!(
                    "No udev rule gives this user access to {}; if the device isn't found, set up USB access and try again",
                    ids(d)
                ));
            }
            Ok(())
        }
        _ => {
            rep.info("No driver needed on this computer");
            Ok(())
        }
    }
}

/// A PowerShell single-quoted string literal. Every character PowerShell
/// treats as a single quote (`'` and U+2018 to U+201B) is doubled, so
/// nothing in `s` can end the literal.
pub fn ps_literal(s: &str) -> String {
    let mut out = String::with_capacity(s.len() + 2);
    out.push('\'');
    for c in s.chars() {
        if matches!(c, '\'' | '\u{2018}' | '\u{2019}' | '\u{201A}' | '\u{201B}') {
            out.push(c);
        }
        out.push(c);
    }
    out.push('\'');
    out
}

/// The start of every elevated script: stop on any error, `$code` 1, the
/// administrators and SYSTEM SIDs, `New-AdminSecurity` (a protected ACL that
/// only they can change; with `$true`, signed-in users may read too) and
/// `Assert-AdminRoot`, which makes `$root` (`<ProgramData>\Thimsy
/// Installer`) a plain folder owned by Administrators or SYSTEM that nobody
/// else may change, or stops with code 6, and `New-AdminFolder`, which
/// makes a new folder in it and checks it's what was made. Standard users
/// can create folders under ProgramData, so an existing one is never taken
/// over or re-permissioned (a junction or a user's folder there would make
/// that change something else).
pub const ADMIN_PRELUDE: &str = r#"$ErrorActionPreference = 'Stop'
$code = 1
$admins = New-Object System.Security.Principal.SecurityIdentifier('S-1-5-32-544')
$system = New-Object System.Security.Principal.SecurityIdentifier('S-1-5-18')
function New-AdminSecurity([bool]$readable) {
  $sec = New-Object System.Security.AccessControl.DirectorySecurity
  $sec.SetAccessRuleProtection($true, $false)
  foreach ($sid in @($admins, $system)) {
    $sec.AddAccessRule((New-Object System.Security.AccessControl.FileSystemAccessRule($sid, 'FullControl', 'ContainerInherit,ObjectInherit', 'None', 'Allow')))
  }
  if ($readable) {
    $users = New-Object System.Security.Principal.SecurityIdentifier('S-1-5-11')
    $sec.AddAccessRule((New-Object System.Security.AccessControl.FileSystemAccessRule($users, 'ReadAndExecute', 'ContainerInherit,ObjectInherit', 'None', 'Allow')))
  }
  $sec.SetOwner($admins)
  return $sec
}
$root = Join-Path ([Environment]::GetFolderPath('CommonApplicationData')) 'Thimsy Installer'
# A plain folder (not a junction or link) owned by Administrators or
# SYSTEM, or stop (code 6).
function Assert-AdminOwned([string]$path) {
  $item = Get-Item -LiteralPath $path -Force
  if (-not $item.PSIsContainer -or ($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint)) {
    $script:code = 6; throw "$path isn't a plain folder"
  }
  $owner = (Get-Acl -LiteralPath $path).GetOwner([System.Security.Principal.SecurityIdentifier])
  if ($owner -ne $admins -and $owner -ne $system) {
    $script:code = 6; throw "$path isn't owned by administrators"
  }
}
function Assert-AdminRoot {
  if (-not (Test-Path -LiteralPath $root)) {
    [void][System.IO.Directory]::CreateDirectory($root, (New-AdminSecurity $false))
  }
  # Checked after creating it too: someone may have made it first.
  Assert-AdminOwned $root
  # Nobody else may make or change anything in it (a folder another
  # installer made with ProgramData's inherited rights may let users).
  $trusted = @($admins, $system,
    (New-Object System.Security.Principal.SecurityIdentifier('S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464')),
    (New-Object System.Security.Principal.SecurityIdentifier('S-1-3-0')))
  $writes = [int][System.Security.AccessControl.FileSystemRights]'CreateFiles, CreateDirectories, WriteAttributes, WriteExtendedAttributes, DeleteSubdirectoriesAndFiles, Delete, ChangePermissions, TakeOwnership'
  # GENERIC_ALL and GENERIC_WRITE, which rules may carry unmapped.
  $writes = $writes -bor 0x10000000 -bor 0x40000000
  foreach ($rule in (Get-Acl -LiteralPath $root).GetAccessRules($true, $true, [System.Security.Principal.SecurityIdentifier])) {
    if ($rule.AccessControlType -eq 'Allow' -and
        -not ($rule.PropagationFlags -band [System.Security.AccessControl.PropagationFlags]::InheritOnly) -and
        ([int]$rule.FileSystemRights -band $writes) -and
        -not ($trusted -contains $rule.IdentityReference)) {
      $script:code = 6; throw "others can change $root"
    }
  }
}
# A new folder only administrators and SYSTEM can change (signed-in users
# may read it with $readable), or stop: code 5 if it exists, 6 if what was
# made isn't one (someone made it first).
function New-AdminFolder([string]$path, [bool]$readable) {
  if (Test-Path -LiteralPath $path) { $script:code = 5; throw "$path already exists" }
  [void][System.IO.Directory]::CreateDirectory($path, (New-AdminSecurity $readable))
  Assert-AdminOwned $path
}
"#;

/// Why an elevated script stopped with code 6.
pub const UNSAFE_ROOT: &str = "the Thimsy Installer folder in ProgramData isn't one only administrators control; delete it and try again";

/// A driver package: its folder, and each file's path in it and SHA-256.
pub type Package = (PathBuf, Vec<(String, String)>);

/// The bundled Unisoc driver package: a folder next to the tools.
pub const UNISOC_DRIVER_DIR: &str = "unisoc-driver";

/// Its files (as GammaOS's UnisocTools has them; signed by Microsoft).
pub const UNISOC_DRIVER_FILES: &[&str] = &[
    "sprdvcom.inf",
    "sprdvcom.cat",
    "amd64/sprdvcom.sys",
    "amd64/WdfCoInstaller01009.dll",
    "i386/sprdvcom.sys",
    "i386/WdfCoInstaller01009.dll",
];

/// The script the elevated PowerShell runs: copies the driver package's
/// `files` (path in the package, SHA-256) from `src` into a new folder only
/// administrators and SYSTEM can change (`<ProgramData>\Thimsy
/// Installer\driver-<random GUID>`), checks each copy is the file the
/// engine hashed, installs the package with Windows' own `pnputil`,
/// removes the folder and exits with pnputil's exit code (0 also when
/// Windows only needs a restart, or had no device to give it to yet).
pub fn driver_script(src: &Path, files: &[(String, String)]) -> String {
    let mut s = String::from(ADMIN_PRELUDE);
    s.push_str(
        r#"$dir = Join-Path $root ('driver-' + [guid]::NewGuid().ToString())
$made = $false
try {
  Assert-AdminRoot
  New-AdminFolder $dir $false
  $made = $true
  $pkg = Join-Path $dir 'pkg'
  New-Item -ItemType Directory -Path (Join-Path $pkg 'amd64'), (Join-Path $pkg 'i386') | Out-Null
"#,
    );
    s.push_str(&format!(
        "  $src = {}
",
        ps_literal(&src.display().to_string())
    ));
    for (file, sha) in files {
        let win = file.replace('/', "\\");
        s.push_str(&format!(
            "  Copy-Item -LiteralPath (Join-Path $src {f}) -Destination (Join-Path $pkg {f})
  if ((Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $pkg {f})).Hash -ne {h}) {{ $code = 3; throw 'the driver package changed before it could be installed' }}
",
            f = ps_literal(&win),
            h = ps_literal(&sha.to_ascii_uppercase())
        ));
    }
    s.push_str(
        r#"  $pnputil = Join-Path ([Environment]::SystemDirectory) 'pnputil.exe'
  & $pnputil '/add-driver' (Join-Path $pkg 'sprdvcom.inf') '/install'
  $code = $LASTEXITCODE
  # 3010: installed, Windows wants a restart; 259: installed, no device to
  # give it to right now.
  if ($code -eq 3010 -or $code -eq 259) { $code = 0 }
} catch {
  if ($code -eq 0) { $code = 1 }
} finally {
  if ($made) { Remove-Item -LiteralPath $dir -Recurse -Force -ErrorAction SilentlyContinue }
}
exit $code
"#,
    );
    s
}

/// PowerShell's `-EncodedCommand` form of a script: base64 of UTF-16LE.
pub fn encode_command(script: &str) -> String {
    let bytes: Vec<u8> = script
        .encode_utf16()
        .flat_map(|u| u.to_le_bytes())
        .collect();
    crate::util::base64(&bytes)
}

/// `ERROR_CANCELLED`: the user declined the UAC prompt.
#[cfg_attr(not(any(windows, test)), allow(dead_code))]
const DECLINED: i32 = 1223;

/// Installs the Unisoc driver package (in `src`, with `files`) when an ID
/// has no driver `spd_dump` can use, behind one elevated run: `run(script)`
/// runs the script elevated and returns its exit code ([`DECLINED`] when
/// the UAC prompt was declined); `covered(vid, pid)` looks in the driver
/// store. Every ID must be covered afterwards.
#[cfg_attr(not(any(windows, test)), allow(dead_code))]
fn install_ids(
    d: &Device,
    package: &dyn Fn() -> Result<Package>,
    rep: &Reporter,
    covered: &dyn Fn(u16, u16) -> bool,
    run: &mut dyn FnMut(&str) -> Result<Option<i32>>,
) -> Result<()> {
    let todo: Vec<_> = d
        .identify
        .usb
        .iter()
        .filter(|u| {
            let have = covered(u.vid, u.pid);
            if have {
                rep.info(format!(
                    "A USB driver the installer can use is installed for {}",
                    u.label
                ));
            }
            !have
        })
        .collect();
    if todo.is_empty() {
        return Ok(());
    }
    let names: Vec<String> = todo
        .iter()
        .map(|u| format!("{} ({:04x}:{:04x})", u.label, u.vid, u.pid))
        .collect();
    let (src, files) = package()?;
    rep.info(format!(
        "Installing Unisoc's USB driver for {}; Windows asks for permission",
        names.join(", ")
    ));
    let code = run(&driver_script(&src, &files))?;
    if code != Some(0) {
        let why = match code {
            Some(DECLINED) => "the Windows permission prompt was declined".to_string(),
            Some(3) => "the driver package changed before it could be installed".to_string(),
            Some(6) => UNSAFE_ROOT.to_string(),
            Some(c) => format!("exit code {c}"),
            None => "stopped".to_string(),
        };
        return Err(EngineError::new(
            Code::ToolFailed,
            format!("driver install for {} failed ({why})", names.join(", ")),
        )
        .hint("Accept the Windows permission prompt, or install Unisoc's USB driver from the UnisocTools of GammaOS Next's installation guide.")
        .into());
    }
    for u in todo {
        if !covered(u.vid, u.pid) {
            return Err(EngineError::new(
                Code::ToolFailed,
                format!(
                    "the driver install finished, but Windows still has no driver the installer can use for {} ({:04x}:{:04x})",
                    u.label, u.vid, u.pid
                ),
            )
            .hint("Accept the Windows permission prompt, or install Unisoc's USB driver from the UnisocTools of GammaOS Next's installation guide.")
            .into());
        }
    }
    Ok(())
}

/// The bundled driver package: its folder and each file's SHA-256.
#[cfg_attr(not(any(windows, test)), allow(dead_code))]
fn bundled_package() -> Result<Package> {
    let dir = crate::tools::locate_dir(UNISOC_DRIVER_DIR)?;
    let mut files = Vec::new();
    for f in UNISOC_DRIVER_FILES {
        let sha = crate::util::sha256_file(&dir.join(f), |_, _| {})?;
        files.push((f.to_string(), sha));
    }
    Ok((dir, files))
}

/// Installs Unisoc's driver when an ID of the device has no driver
/// `spd_dump` can use (Windows only).
fn install(d: &Device, rep: &Reporter) -> Result<()> {
    if let Some(r) = test_install(d, rep) {
        return r;
    }
    platform_install(d, rep)
}

#[cfg(windows)]
fn platform_install(d: &Device, rep: &Reporter) -> Result<()> {
    let ids = usb_ids(d);
    let before = store_covers(&ids);
    // From the one pass until the install ran; looked up again after it.
    let ran = std::cell::Cell::new(false);
    let rescan = std::cell::OnceCell::new();
    install_ids(
        d,
        &bundled_package,
        rep,
        &|v, p| {
            let at = ids.iter().position(|&id| id == (v, p));
            let now = if ran.get() {
                rescan.get_or_init(|| store_covers(&ids))
            } else {
                &before
            };
            at.is_some_and(|i| now[i])
        },
        &mut |script| {
            ran.set(true);
            run_elevated(script)
        },
    )
}

#[cfg(not(windows))]
fn platform_install(_d: &Device, _rep: &Reporter) -> Result<()> {
    Err(EngineError::new(
        Code::UnsupportedHost,
        "Unisoc's USB driver can only be installed on Windows",
    )
    .into())
}

/// Windows PowerShell, by absolute path in the system folder.
#[cfg(windows)]
pub(crate) fn powershell_path() -> Result<PathBuf> {
    use std::os::windows::ffi::OsStringExt;
    use windows_sys::Win32::System::SystemInformation::GetSystemDirectoryW;
    let mut buf = [0u16; 512];
    // SAFETY: the buffer is as long as passed.
    let n = unsafe { GetSystemDirectoryW(buf.as_mut_ptr(), buf.len() as u32) } as usize;
    if n == 0 || n >= buf.len() {
        return Err(std::io::Error::last_os_error().into());
    }
    let sys = PathBuf::from(std::ffi::OsString::from_wide(&buf[..n]));
    Ok(sys.join("WindowsPowerShell\\v1.0\\powershell.exe"))
}

/// Runs `script` in a hidden, elevated Windows PowerShell (one UAC prompt)
/// and waits for it: its exit code, or [`DECLINED`] when the prompt was
/// declined. The elevated process inherits none of the engine's handles.
#[cfg(windows)]
fn run_elevated(script: &str) -> Result<Option<i32>> {
    Ok(match Process::launch(script)? {
        None => Some(DECLINED),
        Some(p) => {
            p.wait(u32::MAX);
            p.exit_code()
        }
    })
}

/// An elevated Windows PowerShell (the driver install).
#[cfg(windows)]
struct Process(windows_sys::Win32::Foundation::HANDLE);

#[cfg(windows)]
impl Process {
    /// Runs `script` in a hidden, elevated Windows PowerShell (one UAC
    /// prompt) without waiting for it; `None` when the prompt was
    /// declined. It inherits none of the engine's handles.
    fn launch(script: &str) -> Result<Option<Process>> {
        use std::os::windows::ffi::OsStrExt;
        use windows_sys::Win32::Foundation::{GetLastError, ERROR_CANCELLED};
        use windows_sys::Win32::UI::Shell::{
            ShellExecuteExW, SEE_MASK_NOASYNC, SEE_MASK_NOCLOSEPROCESS, SHELLEXECUTEINFOW,
        };
        let wide = |s: &std::ffi::OsStr| -> Vec<u16> { s.encode_wide().chain([0]).collect() };
        let exe = wide(powershell_path()?.as_os_str());
        let verb = wide(std::ffi::OsStr::new("runas"));
        let params = wide(std::ffi::OsStr::new(&format!(
            "-NoProfile -NonInteractive -WindowStyle Hidden -EncodedCommand {}",
            encode_command(script)
        )));
        // SAFETY: the strings outlive the call; the process handle is kept
        // and closed when the Process is dropped.
        unsafe {
            let mut info: SHELLEXECUTEINFOW = std::mem::zeroed();
            info.cbSize = std::mem::size_of::<SHELLEXECUTEINFOW>() as u32;
            info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
            info.lpVerb = verb.as_ptr();
            info.lpFile = exe.as_ptr();
            info.lpParameters = params.as_ptr();
            info.nShow = 0; // SW_HIDE
            if ShellExecuteExW(&mut info) == 0 {
                if GetLastError() == ERROR_CANCELLED {
                    return Ok(None);
                }
                return Err(std::io::Error::last_os_error().into());
            }
            if info.hProcess.is_null() {
                return Err(EngineError::new(
                    Code::ToolFailed,
                    "the elevated PowerShell couldn't start (Windows didn't start it); nothing was written",
                )
                .into());
            }
            Ok(Some(Process(info.hProcess)))
        }
    }

    /// Its exit code, once it has ended.
    fn exit_code(&self) -> Option<i32> {
        use windows_sys::Win32::Foundation::WAIT_OBJECT_0;
        use windows_sys::Win32::System::Threading::{GetExitCodeProcess, WaitForSingleObject};
        // SAFETY: the handle stays open until drop.
        unsafe {
            if WaitForSingleObject(self.0, 0) != WAIT_OBJECT_0 {
                return None;
            }
            let mut code = 0u32;
            (GetExitCodeProcess(self.0, &mut code) != 0).then_some(code as i32)
        }
    }

    /// Waits up to `ms` (`u32::MAX`: as long as it takes) for it to end;
    /// whether it has.
    fn wait(&self, ms: u32) -> bool {
        use windows_sys::Win32::Foundation::WAIT_OBJECT_0;
        use windows_sys::Win32::System::Threading::WaitForSingleObject;
        // SAFETY: the handle stays open until drop.
        unsafe { WaitForSingleObject(self.0, ms) == WAIT_OBJECT_0 }
    }
}

#[cfg(windows)]
impl Drop for Process {
    fn drop(&mut self) {
        // SAFETY: the handle is open and closed once, here.
        unsafe {
            windows_sys::Win32::Foundation::CloseHandle(self.0);
        }
    }
}

/// First of `candidates` that exists.
fn first_existing(candidates: &[&str]) -> Option<PathBuf> {
    candidates.iter().map(PathBuf::from).find(|p| p.is_file())
}

/// The fixed shell script `pkexec` runs as root: writes the rules (`$1`)
/// to [`UDEV_RULE_FILE`] and reloads udev with `$2` (udevadm's absolute
/// path). The rules and path are arguments, never part of the script.
pub const UDEV_INSTALL_SCRIPT: &str = "set -e; f=/etc/udev/rules.d/70-thimsy-installer.rules; umask 022; mkdir -p /etc/udev/rules.d; printf '%s' \"$1\" > \"$f.tmp\"; mv -f \"$f.tmp\" \"$f\"; \"$2\" control --reload-rules; \"$2\" trigger";

/// Linux: writes the udev rule through `pkexec` (one password prompt),
/// then checks it took effect.
fn linux_install(d: &Device, rep: &Reporter) -> Result<()> {
    if linux_status(d).status == "ok" {
        rep.info("USB access is already set up");
        return Ok(());
    }
    let pkexec = first_existing(&["/usr/bin/pkexec", "/bin/pkexec", "/usr/local/bin/pkexec"])
        .ok_or_else(|| {
            EngineError::new(Code::ToolMissing, "pkexec isn't installed, so USB access can't be set up from the installer")
                .hint("Install polkit (pkexec), or add a udev rule for the device's download mode yourself, then try again.")
        })?;
    let udevadm = first_existing(&[
        "/usr/bin/udevadm",
        "/bin/udevadm",
        "/usr/sbin/udevadm",
        "/sbin/udevadm",
    ])
    .ok_or_else(|| {
        EngineError::new(Code::ToolMissing, "udevadm isn't installed")
            .hint("Install polkit (pkexec), or add a udev rule for the device's download mode yourself, then try again.")
    })?;
    rep.info(format!(
        "Writing {UDEV_RULE_FILE} for {}; Linux asks for your password",
        ids(d)
    ));
    let status = std::process::Command::new(&pkexec)
        .arg("/bin/sh")
        .arg("-c")
        .arg(UDEV_INSTALL_SCRIPT)
        .arg("sh")
        .arg(udev_rules(d))
        .arg(&udevadm)
        .stdin(std::process::Stdio::null())
        .stdout(std::process::Stdio::null())
        .stderr(std::process::Stdio::null())
        .status()?;
    match status.code() {
        Some(0) => {}
        Some(126) | Some(127) => {
            return Err(EngineError::new(
                Code::ToolFailed,
                "setting up USB access was cancelled (the password prompt was declined)",
            )
            .hint("Enter your password when asked, then try again.")
            .into())
        }
        c => {
            return Err(EngineError::new(
                Code::ToolFailed,
                format!("setting up USB access failed (pkexec exit {c:?})"),
            )
            .hint("Install polkit (pkexec), or add a udev rule for the device's download mode yourself, then try again.")
            .into())
        }
    }
    if linux_status(d).status != "ok" {
        return Err(EngineError::new(
            Code::ToolFailed,
            format!("{UDEV_RULE_FILE} was written, but udev still has no rule for {}", ids(d)),
        )
        .hint("Install polkit (pkexec), or add a udev rule for the device's download mode yourself, then try again.")
        .into());
    }
    Ok(())
}

#[cfg(not(test))]
fn test_install(_d: &Device, _rep: &Reporter) -> Option<Result<()>> {
    None
}

#[cfg(all(not(test), not(windows)))]
fn test_driver() -> bool {
    false
}

// Kept last: `dev i18n-strings` stops reading a file at its first test-only
// item.

/// Test hook: pretend to be Windows with this elevated-run exit code.
#[cfg(test)]
fn test_install(d: &Device, rep: &Reporter) -> Option<Result<()>> {
    let code = test::ELEVATED_EXIT.with(|c| c.get())?;
    Some(install_ids(
        d,
        &|| {
            Ok((
                PathBuf::from(r"C:\Program Files\Thimsy Installer\engine\tools\unisoc-driver"),
                UNISOC_DRIVER_FILES
                    .iter()
                    .map(|f| (f.to_string(), "0".repeat(64)))
                    .collect(),
            ))
        },
        rep,
        &|v, p| covered_all(&[(v, p)]),
        &mut |_| {
            if code == 0 {
                test::DRIVER.with(|w| w.set(true));
            }
            Ok(Some(code))
        },
    ))
}

#[cfg(all(test, not(windows)))]
fn test_driver() -> bool {
    test::DRIVER.with(|w| w.get())
}

#[cfg(test)]
pub(crate) mod test {
    use std::cell::Cell;
    thread_local! {
        /// Some(code): `install` runs the Windows path with this exit code.
        pub static ELEVATED_EXIT: Cell<Option<i32>> = const { Cell::new(None) };
        /// What the driver store says (non-Windows test builds).
        pub static DRIVER: Cell<bool> = const { Cell::new(false) };
    }
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;

    /// Sample .inf texts and whether each is a WinUSB package, and
    /// Unisoc's driver, for 1782:4d00.
    const INF_SAMPLES: &[(&str, bool, bool)] = &[
        // libwdi's (and Zadig's) package.
        (
            "[Devices.NTamd64]\r\n\"Unisoc download mode\" = USB_Install, USB\\VID_1782&PID_4D00\r\n[USB_Install]\r\nInclude = winusb.inf\r\nNeeds = WINUSB.NT\r\n[USB_Install.Services]\r\nInclude    = winusb.inf\r\nAddService = WinUSB,0x00000002,WinUSB_ServiceInstall\r\n[WinUSB_ServiceInstall]\r\nServiceBinary = %12%\\WinUSB.sys\r\n",
            true,
            false,
        ),
        // Unisoc's own driver (as in sprdvcom.inf).
        (
            "[SPRD.NTamd64]\r\n%USBCOMM_DL%  = Serial_Inst, USB\\VID_1782&PID_4D00\r\n[Serial_Inst.Services]\r\nAddService=sprdvcom,0x00000002,Serial_Service \r\n[Serial_Service]\r\nServiceBinary  = %12%\\sprdvcom.sys\r\n",
            false,
            true,
        ),
        // Unisoc's driver for other IDs only.
        (
            "%USBCOMM_DIAG% = Serial_Inst, USB\\VID_1782&PID_4D01\r\nAddService=sprdvcom,0x00000002,Serial_Service\r\n",
            false,
            false,
        ),
        // A vendor package that also uses WinUSB, for another interface.
        (
            "USB\\VID_1782&PID_4D00\r\nAddService = rdavcom,2,S\r\n[Adb]\r\nInclude = winusb.inf\r\nAddService = WinUSB,0x00000002,W\r\n",
            false,
            false,
        ),
        // Mentions both only in a comment.
        (
            "; works like WinUSB or sprdvcom\r\nUSB\\VID_1782&PID_4D00\r\nAddService = usbser,2,S\r\n",
            false,
            false,
        ),
        // A WinUSB package for another device.
        (
            "USB\\VID_18D1&PID_4EE0\r\nInclude = winusb.inf\r\nAddService = WinUSB,0x00000002,W\r\n",
            false,
            false,
        ),
    ];

    #[test]
    fn recognises_the_drivers_spd_dump_can_use() {
        for (text, winusb, unisoc) in INF_SAMPLES {
            assert_eq!(inf_is_winusb_for(text, 0x1782, 0x4d00), *winusb, "{text}");
            assert_eq!(inf_is_unisoc_for(text, 0x1782, 0x4d00), *unisoc, "{text}");
            assert_eq!(
                inf_flat_covers(&inf_flat(text), 0x1782, 0x4d00),
                *winusb || *unisoc,
                "{text}"
            );
        }
        let (libwdi, _, _) = INF_SAMPLES[0];
        assert!(!inf_is_winusb_for(libwdi, 0x1782, 0x4d01));
        // Windows keeps some packages as UTF-16.
        let mut utf16 = vec![0xFF, 0xFE];
        for u in INF_SAMPLES[1].0.encode_utf16() {
            utf16.extend_from_slice(&u.to_le_bytes());
        }
        assert!(inf_is_unisoc_for(&decode_inf(&utf16), 0x1782, 0x4d00));
    }

    #[test]
    fn powershell_literals_double_every_kind_of_single_quote() {
        assert_eq!(ps_literal("plain"), "'plain'");
        assert_eq!(ps_literal("Ann O'Neil"), "'Ann O''Neil'");
        // PowerShell ends a single-quoted string at any of these.
        for q in ['\'', '\u{2018}', '\u{2019}', '\u{201A}', '\u{201B}'] {
            let s = format!("a{q}b; Remove-Item C:\\ -Recurse #");
            let lit = ps_literal(&s);
            assert_eq!(lit, format!("'a{q}{q}b; Remove-Item C:\\ -Recurse #'"));
            // Inside the quotes, every quote character comes in pairs.
            let inner: Vec<char> = lit[1..lit.len() - 1].chars().collect();
            let mut i = 0;
            while i < inner.len() {
                if matches!(
                    inner[i],
                    '\'' | '\u{2018}' | '\u{2019}' | '\u{201A}' | '\u{201B}'
                ) {
                    assert!(i + 1 < inner.len() && inner[i + 1] == inner[i], "{lit}");
                    i += 2;
                } else {
                    i += 1;
                }
            }
        }
        // Not quote characters: left alone.
        assert_eq!(ps_literal("\u{201C}x\u{201D}"), "'\u{201C}x\u{201D}'");
    }

    fn package() -> Result<Package> {
        Ok((
            PathBuf::from("C:\\Users\\Ann O\u{2019}Neil\\Thimsy Installer\\tools\\unisoc-driver"),
            UNISOC_DRIVER_FILES
                .iter()
                .map(|f| (f.to_string(), "abc123".to_string()))
                .collect(),
        ))
    }

    #[test]
    fn driver_script_copies_checks_installs_and_cleans_up() {
        let (src, files) = package().unwrap();
        let s = driver_script(&src, &files);
        assert!(s.contains("$src = 'C:\\Users\\Ann O\u{2019}\u{2019}Neil\\Thimsy Installer\\tools\\unisoc-driver'"), "{s}");
        assert!(s.contains("[guid]::NewGuid()"));
        assert!(s.contains("GetFolderPath('CommonApplicationData')) 'Thimsy Installer'"));
        assert!(s.contains("SetAccessRuleProtection($true, $false)"));
        // The shared start: the ProgramData folder is checked, never taken
        // over (it may be a user's junction), before anything goes in it.
        assert!(s.starts_with(ADMIN_PRELUDE));
        assert!(!s.contains("takeown") && !s.contains("Set-Acl"));
        let checked = s.find("\n  Assert-AdminRoot\n").expect("checks the root");
        assert!(checked < s.find("New-AdminFolder $dir $false").unwrap());
        assert!(ADMIN_PRELUDE.contains("throw \"others can change $root\""));
        assert!(ADMIN_PRELUDE.contains("PropagationFlags]::InheritOnly"));
        assert!(ADMIN_PRELUDE.contains("ReparsePoint"));
        assert!(ADMIN_PRELUDE.contains("GetOwner([System.Security.Principal.SecurityIdentifier])"));
        assert!(ADMIN_PRELUDE.contains("$owner -ne $admins -and $owner -ne $system"));
        assert!(s.contains("'S-1-5-32-544'") && s.contains("'S-1-5-18'"));
        // Every file is copied and checked, in Windows' path form.
        for f in UNISOC_DRIVER_FILES {
            let win = ps_literal(&f.replace('/', "\\"));
            let copy = format!(
                "Copy-Item -LiteralPath (Join-Path $src {win}) -Destination (Join-Path $pkg {win})"
            );
            let at = s.find(&copy).unwrap_or_else(|| panic!("{f}: {s}"));
            let check = format!("(Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $pkg {win})).Hash -ne 'ABC123'");
            assert!(at < s.find(&check).unwrap(), "{f}");
        }
        // Then installed with Windows' own pnputil, by absolute path.
        assert!(s.contains("$pnputil = Join-Path ([Environment]::SystemDirectory) 'pnputil.exe'"));
        assert!(s.contains("& $pnputil '/add-driver' (Join-Path $pkg 'sprdvcom.inf') '/install'\n  $code = $LASTEXITCODE"));
        assert!(s.rfind("Get-FileHash").unwrap() < s.find("& $pnputil").unwrap());
        assert!(s.contains("if ($code -eq 3010 -or $code -eq 259) { $code = 0 }"));
        // The folder is made admin-only (and checked) before anything is
        // copied into it, and removed whatever happens, if it was made.
        let made = s
            .find("New-AdminFolder $dir $false\n  $made = $true")
            .unwrap();
        assert!(made < s.find("Copy-Item").unwrap());
        assert!(
            s.contains("finally {\n  if ($made) { Remove-Item -LiteralPath $dir -Recurse -Force")
        );
        assert!(s.trim_end().ends_with("exit $code"));
        // The encoded form PowerShell decodes back to the script.
        let enc = encode_command("exit 0");
        assert_eq!(enc, "ZQB4AGkAdAAgADAA");
    }

    /// A script run by Windows PowerShell, as the installer runs them.
    #[cfg(windows)]
    fn powershell(script: &str) -> String {
        let out = std::process::Command::new(powershell_path().unwrap())
            .args(["-NoProfile", "-NonInteractive", "-EncodedCommand"])
            .arg(encode_command(script))
            .output()
            .unwrap();
        assert!(
            out.status.success(),
            "{}",
            String::from_utf8_lossy(&out.stderr)
        );
        String::from_utf8_lossy(&out.stdout).trim().to_string()
    }

    #[cfg(windows)]
    #[test]
    fn windows_powershell_parses_the_driver_script() {
        let (src, files) = package().unwrap();
        let s = driver_script(&src, &files);
        let check = format!(
            "$e = $null; [void][System.Management.Automation.Language.Parser]::ParseInput({}, [ref]$null, [ref]$e); $e.Count",
            ps_literal(&s)
        );
        assert_eq!(powershell(&check), "0", "{s}");
    }

    #[test]
    fn declined_or_ineffective_driver_installs_fail_clearly() {
        let cat = crate::catalog::Catalog::bundled().unwrap();
        let d = cat.device("anbernic-rg-cube").unwrap();
        let rep = Reporter::new(std::sync::Arc::new(crate::events::MemorySink::default()));
        let tool_failed = |e: &anyhow::Error| crate::error::code_of(e).0 == Code::ToolFailed;
        // UAC declined.
        let mut ran = Vec::new();
        let err = install_ids(d, &package, &rep, &|_, _| false, &mut |ps| {
            ran.push(ps.to_string());
            Ok(Some(DECLINED))
        })
        .unwrap_err();
        assert!(
            tool_failed(&err) && err.to_string().contains("declined"),
            "{err}"
        );
        assert!(ran[0].starts_with("$ErrorActionPreference = 'Stop'"));
        // PowerShell stopped by an error, or a changed file.
        let err = install_ids(d, &package, &rep, &|_, _| false, &mut |_| Ok(Some(1))).unwrap_err();
        assert!(err.to_string().contains("exit code 1"), "{err}");
        let err = install_ids(d, &package, &rep, &|_, _| false, &mut |_| Ok(Some(3))).unwrap_err();
        assert!(err.to_string().contains("changed before"), "{err}");
        // Reported success, but Windows still has no driver for the ID.
        let err = install_ids(d, &package, &rep, &|_, _| false, &mut |_| Ok(Some(0))).unwrap_err();
        assert!(
            tool_failed(&err)
                && err
                    .to_string()
                    .contains("still has no driver the installer can use"),
            "{err}"
        );
        // No bundled package (as next to the test binary): says so,
        // without asking for permission.
        assert!(bundled_package()
            .unwrap_err()
            .to_string()
            .contains("unisoc-driver isn't bundled"));
        let err = install_ids(
            d,
            &|| {
                Err(anyhow::anyhow!(
                    "unisoc-driver isn't bundled with this installer"
                ))
            },
            &rep,
            &|_, _| false,
            &mut |_| panic!("nothing to run"),
        )
        .unwrap_err();
        assert!(err.to_string().contains("isn't bundled"), "{err}");
        // Installed for real.
        let done = std::cell::Cell::new(false);
        install_ids(d, &package, &rep, &|_, _| done.get(), &mut |_| {
            done.set(true);
            Ok(Some(0))
        })
        .unwrap();
        // Already there (Unisoc's own installer, or WinUSB): nothing runs,
        // and the package isn't even looked for.
        install_ids(d, &|| panic!("not needed"), &rep, &|_, _| true, &mut |_| {
            panic!("nothing to install")
        })
        .unwrap();
    }

    #[test]
    fn linux_udev_rules_are_fixed_text_and_found_case_insensitively() {
        let cat = crate::catalog::Catalog::bundled().unwrap();
        let d = cat.device("anbernic-rg-cube").unwrap();
        let rules = udev_rules(d);
        assert!(rules.contains(
            "SUBSYSTEM==\"usb\", ATTR{idVendor}==\"1782\", ATTR{idProduct}==\"4d00\", TAG+=\"uaccess\"\n"
        ));
        assert_eq!(
            rules.lines().filter(|l| !l.starts_with('#')).count(),
            d.identify.usb.len()
        );
        // The script pkexec runs takes the rules and udevadm as arguments.
        assert!(!UDEV_INSTALL_SCRIPT.contains("1782"));
        assert!(UDEV_INSTALL_SCRIPT.contains("printf '%s' \"$1\""));
        let dir = tempfile::tempdir().unwrap();
        let dirs = vec![dir.path().to_path_buf(), dir.path().join("missing")];
        assert!(!rules_cover(&dirs, d));
        std::fs::write(dir.path().join("50-other.rules"), "# 1782 4d00 commented out\nSUBSYSTEM==\"usb\", ATTR{idVendor}==\"18d1\", MODE=\"0666\"\n").unwrap();
        assert!(!rules_cover(&dirs, d));
        std::fs::write(dir.path().join("51-android.rules"), "SUBSYSTEM==\"usb\", ATTRS{idVendor}==\"1782\", ATTRS{idProduct}==\"4D00\", MODE=\"0660\", GROUP=\"plugdev\"\n").unwrap();
        assert!(rules_cover(&dirs, d));
        // Only .rules files count.
        let other = tempfile::tempdir().unwrap();
        std::fs::write(other.path().join("notes.txt"), rules).unwrap();
        assert!(!rules_cover(&[other.path().to_path_buf()], d));
    }
}
