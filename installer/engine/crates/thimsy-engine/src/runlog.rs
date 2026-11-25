//! A log file for every run that builds or changes something (`build`,
//! `clean`, `prepare`, `flash`, `unlock`, `drivers --install`): the events the
//! app was sent, every line of the tools' output and the result, written line
//! by line, so a run that failed or was killed can still be looked into after
//! the app window is closed.

use crate::cli::{Command, Global};
use crate::events::{Event, Reporter, Sink};
use serde_json::Value;
use std::ffi::OsString;
use std::fs::{self, File, OpenOptions};
use std::io::{self, Write};
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

/// How many logs a folder keeps (the newest).
pub const KEEP: usize = 30;

/// Progress is written at most this often, besides label changes and the end.
const PROGRESS_EVERY: Duration = Duration::from_secs(5);

/// Where a command's log goes and what it is called.
#[derive(Debug, Clone, PartialEq)]
pub struct Target {
    /// The folder, or None when there is none for this user.
    pub dir: Option<PathBuf>,
    pub command: &'static str,
    pub detail: Option<String>,
}

/// The log for `cmd`, or None for commands that aren't logged. `user_dir`
/// gives the per-user folder (unlock and drivers have no workspace, and
/// clean, prepare and flash use it when their workspace or job folder
/// doesn't exist: the log never makes a folder the command wouldn't).
pub fn target(cmd: &Command, user_dir: &dyn Fn() -> Option<PathBuf>) -> Option<Target> {
    let existing = |base: &Path, logs: PathBuf| {
        if base.is_dir() {
            Some(logs)
        } else {
            user_dir()
        }
    };
    let (dir, command, detail) = match cmd {
        // A build makes its workspace anyway.
        Command::Build {
            device,
            profile,
            workspace,
            ..
        } => (
            Some(absolute(workspace).join("logs")),
            "build",
            Some(format!("{device}-{profile}")),
        ),
        Command::Clean {
            workspace,
            dry_run: false,
            ..
        } => (
            existing(workspace, absolute(workspace).join("logs")),
            "clean",
            None,
        ),
        Command::Prepare { job, .. } => (existing(job, job_logs(job)), "prepare", job_name(job)),
        Command::Flash { job, .. } => (existing(job, job_logs(job)), "flash", job_name(job)),
        Command::Unlock { device } => (user_dir(), "unlock", Some(device.clone())),
        Command::Drivers {
            device,
            install: true,
        } => (user_dir(), "drivers", Some(device.clone())),
        _ => return None,
    };
    Some(Target {
        dir,
        command,
        detail,
    })
}

fn absolute(p: &Path) -> PathBuf {
    std::path::absolute(p).unwrap_or_else(|_| p.to_path_buf())
}

/// A job's logs: its workspace's `logs` folder when the job is in the
/// workspace's `jobs` folder, otherwise a `logs` folder in the job.
pub fn job_logs(job: &Path) -> PathBuf {
    let job = absolute(job);
    match job.parent() {
        Some(jobs) if jobs.file_name().is_some_and(|n| n == "jobs") => {
            jobs.parent().unwrap_or(jobs).join("logs")
        }
        _ => job.join("logs"),
    }
}

fn job_name(job: &Path) -> Option<String> {
    absolute(job)
        .file_name()
        .map(|n| n.to_string_lossy().into_owned())
}

/// The per-user log folder on `os` (`std::env::consts::OS`), from the
/// environment `env` reads.
pub fn user_log_dir(os: &str, env: &dyn Fn(&str) -> Option<OsString>) -> Option<PathBuf> {
    let var = |k: &str| env(k).filter(|v| !v.is_empty()).map(PathBuf::from);
    match os {
        "windows" => var("LOCALAPPDATA").map(|d| d.join("Thimsy Installer").join("logs")),
        "macos" => var("HOME").map(|h| h.join("Library").join("Logs").join("Thimsy Installer")),
        _ => var("XDG_STATE_HOME")
            .filter(|d| d.is_absolute())
            .map(|d| d.join("thimsy-installer").join("logs"))
            .or_else(|| {
                var("HOME").map(|h| {
                    h.join(".local")
                        .join("state")
                        .join("thimsy-installer")
                        .join("logs")
                })
            }),
    }
}

