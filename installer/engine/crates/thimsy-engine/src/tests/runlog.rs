//! Run logs: names, folders, dates, thinning, pruning, and what a logged
//! `cli::run` writes.
use crate::cli::{self, Command, Global};
use crate::events::{Event, MemorySink, Reporter};
use crate::runlog::{self, civil_from_days, file_name, prune, utc_stamps, RunLog, Thinner};
use serde_json::Value;
use std::ffi::OsString;
use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::time::{Duration, Instant};

#[test]
fn civil_dates() {
    assert_eq!(civil_from_days(0), (1970, 1, 1));
    assert_eq!(civil_from_days(-1), (1969, 12, 31));
    assert_eq!(civil_from_days(59), (1970, 3, 1));
    assert_eq!(civil_from_days(11_016), (2000, 2, 29));
    assert_eq!(civil_from_days(19_723), (2024, 1, 1));
    assert_eq!(civil_from_days(20_513), (2026, 3, 1));
    // Every day over 400 years follows the one before it.
    let mut prev = civil_from_days(0);
    for day in 1..146_097 {
        let (y, m, d) = civil_from_days(day);
        let next_day = (prev.0, prev.1, prev.2 + 1);
        let next_month = (prev.0, prev.1 + 1, 1);
        let next_year = (prev.0 + 1, 1, 1);
        assert!(
            [next_day, next_month, next_year].contains(&(y, m, d)),
            "{prev:?} then {:?}",
            (y, m, d)
        );
        assert!(d <= 31 && (1..=12).contains(&m));
        prev = (y, m, d);
    }
    assert_eq!(prev, (2369, 12, 31));
    assert_eq!(
        utc_stamps(1_700_000_000),
        (
            "20231114-221320".to_string(),
            "2023-11-14T22:13:20Z".to_string()
        )
    );
}

#[test]
fn file_names() {
    let s = "20261125-101112";
    assert_eq!(file_name(s, "clean", None, 1), "20261125-101112Z-clean.log");
    assert_eq!(
        file_name(s, "build", Some("anbernic-rg-cube-heavy"), 1),
        "20261125-101112Z-build-anbernic-rg-cube-heavy.log"
    );
    assert_eq!(
        file_name(s, "flash", Some("testdev-heavy"), 3),
        "20261125-101112Z-flash-testdev-heavy-3.log"
    );
    assert_eq!(
        file_name(s, "unlock", Some("../a b"), 1),
        "20261125-101112Z-unlock-.._a_b.log"
    );
}

