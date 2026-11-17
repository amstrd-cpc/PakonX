// Scanner start-up: when PakonX opens, bring the scanner from power-on to
// identified without the operator running anything by hand.
//
//   1. the XP VM must be off (its USB autoconnect takes the scanner);
//   2. run tools/pakon_boot_reference.ps1 in the background, the frozen
//      procedure that boots the FX2 into Pakon7 (docs/BOOT_CHAIN.md). On a
//      scanner that is already warm its STEP0 finds no cold interface and
//      it ends there, before any USB request;
//   3. `pakon-cli identify` (retried while the scanner re-enumerates): an
//      F-135+ is ready.
//
// No network: the frozen script downloads F235Ldr.sys and Pakon7.hex from
// GitHub. PakonX replaces exactly that block (FETCH, pinned by a test) with
// a copy from the firmware folder shipped with the app (firmware_dir); every
// other line, the MD5 gates and all USB code included, runs as frozen.
//
// Hardware safety: the script is embedded in this binary at build time from
// the repository's frozen copy; it is never taken from the web view or from
// disk at run time. It carries its own gates (MD5 of both firmware files,
// record shapes, one 0xA9 read, no retry) and writes FX2 RAM only:
// unplugging the scanner restores the cold state. It only writes to a
// scanner that enumerates cold (0f05:f235).
use crate::cli;
use serde::Serialize;
use std::io::Read;
use std::path::PathBuf;
use std::process::{Command, Stdio};
use std::sync::Mutex;
use std::time::{Duration, Instant};
use tauri::{Emitter, Manager};

/// The frozen boot procedure (DO NOT EDIT: see its header).
const BOOT_SCRIPT: &str = include_str!("../../../../tools/pakon_boot_reference.ps1");

/// The frozen script's download of the two firmware files (LF line ends).
const FETCH: &str = r#"    [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
    $u = 'https://raw.githubusercontent.com/plonsker/pakon-scanning-software/master/Pakon%20Update/fx35install/program%20files/Pakon/F-135/F135Driver/'
    $t1 = Join-Path $env:TEMP ('ldr_' + [guid]::NewGuid().ToString('N') + '.sys')
    $t2 = Join-Path $env:TEMP ('pak7_' + [guid]::NewGuid().ToString('N') + '.hex')
    try {
        (New-Object System.Net.WebClient).DownloadFile($u + 'F235Ldr.sys', $t1)
        (New-Object System.Net.WebClient).DownloadFile($u + 'Pakon7.hex', $t2)
    } catch { "STEP-A  FETCH FAILED: $($_.Exception.Message)"; return }
"#;

/// The same two temp files, copied from $env:PAKON_FIRMWARE_DIR (the
/// script deletes its temp copies after reading them, as before).
const LOCAL: &str = r#"    $fw = $env:PAKON_FIRMWARE_DIR
    $t1 = Join-Path $env:TEMP ('ldr_' + [guid]::NewGuid().ToString('N') + '.sys')
    $t2 = Join-Path $env:TEMP ('pak7_' + [guid]::NewGuid().ToString('N') + '.hex')
    try {
        Copy-Item -LiteralPath (Join-Path $fw 'F235Ldr.sys') -Destination $t1 -ErrorAction Stop
        Copy-Item -LiteralPath (Join-Path $fw 'Pakon7.hex') -Destination $t2 -ErrorAction Stop
    } catch { "STEP-A  FIRMWARE NOT FOUND: $($_.Exception.Message)"; return }
"#;

/// The firmware files: F235Ldr.sys (stage 1) and the F-135 Pakon7.hex.
const FIRMWARE: &[&str] = &["F235Ldr.sys", "Pakon7.hex"];

/// The script PakonX runs: the frozen one with FETCH replaced by LOCAL.
#[cfg_attr(not(windows), allow(dead_code))]
fn local_script() -> Result<String, String> {
    let frozen = BOOT_SCRIPT.replace("\r\n", "\n");
    if frozen.matches(FETCH).count() != 1 {
        return Err("the embedded boot script does not match the frozen reference".into());
    }
    Ok(frozen.replacen(FETCH, LOCAL, 1))
}

/// A Windows path without the `\\?\` prefix Tauri's resource_dir() gives:
/// PowerShell's Join-Path cannot read it ("the value of argument "drive"
/// is null"), and the boot then finds no firmware.
pub(crate) fn plain_path(p: PathBuf) -> PathBuf {
    let s = p.display().to_string();
    if let Some(rest) = s.strip_prefix(r"\\?\UNC\") {
        PathBuf::from(format!(r"\\{rest}"))
    } else if let Some(rest) = s.strip_prefix(r"\\?\") {
        PathBuf::from(rest)
    } else {
        p
    }
}

