// Rolls: finished scans on disk (<prefix>.film.pakraw + <prefix>.scan.json),
// opened offline through `pakon-cli frames <prefix> --preview` and saved
// as finished pictures through `frames <prefix> --save jpeg|tiff`.
//
// Hardware safety: `frames` never opens the scanner; it only reads the two
// scan files. The arguments are built here from a validated path and fixed
// flags, never taken from the web view, so nothing else can reach the CLI
// through this command.
use serde::Serialize;
use serde_json::Value;
use std::io::{BufRead, BufReader, Read};
use std::path::{Path, PathBuf};
use std::process::{Child, Stdio};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{SystemTime, UNIX_EPOCH};
use tauri::{Emitter, Manager};

#[derive(Serialize)]
pub struct RollSummary {
    prefix: String,
    name: String,
    mode: String,
    modified: u64,
}

#[derive(Serialize)]
pub struct Roll {
    prefix: String,
    /// The framing correction the previews were made with.
    shift: i64,
    /// Black-and-white film.
    bw: bool,
    name: String,
    mode: String,
    /// Folder holding <name>.frames.json and the preview / thumb BMPs.
    cache: String,
    frames: Value,
    log: String,
    /// Previews reused from an earlier open; pakon-cli did not run.
    cached: bool,
}

fn scan_mode(sidecar: &Path) -> String {
    std::fs::read_to_string(sidecar)
        .ok()
        .and_then(|t| serde_json::from_str::<Value>(&t).ok())
        .and_then(|v| v.get("mode").and_then(|m| m.as_str()).map(str::to_owned))
        .unwrap_or_default()
}

/// A prefix names a roll when both scan files exist next to each other.
pub(crate) fn roll_files(prefix: &str) -> Result<(PathBuf, PathBuf), String> {
    if prefix.is_empty() || prefix.starts_with('-') {
        return Err("not a roll path".into());
    }
    let side = PathBuf::from(format!("{prefix}.scan.json"));
    let raw = PathBuf::from(format!("{prefix}.film.pakraw"));
    if !side.is_file() || !raw.is_file() {
        return Err(format!("{prefix}: no .scan.json + .film.pakraw pair"));
    }
    Ok((side, raw))
}

/// The roll's framing correction, in film lines (`frames --shift`): the
/// "shift" of <prefix>.adjust.json, 0 when there is none.
pub(crate) fn roll_shift(prefix: &str) -> i64 {
    std::fs::read_to_string(adjust_file(prefix))
        .ok()
        .and_then(|t| serde_json::from_str::<Value>(&t).ok())
        .and_then(|v| v.get("shift").and_then(Value::as_i64))
        .filter(|s| s.abs() <= MAX_SHIFT)
        .unwrap_or(0)
}

/// Black-and-white film (`frames --bw`): "film": "bw" in <prefix>.adjust.json,
/// chosen with the order or on the light table.
pub(crate) fn roll_bw(prefix: &str) -> bool {
    std::fs::read_to_string(adjust_file(prefix))
        .ok()
        .and_then(|t| serde_json::from_str::<Value>(&t).ok())
        .is_some_and(|v| v.get("film").and_then(Value::as_str) == Some("bw"))
}

/// Infrared dust removal (`frames --ice`): "ice" in <prefix>.adjust.json,
/// light / normal / strong; "off" when absent or unknown.
pub(crate) fn roll_ice(prefix: &str) -> &'static str {
    roll_choice(prefix, "ice", ICE)
}

/// Frame format (`frames --format`): "format" in <prefix>.adjust.json,
/// full, half or pano; "auto" (from the gaps) when absent or unknown.
pub(crate) fn roll_format(prefix: &str) -> &'static str {
    roll_choice(prefix, "format", FORMATS)
}

/// One of `allowed` from <prefix>.adjust.json[key]; allowed[0] otherwise.
fn roll_choice(prefix: &str, key: &str, allowed: &[&'static str]) -> &'static str {
    let v = std::fs::read_to_string(adjust_file(prefix))
        .ok()
        .and_then(|t| serde_json::from_str::<Value>(&t).ok())
        .and_then(|v| v.get(key).and_then(Value::as_str).map(str::to_owned));
    allowed.iter().copied().find(|k| v.as_deref() == Some(*k)).unwrap_or(allowed[0])
}

const ICE: &[&str] = &["off", "light", "normal", "strong"];
const FORMATS: &[&str] = &["auto", "full", "half", "pano"];

/// `--ice` for the roll, when it is on (the strength is part of every
/// development the roll gets: previews, saves, the OEM look).
pub(crate) fn push_ice(a: &mut Vec<String>, prefix: &str) {
    let ice = roll_ice(prefix);
    if ice != "off" {
        a.push("--ice".to_string());
        a.push(ice.to_string());
    }
    let format = roll_format(prefix);
    if format != "auto" {
        a.push("--format".to_string());
        a.push(format.to_string());
    }
}

fn push_bw(a: &mut Vec<String>, bw: bool) {
    if bw {
        a.push("--bw".to_string());
    }
}

/// `frames --shift` takes -20000..20000; PakonX stays well inside.
const MAX_SHIFT: i64 = 5000;

fn push_shift(a: &mut Vec<String>, shift: i64) {
    if shift != 0 {
        a.push("--shift".to_string());
        a.push(shift.to_string());
    }
}

/// The pakon-cli arguments for opening a roll: fixed flags only.
pub fn frames_args(prefix: &str, out_dir: &Path, tail_first: bool, shift: i64, bw: bool) -> Vec<String> {
    let mut a = vec![
        "frames".to_string(),
        prefix.to_string(),
        "--preview".to_string(),
        "--out-dir".to_string(),
        out_dir.display().to_string(),
    ];
    if tail_first {
        a.push("--tail-first".to_string());
    }
    push_shift(&mut a, shift);
    push_bw(&mut a, bw);
    push_ice(&mut a, prefix);
    a
}

/// Saved file names BASE001, BASE002, … by position on the roll
/// (`frames --name BASE --start N --digits D`).
pub struct Naming<'a> {
    pub base: &'a str,
    pub start: u32,
    pub digits: u8,
}

/// A name base the user typed: printable ASCII (pakon-cli opens files by
/// narrow paths), no folder, nothing Windows refuses in a file name (the
/// number follows it, so a trailing space or dot is fine).
pub(crate) fn check_naming(n: &Naming) -> Result<(), String> {
    let b = n.base;
    let ok = !b.is_empty()
        && b.len() <= 80
        && !b.starts_with('-')
        && b.chars().all(|c| (' '..='~').contains(&c) && !"<>:\"/\\|?*".contains(c));
    if !ok {
        return Err(format!("'{b}': a name is up to 80 letters, digits and punctuation, without <>:\"/\\|?*"));
    }
    if n.start > 999_999 || !(1..=6).contains(&n.digits) {
        return Err("numbering: start 0..999999, 1..6 digits".into());
    }
    Ok(())
}

/// The file name of picture `n` (1-based) under a naming: BASE + number.
pub(crate) fn numbered(n: &Naming, picture: u64) -> String {
    format!("{}{:0width$}", n.base, u64::from(n.start) + picture - 1, width = usize::from(n.digits))
}

/// File formats `frames --save` writes.
fn save_format(format: &str) -> Result<&'static str, String> {
    match format {
        "jpeg" => Ok("jpeg"),
        "tiff" => Ok("tiff"),
        _ => Err(format!("'{format}' is not a picture format (jpeg or tiff)")),
    }
}

