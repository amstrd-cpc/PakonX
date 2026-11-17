// The OEM "Pakon look" for a roll: `pakon-cli frames <prefix> --pakon-look`
// computes TLB's densities and runs the OEM's own PakonIMAu.dll through
// the 32-bit tools/oem_host, which writes <name>.look/lookNN.jpg
// (OEM_RE §13). Windows only.
//
// Safety: no scanner is involved; the DLL only processes pixels. The OEM
// and capture folders come from the user but are checked for the files
// the look needs; oem_host.exe is an executable, so its path is never
// taken from the web view: it is found here ($PAKON_OEM_HOST, else the
// app's bundled oem/, else beside pakon-cli, else beside PakonX).
//
// The installer carries all three (tauri.conf.json bundle.resources, from
// src-tauri/oem/, gitignored): oem/install (the OEM install copy),
// oem/capture (TLB's descriptors) and oem/oem_host.exe. Empty folders from
// the web view mean "the bundled ones".
use serde::Serialize;
use serde_json::Value;
use std::path::{Path, PathBuf};
use tauri::Manager;

use crate::roll::{check_naming, check_saved, modified, numbered, roll_cache, roll_files, roll_name, roll_shift, run_frames, Naming};

#[derive(Serialize)]
pub struct OemLook {
    /// Folder holding look01.jpg, look02.jpg, … (picture order).
    dir: String,
    frames: Value,
    log: String,
    cached: bool,
}

/// The app's bundled oem/ folder, when the installer (or the dev build)
/// put one there.
fn bundled(app: &tauri::AppHandle) -> Option<PathBuf> {
    let dir = crate::boot::plain_path(app.path().resource_dir().ok()?.join("oem"));
    dir.is_dir().then_some(dir)
}

/// Where oem_host.exe lives: $PAKON_OEM_HOST, else the bundled oem/, else
/// beside pakon-cli, else beside this executable.
fn locate_host(app: &tauri::AppHandle) -> Option<PathBuf> {
    if let Some(p) = std::env::var_os("PAKON_OEM_HOST") {
        return Some(PathBuf::from(p)).filter(|p| p.is_file());
    }
    if let Some(h) = bundled(app).map(|d| d.join("oem_host.exe")).filter(|p| p.is_file()) {
        return Some(h);
    }
    let beside = |exe: PathBuf| exe.parent().map(|d| d.join("oem_host.exe")).filter(|p| p.is_file());
    let cli = crate::cli::locate();
    beside(crate::boot::plain_path(std::fs::canonicalize(&cli).unwrap_or(cli)))
        .or_else(|| std::env::current_exe().ok().and_then(beside))
}

/// Folders PakonIMAu.dll needs that hold no files, so the bundle (which
/// skips empty folders) does not carry them; without them PIBegin fails
/// with -20.
const EMPTY_DIRS: &[&str] = &[
    "exif",
    "anselinstalldir/anselCacheDir",
    "anselinstalldir/dataPathItems/asea",
    "anselinstalldir/dataPathItems/ast",
    "anselinstalldir/dataPathItems/citras",
    "anselinstalldir/dataPathItems/area/tmp",
];

fn copy_tree(from: &Path, to: &Path) -> std::io::Result<()> {
    std::fs::create_dir_all(to)?;
    for e in std::fs::read_dir(from)? {
        let e = e?;
        let target = to.join(e.file_name());
        if e.file_type()?.is_dir() {
            copy_tree(&e.path(), &target)?;
        } else {
            std::fs::copy(e.path(), target)?;
        }
    }
    Ok(())
}