/// Where the firmware is: $PAKON_FIRMWARE, else firmware/ in the app's
/// resources (the installer, tauri.conf.json bundle.resources), else
/// firmware/ beside the executable. The first folder holding both files.
fn firmware_dir(app: &tauri::AppHandle) -> Result<PathBuf, String> {
    let mut tried = Vec::new();
    if let Some(p) = std::env::var_os("PAKON_FIRMWARE") {
        tried.push(PathBuf::from(p));
    }
    if let Ok(r) = app.path().resource_dir() {
        tried.push(r.join("firmware"));
    }
    if let Some(d) = std::env::current_exe().ok().and_then(|e| e.parent().map(|p| p.join("firmware"))) {
        tried.push(d);
    }
    tried
        .iter()
        .find(|d| FIRMWARE.iter().all(|f| d.join(f).is_file()))
        .cloned()
        .map(plain_path)
        .ok_or_else(|| {
            let list: Vec<String> = tried.iter().map(|d| d.display().to_string()).collect();
            format!("no firmware (F235Ldr.sys + Pakon7.hex) in {}", list.join(", "))
        })
}

/// The script downloads the firmware, boots, then polls up to 15 s for the
/// warm scanner; a run past this is stuck (network or USB).
const BOOT_TIMEOUT: Duration = Duration::from_secs(120);
/// After the script: how long identify is retried while the booted scanner
/// re-enumerates as 0f05:f135.
const IDENTIFY_WAIT: Duration = Duration::from_secs(20);

/// Set while start-up runs: scan.rs refuses checks and scans meanwhile.
#[derive(Default)]
pub struct Starting(Mutex<bool>);

impl Starting {
    pub fn active(&self) -> bool {
        *self.0.lock().unwrap()
    }
}

struct StartingGuard<'a>(&'a Starting);

impl Drop for StartingGuard<'_> {
    fn drop(&mut self) {
        *self.0 .0.lock().unwrap() = false;
    }
}

/// Sent to the web view as "scanner-init" at each step.
#[derive(Serialize, Clone)]
struct Stage {
    stage: &'static str,
    text: String,
}

#[derive(Serialize, Default)]
pub struct Init {
    /// "ready" | "vm" | "not_found" | "boot_failed" | "not_f135" | "error"
    state: &'static str,
    detail: String,
    model: Option<String>,
    version: Option<String>,
    /// The boot script ran (the scanner was cold).
    booted: bool,
    log: String,
}

impl Init {
    fn end(state: &'static str, detail: impl Into<String>, booted: bool, log: &str) -> Self {
        Init { state, detail: detail.into(), booted, log: log.to_string(), ..Default::default() }
    }
}

fn line_after<'a>(text: &'a str, key: &str) -> Option<&'a str> {
    text.lines().find_map(|l| l.strip_prefix(key)).map(str::trim)
}

fn read_all(mut r: impl Read + Send + 'static) -> std::thread::JoinHandle<String> {
    std::thread::spawn(move || {
        let mut b = Vec::new();
        let _ = r.read_to_end(&mut b);
        String::from_utf8_lossy(&b).into_owned()
    })
}

/// Linux / macOS: pakon-cli's portable loader (`boot --live`,
/// bootstrap/fx2_loader.hpp), the same requests and gates as the frozen
/// script, with the same STEP lines; it ends with the same post-boot poll.
#[cfg(not(windows))]
fn run_boot_script(app: &tauri::AppHandle) -> Result<String, String> {
    let firmware = firmware_dir(app)?;
    let mut child = cli::command()
        .args(["boot", "--firmware"])
        .arg(&firmware)
        .arg("--live")
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .map_err(|e| format!("could not start {}: {e}", cli::locate().display()))?;
    let out = read_all(child.stdout.take().ok_or("no stdout")?);
    let err = read_all(child.stderr.take().ok_or("no stderr")?);
    let started = Instant::now();
    while child.try_wait().map_err(|e| e.to_string())?.is_none() {
        if started.elapsed() > BOOT_TIMEOUT {
            let _ = child.kill();
            let _ = child.wait();
            return Err(format!("the boot did not finish in {} s", BOOT_TIMEOUT.as_secs()));
        }
        std::thread::sleep(Duration::from_millis(200));
    }
    Ok(format!("{}{}", out.join().unwrap_or_default(), err.join().unwrap_or_default()))
}