/// This user's log folder on this computer.
pub fn default_user_dir() -> Option<PathBuf> {
    #[cfg(test)]
    if let Some(d) = test::USER_DIR.with(|d| d.borrow().clone()) {
        return Some(d);
    }
    user_log_dir(std::env::consts::OS, &|k| std::env::var_os(k))
}

/// Days since 1970-01-01 to (year, month, day), proleptic Gregorian
/// (Howard Hinnant's `civil_from_days`).
pub fn civil_from_days(days: i64) -> (i64, u32, u32) {
    let z = days + 719_468;
    let era = z.div_euclid(146_097);
    let doe = z.rem_euclid(146_097);
    let yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = (doy - (153 * mp + 2) / 5 + 1) as u32;
    let m = if mp < 10 { mp + 3 } else { mp - 9 } as u32;
    let y = yoe + era * 400 + i64::from(m <= 2);
    (y, m, d)
}

/// UTC time `secs` after the Unix epoch: (`YYYYMMDD-HHMMSS`,
/// `YYYY-MM-DDTHH:MM:SSZ`).
pub fn utc_stamps(secs: u64) -> (String, String) {
    let (y, mo, d) = civil_from_days((secs / 86_400) as i64);
    let t = secs % 86_400;
    let (h, mi, s) = (t / 3600, t / 60 % 60, t % 60);
    (
        format!("{y:04}{mo:02}{d:02}-{h:02}{mi:02}{s:02}"),
        format!("{y:04}-{mo:02}-{d:02}T{h:02}:{mi:02}:{s:02}Z"),
    )
}

/// `<stamp>Z-<command>[-<detail>][-<n>].log`; `n` from 2 when a log of that
/// name exists.
pub fn file_name(stamp: &str, command: &str, detail: Option<&str>, n: u32) -> String {
    let mut name = format!("{stamp}Z-{command}");
    if let Some(d) = detail {
        name.push('-');
        name.extend(d.chars().map(|c| {
            if c.is_ascii_alphanumeric() || "-_.".contains(c) {
                c
            } else {
                '_'
            }
        }));
    }
    if n > 1 {
        name.push_str(&format!("-{n}"));
    }
    name.push_str(".log");
    name
}

/// Whether `name` is a run log's name (`YYYYMMDD-HHMMSSZ-….log`): the only
/// files [`prune`] counts or removes.
pub fn is_run_log(name: &str) -> bool {
    let b = name.as_bytes();
    let digits = |r: std::ops::Range<usize>| b[r].iter().all(u8::is_ascii_digit);
    b.len() > 21
        && digits(0..8)
        && b[8] == b'-'
        && digits(9..15)
        && &b[15..17] == b"Z-"
        && name.ends_with(".log")
}

/// Keeps the newest `keep` run logs in `dir` and never `current`; other
/// files are left alone. Errors are ignored. Newest is by the time in the
/// name, then (same second) the longer name, so `X-2` and `X-10` come after
/// `X` and `X-9`.
pub fn prune(dir: &Path, keep: usize, current: &Path) {
    let Ok(entries) = fs::read_dir(dir) else {
        return;
    };
    let mut logs: Vec<String> = entries
        .flatten()
        .filter(|e| e.file_type().is_ok_and(|t| t.is_file()))
        .filter_map(|e| e.file_name().into_string().ok())
        .filter(|n| is_run_log(n))
        .collect();
    logs.sort_by(|a, b| (&a[..15], a.len(), a).cmp(&(&b[..15], b.len(), b)));
    let old = logs.len().saturating_sub(keep);
    for name in &logs[..old] {
        let p = dir.join(name);
        if p != current {
            let _ = fs::remove_file(p);
        }
    }
}

/// `[+MM:SS.s]`
fn elapsed(d: Duration) -> String {
    let tenths = d.as_millis() / 100;
    format!(
        "[+{:02}:{:02}.{}]",
        tenths / 600,
        tenths % 600 / 10,
        tenths % 10
    )
}

/// Which progress events go in the log: a new label, the first after
/// [`PROGRESS_EVERY`], and the last (`done >= total`).
#[derive(Default)]
pub struct Thinner {
    last: Option<(Instant, String)>,
}

