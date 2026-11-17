// Loose pictures: JPEG, TIFF and BMP files the user picks (saved frames, the
// OEM's JPEGs, anything else), shown on the light table beside rolls.
//
// The web view decodes neither 16-bit nor TIFF, so each picture gets an
// 8-bit JPEG preview (long side <= 1600) and thumbnail (<= 240) in the
// app cache, the only place the asset protocol may read. No pakon-cli, no
// scanner: the files are only read.
use image::codecs::jpeg::JpegEncoder;
use image::{ColorType, DynamicImage, ImageReader};
use serde::Serialize;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::Mutex;
use std::time::UNIX_EPOCH;
use tauri::Manager;

const PREVIEW_SIDE: u32 = 1600;
const THUMB_SIDE: u32 = 240;

#[derive(Serialize, Clone)]
pub struct Picture {
    path: String,
    name: String,
    format: &'static str,
    width: u32,
    height: u32,
    depth: String,
    bytes: u64,
    preview: String,
    thumb: String,
}

#[derive(Serialize)]
pub struct PictureSet {
    name: String,
    pictures: Vec<Picture>,
    /// "<path>: <why>" for the files that could not be opened.
    skipped: Vec<String>,
}

/// JPEG, TIFF or BMP by extension; None for anything else.
fn picture_format(path: &Path) -> Option<&'static str> {
    let ext = path.extension()?.to_str()?.to_ascii_lowercase();
    match ext.as_str() {
        "jpg" | "jpeg" => Some("JPEG"),
        "tif" | "tiff" => Some("TIFF"),
        "bmp" => Some("BMP"),
        _ => None,
    }
}

fn depth(c: ColorType) -> String {
    let (bits, kind) = match c {
        ColorType::L8 => (8, "grey"),
        ColorType::La8 => (8, "grey + alpha"),
        ColorType::Rgb8 => (8, "RGB"),
        ColorType::Rgba8 => (8, "RGBA"),
        ColorType::L16 => (16, "grey"),
        ColorType::La16 => (16, "grey + alpha"),
        ColorType::Rgb16 => (16, "RGB"),
        ColorType::Rgba16 => (16, "RGBA"),
        ColorType::Rgb32F => (32, "RGB float"),
        ColorType::Rgba32F => (32, "RGBA float"),
        _ => return format!("{c:?}"),
    };
    format!("{bits}-bit {kind}")
}

fn save_jpeg(img: &DynamicImage, path: &Path) -> Result<(), String> {
    let file = std::fs::File::create(path).map_err(|e| format!("{}: {e}", path.display()))?;
    let mut w = std::io::BufWriter::new(file);
    img.to_rgb8()
        .write_with_encoder(JpegEncoder::new_with_quality(&mut w, 90))
        .map_err(|e| format!("{}: {e}", path.display()))
}

/// The picture's cache folder, keyed by path, size and modification time.
fn picture_cache(path: &Path, meta: &std::fs::Metadata, cache_root: &Path) -> PathBuf {
    let modified = meta
        .modified()
        .ok()
        .and_then(|t| t.duration_since(UNIX_EPOCH).ok())
        .map(|d| d.as_secs())
        .unwrap_or(0);
    let key = crate::roll::fnv1a(&format!("{}|{}|{modified}", path.display(), meta.len()));
    cache_root.join(format!("{key:016x}"))
}

/// The picture at full size where the web view can show it: JPEG and BMP
/// files copied as they are, TIFF re-encoded as an 8-bit JPEG (quality 95).
fn full_one(path: &Path, cache_root: &Path) -> Result<PathBuf, String> {
    let format = picture_format(path).ok_or("not a JPEG, TIFF or BMP file")?;
    let meta = std::fs::metadata(path).map_err(|e| e.to_string())?;
    let dir = picture_cache(path, &meta, cache_root);
    std::fs::create_dir_all(&dir).map_err(|e| format!("{}: {e}", dir.display()))?;
    let full = dir.join(if format == "BMP" { "full.bmp" } else { "full.jpg" });
    if full.is_file() {
        return Ok(full);
    }
    // Written under a temporary name, so a half-made file is never served.
    let part = dir.join("full.part");
    if format == "TIFF" {
        let img = ImageReader::open(path).map_err(|e| e.to_string())?.decode().map_err(|e| e.to_string())?;
        let file = std::fs::File::create(&part).map_err(|e| e.to_string())?;
        img.to_rgb8()
            .write_with_encoder(JpegEncoder::new_with_quality(std::io::BufWriter::new(file), 95))
            .map_err(|e| e.to_string())?;
    } else {
        std::fs::copy(path, &part).map_err(|e| e.to_string())?;
    }
    std::fs::rename(&part, &full).map_err(|e| e.to_string())?;
    Ok(full)
}