/// The bundled OEM install, copied once into the app's own data folder:
/// the DLL writes its logs and Ansel cache inside its install folder,
/// which an installed app (Program Files) may not write. Copied again
/// when the bundled DLL or PakonX's version changes.
fn staged_install(app: &tauri::AppHandle) -> Option<PathBuf> {
    let from = bundled(app)?.join("install");
    let dll = std::fs::metadata(from.join("PakonIMAu.dll")).ok()?;
    let to = crate::boot::plain_path(app.path().app_local_data_dir().ok()?.join("oem-install"));
    let stamp = format!("{} {}", env!("CARGO_PKG_VERSION"), dll.len());
    let marker = to.join(".pakonx-copy");
    if std::fs::read_to_string(&marker).ok().as_deref() != Some(stamp.as_str()) {
        let _ = std::fs::remove_dir_all(&to);
        copy_tree(&from, &to).ok()?;
        std::fs::write(&marker, &stamp).ok()?;
    }
    for d in EMPTY_DIRS {
        std::fs::create_dir_all(to.join(d)).ok()?;
    }
    Some(to)
}

/// The OEM and capture folders to use: the user's when set and holding
/// what the look needs, else the bundled oem/install and oem/capture (a
/// stale or wrong choice never blocks the look the app carries).
fn folders(app: &tauri::AppHandle, oem: &str, capture: &str) -> (String, String) {
    let b = bundled(app);
    let pick = |given: &str, sub: &str, need: &str| {
        let ours = if sub == "install" { staged_install(app) } else { b.as_ref().map(|d| d.join(sub)) };
        let ours = ours.filter(|d| d.join(need).is_file());
        if !given.is_empty() && (Path::new(given).join(need).is_file() || ours.is_none()) {
            return given.to_string();
        }
        ours.map(|d| d.display().to_string()).unwrap_or_default()
    };
    (pick(oem, "install", "PakonIMAu.dll"), pick(capture, "capture", "settings.bin"))
}

#[derive(Serialize)]
pub struct Bundled {
    oem: Option<String>,
    capture: Option<String>,
    host: bool,
}

/// What the app carries for the OEM look, for the settings panel.
#[tauri::command]
pub fn bundled_oem(app: tauri::AppHandle) -> Bundled {
    let b = bundled(&app);
    let sub = |s: &str, need: &str| b.as_ref().map(|d| d.join(s)).filter(|d| d.join(need).is_file()).map(|d| d.display().to_string());
    Bundled { oem: sub("install", "PakonIMAu.dll"), capture: sub("capture", "settings.bin"), host: locate_host(&app).is_some() }
}

/// The user's OEM inputs, checked: the OEM install copy (PakonIMAu.dll),
/// TLB's captured descriptors (settings.bin) and, for scans whose sidecar
/// lacks the unit's NegMatrix, an EEPROM dump.
pub struct LookInputs {
    oem: PathBuf,
    capture: PathBuf,
    eeprom: Option<PathBuf>,
    host: PathBuf,
}

fn look_inputs(oem: &str, capture: &str, eeprom: Option<&str>, host: Option<PathBuf>) -> Result<LookInputs, String> {
    let dir = |s: &str, need: &str, what: &str| -> Result<PathBuf, String> {
        let p = PathBuf::from(s);
        if s.starts_with('-') || !p.is_absolute() || !p.join(need).is_file() {
            return Err(format!("{what}: {s} has no {need}"));
        }
        Ok(p)
    };
    let oem = dir(oem, "PakonIMAu.dll", "OEM folder")?;
    let capture = dir(capture, "settings.bin", "Capture folder")?;
    let eeprom = match eeprom.filter(|s| !s.is_empty()) {
        Some(s) if !s.starts_with('-') && Path::new(s).is_absolute() && Path::new(s).is_file() => Some(PathBuf::from(s)),
        Some(s) => return Err(format!("EEPROM dump: {s} is not a file")),
        None => None,
    };
    let host = host.ok_or(
        "oem_host.exe not found: put it in src-tauri/oem/ (bundled), or beside pakon-cli, or set PAKON_OEM_HOST",
    )?;
    Ok(LookInputs { oem, capture, eeprom, host })
}