impl Thinner {
    pub fn keep(&mut self, now: Instant, label: &str, done: u64, total: u64) -> bool {
        let keep = match &self.last {
            None => true,
            Some((t, shown)) => {
                shown != label
                    || now.saturating_duration_since(*t) >= PROGRESS_EVERY
                    || (total > 0 && done >= total)
            }
        };
        if keep {
            self.last = Some((now, label.to_string()));
        }
        keep
    }
}

/// One run's log file. Each line is written through and flushed; once a
/// write fails, one warning goes to the app and the log stops.
pub struct RunLog {
    path: PathBuf,
    start: Instant,
    state: Mutex<State>,
    /// Where the warning about a failed write goes (the app's sink).
    warn: Arc<dyn Sink>,
}

struct State {
    file: Option<File>,
    thin: Thinner,
}

impl RunLog {
    /// A new log in `dir` (created if needed), named for this second.
    pub fn create(
        dir: &Path,
        command: &str,
        detail: Option<&str>,
        warn: Arc<dyn Sink>,
    ) -> io::Result<RunLog> {
        let dir = absolute(dir);
        fs::create_dir_all(&dir)?;
        let secs = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .map_or(0, |d| d.as_secs());
        let (stamp, _) = utc_stamps(secs);
        for n in 1..1000 {
            let path = dir.join(file_name(&stamp, command, detail, n));
            match OpenOptions::new().write(true).create_new(true).open(&path) {
                Ok(f) => return Ok(RunLog::from_file(f, path, warn)),
                Err(e) if e.kind() == io::ErrorKind::AlreadyExists => continue,
                Err(e) => return Err(e),
            }
        }
        Err(io::Error::new(
            io::ErrorKind::AlreadyExists,
            "too many logs in one second",
        ))
    }

    /// A log written to `file` (tests).
    pub fn from_file(file: File, path: PathBuf, warn: Arc<dyn Sink>) -> RunLog {
        stop_signal::register(&file);
        RunLog {
            path,
            start: Instant::now(),
            state: Mutex::new(State {
                file: Some(file),
                thin: Thinner::default(),
            }),
            warn,
        }
    }

    pub fn path(&self) -> &Path {
        &self.path
    }

    /// The state, even after a panic while it was held (the log matters
    /// most then).
    fn state(&self) -> std::sync::MutexGuard<'_, State> {
        self.state.lock().unwrap_or_else(|e| e.into_inner())
    }

    fn write(&self, text: &str) {
        let failed = {
            let mut st = self.state();
            let Some(f) = st.file.as_mut() else {
                return;
            };
            let mut line = text.to_string();
            line.push('\n');
            match f.write_all(line.as_bytes()).and_then(|_| f.flush()) {
                Ok(()) => return,
                Err(e) => {
                    if let Some(f) = st.file.take() {
                        stop_signal::forget(&f);
                    }
                    e
                }
            }
        };
        self.warn.emit(&Event::Log {
            level: "warn".into(),
            message: format!(
                "Couldn't write the log of this run ({}): {failed}",
                self.path.display()
            ),
        });
    }

    /// A line with the time since the start; later lines of `text` are
    /// indented under the first.
    pub fn line(&self, text: &str) {
        let prefix = elapsed(self.start.elapsed());
        let indent = " ".repeat(prefix.len());
        let mut out = String::new();
        for (i, l) in text.lines().enumerate() {
            if i > 0 {
                out.push('\n');
            }
            out.push_str(if i == 0 { &prefix } else { &indent });
            out.push(' ');
            out.push_str(l);
        }
        if out.is_empty() {
            out = prefix;
        }
        self.write(&out);
    }

    pub fn header(&self, g: &Global, catalog: Option<&str>) {
        let secs = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .map_or(0, |d| d.as_secs());
        let (_, iso) = utc_stamps(secs);
        let mut host = format!("{} {}", std::env::consts::OS, std::env::consts::ARCH);
        if let Some(h) = &g.host {
            host.push_str(&format!(" (acting as {h})"));
        }
        let command = std::env::args_os()
            .map(|a| quote(&a.to_string_lossy()))
            .collect::<Vec<_>>()
            .join(" ");
        let lang = if g.lang.is_empty() { "en" } else { &g.lang };
        self.write(&format!(
            "Thimsy Installer run log\n\
             Started:   {iso}\n\
             Engine:    {}\n\
             Installer: {}\n\
             Catalog:   {}\n\
             Host:      {host}\n\
             Language:  {lang}\n\
             Command:   {command}\n",
            crate::VERSION,
            crate::INSTALLER_VERSION,
            catalog.unwrap_or("not loaded"),
        ));
    }

    /// An event, as one line (progress thinned out).
    pub fn event(&self, e: &Event) {
        let text = match e {
            Event::Stage { stage, title } => format!("== {title} [{stage}]"),
            Event::Progress {
                label, done, total, ..
            } => {
                let keep = self.state().thin.keep(Instant::now(), label, *done, *total);
                if !keep {
                    return;
                }
                if *total > 0 {
                    format!(
                        "{label}: {done} of {total} ({:.1}%)",
                        *done as f64 * 100.0 / *total as f64
                    )
                } else {
                    format!("{label}: {done}")
                }
            }
            Event::Log { level, message } => format!("[{level}] {message}"),
            Event::Prompt {
                id, title, body, ..
            } => format!(">> {title} ({id})\n{body}"),
            Event::Error {
                code,
                message,
                hint,
            } => match hint {
                Some(h) => format!("error ({code}): {message}\nhint: {h}"),
                None => format!("error ({code}): {message}"),
            },
            Event::LogFile { path } => format!("Log: {path}"),
        };
        self.line(&text);
    }

    /// One of the command's output lines (its result).
    pub fn output(&self, v: &Value) {
        self.line(&format!("Output: {v}"));
    }

    pub fn finish(&self, code: i32) {
        let d = self.start.elapsed();
        self.line(&format!(
            "Finished: exit code {code} after {:.1} s",
            d.as_secs_f64()
        ));
    }

    /// The end of a run that panicked: what and where, then the footer.
    pub fn panicked(&self, what: &str) {
        self.line(&format!("panic: {what}"));
        self.line(&format!(
            "Finished: panicked after {:.1} s",
            self.start.elapsed().as_secs_f64()
        ));
    }
}

