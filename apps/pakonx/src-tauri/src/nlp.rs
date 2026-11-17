// Negative Lab Pro (a Lightroom Classic plugin): the roll's negatives as
// flat-fielded linear 16-bit TIFFs (`frames --save negative`), written
// beside the scan in <roll>_negatives, then handed to Lightroom Classic,
// which opens its import for them. NLP itself runs inside Lightroom on the
// imported negatives (its source setting "Scanner - Linear").
//
// Offline: no device, only pakon-cli's frames and Lightroom's own exe.
use crate::roll::{roll_files, roll_name, run_frames, save_args, SaveSpec};
use serde::Serialize;
use std::path::{Path, PathBuf};
use std::process::Command;

#[derive(Serialize)]
pub struct Nlp {
    /// Lightroom Classic's exe, when installed.
    lightroom: Option<String>,
    /// NegativeLabPro*.lrplugin in Lightroom's Modules folder.
    plugin: bool,
}

/// Lightroom Classic under Program Files\Adobe (the folder name carries
/// "Lightroom Classic" in every version since 2017; the cloud "Lightroom"
/// takes no plugins); on macOS the .app in /Applications. None on Linux.
#[cfg(target_os = "macos")]
fn find_lightroom() -> Option<PathBuf> {
    let dirs = std::fs::read_dir("/Applications").ok()?;
    dirs.flatten().map(|d| d.path()).find_map(|d| {
        let name = d.file_name()?.to_string_lossy().to_lowercase();
        if !name.contains("lightroom classic") {
            return None;
        }
        // "Adobe Lightroom Classic/Adobe Lightroom Classic.app", or the .app itself.
        if name.ends_with(".app") {
            return Some(d);
        }
        std::fs::read_dir(&d).ok()?.flatten().map(|e| e.path()).find(|p| {
            p.extension().is_some_and(|x| x == "app") && p.to_string_lossy().to_lowercase().contains("lightroom classic")
        })
    })
}

#[cfg(target_os = "linux")]
fn find_lightroom() -> Option<PathBuf> {
    None
}

#[cfg(windows)]
fn find_lightroom() -> Option<PathBuf> {
    let roots = ["ProgramW6432", "ProgramFiles"].iter().filter_map(std::env::var_os).map(PathBuf::from);
    for root in roots {
        let Ok(dirs) = std::fs::read_dir(root.join("Adobe")) else { continue };
        for d in dirs.flatten() {
            let name = d.file_name().to_string_lossy().to_lowercase();
            let exe = d.path().join("Lightroom.exe");
            if name.contains("lightroom classic") && exe.is_file() {
                return Some(exe);
            }
        }
    }
    None
}

/// NLP's installer puts NegativeLabPro.lrplugin in Lightroom's Modules
/// folder; a plugin added by hand elsewhere is not seen (the button still
/// works, the log says so).
fn plugin_installed() -> bool {
    #[cfg(target_os = "macos")]
    let base = std::env::var_os("HOME").map(|h| Path::new(&h).join("Library").join("Application Support"));
    #[cfg(not(target_os = "macos"))]
    let base = std::env::var_os("APPDATA").map(PathBuf::from);
    let Some(base) = base else { return false };
    let modules = base.join("Adobe").join("Lightroom").join("Modules");
    std::fs::read_dir(modules).map_or(false, |d| {
        d.flatten().any(|e| {
            let n = e.file_name().to_string_lossy().to_lowercase();
            n.starts_with("negativelabpro") && n.ends_with(".lrplugin")
        })
    })
}

#[tauri::command]
pub fn nlp_status() -> Nlp {
    Nlp { lightroom: find_lightroom().map(|p| p.display().to_string()), plugin: plugin_installed() }
}

#[derive(Serialize)]
pub struct Opened {
    written: Vec<String>,
    plugin: bool,
    log: String,
}

/// Writes the negatives of `pictures` (all when empty) and opens them in
/// Lightroom Classic.
#[tauri::command]
pub async fn open_in_nlp(app: tauri::AppHandle, prefix: String, tail_first: bool, pictures: Vec<u32>) -> Result<Opened, String> {
    roll_files(&prefix)?;
    if pictures.contains(&0) {
        return Err("pictures are numbered from 1".into());
    }
    let lightroom = find_lightroom().ok_or("Lightroom Classic is not installed (Negative Lab Pro runs inside it).")?;
    let dir = Path::new(&prefix).with_file_name(format!("{}_negatives", roll_name(&prefix)?));
    std::fs::create_dir_all(&dir).map_err(|e| format!("{}: {e}", dir.display()))?;
    let spec = SaveSpec {
        format: "negative",
        quality: 92,
        dx_names: true,
        adjust: None,
        shift: crate::roll::roll_shift(&prefix),
        bw: false,
        naming: None,
    };
    let expected = (!pictures.is_empty()).then_some(pictures.len());
    let log = run_frames(&app, "save", save_args(&prefix, &dir, tail_first, &spec, &pictures), expected).await?;
    let written: Vec<String> = log.lines().filter_map(|l| l.strip_prefix("written: ")).map(str::to_owned).collect();
    if written.is_empty() {
        return Err(format!("no negatives were written\n{log}"));
    }
    #[cfg(target_os = "macos")]
    let launched = Command::new("open").arg("-a").arg(&lightroom).args(&written).spawn();
    #[cfg(not(target_os = "macos"))]
    let launched = Command::new(&lightroom).args(&written).spawn();
    launched
        .map_err(|e| format!("could not start {}: {e}", lightroom.display()))?;
    Ok(Opened { written, plugin: plugin_installed(), log })
}