/// The pakon-cli arguments for the look: fixed flags and checked paths.
pub fn look_args(prefix: &str, out_dir: &Path, i: &LookInputs, shift: i64, bw: bool, raw: bool) -> Vec<String> {
    let mut a: Vec<String> = vec![
        "frames".into(),
        prefix.into(),
        "--pakon-look".into(),
        "--oem".into(),
        i.oem.display().to_string(),
        "--capture".into(),
        i.capture.display().to_string(),
        "--oem-host".into(),
        i.host.display().to_string(),
        "--out-dir".into(),
        out_dir.display().to_string(),
    ];
    if let Some(e) = &i.eeprom {
        a.push("--eeprom".into());
        a.push(e.display().to_string());
    }
    if shift != 0 {
        a.push("--shift".into());
        a.push(shift.to_string());
    }
    if bw {
        a.push("--bw".into());
    }
    if raw {
        a.push("--look-raw".into());
    }
    crate::roll::push_ice(&mut a, prefix);
    a
}

/// frames.json of an earlier look run when every picture's lookNN.jpg is
/// there and newer than all inputs.
fn cached_look(dir: &Path, name: &str, inputs: &[PathBuf]) -> Option<Value> {
    let json = dir.join(format!("{name}.frames.json"));
    let made = modified(&json)?;
    if inputs.iter().any(|p| modified(p).is_some_and(|m| m > made)) {
        return None;
    }
    let frames: Value = serde_json::from_str(&std::fs::read_to_string(&json).ok()?).ok()?;
    let looks = dir.join(format!("{name}.look"));
    for f in frames.get("frames")?.as_array()? {
        let Some(n) = f.get("picture").and_then(Value::as_u64) else { continue };
        if !looks.join(format!("look{n:02}.jpg")).is_file() {
            return None;
        }
    }
    Some(frames)
}

/// The OEM look of a roll, from the cache when it is current. `raw`: also
/// each picture's 8-bit planar RGB (lookNN.rgb, frames --look-raw), in a
/// folder of its own, for TIFF and other JPEG qualities.
async fn ensure_look_dir(app: &tauri::AppHandle, prefix: &str, i: &LookInputs, raw_rgb: bool) -> Result<OemLook, String> {
    let (side, raw) = roll_files(prefix)?;
    let name = roll_name(prefix)?;
    // One folder per set of OEM inputs, framing shift and film.
    let shift = roll_shift(prefix);
    let bw = crate::roll::roll_bw(prefix);
    // ICE joins the key only when on, so earlier caches stay valid.
    let ice = match crate::roll::roll_ice(prefix) { "off" => String::new(), k => format!("|{k}") };
    let ice = match crate::roll::roll_format(prefix) { "full" => ice, f => format!("{ice}|{f}") };
    let key = crate::roll::fnv1a(&format!("{}|{}|{:?}|{shift}|{bw}{ice}", i.oem.display(), i.capture.display(), i.eeprom));
    let dir = roll_cache(app, prefix, false)?.join(format!("look-{key:016x}{}", if raw_rgb { "-raw" } else { "" }));
    std::fs::create_dir_all(&dir).map_err(|e| format!("{}: {e}", dir.display()))?;
    let inputs = [raw, side, crate::cli::locate(), i.host.clone(), i.capture.join("settings.bin")];
    let looks = dir.join(format!("{name}.look"));
    let rgb_present = |frames: &Value| {
        frames.get("frames").and_then(Value::as_array).into_iter().flatten().all(|f| {
            f.get("picture").and_then(Value::as_u64).map_or(true, |n| looks.join(format!("look{n:02}.rgb")).is_file())
        })
    };
    if let Some(frames) = cached_look(&dir, &name, &inputs).filter(|f| !raw_rgb || rgb_present(f)) {
        return Ok(OemLook { dir: looks.display().to_string(), frames, log: "OEM look from the cache.".into(), cached: true });
    }
    let log = run_frames(app, "look", look_args(prefix, &dir, i, shift, bw, raw_rgb), None).await?;
    let frames = cached_look(&dir, &name, &[]).ok_or_else(|| format!("the OEM look did not write every picture\n{log}"))?;
    Ok(OemLook { dir: looks.display().to_string(), frames, log, cached: false })
}

async fn ensure_look(app: &tauri::AppHandle, prefix: &str, i: &LookInputs) -> Result<OemLook, String> {
    ensure_look_dir(app, prefix, i, false).await
}