/// How `frames --save` writes: a checked format, JPEG quality 1..=100,
/// names by DX frame number (19A) or picture number (frameNN).
pub struct SaveSpec<'a> {
    pub format: &'a str,
    pub quality: u8,
    pub dx_names: bool,
    /// <prefix>.adjust.json, the per-picture looks, when there is one.
    pub adjust: Option<&'a Path>,
    /// The roll's framing correction (roll_shift).
    pub shift: i64,
    /// Black-and-white film (roll_bw).
    pub bw: bool,
    /// Custom names; None: dx_names or picture numbers.
    pub naming: Option<Naming<'a>>,
}

/// The pakon-cli arguments for saving pictures: fixed flags, a checked
/// spec and picture numbers; an empty list saves every picture.
pub fn save_args(prefix: &str, out_dir: &Path, tail_first: bool, spec: &SaveSpec, pictures: &[u32]) -> Vec<String> {
    let mut a = vec![
        "frames".to_string(),
        prefix.to_string(),
        "--save".to_string(),
        spec.format.to_string(),
        "--quality".to_string(),
        spec.quality.to_string(),
        "--out-dir".to_string(),
        out_dir.display().to_string(),
    ];
    if let Some(n) = &spec.naming {
        a.extend(["--name".to_string(), n.base.to_string(), "--start".to_string(), n.start.to_string(),
                  "--digits".to_string(), n.digits.to_string()]);
    } else if spec.dx_names {
        a.push("--dx-names".to_string());
    }
    if let Some(p) = spec.adjust {
        a.push("--adjust".to_string());
        a.push(p.display().to_string());
    }
    if !pictures.is_empty() {
        let list: Vec<String> = pictures.iter().map(u32::to_string).collect();
        a.push("--pictures".to_string());
        a.push(list.join(","));
    }
    if tail_first {
        a.push("--tail-first".to_string());
    }
    push_shift(&mut a, spec.shift);
    push_bw(&mut a, spec.bw);
    push_ice(&mut a, prefix);
    a
}

/// The pakon-cli run in progress (one at a time: the UI is busy meanwhile),
/// so `cancel_frames` can stop it.
#[derive(Default)]
pub struct Running {
    child: Mutex<Option<Arc<Mutex<Child>>>>,
    cancelled: AtomicBool,
}

impl Running {
    pub(crate) fn busy(&self) -> bool {
        self.child.lock().unwrap().is_some()
    }
}

/// Sent to the web view as "frames-progress" while pakon-cli works.
#[derive(Serialize, Clone)]
struct Progress {
    op: &'static str,
    done: usize,
    total: usize,
}

/// Counts pictures in pakon-cli's stdout: each "  frame N:" line lists a
/// picture of the roll, each "written: " line (not frames.json) finishes
/// one; for the OEM look ("look"), each JPEG the OEM DLL saves does, as
/// it happens (its "written:" lines come all at the end). Returns true
/// when the counts changed.
fn count_progress(op: &str, line: &str, total: &mut usize, done: &mut usize) -> bool {
    if op == "look" {
        if line.starts_with("PISaveFilePlanar_8") {
            *done += 1;
            return true;
        }
        if line.starts_with("written: ") {
            return false;
        }
    }
    if let Some(path) = line.strip_prefix("written: ") {
        if !path.ends_with(".frames.json") {
            *done += 1;
            return true;
        }
    } else if line.starts_with("  frame ") && line.contains(':') {
        *total += 1;
        return true;
    }
    false
}