#[test]
fn where_each_command_logs() {
    let tmp = tempfile::tempdir().unwrap();
    let base = tmp.path().to_path_buf();
    let ws = base.join("ws");
    let user = base.join("user-logs");
    let user_dir = || Some(user.clone());
    let t = |cmd: Command| runlog::target(&cmd, &user_dir);

    let build = t(Command::Build {
        device: "dev".into(),
        profile: "heavy".into(),
        workspace: ws.clone(),
        import: vec![],
        set: vec![],
        delete_downloads: false,
    })
    .unwrap();
    assert_eq!(build.dir, Some(ws.join("logs")));
    assert_eq!(
        (build.command, build.detail.as_deref()),
        ("build", Some("dev-heavy"))
    );

    let clean = |dry_run| Command::Clean {
        workspace: ws.clone(),
        downloads: true,
        image: false,
        scratch: false,
        backup: false,
        paths: vec![],
        dry_run,
    };
    assert_eq!(t(clean(true)), None);
    // A workspace that isn't there: the per-user folder (making `ws/logs`
    // would make the workspace clean leaves alone).
    assert_eq!(t(clean(false)).unwrap().dir, Some(user.clone()));
    let job = ws.join("jobs").join("dev-heavy");
    let elsewhere = base.join("somewhere").join("my-job");
    let flash = |job: &Path| Command::Flash {
        job: job.to_path_buf(),
        mode: "update".into(),
        dry_run: false,
    };
    assert_eq!(t(flash(&job)).unwrap().dir, Some(user.clone()));
    assert_eq!(t(flash(&elsewhere)).unwrap().dir, Some(user.clone()));
    std::fs::create_dir_all(&job).unwrap();
    std::fs::create_dir_all(&elsewhere).unwrap();
    let c = t(clean(false)).unwrap();
    assert_eq!(
        (c.dir, c.command, c.detail),
        (Some(ws.join("logs")), "clean", None)
    );

    // A job in the workspace's jobs folder logs to the workspace.
    for (cmd, name) in [
        (
            Command::Flash {
                job: job.clone(),
                mode: "full".into(),
                dry_run: true,
            },
            "flash",
        ),
        (
            Command::Prepare {
                job: job.clone(),
                mode: "full".into(),
            },
            "prepare",
        ),
    ] {
        let f = t(cmd).unwrap();
        assert_eq!(f.dir, Some(ws.join("logs")));
        assert_eq!((f.command, f.detail.as_deref()), (name, Some("dev-heavy")));
    }
    // Anywhere else, in the job.
    let f = t(Command::Flash {
        job: elsewhere.clone(),
        mode: "update".into(),
        dry_run: false,
    })
    .unwrap();
    assert_eq!(f.dir, Some(elsewhere.join("logs")));
    assert_eq!(f.detail.as_deref(), Some("my-job"));
    // A relative job is made absolute.
    let rel = t(Command::Prepare {
        job: PathBuf::from("src/tests"),
        mode: "full".into(),
    })
    .unwrap();
    assert_eq!(
        rel.dir,
        Some(std::env::current_dir().unwrap().join("src/tests/logs"))
    );

    // No workspace: the per-user folder.
    let u = t(Command::Unlock {
        device: "anbernic-rg-cube".into(),
    })
    .unwrap();
    assert_eq!(u.dir, Some(user.clone()));
    assert_eq!(
        (u.command, u.detail.as_deref()),
        ("unlock", Some("anbernic-rg-cube"))
    );
    let d = |install| Command::Drivers {
        device: "anbernic-rg-cube".into(),
        install,
    };
    assert_eq!(t(d(true)).unwrap().dir, Some(user.clone()));
    assert_eq!(t(d(true)).unwrap().command, "drivers");

    // Not logged.
    assert_eq!(t(d(false)), None);
    assert_eq!(t(Command::Catalog), None);
    assert_eq!(t(Command::Detect { watch: false }), None);
    assert_eq!(
        t(Command::Resolve {
            device: "x".into(),
            profile: "y".into(),
            set: vec![]
        }),
        None
    );
    assert_eq!(
        t(Command::Space {
            device: "x".into(),
            profile: "y".into(),
            workspace: ws.clone(),
            import: vec![],
            delete_downloads: false
        }),
        None
    );
}

#[test]
fn per_user_folders() {
    fn env<'a>(vars: &'a [(&'a str, &'a str)]) -> impl Fn(&str) -> Option<OsString> + 'a {
        move |k| {
            vars.iter()
                .find(|(n, _)| *n == k)
                .map(|(_, v)| OsString::from(v))
        }
    }
    let dir = |os: &str, vars: &[(&str, &str)]| runlog::user_log_dir(os, &env(vars));
    assert_eq!(
        dir("windows", &[("LOCALAPPDATA", "/la"), ("HOME", "/h")]),
        Some(Path::new("/la").join("Thimsy Installer").join("logs"))
    );
    assert_eq!(dir("windows", &[("HOME", "/h")]), None);
    assert_eq!(
        dir("macos", &[("HOME", "/h"), ("XDG_STATE_HOME", "/x")]),
        Some(
            Path::new("/h")
                .join("Library")
                .join("Logs")
                .join("Thimsy Installer")
        )
    );
    let state = std::env::temp_dir().join("state");
    let state_s = state.to_str().unwrap();
    assert_eq!(
        dir("linux", &[("HOME", "/h"), ("XDG_STATE_HOME", state_s)]),
        Some(state.join("thimsy-installer").join("logs"))
    );
    // A relative XDG_STATE_HOME is ignored, as the spec says.
    let home = Path::new("/h")
        .join(".local")
        .join("state")
        .join("thimsy-installer")
        .join("logs");
    assert_eq!(
        dir("linux", &[("HOME", "/h"), ("XDG_STATE_HOME", "rel")]),
        Some(home.clone())
    );
    assert_eq!(
        dir("linux", &[("HOME", "/h"), ("XDG_STATE_HOME", "")]),
        Some(home)
    );
    assert_eq!(dir("linux", &[]), None);
}

#[test]
fn progress_is_thinned() {
    let mut th = Thinner::default();
    let t0 = Instant::now();
    let at = |ms: u64| t0 + Duration::from_millis(ms);
    assert!(th.keep(at(0), "a", 1, 100));
    assert!(!th.keep(at(1000), "a", 2, 100));
    assert!(!th.keep(at(4900), "a", 3, 100));
    assert!(th.keep(at(5000), "a", 4, 100));
    assert!(!th.keep(at(5100), "a", 5, 100));
    // The end of it, and a new label, at once.
    assert!(th.keep(at(5200), "a", 100, 100));
    assert!(th.keep(at(5300), "b", 1, 0));
    assert!(!th.keep(at(5400), "b", 2, 0));
    assert!(th.keep(at(5500), "a", 1, 100));
}