#[tauri::command]
pub async fn open_look(app: tauri::AppHandle, prefix: String, oem: String, capture: String, eeprom: Option<String>) -> Result<OemLook, String> {
    let (oem, capture) = folders(&app, &oem, &capture);
    let i = look_inputs(&oem, &capture, eeprom.as_deref(), locate_host(&app))?;
    ensure_look(&app, &prefix, &i).await
}

/// The saved file's name, as frames --save: roll + film number
/// (<roll>_19A.jpg) when asked, by place on the roll when no DX number was
/// read (<roll>_05.jpg), else by picture number (<roll>.frameNN.jpg).
fn look_name(name: &str, frame: &Value, n: u64, dx_names: bool, taken: &[String]) -> String {
    if !dx_names {
        return format!("{name}.frame{n:02}.jpg");
    }
    match frame.get("dx_frame").and_then(Value::as_str) {
        Some(d) if !taken.contains(&format!("{name}_{d}.jpg")) => format!("{name}_{d}.jpg"),
        _ => format!("{name}_{n:02}.jpg"),
    }
}

/// The rotation of each picture from <prefix>.adjust.json (none: 0).
fn rotations(prefix: &str) -> Value {
    crate::roll::load_adjust(prefix.to_string()).ok().and_then(|v| v.get("pictures").cloned()).unwrap_or(Value::Null)
}

/// The look's curve for one 8-bit channel, as the light table shows it
/// (web/app.js lookTable, process/positive.hpp Adjust): in x = 1 +
/// log10(y)/R, x' = grey + (x - grey) * contrast + (exposure + balance)
/// * log10(2) / R. A function of the output value only, so it applies to
/// the OEM's JPEGs exactly as on screen. None when the look is neutral.
pub(crate) fn tone_lut(look: &Value) -> Option<[[u8; 256]; 3]> {
    const RANGE: f64 = 2.2; // PositiveOptions::range_decades
    let f = |k: &str, d: f64| look.get(k).and_then(Value::as_f64).unwrap_or(d);
    let (exposure, contrast) = (f("exposure", 0.0), f("contrast", 1.0));
    let balance = [f("red", 0.0), f("green", 0.0), f("blue", 0.0)];
    if exposure == 0.0 && contrast == 1.0 && balance == [0.0; 3] {
        return None;
    }
    let floor = 10f64.powf(-RANGE);
    let grey = 1.0 + (0.18 * (1.0 - floor) + floor).log10() / RANGE;
    let mut lut = [[0u8; 256]; 3];
    for (c, table) in lut.iter_mut().enumerate() {
        let shift = (exposure + balance[c]) * 2f64.log10() / RANGE;
        for (i, out) in table.iter_mut().enumerate() {
            let v = i as f64 / 255.0;
            let y = if v <= 0.04045 { v / 12.92 } else { ((v + 0.055) / 1.055).powf(2.4) };
            let x = 1.0 + (y * (1.0 - floor) + floor).log10() / RANGE;
            let x = (grey + (x - grey) * contrast + shift).clamp(0.0, 1.0);
            let y2 = ((10f64.powf(RANGE * (x - 1.0)) - floor) / (1.0 - floor)).clamp(0.0, 1.0);
            let s = if y2 <= 0.0031308 { 12.92 * y2 } else { 1.055 * y2.powf(1.0 / 2.4) - 0.055 };
            *out = (s * 255.0).round().clamp(0.0, 255.0) as u8;
        }
    }
    Some(lut)
}

/// The JPEG quality the OEM DLL saves with (PISaveFilePlanar_8, 0x5a).
const OEM_QUALITY: u8 = 90;