/// Runs pakon-cli off the UI thread, reporting progress as it prints;
/// Ok(log) on exit 0. `expected`: the picture count when not the whole roll.
pub(crate) async fn run_frames(app: &tauri::AppHandle, op: &'static str, args: Vec<String>, expected: Option<usize>) -> Result<String, String> {
    // Developing beside a film pass could starve its USB stream.
    if app.state::<crate::scan::Scanning>().active() {
        return Err("A scan is running: develop or save the roll when it is done.".into());
    }
    let exe = crate::cli::locate();
    let app = app.clone();
    tauri::async_runtime::spawn_blocking(move || {
        let mut cmd = crate::cli::command();
        cmd.args(&args).stdout(Stdio::piped()).stderr(Stdio::piped());
        let mut child = cmd.spawn().map_err(|e| format!("could not start {}: {e}", exe.display()))?;
        let stdout = child.stdout.take().ok_or("no stdout")?;
        let mut stderr = child.stderr.take().ok_or("no stderr")?;
        let child = Arc::new(Mutex::new(child));
        let running = app.state::<Running>();
        running.cancelled.store(false, Ordering::SeqCst);
        *running.child.lock().unwrap() = Some(child.clone());
        let errs = std::thread::spawn(move || {
            let mut s = String::new();
            let _ = stderr.read_to_string(&mut s);
            s
        });
        let (mut total, mut done) = (0, 0);
        let mut log = String::new();
        for line in BufReader::new(stdout).lines().map_while(Result::ok) {
            if count_progress(op, &line, &mut total, &mut done) {
                let total = expected.unwrap_or(total);
                let _ = app.emit("frames-progress", Progress { op, done: done.min(total), total });
            }
            log.push_str(&line);
            log.push('\n');
        }
        log.push_str(&errs.join().unwrap_or_default());
        // stdout closed: the process has ended, so wait() does not block.
        let status = child.lock().unwrap().wait().map_err(|e| e.to_string());
        *running.child.lock().unwrap() = None;
        if running.cancelled.load(Ordering::SeqCst) {
            return Err(format!("Cancelled.\n{log}"));
        }
        let status = status?;
        if !status.success() {
            return Err(format!("pakon-cli {} failed ({:?})\n{log}", args[0], status.code()));
        }
        Ok(log)
    })
    .await
    .map_err(|e| e.to_string())?
}

/// Stops the pakon-cli run in progress, if any. Partly written previews
/// are redone on the next open; pictures already saved stay.
#[tauri::command]
pub fn cancel_frames(running: tauri::State<'_, Running>) -> bool {
    let child = running.child.lock().unwrap().clone();
    match child {
        Some(c) => {
            running.cancelled.store(true, Ordering::SeqCst);
            c.lock().unwrap().kill().is_ok()
        }
        None => false,
    }
}

pub(crate) fn modified(p: &Path) -> Option<SystemTime> {
    std::fs::metadata(p).and_then(|m| m.modified()).ok()
}

/// frames.json from an earlier open when it is still good: newer than the
/// scan files and pakon-cli itself, with every picture's preview and
/// thumbnail on disk (an interrupted open leaves some missing).
fn cached_frames(cache: &Path, name: &str, inputs: &[PathBuf]) -> Option<Value> {
    let json = cache.join(format!("{name}.frames.json"));
    let made = modified(&json)?;
    if inputs.iter().any(|p| modified(p).map_or(false, |m| m > made)) {
        return None;
    }
    let frames: Value = serde_json::from_str(&std::fs::read_to_string(&json).ok()?).ok()?;
    for f in frames.get("frames")?.as_array()? {
        let Some(n) = f.get("picture").and_then(Value::as_u64) else { continue };
        for kind in ["preview", "thumb"] {
            if !cache.join(format!("{name}.frame{n:02}.{kind}.bmp")).is_file() {
                return None;
            }
        }
    }
    Some(frames)
}

#[tauri::command]
pub fn list_rolls(dir: String) -> Result<Vec<RollSummary>, String> {
    let mut out = Vec::new();
    let entries = std::fs::read_dir(&dir).map_err(|e| format!("{dir}: {e}"))?;
    for e in entries.flatten() {
        let path = e.path();
        let Some(file) = path.file_name().and_then(|f| f.to_str()) else { continue };
        let Some(name) = file.strip_suffix(".scan.json") else { continue };
        let prefix = path.with_file_name(name);
        let prefix_s = prefix.display().to_string();
        if roll_files(&prefix_s).is_err() {
            continue; // first-light runs have no film window
        }
        let modified = e
            .metadata()
            .and_then(|m| m.modified())
            .ok()
            .and_then(|t| t.duration_since(UNIX_EPOCH).ok())
            .map(|d| d.as_secs())
            .unwrap_or(0);
        out.push(RollSummary { prefix: prefix_s, name: name.to_string(), mode: scan_mode(&path), modified });
    }
    out.sort_by(|a, b| b.modified.cmp(&a.modified));
    Ok(out)
}

pub(crate) fn roll_name(prefix: &str) -> Result<String, String> {
    Ok(Path::new(prefix).file_name().and_then(|f| f.to_str()).ok_or("not a roll path")?.to_string())
}

#[derive(Serialize)]
pub struct RollInfo {
    prefix: String,
    name: String,
    mode: String,
    shift: i64,
    bw: bool,
}

/// What the light table needs to open a roll straight into the OEM look,
/// without developing PakonX previews first.
#[tauri::command]
pub fn roll_info(prefix: String) -> Result<RollInfo, String> {
    let (side, _) = roll_files(&prefix)?;
    Ok(RollInfo { name: roll_name(&prefix)?, mode: scan_mode(&side), shift: roll_shift(&prefix), bw: roll_bw(&prefix), prefix })
}

/// The roll's cache folder: one per roll and orientation, keyed by the
/// full prefix.
pub(crate) fn roll_cache(app: &tauri::AppHandle, prefix: &str, tail_first: bool) -> Result<PathBuf, String> {
    let base = app.path().app_cache_dir().map_err(|e| e.to_string())?;
    let key = format!("{:016x}", fnv1a(&format!("{prefix}|{tail_first}")));
    Ok(base.join("rolls").join(format!("{}-{key}", roll_name(prefix)?)))
}