#[tauri::command]
pub async fn full_picture_file(app: tauri::AppHandle, path: String) -> Result<String, String> {
    let cache_root = app.path().app_cache_dir().map_err(|e| e.to_string())?.join("rolls").join("pictures");
    tauri::async_runtime::spawn_blocking(move || full_one(Path::new(&path), &cache_root).map(|p| p.display().to_string()))
        .await
        .map_err(|e| e.to_string())?
}

/// Decodes one picture and writes its preview and thumbnail into
/// `cache_root/<key>/`, keyed by path, size and modification time so an
/// unchanged file is decoded once.
fn open_one(path: &Path, cache_root: &Path) -> Result<Picture, String> {
    let format = picture_format(path).ok_or("not a JPEG, TIFF or BMP file")?;
    let meta = std::fs::metadata(path).map_err(|e| e.to_string())?;
    if !meta.is_file() {
        return Err("not a file".into());
    }
    let path_s = path.display().to_string();
    let dir = picture_cache(path, &meta, cache_root);
    let preview = dir.join("preview.jpg");
    let thumb = dir.join("thumb.jpg");
    let info = dir.join("info.txt");
    // info.txt: "<width> <height> <depth>", written last.
    let cached = std::fs::read_to_string(&info).ok().and_then(|t| {
        let mut it = t.splitn(3, ' ');
        let w = it.next()?.parse().ok()?;
        let h = it.next()?.parse().ok()?;
        Some((w, h, it.next()?.trim().to_string()))
    });
    let (width, height, depth) = match cached {
        Some(c) if preview.is_file() && thumb.is_file() => c,
        _ => {
            let img = ImageReader::open(path)
                .map_err(|e| e.to_string())?
                .with_guessed_format()
                .map_err(|e| e.to_string())?
                .decode()
                .map_err(|e| e.to_string())?;
            let (w, h, d) = (img.width(), img.height(), depth(img.color()));
            std::fs::create_dir_all(&dir).map_err(|e| format!("{}: {e}", dir.display()))?;
            let p = img.thumbnail(PREVIEW_SIDE, PREVIEW_SIDE);
            drop(img);
            save_jpeg(&p, &preview)?;
            save_jpeg(&p.thumbnail(THUMB_SIDE, THUMB_SIDE), &thumb)?;
            std::fs::write(&info, format!("{w} {h} {d}")).map_err(|e| e.to_string())?;
            (w, h, d)
        }
    };
    Ok(Picture {
        name: path.file_name().and_then(|f| f.to_str()).unwrap_or(&path_s).to_string(),
        path: path_s,
        format,
        width,
        height,
        depth,
        bytes: meta.len(),
        preview: preview.display().to_string(),
        thumb: thumb.display().to_string(),
    })
}