/// A picture of the OEM look from its 8-bit planar RGB (lookNN.rgb, the
/// size of lookNN.jpg), with the user's look, turned, written as JPEG at
/// `quality` or as an 8-bit TIFF: no second JPEG loss.
fn write_from_rgb(rgb: &Path, jpeg: &Path, to: &Path, rotate: u64, lut: Option<&[[u8; 256]; 3]>, tiff: bool, quality: u8) -> Result<(), String> {
    let (w, h) = image::image_dimensions(jpeg).map_err(|e| format!("{}: {e}", jpeg.display()))?;
    let planes = std::fs::read(rgb).map_err(|e| format!("{}: {e}", rgb.display()))?;
    let n = (w as usize) * (h as usize);
    if planes.len() < 3 * n {
        return Err(format!("{}: {} bytes, wants {}", rgb.display(), planes.len(), 3 * n));
    }
    let mut img = image::RgbImage::new(w, h);
    for (k, p) in img.pixels_mut().enumerate() {
        for c in 0..3 {
            let v = planes[c * n + k];
            p.0[c] = lut.map_or(v, |l| l[c][usize::from(v)]);
        }
    }
    let img = image::DynamicImage::ImageRgb8(img);
    let turned = match rotate {
        90 => img.rotate90(),
        180 => img.rotate180(),
        270 => img.rotate270(),
        _ => img,
    };
    let file = std::fs::File::create(to).map_err(|e| format!("{}: {e}", to.display()))?;
    let out = std::io::BufWriter::new(file);
    let r = if tiff {
        turned.to_rgb8().write_with_encoder(image::codecs::tiff::TiffEncoder::new(out))
    } else {
        turned.to_rgb8().write_with_encoder(image::codecs::jpeg::JpegEncoder::new_with_quality(out, quality))
    };
    r.map_err(|e| format!("{}: {e}", to.display()))
}

/// Writes the OEM JPEG with the user's look and turned clockwise
/// (re-encoded at quality 95), or copies it as it is when neither applies.
fn write_turned(from: &Path, to: &Path, rotate: u64, lut: Option<&[[u8; 256]; 3]>) -> Result<(), String> {
    if rotate == 0 && lut.is_none() {
        return std::fs::copy(from, to).map(|_| ()).map_err(|e| format!("{}: {e}", to.display()));
    }
    let mut img = image::open(from).map_err(|e| format!("{}: {e}", from.display()))?.to_rgb8();
    if let Some(l) = lut {
        for p in img.pixels_mut() {
            for c in 0..3 {
                p.0[c] = l[c][usize::from(p.0[c])];
            }
        }
    }
    let img = image::DynamicImage::ImageRgb8(img);
    let turned = match rotate {
        90 => img.rotate90(),
        180 => img.rotate180(),
        270 => img.rotate270(),
        _ => img,
    };
    let file = std::fs::File::create(to).map_err(|e| format!("{}: {e}", to.display()))?;
    let enc = image::codecs::jpeg::JpegEncoder::new_with_quality(std::io::BufWriter::new(file), 95);
    turned.to_rgb8().write_with_encoder(enc).map_err(|e| format!("{}: {e}", to.display()))
}