#[test]
fn old_logs_are_pruned() {
    let tmp = tempfile::tempdir().unwrap();
    let dir = tmp.path();
    for i in 0..35 {
        std::fs::write(dir.join(format!("20260101-0000{i:02}Z-build.log")), "x").unwrap();
    }
    // Other programs' files, logs or not, are left alone (the workspace is
    // a folder the user picked).
    let others = ["notes.txt", "1.log", "2019-crash.log", "20190101-crash.log"];
    for name in others {
        std::fs::write(dir.join(name), "keep").unwrap();
    }
    // The run's own log is kept even when its name sorts first.
    let current = dir.join("20250101-000000Z-flash.log");
    std::fs::write(&current, "x").unwrap();
    prune(dir, 30, &current);
    let left: Vec<String> = std::fs::read_dir(dir)
        .unwrap()
        .map(|e| e.unwrap().file_name().to_string_lossy().into_owned())
        .collect();
    assert_eq!(left.len(), 35, "{left:?}");
    for name in others {
        assert!(left.iter().any(|l| l == name), "{name} removed: {left:?}");
    }
    let has = |n: &str| left.iter().any(|l| l == n);
    assert!(has("20250101-000000Z-flash.log"));
    assert!(!has("20260101-000004Z-build.log"));
    assert!(has("20260101-000005Z-build.log"));
    assert!(has("20260101-000034Z-build.log"));
    // A missing folder is no error.
    prune(&dir.join("missing"), 30, &current);
}

#[test]
fn same_second_logs_prune_oldest_first() {
    let tmp = tempfile::tempdir().unwrap();
    let dir = tmp.path();
    let s = "20261125-101112";
    // Made in this order: X, X-2 ... X-10, then one a second later.
    let mut names: Vec<String> = (1..=10)
        .map(|n| file_name(s, "flash", Some("j"), n))
        .collect();
    names.push(file_name("20261125-101113", "clean", None, 1));
    for n in &names {
        std::fs::write(dir.join(n), "x").unwrap();
    }
    prune(dir, 3, &dir.join("none.log"));
    let mut left: Vec<String> = std::fs::read_dir(dir)
        .unwrap()
        .map(|e| e.unwrap().file_name().to_string_lossy().into_owned())
        .collect();
    left.sort();
    let mut want = names[8..].to_vec();
    want.sort();
    assert_eq!(left, want);
    assert!(runlog::is_run_log("20261125-101112Z-clean.log"));
    assert!(!runlog::is_run_log("20261125-101112Z-.log"));
    assert!(!runlog::is_run_log("20261125-101112-clean.log"));
}

#[test]
fn a_panic_ends_the_log() {
    let tmp = tempfile::tempdir().unwrap();
    let ws = tmp.path().join("ws");
    std::fs::create_dir_all(&ws).unwrap();
    let cap = Arc::new(MemorySink::default());
    let rep = Reporter::new(cap.clone());
    let cmd = Command::Clean {
        workspace: ws.clone(),
        downloads: true,
        image: false,
        scratch: false,
        backup: false,
        paths: vec![],
        dry_run: false,
    };
    // As when printing the result panics.
    let r = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        cli::run(&global(None), cmd, &rep, &mut |_| {
            panic!("printing the result broke")
        })
    }));
    assert!(r.is_err());
    let path = cap
        .0
        .lock()
        .unwrap()
        .iter()
        .find_map(|e| match e {
            Event::LogFile { path } => Some(PathBuf::from(path)),
            _ => None,
        })
        .unwrap();
    let text = std::fs::read_to_string(path).unwrap();
    let lines: Vec<&str> = text.lines().collect();
    let n = lines.len();
    assert!(
        lines[n - 2].contains("panic: printing the result broke at ")
            && lines[n - 2].contains("runlog.rs"),
        "{text}"
    );
    assert!(lines[n - 1].contains("Finished: panicked after"), "{text}");
}

#[test]
fn clean_in_a_missing_workspace_makes_no_folder() {
    let tmp = tempfile::tempdir().unwrap();
    let ws = tmp.path().join("missing");
    let user = tmp.path().join("user-logs");
    runlog::test::USER_DIR.with(|d| *d.borrow_mut() = Some(user.clone()));
    let (code, l) = run(
        &global(None),
        Command::Clean {
            workspace: ws.clone(),
            downloads: true,
            image: false,
            scratch: false,
            backup: false,
            paths: vec![],
            dry_run: false,
        },
    );
    runlog::test::USER_DIR.with(|d| *d.borrow_mut() = None);
    assert_eq!(code, 0, "{l:#?}");
    assert!(!ws.exists());
    let (path, _) = log_of(&l);
    assert_eq!(path.parent().unwrap(), user);
}

