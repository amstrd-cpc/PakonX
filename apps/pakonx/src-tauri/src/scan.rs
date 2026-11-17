// Scanning: the one live command PakonX runs, `pakon-cli scan --live-scan`,
// a film pass into <folder>/<name>.film.pakraw + .scan.json.
//
// Hardware safety (docs/HARDWARE_RUNBOOK.md):
// - the arguments are built here from a checked mode, a bounded row cap, an
//   existing folder and a plain roll name; the web view never passes flags;
// - one scan at a time, never while `frames` runs, and never over the files
//   of an earlier scan;
// - Stop writes "stop" to the scan's stdin (`--stop-on-stdin`), which
//   pakon-cli handles like Ctrl+C: the teardown (lamp off, motor stop)
//   always runs. The process is never killed. If PakonX itself goes away,
//   the pipe closes and pakon-cli stops the same way;
// - every scan first runs the runbook's read-only steps (checks): the XP VM
//   off, the scanner warm, identify, status and EEPROM; the first that fails
//   ends the request before the lamp or the motor is touched.
use crate::cli::{self, CliRun};
use serde::Serialize;
use std::io::{BufRead, BufReader, Write};
use std::path::{Path, PathBuf};
use std::process::{ChildStdin, Stdio};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Mutex;
use tauri::{Emitter, Manager};

/// The modes `scan --config` takes.
const MODES: &[&str] = &["base4", "base4-ir", "base8", "base8-ir", "base16", "base16-ir"];

/// The row caps the UI may ask for: a whole 36-exposure roll at Base 16
/// with its leader stays below the top (roll1: 75 000 rows at Base 4).
const MIN_ROWS: u32 = 1_000;
// The cap is a safety bound only: the film end stops the pass (Base 16
// IR roll 7919 ended at 132 736 rows; 7918 ran into a 150 000 cap).
const MAX_ROWS: u32 = 250_000;

/// The scan in progress: its stdin, so `stop_scan` can reach it, and a
/// Stop that came during the checks, before pakon-cli started.
#[derive(Default)]
pub struct Scanning {
    stdin: Mutex<Option<ChildStdin>>,
    active: Mutex<bool>,
    stop_early: AtomicBool,
}

impl Scanning {
    pub fn active(&self) -> bool {
        *self.active.lock().unwrap()
    }
}

#[derive(Serialize)]
pub struct Check {
    name: &'static str,
    ok: bool,
    detail: String,
}

/// The XP VM's USB autoconnect takes the scanner from the host
/// (usb_device_not_found): a running VMware VM fails the check.
pub(crate) fn vm_running() -> bool {
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        let mut cmd = std::process::Command::new("tasklist");
        cmd.args(["/FI", "IMAGENAME eq vmware-vmx.exe", "/NH"]).creation_flags(cli::CREATE_NO_WINDOW);
        cmd.output()
            .map(|o| String::from_utf8_lossy(&o.stdout).to_ascii_lowercase().contains("vmware-vmx.exe"))
            .unwrap_or(false)
    }
    #[cfg(not(windows))]
    {
        false
    }
}

fn first_line(r: &CliRun) -> String {
    let text = if r.stderr.trim().is_empty() { &r.stdout } else { &r.stderr };
    text.lines().map(str::trim).find(|l| !l.is_empty()).unwrap_or("no output").to_string()
}

/// What a read-only step's output means for scanning: (ok, detail).
fn judge(step: &str, r: &CliRun) -> (bool, String) {
    let out = &r.stdout;
    match step {
        "list" if out.contains("(operational)") => (true, "Warm".into()),
        "list" if out.contains("cold/bootstrap") => {
            (false, "Cold: boot it with tools\\pakon_boot_reference.ps1, then check again".into())
        }
        "list" => (false, "Not found: check power, the USB cable and that the XP VM is off".into()),
        _ if r.code() != Some(0) => (false, first_line(r)),
        "identify" => match out.lines().find_map(|l| l.strip_prefix("Model: ")) {
            Some(m) if m.trim() == "F-135+" => (true, "F-135+".into()),
            Some(m) => (false, format!("{}: scanning needs an F-135+", m.trim())),
            None => (false, "No model reported".into()),
        },
        "status" => (true, "All polls answered".into()),
        "eeprom" if r.stderr.contains("FALLBACK") => {
            (false, "This unit's EEPROM could not be read (FALLBACK values)".into())
        }
        "eeprom" => (
            true,
            out.lines().find_map(|l| l.trim().strip_prefix("EEPROM:")).unwrap_or("Read").trim().to_string(),
        ),
        _ => (false, format!("unknown step {step}")),
    }
}