/// The previews for one framing: a subfolder of the roll's cache per shift,
/// so deleting the roll's cache drops them all.
pub(crate) fn frames_cache(app: &tauri::AppHandle, prefix: &str, tail_first: bool, shift: i64, bw: bool) -> Result<PathBuf, String> {
    let c = roll_cache(app, prefix, tail_first)?;
    let c = if shift == 0 { c } else { c.join(format!("shift{shift}")) };
    let c = if bw { c.join("bw") } else { c };
    let c = match roll_format(prefix) {
        "full" => c,
        f => c.join(format!("format-{f}")),
    };
    Ok(match roll_ice(prefix) {
        "off" => c,
        ice => c.join(format!("ice-{ice}")),
    })
}

#[tauri::command]
pub async fn open_roll(app: tauri::AppHandle, prefix: String, tail_first: bool) -> Result<Roll, String> {
    let (side, raw) = roll_files(&prefix)?;
    let name = roll_name(&prefix)?;
    let shift = roll_shift(&prefix);
    let bw = roll_bw(&prefix);
    let cache = frames_cache(&app, &prefix, tail_first, shift, bw)?;
    std::fs::create_dir_all(&cache).map_err(|e| format!("{}: {e}", cache.display()))?;
    let mode = scan_mode(&side);
    let inputs = [raw, side, crate::cli::locate()];
    if let Some(frames) = cached_frames(&cache, &name, &inputs) {
        let log = format!("Previews from the cache ({}).", cache.display());
        return Ok(Roll { prefix, shift, bw, name, mode, cache: cache.display().to_string(), frames, log, cached: true });
    }
    let log = run_frames(&app, "open", frames_args(&prefix, &cache, tail_first, shift, bw), None).await?;
    let json = std::fs::read_to_string(cache.join(format!("{name}.frames.json")))
        .map_err(|e| format!("frames.json: {e}"))?;
    let frames: Value = serde_json::from_str(&json).map_err(|e| format!("frames.json: {e}"))?;
    Ok(Roll { prefix, shift, bw, name, mode, cache: cache.display().to_string(), frames, log, cached: false })
}

/// What a scan leaves on disk besides the pictures saved from it
/// (scan_cli.cpp: <prefix>.film.pakraw, .film.tiff, .scan-stats.txt,
/// .scan.json).
/// <prefix>.adjust.json is PakonX's per-picture looks for the roll.
pub(crate) const SCAN_SUFFIXES: &[&str] = &[".film.pakraw", ".film.tiff", ".scan-stats.txt", ".scan.json", ".adjust.json"];

#[derive(Serialize)]
pub struct ScanFile {
    path: String,
    bytes: u64,
}

fn scan_files_of(prefix: &str) -> Vec<ScanFile> {
    SCAN_SUFFIXES
        .iter()
        .map(|s| PathBuf::from(format!("{prefix}{s}")))
        .filter_map(|p| {
            let m = std::fs::metadata(&p).ok().filter(|m| m.is_file())?;
            Some(ScanFile { path: p.display().to_string(), bytes: m.len() })
        })
        .collect()
}

/// The files `delete_scan` would remove, for the confirmation.
#[tauri::command]
pub fn scan_files(prefix: String) -> Result<Vec<ScanFile>, String> {
    roll_files(&prefix)?;
    Ok(scan_files_of(&prefix))
}

/// Deletes a roll's scan files after its pictures were saved. Refused
/// unless every file in `saved` exists and is not empty, so a failed or
/// partial save never costs the scan. The roll's preview cache goes too.
#[tauri::command]
pub fn delete_scan(app: tauri::AppHandle, prefix: String, saved: Vec<String>) -> Result<Vec<String>, String> {
    roll_files(&prefix)?;
    check_saved(&saved)?;
    let mut removed = Vec::new();
    for f in scan_files_of(&prefix) {
        std::fs::remove_file(&f.path).map_err(|e| format!("{}: {e}", f.path))?;
        removed.push(f.path);
    }
    for tail_first in [false, true] {
        if let Ok(cache) = roll_cache(&app, &prefix, tail_first) {
            let _ = std::fs::remove_dir_all(cache);
        }
    }
    Ok(removed)
}

pub(crate) fn check_saved(saved: &[String]) -> Result<(), String> {
    if saved.is_empty() {
        return Err("no saved pictures: the scan is kept".into());
    }
    for s in saved {
        let ok = std::fs::metadata(s).map(|m| m.is_file() && m.len() > 0).unwrap_or(false);
        if !ok {
            return Err(format!("{s}: not saved; the scan is kept"));
        }
    }
    Ok(())
}

#[derive(Serialize)]
pub struct Saved {
    /// The files pakon-cli wrote, in picture order.
    written: Vec<String>,
    log: String,
}

impl Saved {
    pub(crate) fn new(written: Vec<String>, log: String) -> Self {
        Saved { written, log }
    }
}

/// Saves finished pictures of a roll into `dest` (a folder the user chose):
/// every picture when `pictures` is empty, else those numbers (1-based,
/// as in frames.json's "picture").
#[tauri::command]
#[allow(clippy::too_many_arguments)]
pub async fn save_pictures(
    app: tauri::AppHandle,
    prefix: String,
    tail_first: bool,
    format: String,
    quality: u8,
    dx_names: bool,
    pictures: Vec<u32>,
    dest: String,
    name: Option<String>,
    start: u32,
    digits: u8,
) -> Result<Saved, String> {
    roll_files(&prefix)?;
    let naming = name.as_deref().map(|base| Naming { base, start, digits });
    if let Some(n) = &naming {
        check_naming(n)?;
    }
    let format = save_format(&format)?;
    if !(1..=100).contains(&quality) {
        return Err(format!("JPEG quality {quality}: wants 1..100"));
    }
    let adjust_path = adjust_file(&prefix);
    let spec = SaveSpec {
        format,
        quality,
        dx_names,
        adjust: adjust_path.is_file().then_some(adjust_path.as_path()),
        shift: roll_shift(&prefix),
        bw: roll_bw(&prefix),
        naming,
    };
    if pictures.contains(&0) {
        return Err("pictures are numbered from 1".into());
    }
    let dest_path = PathBuf::from(&dest);
    if dest.starts_with('-') || !dest_path.is_absolute() || !dest_path.is_dir() {
        return Err(format!("{dest}: not a folder"));
    }
    let expected = (!pictures.is_empty()).then_some(pictures.len());
    let log = run_frames(&app, "save", save_args(&prefix, &dest_path, tail_first, &spec, &pictures), expected).await?;
    let written = log
        .lines()
        .filter_map(|l| l.strip_prefix("written: "))
        .map(str::to_owned)
        .collect();
    Ok(Saved { written, log })
}