#[cfg(unix)]
#[test]
fn a_stop_signal_line_needs_no_allocation() {
    let mut buf = [0u8; 128];
    let n = runlog::stop_signal::line(15, &mut buf);
    assert_eq!(
        std::str::from_utf8(&buf[..n]).unwrap(),
        "Finished: stopped by signal 15 (the run was cancelled or the engine was closed)\n"
    );
    let n = runlog::stop_signal::line(1, &mut buf);
    assert!(std::str::from_utf8(&buf[..n])
        .unwrap()
        .starts_with("Finished: stopped by signal 1 ("));
}

#[test]
fn log_lines_have_times_and_thinned_progress() {
    let tmp = tempfile::tempdir().unwrap();
    let sink = Arc::new(MemorySink::default());
    let log = Arc::new(RunLog::create(tmp.path(), "flash", Some("j"), sink.clone()).unwrap());
    let again = RunLog::create(tmp.path(), "flash", Some("j"), sink.clone()).unwrap();
    assert_ne!(log.path(), again.path());
    let rep = runlog::reporter(&Reporter::new(sink.clone()), &log);
    rep.stage("flash", "Installing Thimsy");
    for i in 0..=1000u64 {
        rep.progress("Writing", i, 1000);
    }
    rep.info("spd_dump: line 1");
    rep.error(&anyhow::anyhow!("it broke"));
    log.finish(1);
    let text = std::fs::read_to_string(log.path()).unwrap();
    let lines: Vec<&str> = text.lines().collect();
    assert!(lines[0].starts_with("[+00:00."), "{text}");
    assert!(lines[0].ends_with("== Installing Thimsy [flash]"), "{text}");
    let progress = lines.iter().filter(|l| l.contains("Writing:")).count();
    assert_eq!(progress, 2, "{text}");
    assert!(text.contains("Writing: 1000 of 1000 (100.0%)"), "{text}");
    assert!(text.contains("[info] spd_dump: line 1"));
    assert!(text.contains("error (internal): it broke"), "{text}");
    assert!(lines
        .last()
        .unwrap()
        .contains("Finished: exit code 1 after"));
    // The app saw everything, nothing about the log.
    let ev = sink.0.lock().unwrap();
    assert!(ev
        .iter()
        .all(|e| !matches!(e, Event::Log { level, .. } if level == "warn")));
}

/// A full disk: one warning, then the run carries on without the log.
#[cfg(target_os = "linux")]
#[test]
fn a_failed_write_warns_once() {
    let full = std::fs::OpenOptions::new()
        .write(true)
        .open("/dev/full")
        .unwrap();
    let sink = Arc::new(MemorySink::default());
    let log = Arc::new(RunLog::from_file(full, "/dev/full".into(), sink.clone()));
    let rep = runlog::reporter(&Reporter::new(sink.clone()), &log);
    rep.info("one");
    rep.info("two");
    log.finish(0);
    let ev = sink.0.lock().unwrap();
    let warns = ev
        .iter()
        .filter(|e| matches!(e, Event::Log { level, message } if level == "warn" && message.contains("Couldn't write the log")))
        .count();
    assert_eq!(warns, 1, "{ev:?}");
    assert_eq!(ev.len(), 3, "{ev:?}");
}

fn run(g: &Global, cmd: Command) -> (i32, Vec<Value>) {
    let cap = Arc::new(MemorySink::default());
    let rep = Reporter::new(cap.clone());
    let mut outputs = Vec::new();
    let code = cli::run(g, cmd, &rep, &mut |v| outputs.push(v));
    let mut lines: Vec<Value> = cap
        .0
        .lock()
        .unwrap()
        .iter()
        .map(|e| serde_json::to_value(e).unwrap())
        .collect();
    lines.extend(outputs);
    (code, lines)
}

fn global(catalog: Option<&Path>) -> Global {
    Global {
        json: true,
        catalog: catalog.map(Path::to_path_buf),
        lang: "en".into(),
        host: Some("linux".into()),
    }
}