/// Copies the OEM look of the chosen pictures (all when empty) into
/// `dest` with the user's OEM-look tones and turns from the light table,
/// under the chosen names.
#[tauri::command]
#[allow(clippy::too_many_arguments)]
pub async fn save_look(
    app: tauri::AppHandle,
    prefix: String,
    oem: String,
    capture: String,
    eeprom: Option<String>,
    pictures: Vec<u32>,
    dest: String,
    dx_names: bool,
    name: Option<String>,
    start: u32,
    digits: u8,
    tail_first: bool,
    format: Option<String>,
    quality: Option<u8>,
) -> Result<crate::roll::Saved, String> {
    let tiff = match format.as_deref().unwrap_or("jpeg") {
        "jpeg" => false,
        "tiff" => true,
        f => return Err(format!("'{f}' is not a picture format (jpeg or tiff)")),
    };
    let quality = quality.unwrap_or(OEM_QUALITY);
    if !(1..=100).contains(&quality) {
        return Err(format!("JPEG quality {quality}: wants 1..100"));
    }
    let (oem, capture) = folders(&app, &oem, &capture);
    let i = look_inputs(&oem, &capture, eeprom.as_deref(), locate_host(&app))?;
    let naming = name.as_deref().map(|base| Naming { base, start, digits });
    if let Some(n) = &naming {
        check_naming(n)?;
    }
    let dest_path = PathBuf::from(&dest);
    if dest.starts_with('-') || !dest_path.is_absolute() || !dest_path.is_dir() {
        return Err(format!("{dest}: not a folder"));
    }
    let look = ensure_look(&app, &prefix, &i).await?;
    let name = roll_name(&prefix)?;
    let turns = rotations(&prefix);
    let tones = crate::roll::oem_tones(&prefix);
    // The OEM's own JPEG (quality 90) is copied as it is when nothing
    // changes it; TIFF, another quality, a look or a turn start from its
    // 8-bit RGB (one JPEG compression, not two).
    let chosen = |n: u64| pictures.is_empty() || pictures.contains(&(n as u32));
    let changed = tail_first
        || tones.as_object().is_some_and(|m| m.keys().filter_map(|k| k.parse::<u64>().ok()).any(chosen))
        || turns.as_object().is_some_and(|m| {
            m.iter().any(|(k, l)| k.parse::<u64>().is_ok_and(chosen) && l.get("rotate").and_then(Value::as_u64).unwrap_or(0) != 0)
        });
    let from_rgb = tiff || quality != OEM_QUALITY || changed;
    let rgb = if from_rgb { Some(ensure_look_dir(&app, &prefix, &i, true).await?) } else { None };
    let mut written = Vec::new();
    let mut names = Vec::new();
    for f in look.frames.get("frames").and_then(Value::as_array).into_iter().flatten() {
        let Some(n) = f.get("picture").and_then(Value::as_u64) else { continue };
        if !pictures.is_empty() && !pictures.contains(&(n as u32)) {
            continue;
        }
        let file = match &naming {
            Some(nm) => format!("{}.jpg", numbered(nm, n)),
            None => look_name(&name, f, n, dx_names, &names),
        };
        let file = if tiff { format!("{}.tif", file.trim_end_matches(".jpg")) } else { file };
        let to = dest_path.join(&file);
        // Roll 180° (tail-first feed) is a half turn of every picture.
        let own = turns.get(n.to_string()).and_then(|l| l.get("rotate")).and_then(Value::as_u64).unwrap_or(0);
        let rotate = (own + if tail_first { 180 } else { 0 }) % 360;
        let lut = tones.get(n.to_string()).and_then(tone_lut);
        match &rgb {
            Some(r) => {
                let d = Path::new(&r.dir);
                write_from_rgb(&d.join(format!("look{n:02}.rgb")), &d.join(format!("look{n:02}.jpg")), &to, rotate, lut.as_ref(), tiff, quality)?
            }
            None => write_turned(&Path::new(&look.dir).join(format!("look{n:02}.jpg")), &to, rotate, lut.as_ref())?,
        }
        names.push(file);
        written.push(to.display().to_string());
    }
    check_saved(&written)?;
    let log = written.iter().map(|w| format!("written: {w}\n")).collect();
    Ok(crate::roll::Saved::new(written, log))
}