/// Opens the picked files as one set, in the order given; files that
/// cannot be read are listed in `skipped`, not fatal.
#[tauri::command]
pub async fn open_pictures(app: tauri::AppHandle, paths: Vec<String>) -> Result<PictureSet, String> {
    if paths.is_empty() {
        return Err("no pictures chosen".into());
    }
    let cache_root = app
        .path()
        .app_cache_dir()
        .map_err(|e| e.to_string())?
        .join("rolls")
        .join("pictures");
    let paths: Vec<PathBuf> = paths.into_iter().map(PathBuf::from).collect();
    let name = common_folder(&paths);
    tauri::async_runtime::spawn_blocking(move || {
        // Big TIFFs take a while each: decode a few at once.
        let results: Vec<Mutex<Option<Result<Picture, String>>>> =
            paths.iter().map(|_| Mutex::new(None)).collect();
        let next = AtomicUsize::new(0);
        let workers = std::thread::available_parallelism().map_or(2, |n| n.get()).min(4);
        std::thread::scope(|s| {
            for _ in 0..workers {
                s.spawn(|| loop {
                    let i = next.fetch_add(1, Ordering::Relaxed);
                    let Some(p) = paths.get(i) else { break };
                    let r = open_one(p, &cache_root);
                    *results[i].lock().unwrap() = Some(r);
                });
            }
        });
        let mut pictures = Vec::new();
        let mut skipped = Vec::new();
        for (p, r) in paths.iter().zip(results) {
            match r.into_inner().unwrap() {
                Some(Ok(pic)) => pictures.push(pic),
                Some(Err(e)) => skipped.push(format!("{}: {e}", p.display())),
                None => skipped.push(format!("{}: not read", p.display())),
            }
        }
        Ok(PictureSet { name, pictures, skipped })
    })
    .await
    .map_err(|e| e.to_string())?
}

/// The folder all pictures share, else "Pictures".
fn common_folder(paths: &[PathBuf]) -> String {
    let first = paths.first().and_then(|p| p.parent());
    if paths.iter().all(|p| p.parent() == first) {
        if let Some(n) = first.and_then(|d| d.file_name()).and_then(|n| n.to_str()) {
            return n.to_string();
        }
    }
    "Pictures".to_string()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn only_jpeg_tiff_and_bmp_are_pictures() {
        assert_eq!(picture_format(Path::new("a/roll1.frame04.JPG")), Some("JPEG"));
        assert_eq!(picture_format(Path::new("a/x.jpeg")), Some("JPEG"));
        assert_eq!(picture_format(Path::new("a/x.pos.tiff")), Some("TIFF"));
        assert_eq!(picture_format(Path::new("a/x.TIF")), Some("TIFF"));
        assert_eq!(picture_format(Path::new("a/x.frame01.preview.BMP")), Some("BMP"));
        for p in ["a/x.png", "a/x.pakraw", "a/x.scan.json", "a/jpg"] {
            assert_eq!(picture_format(Path::new(p)), None, "{p}");
        }
    }

    #[test]
    fn a_16_bit_tiff_opens_with_an_8_bit_preview() {
        let dir = std::env::temp_dir().join(format!("pakonx-pictures-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let src = dir.join("wide.tiff");
        let img = image::ImageBuffer::<image::Rgb<u16>, _>::from_fn(3200, 800, |x, y| {
            image::Rgb([(x * 20) as u16, (y * 80) as u16, 40000])
        });
        DynamicImage::ImageRgb16(img).save(&src).unwrap();
        let pic = open_one(&src, &dir.join("cache")).unwrap();
        assert_eq!((pic.format, pic.width, pic.height), ("TIFF", 3200, 800));
        assert_eq!(pic.depth, "16-bit RGB");
        let preview = image::open(&pic.preview).unwrap();
        assert_eq!((preview.width(), preview.height()), (1600, 400));
        let thumb = image::open(&pic.thumb).unwrap();
        assert_eq!((thumb.width(), thumb.height()), (240, 60));
        // Second open comes from the cache.
        let again = open_one(&src, &dir.join("cache")).unwrap();
        assert_eq!(again.depth, "16-bit RGB");
        assert!(open_one(&dir.join("missing.jpg"), &dir.join("cache")).is_err());
        let full = full_one(&src, &dir.join("cache")).unwrap();
        assert_eq!(image::open(&full).unwrap().width(), 3200);
        assert_eq!(full_one(&src, &dir.join("cache")).unwrap(), full);
        let bmp = dir.join("small.bmp");
        image::RgbImage::from_pixel(30, 50, image::Rgb([200, 100, 50])).save(&bmp).unwrap();
        let b = open_one(&bmp, &dir.join("cache")).unwrap();
        assert_eq!((b.format, b.width, b.height, b.depth.as_str()), ("BMP", 30, 50, "8-bit RGB"));
        std::fs::remove_dir_all(&dir).ok();
    }
}