#[test]
fn a_log_that_cant_be_made_never_fails_the_run() {
    let tmp = tempfile::tempdir().unwrap();
    let ws = tmp.path().join("ws");
    std::fs::create_dir_all(&ws).unwrap();
    // `logs` is a file: no folder can be made there.
    std::fs::write(ws.join("logs"), "not a folder").unwrap();
    let (code, l) = run(
        &global(None),
        Command::Clean {
            workspace: ws.clone(),
            downloads: true,
            image: false,
            scratch: false,
            backup: false,
            paths: vec![],
            dry_run: false,
        },
    );
    assert_eq!(code, 0, "{l:#?}");
    assert!(l.iter().all(|v| v["type"] != "log_file"), "{l:#?}");
    assert_eq!(l[0]["type"], "log");
    assert_eq!(l[0]["level"], "warn");
    assert!(l[0]["message"]
        .as_str()
        .unwrap()
        .starts_with("Couldn't save a log of this run"));
    assert_eq!(l.last().unwrap()["ok"], true);
}

#[test]
fn unlogged_commands_send_no_log_file() {
    let (code, l) = run(&global(None), Command::Catalog);
    assert_eq!(code, 0);
    assert_eq!(l.len(), 1);
    assert_eq!(l[0]["type"], "catalog");
}

/// The fixture's catalog as files, for `--catalog`.
fn catalog_dir(fx: &super::build::Fixture) -> PathBuf {
    let dir = fx.dir.path().join("catalog");
    for (name, text) in &fx.catalog.files {
        let p = dir.join(name);
        std::fs::create_dir_all(p.parent().unwrap()).unwrap();
        std::fs::write(p, text).unwrap();
    }
    dir
}

fn log_of(lines: &[Value]) -> (PathBuf, String) {
    assert_eq!(lines[0]["type"], "log_file", "{lines:#?}");
    let path = PathBuf::from(lines[0]["path"].as_str().unwrap());
    assert!(path.is_absolute());
    assert_eq!(lines.iter().filter(|v| v["type"] == "log_file").count(), 1);
    let text = std::fs::read_to_string(&path).unwrap();
    (path, text)
}

#[test]
fn flash_dry_run_writes_a_log() {
    let fx = super::build::good_fixture();
    let cat_dir = catalog_dir(&fx);
    let g = global(Some(&cat_dir));
    let ws = fx.dir.path().join("ws");
    let (code, l) = run(
        &g,
        Command::Build {
            device: "testdev".into(),
            profile: "heavy".into(),
            workspace: ws.clone(),
            import: vec![],
            set: vec![],
            delete_downloads: false,
        },
    );
    assert_eq!(code, 0, "{l:#?}");
    let (build_log, text) = log_of(&l);
    assert_eq!(build_log.parent().unwrap(), ws.join("logs"));
    let name = build_log.file_name().unwrap().to_str().unwrap();
    assert!(name.ends_with("Z-build-testdev-heavy.log"), "{name}");
    assert!(text.contains("\"type\":\"result\""), "{text}");

    let job = crate::job::job_dir(&ws, "testdev", "heavy");
    let flash = |mode: &str| Command::Flash {
        job: job.clone(),
        mode: mode.into(),
        dry_run: true,
    };
    let (code, l) = run(&g, flash("full"));
    assert_eq!(code, 0, "{l:#?}");
    let (path, text) = log_of(&l);
    assert_eq!(path.parent().unwrap(), ws.join("logs"));
    assert!(path
        .file_name()
        .unwrap()
        .to_str()
        .unwrap()
        .contains("Z-flash-testdev-heavy"));
    for want in [
        "Thimsy Installer run log",
        "Started:   20",
        &format!("Installer: {}", crate::INSTALLER_VERSION),
        "Catalog:   ",
        "Host:      ",
        "(acting as linux)",
        "Command:   ",
        "spd_dump",
        "Output: {",
        "\"ok\":true",
        "Finished: exit code 0 after",
    ] {
        assert!(text.contains(want), "{want:?} not in:\n{text}");
    }
    // Every event line has its time (later lines of one are indented).
    let body = text.split("\n\n").nth(1).unwrap();
    assert!(body.starts_with("[+"), "{body}");
    assert!(
        body.lines()
            .all(|l| l.starts_with("[+") || l.starts_with("    ")),
        "{body}"
    );

    // A failed run: its error and the ok:false result are in its log too.
    let (code, l) = run(&g, flash("sideways"));
    assert_eq!(code, 1);
    let (_, text) = log_of(&l);
    assert!(
        text.contains("error (internal): mode must be full or update"),
        "{text}"
    );
    assert!(text.contains("\"ok\":false"), "{text}");
    assert!(text.contains("Finished: exit code 1"), "{text}");
}