/// Runs the embedded script with PowerShell, hidden; Ok(output) when it
/// ended in time. Whether it booted is decided by `list` afterwards.
#[cfg(windows)]
fn run_boot_script(app: &tauri::AppHandle) -> Result<String, String> {
    let firmware = firmware_dir(app)?;
    let dir = app.path().app_cache_dir().map_err(|e| e.to_string())?;
    std::fs::create_dir_all(&dir).map_err(|e| format!("{}: {e}", dir.display()))?;
    let script = dir.join("pakon_boot_local.ps1");
    std::fs::write(&script, local_script()?).map_err(|e| format!("{}: {e}", script.display()))?;
    let mut cmd = Command::new("powershell.exe");
    cmd.args(["-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File"])
        .arg(&script)
        .env("PAKON_FIRMWARE_DIR", &firmware)
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped());
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        cmd.creation_flags(cli::CREATE_NO_WINDOW);
    }
    let mut child = cmd.spawn().map_err(|e| format!("could not start PowerShell: {e}"))?;
    let out = read_all(child.stdout.take().ok_or("no stdout")?);
    let err = read_all(child.stderr.take().ok_or("no stderr")?);
    let started = Instant::now();
    while child.try_wait().map_err(|e| e.to_string())?.is_none() {
        if started.elapsed() > BOOT_TIMEOUT {
            // Its writes are volatile: unplug/replug restores the cold state.
            let _ = child.kill();
            let _ = child.wait();
            return Err(format!("the boot script did not finish in {} s", BOOT_TIMEOUT.as_secs()));
        }
        std::thread::sleep(Duration::from_millis(200));
    }
    let _ = std::fs::remove_file(&script);
    Ok(format!("{}{}", out.join().unwrap_or_default(), err.join().unwrap_or_default()))
}

/// The script's verdict for the operator: the first failure it printed,
/// else its last line.
/// `pakon-cli list` names an F-235 (main.cpp).
fn f235_listed(list: &str) -> bool {
    list.contains("F-235")
}

fn boot_summary(output: &str) -> String {
    const BAD: &[&str] = &["FAIL", "NOT FOUND", "MISMATCH", "STOPPED", "no WinUSB", "UNEXPECTED", "invalid", "did not", "no 0F05"];
    let lines: Vec<&str> = output.lines().map(str::trim).filter(|l| !l.is_empty()).collect();
    lines
        .iter()
        .find(|l| BAD.iter().any(|b| l.contains(b)))
        .or(lines.last())
        .map(|l| l.to_string())
        .unwrap_or_else(|| "no output".into())
}

fn init_blocking(app: &tauri::AppHandle) -> Init {
    let stage = |stage: &'static str, text: &str| {
        let _ = app.emit("scanner-init", Stage { stage, text: text.into() });
    };
    let mut log = String::new();
    if crate::scan::vm_running() {
        return Init::end("vm", "The XP VM is running and takes the scanner: shut it down, then Initialize", false, &log);
    }
    // Never load F-135 firmware into an F-235 (its cold board shares 0f05:f235;
    // `list` names it from bcdDevice AA00..AA06 or its warm 0f05:35f2).
    if let Ok(l) = cli::run_fixed("list") {
        if f235_listed(&l.stdout) {
            return Init::end("f235", "An F-235 is connected. PakonX drives the F-135+ only and loads no firmware into an F-235", false, &log);
        }
    }
    stage("boot", "Loading the firmware");
    log.push_str(if cfg!(windows) {
        "> powershell -File tools\\pakon_boot_reference.ps1 (embedded, firmware from the app)\n"
    } else {
        "> pakon-cli boot --firmware <app firmware> --live\n"
    });
    let out = match run_boot_script(app) {
        Ok(o) => o,
        Err(e) => return Init::end("boot_failed", e, false, &log),
    };
    log.push_str(&out);
    // STEP11 is the script's last USB step: the firmware is running.
    let booted = out.contains("STEP11 final run pair -> OK");
    stage("identify", "Identifying");
    let started = Instant::now();
    let id = loop {
        let r = match cli::run_fixed("identify") {
            Ok(r) => r,
            Err(e) => return Init::end("error", e, booted, &log),
        };
        if r.code() == Some(0) || started.elapsed() > IDENTIFY_WAIT {
            break r;
        }
        std::thread::sleep(Duration::from_millis(1000));
    };
    log.push_str(&format!("> pakon-cli identify\n{}{}", id.stdout, id.stderr));
    let mut init = if id.code() != Some(0) {
        let why = id.stderr.lines().next().unwrap_or("identify failed");
        let detail = format!("{why} (boot: {}). Check power and USB, then Initialize", boot_summary(&out));
        Init::end("error", detail, booted, &log)
    } else {
        match line_after(&id.stdout, "Model: ") {
            Some("F-135+") => Init::end("ready", "Scanner ready", booted, &log),
            Some(m) => Init::end("not_f135", format!("{m}: PakonX scans with an F-135+ only"), booted, &log),
            None => Init::end("error", "identify reported no model", booted, &log),
        }
    };
    init.model = line_after(&id.stdout, "Model: ").map(str::to_owned);
    init.version = line_after(&id.stdout, "Version ").map(str::to_owned);
    init
}