/// The runbook's read-only steps in order, up to the first that fails;
/// each one's output goes to the log as it finishes.
fn checks(app: &tauri::AppHandle) -> Result<Vec<Check>, String> {
    let report = |c: &Check, log: &str| {
        let text = format!("{log}check {}: {} — {}", c.name, if c.ok { "OK" } else { "FAILED" }, c.detail);
        let _ = app.emit("scan-line", ScanLine { text, err: !c.ok });
    };
    let mut checks = Vec::new();
    let vm = vm_running();
    let c = Check {
        name: "XP VM off",
        ok: !vm,
        detail: if vm { "VMware is running: shut the XP VM down, it takes the scanner".into() } else { "Not running".into() },
    };
    report(&c, "");
    checks.push(c);
    if vm {
        return Ok(checks);
    }
    for (step, name) in [("list", "Scanner"), ("identify", "Identify"), ("status", "Status"), ("eeprom", "EEPROM")] {
        let r = cli::run_fixed(step)?;
        let (ok, detail) = judge(step, &r);
        let c = Check { name, ok, detail };
        report(&c, &format!("> pakon-cli {step}\n{}{}", r.stdout, r.stderr));
        checks.push(c);
        if !ok {
            break;
        }
    }
    Ok(checks)
}

/// Err naming every failed check, when one failed.
fn ready(checks: &[Check]) -> Result<(), String> {
    let failed: Vec<String> = checks.iter().filter(|c| !c.ok).map(|c| format!("{}: {}", c.name, c.detail)).collect();
    if failed.is_empty() {
        Ok(())
    } else {
        Err(format!("Scanner not ready. {}", failed.join("; ")))
    }
}

/// The roll number from the roll's sticker (1111): it names the scan files
/// and the saved pictures (1111_19A.jpg). 1-40 letters, digits, '-' and
/// '_', not starting with '-'.
fn check_name(name: &str) -> Result<(), String> {
    let ok = !name.is_empty()
        && name.len() <= 40
        && !name.starts_with('-')
        && name.chars().all(|c| c.is_ascii_alphanumeric() || c == '-' || c == '_');
    if ok {
        Ok(())
    } else {
        Err(format!("'{name}': a roll number is 1-40 letters, digits, '-' or '_'"))
    }
}

fn check_mode(mode: &str) -> Result<&'static str, String> {
    MODES.iter().copied().find(|m| *m == mode).ok_or(format!("'{mode}' is not a scan mode"))
}

/// <folder>/<name>, refused unless the folder exists and holds no file of
/// an earlier scan by that name: a scan never overwrites one.
fn scan_prefix(folder: &str, name: &str) -> Result<PathBuf, String> {
    check_name(name)?;
    let dir = Path::new(folder);
    if folder.starts_with('-') || !dir.is_absolute() || !dir.is_dir() {
        return Err(format!("{folder}: not a folder"));
    }
    let prefix = dir.join(name);
    let s = prefix.display().to_string();
    if let Some(f) = crate::roll::SCAN_SUFFIXES.iter().map(|x| format!("{s}{x}")).find(|f| Path::new(f).exists()) {
        return Err(format!("{f} exists: roll {name} is already scanned in this folder (delete or move that scan, or use another folder)"));
    }
    Ok(prefix)
}

/// Where order folders go when the user has not chosen a base folder:
/// Pictures\PakonX.
#[tauri::command]
pub fn default_order_base(app: tauri::AppHandle) -> Result<String, String> {
    let pictures = app.path().picture_dir().map_err(|e| e.to_string())?;
    Ok(crate::boot::plain_path(pictures.join("PakonX")).display().to_string())
}