impl Drop for RunLog {
    fn drop(&mut self) {
        if let Some(f) = &self.state().file {
            stop_signal::forget(f);
        }
    }
}

thread_local! {
    /// The last panic on this thread, with where it happened (the panic
    /// hook sees the place; `catch_unwind` only the message).
    static LAST_PANIC: std::cell::RefCell<Option<String>> = const { std::cell::RefCell::new(None) };
}

fn payload_text(p: &(dyn std::any::Any + Send)) -> String {
    if let Some(s) = p.downcast_ref::<&str>() {
        s.to_string()
    } else if let Some(s) = p.downcast_ref::<String>() {
        s.clone()
    } else {
        "unknown panic".to_string()
    }
}

/// Keeps each panic's message and place for [`panic_text`] (once; the
/// panic is still printed as before).
fn install_panic_hook() {
    static ONCE: std::sync::Once = std::sync::Once::new();
    ONCE.call_once(|| {
        let prev = std::panic::take_hook();
        std::panic::set_hook(Box::new(move |info| {
            let mut text = payload_text(info.payload());
            if let Some(l) = info.location() {
                text.push_str(&format!(" at {}:{}:{}", l.file(), l.line(), l.column()));
            }
            let _ = LAST_PANIC.try_with(|p| *p.borrow_mut() = Some(text));
            prev(info);
        }));
    });
}

/// What the panic `payload` (caught on this thread) was, with where it
/// happened when the hook saw it.
pub fn panic_text(payload: &(dyn std::any::Any + Send)) -> String {
    LAST_PANIC
        .try_with(|p| p.borrow_mut().take())
        .ok()
        .flatten()
        .unwrap_or_else(|| payload_text(payload))
}

/// Unix: the open log's file descriptor, so the handler for SIGTERM,
/// SIGINT and SIGHUP (the app cancelling, see `tools::on_stop_signal`) can
/// end the log with a line before the engine exits. On Windows the app
/// stops the engine with TerminateProcess, which runs nothing: a cancelled
/// run's log just ends.
pub(crate) mod stop_signal {
    use std::fs::File;

    #[cfg(unix)]
    static LOG_FD: std::sync::atomic::AtomicI32 = std::sync::atomic::AtomicI32::new(-1);

    #[cfg(unix)]
    pub fn register(f: &File) {
        use std::os::fd::AsRawFd;
        LOG_FD.store(f.as_raw_fd(), std::sync::atomic::Ordering::SeqCst);
    }