/// One picture of a roll at full size for the 100% view: the saved JPEG
/// (quality 95, picture-number name) in the roll's cache, made once per
/// framing and rotation.
#[tauri::command]
pub async fn full_picture(app: tauri::AppHandle, prefix: String, tail_first: bool, picture: u32, rotate: u16) -> Result<String, String> {
    let (side, raw) = roll_files(&prefix)?;
    if picture == 0 {
        return Err("pictures are numbered from 1".into());
    }
    if ![0, 90, 180, 270].contains(&rotate) {
        return Err(format!("rotate {rotate}: 0, 90, 180 or 270"));
    }
    let shift = roll_shift(&prefix);
    let bw = roll_bw(&prefix);
    let dir = frames_cache(&app, &prefix, tail_first, shift, bw)?.join("full").join(format!("r{rotate}"));
    std::fs::create_dir_all(&dir).map_err(|e| format!("{}: {e}", dir.display()))?;
    let path = dir.join(format!("{}.frame{picture:02}.jpg", roll_name(&prefix)?));
    let inputs = [raw, side, crate::cli::locate()];
    let fresh = modified(&path).is_some_and(|made| inputs.iter().all(|p| modified(p).map_or(true, |m| m <= made)));
    if !fresh {
        // No look: the web view lays it over the picture, as on the
        // previews. Only the rotation is baked in.
        let turn = dir.join("rotate.json");
        if rotate != 0 {
            let doc = serde_json::json!({ "schema": "pakon-adjust/1", "pictures": { picture.to_string(): { "rotate": rotate } } });
            std::fs::write(&turn, doc.to_string()).map_err(|e| format!("{}: {e}", turn.display()))?;
        }
        let spec = SaveSpec { format: "jpeg", quality: 95, dx_names: false, adjust: (rotate != 0).then_some(turn.as_path()), shift, bw, naming: None };
        run_frames(&app, "zoom", save_args(&prefix, &dir, tail_first, &spec, &[picture]), Some(1)).await?;
    }
    Ok(path.display().to_string())
}

pub(crate) fn adjust_file(prefix: &str) -> PathBuf {
    PathBuf::from(format!("{prefix}.adjust.json"))
}

/// The look's fields and their ranges (process/positive.hpp, Adjust).
const ADJUST_FIELDS: &[(&str, f64, f64)] =
    &[("exposure", -3.0, 3.0), ("red", -2.0, 2.0), ("green", -2.0, 2.0), ("blue", -2.0, 2.0), ("contrast", 0.5, 2.0)];

/// Keeps only well-formed looks: picture numbers from 1, known fields in
/// range (rotate 0/90/180/270), pictures left at the defaults dropped.
fn clean_adjust(pictures: &Value) -> Result<Value, String> {
    let mut out = serde_json::Map::new();
    for (key, look) in pictures.as_object().ok_or("looks: not an object")? {
        if key.parse::<u32>().map_or(true, |n| n == 0) {
            return Err(format!("looks: picture '{key}'"));
        }
        let mut fields = serde_json::Map::new();
        for (name, lo, hi) in ADJUST_FIELDS {
            let Some(v) = look.get(*name) else { continue };
            let x = v.as_f64().filter(|x| (*lo..=*hi).contains(x)).ok_or(format!("looks: picture {key}: {name}"))?;
            let neutral = if *name == "contrast" { 1.0 } else { 0.0 };
            if x != neutral {
                fields.insert(name.to_string(), Value::from(x));
            }
        }
        if let Some(v) = look.get("rotate") {
            let r = v.as_u64().filter(|r| [0, 90, 180, 270].contains(r)).ok_or(format!("looks: picture {key}: rotate"))?;
            if r != 0 {
                fields.insert("rotate".into(), Value::from(r));
            }
        }
        if !fields.is_empty() {
            out.insert(key.clone(), Value::Object(fields));
        }
    }
    Ok(Value::Object(out))
}

fn read_adjust(prefix: &str) -> Result<Value, String> {
    let Ok(text) = std::fs::read_to_string(adjust_file(prefix)) else { return Ok(Value::Null) };
    serde_json::from_str(&text).map_err(|e| format!("{prefix}.adjust.json: {e}"))
}

/// <prefix>.adjust.json as stored: PakonX looks and turns ("pictures",
/// read by pakon-cli --adjust), OEM-look tones ("oem", applied by PakonX
/// to the OEM JPEGs) and the framing shift.
struct AdjustDoc {
    pictures: Value,
    oem: Value,
    shift: i64,
    bw: bool,
    ice: &'static str,
    format: &'static str,
}

fn load_doc(prefix: &str) -> Result<AdjustDoc, String> {
    let doc = read_adjust(prefix)?;
    let part = |k: &str| match doc.get(k) {
        Some(p) => clean_adjust(p),
        None => Ok(Value::Object(Default::default())),
    };
    Ok(AdjustDoc { pictures: part("pictures")?, oem: without_turns(part("oem")?), shift: roll_shift(prefix), bw: roll_bw(prefix), ice: roll_ice(prefix), format: roll_format(prefix) })
}