/// New order (as PSI): <base>\<roll number>, created with the base when
/// missing. Refused when that roll is already scanned there; an existing
/// folder without its scan is reused.
fn order_folder(base: &str, roll: &str) -> Result<PathBuf, String> {
    check_name(roll)?;
    let b = Path::new(base);
    if base.starts_with('-') || !b.is_absolute() {
        return Err(format!("{base}: the base folder must be a full path"));
    }
    let dir = b.join(roll);
    std::fs::create_dir_all(&dir).map_err(|e| format!("{}: {e}", dir.display()))?;
    scan_prefix(&dir.display().to_string(), roll)?;
    Ok(dir)
}

#[tauri::command]
pub fn new_order(base: String, roll: String) -> Result<String, String> {
    order_folder(&base, &roll).map(|d| d.display().to_string())
}

/// The pakon-cli arguments for a film pass: fixed flags only.
pub fn scan_args(mode: &str, rows: u32, prefix: &Path, film: &str, gate: &str, hot: Option<&Path>) -> Vec<String> {
    let mut a: Vec<String> = vec![
        "scan".into(),
        "--live-scan".into(),
        "--config".into(),
        mode.into(),
        "--film-rows".into(),
        rows.to_string(),
        "--out-prefix".into(),
        prefix.display().to_string(),
        "--stop-on-stdin".into(),
        // Exposure and film-end detection (src/pakon/scan/film.hpp).
        "--film".into(),
        film.into(),
        "--film-gate".into(),
        gate.into(),
    ];
    // Batch: the previous strip's corrections, when they still hold.
    if let Some(h) = hot {
        a.push("--hot-start".into());
        a.push(h.display().to_string());
    }
    a
}

/// `scan --film-gate`: how the film end is decided.
const GATES: &[&str] = &["auto", "brightness", "off"];

fn check_gate(gate: Option<&str>) -> Result<&'static str, String> {
    let g = gate.unwrap_or("auto");
    GATES.iter().copied().find(|k| *k == g).ok_or_else(|| format!("unknown film gate '{g}'"))
}

/// The hot start: <folder>/<earlier>.scan.json of a strip scanned before
/// in the same folder (batch scanning).
fn hot_start_file(folder: &str, earlier: &str) -> Result<PathBuf, String> {
    check_name(earlier)?;
    let f = Path::new(folder).join(format!("{earlier}.scan.json"));
    if f.is_file() { Ok(f) } else { Err(format!("{}: no earlier scan for the hot start", f.display())) }
}

/// The film kinds `scan --film` takes (scan/film.hpp FilmKind).
const FILMS: &[&str] = &["c41", "c41-unmasked", "e6", "bw-silver", "bw-c41"];

fn check_film(film: Option<&str>) -> Result<&'static str, String> {
    let f = film.unwrap_or("c41");
    FILMS.iter().copied().find(|k| *k == f).ok_or_else(|| format!("unknown film '{f}'"))
}

/// The film rows of a "progress: film rows N" line (scan_cli.cpp, FileRowSink).
fn film_rows(line: &str) -> Option<u64> {
    line.strip_prefix("progress: film rows ")?.trim().parse().ok()
}

/// Sent to the web view as "scan-progress" while film streams.
#[derive(Serialize, Clone)]
struct ScanProgress {
    rows: u64,
    cap: u32,
}

/// Every other line pakon-cli prints, as "scan-line".
#[derive(Serialize, Clone)]
struct ScanLine {
    text: String,
    err: bool,
}

#[derive(Serialize)]
pub struct ScanDone {
    prefix: String,
    code: Option<i32>,
    /// Every teardown step acknowledged: lamp off, motor stopped.
    teardown_ok: bool,
    /// Ended by Stop.
    stopped: bool,
    /// The film files are there to open.
    roll: bool,
    log: String,
}

/// Clears the scan state however start_scan ends.
struct ActiveGuard<'a>(&'a Scanning);