    /// Before `f` is closed (its number may be reused).
    #[cfg(unix)]
    pub fn forget(f: &File) {
        use std::os::fd::AsRawFd;
        use std::sync::atomic::Ordering::SeqCst;
        let _ = LOG_FD.compare_exchange(f.as_raw_fd(), -1, SeqCst, SeqCst);
    }

    #[cfg(not(unix))]
    pub fn register(_f: &File) {}

    #[cfg(not(unix))]
    pub fn forget(_f: &File) {}

    /// `Finished: stopped by signal <sig> …` and a newline, into `buf`
    /// without allocating; returns its length.
    #[cfg(unix)]
    pub fn line(sig: i32, buf: &mut [u8; 128]) -> usize {
        let mut n = 0;
        let mut put = |bytes: &[u8], n: &mut usize| {
            for &b in bytes {
                if *n < buf.len() {
                    buf[*n] = b;
                    *n += 1;
                }
            }
        };
        put(b"Finished: stopped by signal ", &mut n);
        let mut digits = [0u8; 10];
        let mut v = sig.unsigned_abs();
        let mut d = digits.len();
        loop {
            d -= 1;
            digits[d] = b'0' + (v % 10) as u8;
            v /= 10;
            if v == 0 {
                break;
            }
        }
        put(&digits[d..], &mut n);
        put(
            b" (the run was cancelled or the engine was closed)\n",
            &mut n,
        );
        n
    }

    /// Ends the open log, if any, for signal `sig`. Async-signal-safe: an
    /// atomic load and write(2), no locks or allocation.
    #[cfg(unix)]
    pub fn note(sig: i32) {
        let fd = LOG_FD.load(std::sync::atomic::Ordering::SeqCst);
        if fd < 0 {
            return;
        }
        let mut buf = [0u8; 128];
        let n = line(sig, &mut buf);
        // SAFETY: write(2) on a descriptor we keep open while it's stored.
        unsafe {
            libc::write(fd, buf.as_ptr().cast(), n);
        }
    }
}

/// Quotes a command-line argument that needs it.
fn quote(a: &str) -> String {
    if !a.is_empty() && !a.chars().any(|c| c.is_whitespace() || c == '"') {
        return a.to_string();
    }
    format!("\"{}\"", a.replace('"', "\\\""))
}

/// Sends events on to the app and writes them to the log.
pub struct LogSink {
    inner: Arc<dyn Sink>,
    log: Arc<RunLog>,
}

impl Sink for LogSink {
    fn emit(&self, event: &Event) {
        self.log.event(event);
        self.inner.emit(event);
    }
}

/// Starts the log for `cmd` if it gets one: creates it, writes the header,
/// prunes old logs and tells the app where it is (`log_file`). A log that
/// can't be created only gets a warning.
pub fn start(
    g: &Global,
    cmd: &Command,
    catalog: Option<&str>,
    rep: &Reporter,
) -> Option<Arc<RunLog>> {
    let t = target(cmd, &default_user_dir)?;
    let Some(dir) = t.dir else {
        rep.warn("Couldn't save a log of this run: there is no folder for logs");
        return None;
    };
    let log = match RunLog::create(&dir, t.command, t.detail.as_deref(), rep.sink()) {
        Ok(l) => Arc::new(l),
        Err(e) => {
            rep.warn(format!(
                "Couldn't save a log of this run in {}: {e}",
                dir.display()
            ));
            return None;
        }
    };
    log.header(g, catalog);
    // A panic or a stop signal still ends the log with a line.
    install_panic_hook();
    crate::tools::install_signal_handlers();
    prune(log.path().parent().unwrap_or(&dir), KEEP, log.path());
    rep.emit(Event::LogFile {
        path: log.path().display().to_string(),
    });
    Some(log)
}

/// `rep`, with every event also written to `log`.
pub fn reporter(rep: &Reporter, log: &Arc<RunLog>) -> Reporter {
    rep.wrapped(|inner| {
        Arc::new(LogSink {
            inner,
            log: log.clone(),
        })
    })
}

#[cfg(test)]
pub(crate) mod test {
    use std::cell::RefCell;
    use std::path::PathBuf;
    thread_local! {
        /// The per-user log folder, instead of the real one.
        pub static USER_DIR: RefCell<Option<PathBuf>> = const { RefCell::new(None) };
    }
}