/// OEM tones carry no turn: turns live with the pictures, one per picture.
fn without_turns(mut looks: Value) -> Value {
    if let Some(m) = looks.as_object_mut() {
        for l in m.values_mut() {
            if let Some(o) = l.as_object_mut() {
                o.remove("rotate");
            }
        }
        m.retain(|_, l| l.as_object().is_some_and(|o| !o.is_empty()));
    }
    looks
}

/// Writes <prefix>.adjust.json beside the scan; with nothing left in it
/// it removes it.
fn write_adjust(prefix: &str, d: &AdjustDoc) -> Result<(), String> {
    let path = adjust_file(prefix);
    let empty = |v: &Value| v.as_object().is_some_and(|m| m.is_empty());
    if empty(&d.pictures) && empty(&d.oem) && d.shift == 0 && !d.bw && d.ice == "off" && d.format == "auto" {
        let _ = std::fs::remove_file(&path);
        return Ok(());
    }
    let mut doc = serde_json::json!({ "schema": "pakon-adjust/1", "pictures": d.pictures });
    if !empty(&d.oem) {
        doc["oem"] = d.oem.clone();
    }
    if d.shift != 0 {
        doc["shift"] = Value::from(d.shift);
    }
    if d.bw {
        doc["film"] = Value::from("bw");
    }
    if d.ice != "off" {
        doc["ice"] = Value::from(d.ice);
    }
    if d.format != "auto" {
        doc["format"] = Value::from(d.format);
    }
    let tmp = path.with_extension("json.part");
    std::fs::write(&tmp, serde_json::to_string_pretty(&doc).map_err(|e| e.to_string())?)
        .map_err(|e| format!("{}: {e}", tmp.display()))?;
    std::fs::rename(&tmp, &path).map_err(|e| format!("{}: {e}", path.display()))
}

/// The OEM-look tones of a roll ({} when none), for saving OEM JPEGs.
pub(crate) fn oem_tones(prefix: &str) -> Value {
    load_doc(prefix).map(|d| d.oem).unwrap_or(Value::Null)
}

/// The roll's looks: PakonX looks and turns, OEM-look tones, the shift.
#[tauri::command]
pub fn load_adjust(prefix: String) -> Result<Value, String> {
    roll_files(&prefix)?;
    let d = load_doc(&prefix)?;
    Ok(serde_json::json!({ "pictures": d.pictures, "oem": d.oem, "shift": d.shift, "bw": d.bw, "ice": d.ice, "format": d.format }))
}

/// Saves the looks (both developments), keeping the roll's shift.
#[tauri::command]
pub fn save_adjust(prefix: String, pictures: Value, oem: Option<Value>) -> Result<(), String> {
    roll_files(&prefix)?;
    let mut d = load_doc(&prefix)?;
    d.pictures = clean_adjust(&pictures)?;
    if let Some(o) = oem {
        d.oem = without_turns(clean_adjust(&o)?);
    }
    write_adjust(&prefix, &d)
}

/// Film presets: a PakonX look per film, keyed by the DX code the scan
/// read ("39/1" = product / specifier), applied when a roll of that film
/// opens without a look of its own. Stored for this user, not per roll:
/// <app config>/dx-presets.json.
fn presets_file(app: &tauri::AppHandle) -> Result<PathBuf, String> {
    Ok(app.path().app_config_dir().map_err(|e| e.to_string())?.join("dx-presets.json"))
}

/// Only "P/S" keys (DX product 0..999, specifier 0..99) and the tone fields
/// of a look (no rotation); a neutral look drops its film.
fn clean_presets(presets: &Value) -> Result<Value, String> {
    let mut out = serde_json::Map::new();
    for (key, look) in presets.as_object().ok_or("presets: not an object")? {
        let ok = key.split_once('/').is_some_and(|(p, s)| {
            p.parse::<u16>().is_ok_and(|p| p <= 999) && s.parse::<u8>().is_ok_and(|s| s <= 99)
        });
        if !ok {
            return Err(format!("presets: '{key}' is not a DX code"));
        }
        let one = without_turns(clean_adjust(&serde_json::json!({ "1": look }))?);
        if let Some(tone) = one.get("1") {
            out.insert(key.clone(), tone.clone());
        }
    }
    Ok(Value::Object(out))
}

#[tauri::command]
pub fn load_presets(app: tauri::AppHandle) -> Result<Value, String> {
    let Ok(text) = std::fs::read_to_string(presets_file(&app)?) else { return Ok(serde_json::json!({})) };
    // A damaged file loses no rolls: start again from no presets.
    Ok(serde_json::from_str::<Value>(&text).ok().and_then(|v| clean_presets(&v).ok()).unwrap_or(serde_json::json!({})))
}

#[tauri::command]
pub fn save_presets(app: tauri::AppHandle, presets: Value) -> Result<(), String> {
    let clean = clean_presets(&presets)?;
    let path = presets_file(&app)?;
    if let Some(dir) = path.parent() {
        std::fs::create_dir_all(dir).map_err(|e| format!("{}: {e}", dir.display()))?;
    }
    std::fs::write(&path, clean.to_string()).map_err(|e| format!("{}: {e}", path.display()))
}

/// Sets the roll's film: black-and-white or colour, keeping the looks.
#[tauri::command]
pub fn save_film(prefix: String, bw: bool) -> Result<(), String> {
    roll_files(&prefix)?;
    let mut d = load_doc(&prefix)?;
    d.bw = bw;
    write_adjust(&prefix, &d)
}

