// PakonX desktop shell. The scanner stack stays in C++: this process boots
// a cold scanner with the frozen reference script (boot.rs) and otherwise only
// runs pakon-cli (read-only commands, `frames` on scans on disk, and the
// film pass from the scan window, scan.rs), hands its output to the web
// UI, and reads picture files the user opens.
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod boot;
mod cli;
mod look;
mod nlp;
mod pictures;
mod roll;
mod scan;
mod updater;

fn main() {
    tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .plugin(tauri_plugin_updater::Builder::new().build())
        .manage(roll::Running::default())
        .manage(scan::Scanning::default())
        .manage(boot::Starting::default())
        .invoke_handler(tauri::generate_handler![
            cli::run_cli,
            cli::cli_path,
            cli::platform,
            cli::open_link,
            roll::list_rolls,
            roll::open_roll,
            roll::save_pictures,
            roll::cancel_frames,
            roll::full_picture,
            roll::load_adjust,
            look::open_look,
            look::save_look,
            look::bundled_oem,
            look::oem_full_picture,
            roll::roll_info,
            roll::save_adjust,
            roll::save_shift,
            roll::save_film,
            roll::load_presets,
            roll::save_presets,
            roll::save_ice,
            roll::save_frame_format,
            pictures::full_picture_file,
            roll::scan_files,
            roll::delete_scan,
            nlp::nlp_status,
            nlp::open_in_nlp,
            pictures::open_pictures,
            scan::start_scan,
            scan::drive_film,
            scan::stop_scan,
            scan::new_order,
            scan::default_order_base,
            boot::init_scanner,
            updater::check_update,
            updater::install_update
        ])
        .run(tauri::generate_context!())
        .expect("error while running PakonX");
}