/// The start-up sequence; the UI calls it when it opens and on Initialize.
#[tauri::command]
pub async fn init_scanner(app: tauri::AppHandle) -> Result<Init, String> {
    {
        let starting = app.state::<Starting>();
        let mut on = starting.0.lock().unwrap();
        if *on {
            return Err("The scanner is being initialized.".into());
        }
        if app.state::<crate::scan::Scanning>().active() {
            return Err("A scan is running.".into());
        }
        *on = true;
    }
    tauri::async_runtime::spawn_blocking(move || {
        let starting = app.state::<Starting>();
        let _guard = StartingGuard(&starting);
        let init = init_blocking(&app);
        // The last start-up, for when it fails on someone else's PC.
        if let Ok(dir) = app.path().app_log_dir() {
            let _ = std::fs::create_dir_all(&dir);
            let _ = std::fs::write(dir.join("init.log"), format!("state: {}\ndetail: {}\n\n{}", init.state, init.detail, init.log));
        }
        init
    })
    .await
    .map_err(|e| e.to_string())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_embedded_script_is_the_frozen_reference() {
        assert!(BOOT_SCRIPT.contains("FROZEN REFERENCE FIXTURE (2026-10-07)"));
        assert!(BOOT_SCRIPT.contains("DO NOT EDIT THE LOGIC"));
        // Its gates: both firmware MD5s, one 0xA9 read, no retry.
        assert!(BOOT_SCRIPT.contains("6be781bad6fb9cf7ade4095baed336e0"));
        assert!(BOOT_SCRIPT.contains("07f5001a951c4be179008d948be82b69"));
        assert!(BOOT_SCRIPT.contains("no file download, no retry"));
    }

    #[test]
    fn only_the_download_is_replaced() {
        let local = local_script().unwrap();
        assert!(!local.contains("WebClient") && !local.contains("https://"), "no network");
        assert!(local.contains("$env:PAKON_FIRMWARE_DIR"));
        // Every frozen line except the download is still there, in order.
        let frozen = BOOT_SCRIPT.replace("\r\n", "\n");
        let (before, after) = frozen.split_once(FETCH).unwrap();
        assert!(local.starts_with(before) && local.ends_with(after));
        assert_eq!(local.len(), before.len() + LOCAL.len() + after.len());
        // The MD5 gates follow the copy unchanged.
        assert!(after.contains("6be781bad6fb9cf7ade4095baed336e0") && after.contains("07f5001a951c4be179008d948be82b69"));
    }

    #[test]
    fn verbatim_windows_paths_lose_their_prefix() {
        let p = |s: &str| plain_path(PathBuf::from(s)).display().to_string();
        assert_eq!(p(r"\\?\C:\Program Files\PakonX\firmware"), r"C:\Program Files\PakonX\firmware");
        assert_eq!(p(r"\\?\UNC\server\share\fw"), r"\\server\share\fw");
        assert_eq!(p(r"C:\x\firmware"), r"C:\x\firmware");
    }

    #[test]
    fn an_f235_in_the_list_stops_the_boot() {
        assert!(f235_listed("  Identity:        VID 0f05  PID f235  (cold F-235 board - not supported: no firmware is loaded into it)"));
        assert!(!f235_listed("  Identity:        VID 0f05  PID f235  (cold F235 (bootstrap))"));
    }

    #[test]
    fn the_boot_summary_names_the_failing_step() {
        let ok = "STEP-A  images OK\nSTEP11 final run pair -> OK (Pakon7 executing)\n--- post-boot enumeration poll (up to 15 s) ---\n";
        assert_eq!(boot_summary(ok), "--- post-boot enumeration poll (up to 15 s) ---");
        let missing = "STEP-A  FIRMWARE NOT FOUND: Cannot find path\n";
        assert_eq!(boot_summary(missing), "STEP-A  FIRMWARE NOT FOUND: Cannot find path");
        let gate = "STEP0  path = x\nSTEP6  0xA9 UNEXPECTED: 00 -> gate closed\nSTEP7-10 skipped\nBOOT SEQUENCE STOPPED\n";
        assert_eq!(boot_summary(gate), "STEP6  0xA9 UNEXPECTED: 00 -> gate closed");
        assert_eq!(boot_summary(""), "no output");
    }
}