/// Sets the roll's infrared dust removal (off, light, normal, strong),
/// keeping the looks. Each strength develops into a cache of its own.
#[tauri::command]
pub fn save_ice(prefix: String, ice: String) -> Result<(), String> {
    roll_files(&prefix)?;
    let mut d = load_doc(&prefix)?;
    d.ice = ICE.iter().copied().find(|k| *k == ice).ok_or_else(|| format!("unknown dust removal '{ice}'"))?;
    write_adjust(&prefix, &d)
}

/// Sets the roll's frame format (full, half, auto), keeping the looks.
#[tauri::command]
pub fn save_frame_format(prefix: String, format: String) -> Result<(), String> {
    roll_files(&prefix)?;
    let mut d = load_doc(&prefix)?;
    d.format = FORMATS.iter().copied().find(|k| *k == format).ok_or_else(|| format!("unknown frame format '{format}'"))?;
    write_adjust(&prefix, &d)
}

/// Saves the roll's framing shift (film lines), keeping the looks.
#[tauri::command]
pub fn save_shift(prefix: String, shift: i64) -> Result<(), String> {
    roll_files(&prefix)?;
    if shift.abs() > MAX_SHIFT {
        return Err(format!("shift {shift}: wants -{MAX_SHIFT}..{MAX_SHIFT} lines"));
    }
    let mut d = load_doc(&prefix)?;
    d.shift = shift;
    write_adjust(&prefix, &d)
}