impl Drop for ActiveGuard<'_> {
    fn drop(&mut self) {
        *self.0.stdin.lock().unwrap() = None;
        *self.0.active.lock().unwrap() = false;
    }
}

/// The one live command at a time (scan or film drive): refused while one
/// runs, the scanner starts up, or a roll is developed or saved (a pakon-cli
/// beside a live run could starve its USB stream).
fn claim(app: &tauri::AppHandle) -> Result<(), String> {
    let scanning = app.state::<Scanning>();
    let mut active = scanning.active.lock().unwrap();
    if *active {
        return Err("A scan or film drive is already running.".into());
    }
    if app.state::<crate::boot::Starting>().active() {
        return Err("The scanner is being initialized.".into());
    }
    if app.state::<crate::roll::Running>().busy() {
        return Err("Wait until the roll being developed or saved is done.".into());
    }
    *active = true;
    scanning.stop_early.store(false, Ordering::SeqCst);
    Ok(())
}

#[tauri::command]
pub async fn start_scan(app: tauri::AppHandle, mode: String, rows: u32, folder: String, name: String, film: Option<String>, film_gate: Option<String>, hot_start: Option<String>) -> Result<ScanDone, String> {
    let mode = check_mode(&mode)?;
    let film = check_film(film.as_deref())?;
    let gate = check_gate(film_gate.as_deref())?;
    if !(MIN_ROWS..=MAX_ROWS).contains(&rows) {
        return Err(format!("row cap {rows}: wants {MIN_ROWS}..{MAX_ROWS}"));
    }
    let prefix = scan_prefix(&folder, &name)?;
    claim(&app)?;
    let hot = hot_start.as_deref().map(|e| hot_start_file(&folder, e)).transpose()?;
    let args = scan_args(mode, rows, &prefix, film, gate, hot.as_deref());
    tauri::async_runtime::spawn_blocking(move || {
        let scanning = app.state::<Scanning>();
        let _guard = ActiveGuard(&scanning);
        ready(&checks(&app)?)?;
        let _ = app.emit("scan-checked", ());
        let mut child = {
            // Under the stdin lock: a Stop either landed before this (no
            // film pass) or reaches the child's stdin.
            let mut stdin = scanning.stdin.lock().unwrap();
            if scanning.stop_early.load(Ordering::SeqCst) {
                return Err("Cancelled. Stopped before the film pass: the lamp and the motor were not used.".into());
            }
            let mut cmd = cli::command();
            cmd.args(&args).stdin(Stdio::piped()).stdout(Stdio::piped()).stderr(Stdio::piped());
            let mut child = cmd.spawn().map_err(|e| format!("could not start {}: {e}", cli::locate().display()))?;
            *stdin = child.stdin.take();
            child
        };
        let stdout = child.stdout.take().ok_or("no stdout")?;
        let stderr = child.stderr.take().ok_or("no stderr")?;
        let errs = {
            let app = app.clone();
            std::thread::spawn(move || {
                let mut s = String::new();
                for line in BufReader::new(stderr).lines().map_while(Result::ok) {
                    let _ = app.emit("scan-line", ScanLine { text: line.clone(), err: true });
                    s.push_str(&line);
                    s.push('\n');
                }
                s
            })
        };
        let mut log = String::new();
        for line in BufReader::new(stdout).lines().map_while(Result::ok) {
            if let Some(n) = film_rows(&line) {
                let _ = app.emit("scan-progress", ScanProgress { rows: n, cap: rows });
                continue;
            }
            let _ = app.emit("scan-line", ScanLine { text: line.clone(), err: false });
            log.push_str(&line);
            log.push('\n');
        }
        log.push_str(&errs.join().unwrap_or_default());
        // stdout closed: the process has ended, so wait() does not block.
        let status = child.wait().map_err(|e| e.to_string())?;
        let prefix_s = prefix.display().to_string();
        Ok(ScanDone {
            teardown_ok: log.contains("teardown: all steps acknowledged"),
            stopped: log.contains("cancelled"),
            roll: crate::roll::roll_files(&prefix_s).is_ok(),
            code: status.code(),
            prefix: prefix_s,
            log,
        })
    })
    .await
    .map_err(|e| e.to_string())?
}