/// One OEM look picture for the 100 % view, turned as on the light table:
/// the JPEG itself when not turned, else a turned copy made once beside it.
/// `dir` must be a look folder in this app's cache.
#[tauri::command]
pub fn oem_full_picture(app: tauri::AppHandle, dir: String, picture: u32, rotate: u16) -> Result<String, String> {
    if picture == 0 || ![0, 90, 180, 270].contains(&rotate) {
        return Err("picture from 1, rotate 0/90/180/270".into());
    }
    let cache = crate::boot::plain_path(app.path().app_cache_dir().map_err(|e| e.to_string())?);
    let looks = crate::boot::plain_path(PathBuf::from(&dir));
    if !looks.starts_with(&cache) || looks.components().any(|c| c == std::path::Component::ParentDir) {
        return Err(format!("{dir}: not an OEM look folder of this app"));
    }
    let src = looks.join(format!("look{picture:02}.jpg"));
    if !src.is_file() {
        return Err(format!("{}: missing", src.display()));
    }
    if rotate == 0 {
        return Ok(src.display().to_string());
    }
    let out = looks.join(format!("r{rotate}"));
    std::fs::create_dir_all(&out).map_err(|e| format!("{}: {e}", out.display()))?;
    let to = out.join(format!("look{picture:02}.jpg"));
    if !crate::roll::modified(&to).is_some_and(|t| crate::roll::modified(&src).is_some_and(|s| t >= s)) {
        // The look is laid over on screen, as on the previews.
        write_turned(&src, &to, u64::from(rotate), None)?;
    }
    Ok(to.display().to_string())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_look_runs_with_checked_paths_and_a_located_host_only() {
        let dir = std::env::temp_dir().join(format!("pakonx-look-{}", std::process::id()));
        let (oem, cap) = (dir.join("oem"), dir.join("cap"));
        std::fs::create_dir_all(&oem).unwrap();
        std::fs::create_dir_all(&cap).unwrap();
        let s = |p: &Path| p.display().to_string();
        let host = Some(dir.join("oem_host.exe"));
        assert!(look_inputs(&s(&oem), &s(&cap), None, host.clone()).is_err(), "no PakonIMAu.dll yet");
        std::fs::write(oem.join("PakonIMAu.dll"), b"x").unwrap();
        assert!(look_inputs(&s(&oem), &s(&cap), None, host.clone()).is_err(), "no settings.bin yet");
        std::fs::write(cap.join("settings.bin"), b"x").unwrap();
        assert!(look_inputs(&s(&oem), &s(&cap), None, None).is_err(), "no host");
        assert!(look_inputs(&s(&oem), &s(&cap), Some("--live-scan"), host.clone()).is_err());
        assert!(look_inputs("--oem", &s(&cap), None, host.clone()).is_err());
        let i = look_inputs(&s(&oem), &s(&cap), Some(""), host).unwrap();
        let a = look_args("C:/scans/r", Path::new("C:/cache/l"), &i, 0, false, false);
        assert_eq!(&a[..3], ["frames", "C:/scans/r", "--pakon-look"]);
        assert!(!a.iter().any(|x| x.contains("live") || x == "--eeprom"));
        std::fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn the_saved_tone_curve_is_the_light_tables() {
        assert!(tone_lut(&serde_json::json!({})).is_none());
        assert!(tone_lut(&serde_json::json!({ "contrast": 1.0 })).is_none());
        let lut = tone_lut(&serde_json::json!({ "exposure": 1.0 })).unwrap();
        // Brighter, black and white kept at the ends, monotonic, same in R/G/B.
        assert!(lut[0][128] > 128 && lut[0][255] == 255 && lut[0] == lut[2]);
        assert!(lut[0].windows(2).all(|w| w[0] <= w[1]));
        let red = tone_lut(&serde_json::json!({ "red": 0.5 })).unwrap();
        assert!(red[0][100] > 100 && red[1][100] == 100 && red[2][100] == 100);
        // The same values as web/app.js lookTable (computed there with node).
        assert_eq!((lut[1][128], lut[1][40]), (177, 62));
        assert_eq!(tone_lut(&serde_json::json!({ "red": 0.5, "contrast": 1.3 })).unwrap()[0][200], 255);
        assert_eq!(tone_lut(&serde_json::json!({ "blue": -0.7, "exposure": -0.4 })).unwrap()[2][90], 59);
    }

    #[test]
    fn saved_looks_are_named_like_frames_save() {
        let f = serde_json::json!({ "picture": 3, "dx_frame": "21A" });
        assert_eq!(look_name("1111", &f, 3, true, &[]), "1111_21A.jpg");
        assert_eq!(look_name("1111", &f, 3, false, &[]), "1111.frame03.jpg");
        assert_eq!(look_name("1111", &f, 3, true, &["1111_21A.jpg".into()]), "1111_03.jpg");
        assert_eq!(look_name("1111", &serde_json::json!({ "picture": 4, "dx_frame": null }), 4, true, &[]), "1111_04.jpg");
    }
}