pub(crate) fn fnv1a(s: &str) -> u64 {
    s.bytes().fold(0xcbf2_9ce4_8422_2325, |h, b| (h ^ u64::from(b)).wrapping_mul(0x0100_0000_01b3))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn ice_comes_from_the_rolls_adjust_file_and_only_known_strengths_pass() {
        let dir = std::env::temp_dir().join(format!("pakonx-ice-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let prefix = dir.join("r").display().to_string();
        assert_eq!(roll_ice(&prefix), "off");
        std::fs::write(adjust_file(&prefix), r#"{"pictures":{},"ice":"normal"}"#).unwrap();
        assert_eq!(roll_ice(&prefix), "normal");
        let a = frames_args(&prefix, Path::new("C:/cache/x"), false, 0, false);
        assert_eq!(&a[a.len() - 2..], ["--ice", "normal"]);
        std::fs::write(adjust_file(&prefix), r#"{"ice":"normal --save"}"#).unwrap();
        assert_eq!(roll_ice(&prefix), "off");
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn open_roll_runs_frames_with_fixed_flags_only() {
        let a = frames_args("C:/scans/strip8dx", Path::new("C:/cache/x"), true, 0, false);
        assert_eq!(a[0], "frames");
        assert_eq!(a[1], "C:/scans/strip8dx");
        assert!(a.iter().skip(2).all(|s| s == "--preview" || s == "--out-dir" || s == "C:/cache/x" || s == "--tail-first"));
        assert!(!a.iter().any(|s| s.contains("live")));
        let s = frames_args("C:/scans/strip8dx", Path::new("C:/cache/x"), false, -120, true);
        assert_eq!(&s[5..], ["--shift", "-120", "--bw"]);
    }

    #[test]
    fn save_runs_frames_with_fixed_flags_only() {
        let jpeg = SaveSpec { format: "jpeg", quality: 92, dx_names: true, adjust: None, shift: 0, bw: false, naming: None };
        let a = save_args("C:/scans/strip8dx", Path::new("D:/out"), false, &jpeg, &[1, 4, 5]);
        assert_eq!(a, ["frames", "C:/scans/strip8dx", "--save", "jpeg", "--quality", "92", "--out-dir", "D:/out",
                       "--dx-names", "--pictures", "1,4,5"]);
        let tiff = SaveSpec { format: "tiff", quality: 92, dx_names: false, adjust: Some(Path::new("C:/scans/strip8dx.adjust.json")), shift: 0, bw: false, naming: None };
        let all = save_args("C:/scans/strip8dx", Path::new("D:/out"), true, &tiff, &[]);
        assert_eq!(all, ["frames", "C:/scans/strip8dx", "--save", "tiff", "--quality", "92", "--out-dir", "D:/out",
                         "--adjust", "C:/scans/strip8dx.adjust.json", "--tail-first"]);
    }

    #[test]
    fn custom_names_and_the_shift_reach_the_save() {
        let named = SaveSpec { format: "jpeg", quality: 92, dx_names: true, adjust: None, shift: 40, bw: true,
                               naming: Some(Naming { base: "Trip ", start: 7, digits: 2 }) };
        let a = save_args("C:/scans/r", Path::new("D:/out"), false, &named, &[2]);
        assert_eq!(a, ["frames", "C:/scans/r", "--save", "jpeg", "--quality", "92", "--out-dir", "D:/out",
                       "--name", "Trip ", "--start", "7", "--digits", "2", "--pictures", "2", "--shift", "40", "--bw"]);
        assert_eq!(numbered(&Naming { base: "Trip_", start: 1, digits: 3 }, 12), "Trip_012");
        for ok in ["Trip_", "Lisbon 2026 - ", "a(1)"] {
            assert!(check_naming(&Naming { base: ok, start: 1, digits: 3 }).is_ok(), "{ok}");
        }
        for bad in ["", "-x", "a/b", "a\\b", "..\\up", "x:", "Vilnius\u{e4}", "a*"] {
            assert!(check_naming(&Naming { base: bad, start: 1, digits: 3 }).is_err(), "{bad}");
        }
        assert!(check_naming(&Naming { base: "a", start: 1, digits: 0 }).is_err());
        assert!(check_naming(&Naming { base: "a", start: 1_000_000, digits: 3 }).is_err());
    }

    #[test]
    fn looks_are_cleaned_before_they_are_written() {
        let v = serde_json::json!({
            "4": { "exposure": 0.5, "red": 0.0, "contrast": 1.0, "extra": 7 },
            "5": { "exposure": 0.0 }
        });
        assert_eq!(clean_adjust(&v).unwrap(), serde_json::json!({ "4": { "exposure": 0.5 } }));
        let r = serde_json::json!({ "2": { "rotate": 90 }, "3": { "rotate": 0 } });
        assert_eq!(clean_adjust(&r).unwrap(), serde_json::json!({ "2": { "rotate": 90 } }));
        assert!(clean_adjust(&serde_json::json!({ "2": { "rotate": 45 } })).is_err());
        for bad in [
            serde_json::json!({ "0": {} }),
            serde_json::json!({ "x": {} }),
            serde_json::json!({ "1": { "exposure": 9 } }),
            serde_json::json!({ "1": { "contrast": "hi" } }),
            serde_json::json!([1]),
        ] {
            assert!(clean_adjust(&bad).is_err(), "{bad}");
        }
    }

    #[test]
    fn progress_counts_listed_and_written_pictures() {
        let out = "frames: base8, 20892 lines\n  frame  1: lines 6810..9060 (2250)  [nice]  DX 19A\n  \
                   blank:   lines 9060..9100\n  frame  2: lines 9100..11300 (2200)  [nice]\n\
                   written: c/x.frames.json\nwritten: c/x.frame01.preview.bmp (+ .thumb.bmp)\nwritten: d/x-20A.jpg\n";
        let (mut total, mut done) = (0, 0);
        for l in out.lines() {
            count_progress("open", l, &mut total, &mut done);
        }
        assert_eq!((total, done), (2, 2));
        let look = "  frame  1: lines 1..2 (1)  [nice]\n  frame  2: lines 3..4 (1)  [nice]\nPISaveFilePlanar_8 -> 00000000 (62 ms)\n\
                    written: c/x.look/look01.jpg\nwritten: c/x.look/look02.jpg\n";
        let (mut total, mut done) = (0, 0);
        for l in look.lines() {
            count_progress("look", l, &mut total, &mut done);
        }
        assert_eq!((total, done), (2, 1));
    }

    #[test]
    fn cached_previews_are_used_only_when_complete_and_fresh() {
        let dir = std::env::temp_dir().join(format!("pakonx-cache-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let scan = dir.join("r.film.pakraw");
        std::fs::write(&scan, b"raw").unwrap();
        std::thread::sleep(std::time::Duration::from_millis(20));
        std::fs::write(dir.join("r.frames.json"),
                       r#"{"frames":[{"picture":1},{"picture":null},{"picture":2}]}"#).unwrap();
        for n in ["01", "02"] {
            std::fs::write(dir.join(format!("r.frame{n}.preview.bmp")), b"b").unwrap();
        }
        std::fs::write(dir.join("r.frame01.thumb.bmp"), b"b").unwrap();
        let inputs = [scan.clone()];
        assert!(cached_frames(&dir, "r", &inputs).is_none(), "thumb 02 missing");
        std::fs::write(dir.join("r.frame02.thumb.bmp"), b"b").unwrap();
        assert!(cached_frames(&dir, "r", &inputs).is_some());
        std::thread::sleep(std::time::Duration::from_millis(20));
        std::fs::write(&scan, b"rescanned").unwrap();
        assert!(cached_frames(&dir, "r", &inputs).is_none(), "scan newer than the previews");
        std::fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn save_refuses_unknown_formats() {
        assert!(save_format("jpeg").is_ok() && save_format("tiff").is_ok());
        for f in ["png", "", "--live-scan", "jpeg --live-scan"] {
            assert!(save_format(f).is_err(), "{f}");
        }
    }

    #[test]
    fn the_scan_is_kept_unless_every_picture_was_saved() {
        let dir = std::env::temp_dir().join(format!("pakonx-saved-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let good = dir.join("r.frame01.jpg");
        let empty = dir.join("r.frame02.jpg");
        std::fs::write(&good, b"jpeg").unwrap();
        std::fs::write(&empty, b"").unwrap();
        let s = |p: &Path| p.display().to_string();
        assert!(check_saved(&[]).is_err());
        assert!(check_saved(&[s(&good)]).is_ok());
        assert!(check_saved(&[s(&good), s(&empty)]).is_err());
        assert!(check_saved(&[s(&good), s(&dir.join("missing.jpg"))]).is_err());
        std::fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn scan_files_are_the_scan_outputs_only() {
        let dir = std::env::temp_dir().join(format!("pakonx-scan-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        for f in ["r.film.pakraw", "r.scan.json", "r.film.tiff", "r.frame01.jpg", "r.console.txt"] {
            std::fs::write(dir.join(f), b"x").unwrap();
        }
        let prefix = dir.join("r").display().to_string();
        let mut names: Vec<String> = scan_files_of(&prefix)
            .into_iter()
            .map(|f| Path::new(&f.path).file_name().unwrap().to_str().unwrap().to_string())
            .collect();
        names.sort();
        assert_eq!(names, ["r.film.pakraw", "r.film.tiff", "r.scan.json"]); // no .adjust.json here
        std::fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn a_roll_path_cannot_be_an_option() {
        assert!(roll_files("--live-scan").is_err());
        assert!(roll_files("").is_err());
        assert!(roll_files("/no/such/roll").is_err());
    }
    #[test]
    fn presets_keep_dx_keys_and_tones_only() {
        let p = clean_presets(&serde_json::json!({
            "39/1": { "exposure": -0.3, "rotate": 90, "contrast": 1.0 },
            "96/2": { "contrast": 1.0 }
        }))
        .unwrap();
        assert_eq!(p, serde_json::json!({ "39/1": { "exposure": -0.3 } }));
        assert!(clean_presets(&serde_json::json!({ "x/1": {} })).is_err());
        assert!(clean_presets(&serde_json::json!({ "39/1": { "exposure": 9 } })).is_err());
    }

}