/// PSI's Advance / Reverse Film, for film stuck in the transport: the
/// motor alone at the Base 4 scan rate (`pakon-cli film-drive`) until Stop
/// (stop_scan) or DRIVE_SECONDS, then the full teardown. Hardware: shares
/// the one-live-command guard with the scan.
const DRIVE_SECONDS: u32 = 60;

pub fn drive_args(reverse: bool) -> Vec<String> {
    vec![
        "film-drive".into(),
        if reverse { "--reverse" } else { "--forward" }.into(),
        "--seconds".into(),
        DRIVE_SECONDS.to_string(),
        "--stop-on-stdin".into(),
        "--live".into(),
    ]
}

#[derive(Serialize)]
pub struct DriveDone {
    code: Option<i32>,
    teardown_ok: bool,
    log: String,
}

#[tauri::command]
pub async fn drive_film(app: tauri::AppHandle, reverse: bool) -> Result<DriveDone, String> {
    claim(&app)?;
    tauri::async_runtime::spawn_blocking(move || {
        let scanning = app.state::<Scanning>();
        let _guard = ActiveGuard(&scanning);
        let child = {
            let mut stdin = scanning.stdin.lock().unwrap();
            if scanning.stop_early.load(Ordering::SeqCst) {
                return Err("Stopped before the film drive started: the motor was not used.".into());
            }
            let mut cmd = cli::command();
            cmd.args(drive_args(reverse)).stdin(Stdio::piped()).stdout(Stdio::piped()).stderr(Stdio::piped());
            let mut child = cmd.spawn().map_err(|e| format!("could not start {}: {e}", cli::locate().display()))?;
            *stdin = child.stdin.take();
            child
        };
        let out = child.wait_with_output().map_err(|e| e.to_string())?;
        let log = format!("{}{}", String::from_utf8_lossy(&out.stdout), String::from_utf8_lossy(&out.stderr));
        Ok(DriveDone { code: out.status.code(), teardown_ok: log.contains("teardown: all steps acknowledged"), log })
    })
    .await
    .map_err(|e| e.to_string())?
}

