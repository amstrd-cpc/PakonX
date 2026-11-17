// Updates from GitHub releases. The endpoint (tauri.conf.json,
// plugins.updater, latest.json of the newest release) names the newest
// installer and its signature; the plugin
// installs only an installer signed with the key whose public half is in
// tauri.conf.json (the private half stays outside the repository,
// pakondev/updater-key). Installing runs the NSIS installer in passive mode,
// which closes PakonX and starts the new version.
use serde::Serialize;
use tauri::Emitter;
use tauri_plugin_updater::UpdaterExt;

#[derive(Serialize)]
pub struct Available {
    version: String,
    notes: String,
}

#[derive(Serialize, Clone)]
struct Progress {
    done: u64,
    total: u64,
}

/// The newer version the site offers, or None when this one is the newest.
#[tauri::command]
pub async fn check_update(app: tauri::AppHandle) -> Result<Option<Available>, String> {
    let update = app.updater().map_err(|e| e.to_string())?.check().await.map_err(|e| format!("Update check failed: {e}"))?;
    Ok(update.map(|u| Available { version: u.version, notes: u.body.unwrap_or_default() }))
}

/// Downloads the update ("update-progress" events) and installs it.
#[tauri::command]
pub async fn install_update(app: tauri::AppHandle) -> Result<(), String> {
    let update = app.updater().map_err(|e| e.to_string())?.check().await.map_err(|e| format!("Update check failed: {e}"))?;
    let Some(update) = update else { return Err("PakonX is up to date.".into()) };
    let mut done = 0u64;
    update
        .download_and_install(
            |chunk, total| {
                done += chunk as u64;
                let _ = app.emit("update-progress", Progress { done, total: total.unwrap_or(0) });
            },
            || {},
        )
        .await
        .map_err(|e| format!("Update not installed: {e}"))?;
    app.restart();
}
