// Bridge to pakon-cli.
//
// Hardware safety: `run_cli` only runs the read-only commands below. The
// one live command, `scan --live-scan` (lamp on, film moving), has its own
// bridge in scan.rs, built around docs/HARDWARE_RUNBOOK.md: fixed flags,
// the read-only checks first, Stop through the teardown.
use serde::Serialize;
use std::path::PathBuf;
use std::process::Command;
use std::time::Instant;

const ALLOWED: &[&str] = &["list", "attrib", "descriptors", "identify", "status"];

/// Read-only steps scan.rs runs before a scan, besides ALLOWED.
const CHECKS: &[&str] = &["eeprom"];

#[cfg(windows)]
pub const CREATE_NO_WINDOW: u32 = 0x0800_0000;

#[derive(Serialize)]
pub struct CliRun {
    command: String,
    code: Option<i32>,
    pub stdout: String,
    pub stderr: String,
    millis: u128,
}

impl CliRun {
    #[cfg(test)]
    pub fn new(command: &str, code: Option<i32>, stdout: &str, stderr: &str) -> Self {
        CliRun { command: command.into(), code, stdout: stdout.into(), stderr: stderr.into(), millis: 0 }
    }

    pub fn code(&self) -> Option<i32> {
        self.code
    }
}

/// Where pakon-cli lives: $PAKON_CLI, else next to this executable, else PATH.
pub fn locate() -> PathBuf {
    if let Some(p) = std::env::var_os("PAKON_CLI") {
        return PathBuf::from(p);
    }
    let name = if cfg!(windows) { "pakon-cli.exe" } else { "pakon-cli" };
    if let Some(dir) = std::env::current_exe().ok().and_then(|e| e.parent().map(PathBuf::from)) {
        let beside = dir.join(name);
        if beside.is_file() {
            return beside;
        }
    }
    PathBuf::from(name)
}

/// pakon-cli, without a console window of its own.
pub fn command() -> Command {
    let mut cmd = Command::new(locate());
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        cmd.creation_flags(CREATE_NO_WINDOW);
    }
    cmd
}

/// Runs one read-only command (ALLOWED or CHECKS) with no arguments, blocking.
pub fn run_fixed(command: &str) -> Result<CliRun, String> {
    if !ALLOWED.contains(&command) && !CHECKS.contains(&command) {
        return Err(format!("'{command}' is not available from PakonX"));
    }
    let started = Instant::now();
    let out = self::command()
        .arg(command)
        .output()
        .map_err(|e| format!("could not start {}: {e}", locate().display()))?;
    Ok(CliRun {
        command: command.to_string(),
        code: out.status.code(),
        stdout: String::from_utf8_lossy(&out.stdout).into_owned(),
        stderr: String::from_utf8_lossy(&out.stderr).into_owned(),
        millis: started.elapsed().as_millis(),
    })
}

/// "windows", "macos" or "linux": the web view hides what a platform lacks
/// (the OEM look is Windows only: a 32-bit Windows DLL).
#[tauri::command]
pub fn platform() -> &'static str {
    std::env::consts::OS
}

#[tauri::command]
pub fn cli_path() -> String {
    locate().display().to_string()
}

#[tauri::command]
pub async fn run_cli(command: String, starting: tauri::State<'_, crate::boot::Starting>) -> Result<CliRun, String> {
    if !ALLOWED.contains(&command.as_str()) {
        return Err(format!("'{command}' is not available from PakonX"));
    }
    // The boot script owns the scanner until it is warm.
    if starting.active() {
        return Err("The scanner is being initialized.".into());
    }
    tauri::async_runtime::spawn_blocking(move || run_fixed(&command))
        .await
        .map_err(|e| e.to_string())?
}

/// The author's pages (Settings, About). Fixed here: the web view names a
/// page, never a URL.
const LINKS: &[(&str, &str)] = &[
    ("github", "https://github.com/amstrd-cpc"),
    ("instagram", "https://www.instagram.com/amstrd_cpc"),
    // PakonX is free software: its source, and a way to say thanks.
    ("source", "https://github.com/amstrd-cpc/PakonX"),
    ("kofi", "https://ko-fi.com/laborantsoftworks"),
];

#[tauri::command]
pub fn open_link(page: String) -> Result<(), String> {
    let url = LINKS.iter().find(|(k, _)| *k == page).map(|(_, u)| *u).ok_or(format!("'{page}' is not a link"))?;
    #[cfg(windows)]
    let r = Command::new("explorer.exe").arg(url).spawn();
    #[cfg(target_os = "macos")]
    let r = Command::new("open").arg(url).spawn();
    #[cfg(all(unix, not(target_os = "macos")))]
    let r = Command::new("xdg-open").arg(url).spawn();
    r.map(|_| ()).map_err(|e| format!("could not open {url}: {e}"))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn only_the_authors_pages_open() {
        assert!(open_link("https://evil.example".into()).is_err());
        assert!(open_link("".into()).is_err());
    }

    #[test]
    fn live_scan_and_unknown_commands_are_refused() {
        for command in ["scan", "scan --live-scan", "eeprom", "probe", "frames", "status; rm"] {
            assert!(!ALLOWED.contains(&command), "{command} must not reach pakon-cli");
        }
        for command in ["scan", "scan --live-scan", "eeprom --out x", "probe", "frames"] {
            assert!(run_fixed(command).is_err(), "{command} must not reach pakon-cli");
        }
    }
}