/// Asks the running scan to stop at its next bounded point; the teardown
/// runs. During the checks it cancels the scan before pakon-cli starts.
/// False when no scan is running.
#[tauri::command]
pub fn stop_scan(scanning: tauri::State<'_, Scanning>) -> bool {
    let mut stdin = scanning.stdin.lock().unwrap();
    match stdin.as_mut() {
        Some(s) => s.write_all(b"stop\n").and_then(|_| s.flush()).is_ok(),
        None if scanning.active() => {
            scanning.stop_early.store(true, Ordering::SeqCst);
            true
        }
        None => false,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn scan_runs_with_fixed_flags_only() {
        let a = scan_args("base8", 20000, Path::new("C:/scans/strip9"), "c41", "auto", None);
        assert_eq!(a, ["scan", "--live-scan", "--config", "base8", "--film-rows", "20000",
                       "--out-prefix", "C:/scans/strip9", "--stop-on-stdin", "--film", "c41", "--film-gate", "auto"]);
        let h = scan_args("base8", 20000, Path::new("C:/scans/strip9"), "c41", "off", Some(Path::new("C:/scans/strip8.scan.json")));
        assert_eq!(&h[h.len() - 4..], ["--film-gate", "off", "--hot-start", "C:/scans/strip8.scan.json"]);
        assert_eq!(check_gate(None), Ok("auto"));
        assert!(check_gate(Some("auto --dry-run")).is_err());
        assert!(hot_start_file("C:/scans", "../x").is_err());
        assert_eq!(check_film(None), Ok("c41"));
        assert_eq!(drive_args(true), ["film-drive", "--reverse", "--seconds", "60", "--stop-on-stdin", "--live"]);
        assert_eq!(check_film(Some("e6")), Ok("e6"));
        assert!(check_film(Some("e6 --first-light")).is_err());
        assert!(check_film(Some("bw")).is_err());
    }

    #[test]
    fn only_known_modes_and_plain_names_pass() {
        for m in MODES {
            assert!(check_mode(m).is_ok());
        }
        for m in ["base4 --first-light", "base32", "", "--dry-run"] {
            assert!(check_mode(m).is_err(), "{m}");
        }
        for n in ["strip9", "roll_2026-10-23", "A1"] {
            assert!(check_name(n).is_ok(), "{n}");
        }
        let long = "y".repeat(41);
        for n in ["", "-x", "--live-scan", "a b", "../up", "a/b", "a\\b", "x.film", long.as_str()] {
            assert!(check_name(n).is_err(), "{n}");
        }
    }

    #[test]
    fn a_scan_never_overwrites_an_earlier_one() {
        let dir = std::env::temp_dir().join(format!("pakonx-scanprefix-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let folder = dir.display().to_string();
        assert!(scan_prefix(&folder, "new").is_ok());
        std::fs::write(dir.join("old.scan-stats.txt"), b"x").unwrap();
        assert!(scan_prefix(&folder, "old").is_err());
        assert!(scan_prefix("relative/dir", "new").is_err());
        assert!(scan_prefix(&dir.join("missing").display().to_string(), "new").is_err());
        std::fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn a_new_order_makes_its_folder_once() {
        let base = std::env::temp_dir().join(format!("pakonx-orders-{}", std::process::id()));
        let b = base.display().to_string();
        let dir = order_folder(&b, "1111").unwrap();
        assert_eq!(dir, base.join("1111"));
        assert!(dir.is_dir());
        assert!(order_folder(&b, "1111").is_ok(), "an empty order folder is reused");
        std::fs::write(dir.join("1111.scan.json"), b"{}").unwrap();
        assert!(order_folder(&b, "1111").is_err(), "already scanned");
        assert!(order_folder(&b, "../x").is_err());
        assert!(order_folder("relative", "2222").is_err());
        std::fs::remove_dir_all(&base).ok();
    }

    #[test]
    fn progress_lines_are_read_and_others_are_not() {
        assert_eq!(film_rows("progress: film rows 3000"), Some(3000));
        assert_eq!(film_rows("scan-stats:"), None);
        assert_eq!(film_rows("  rows 21059, samples/row 4500"), None);
    }

    #[test]
    fn a_failed_check_names_itself() {
        let c = |name, ok| Check { name, ok, detail: "why".into() };
        assert!(ready(&[c("XP VM off", true), c("Scanner", true)]).is_ok());
        let e = ready(&[c("XP VM off", true), c("Scanner", false)]).unwrap_err();
        assert_eq!(e, "Scanner not ready. Scanner: why");
    }

    fn out(stdout: &str, stderr: &str, code: i32) -> CliRun {
        CliRun::new("x", Some(code), stdout, stderr)
    }

    #[test]
    fn checks_read_the_runbook_signatures() {
        assert!(judge("list", &out("PID: f135  (operational)\n", "", 0)).0);
        assert!(!judge("list", &out("PID: f235  (cold/bootstrap - firmware not loaded)\n", "", 0)).0);
        assert!(!judge("list", &out("No Pakon F-X35 devices detected.\n", "", 0)).0);
        assert!(judge("identify", &out("Model: F-135+\n", "", 0)).0);
        assert!(!judge("identify", &out("Model: F-235\n", "", 0)).0);
        assert!(!judge("identify", &out("", "error: usb_open_failed\n", 1)).0);
        assert!(judge("status", &out("ok\n", "", 0)).0);
        let fallback = "WARNING: EEPROM sections unreadable - FALLBACK values shown\n";
        assert!(!judge("eeprom", &out("EEPROM:  serial 17373\n", fallback, 0)).0);
        assert_eq!(judge("eeprom", &out("EEPROM:  serial 17373, type 1351\n", "", 0)),
                   (true, "serial 17373, type 1351".to_string()));
    }
}
