// RPCS3 Slim: machine-readable mode used by RPCS3's Game Update Downloader dialog.
// Every stdout line is one JSON object with an "event" field:
//   title     {title_id, title, platform, packages: [{index, id, version, size, system_ver, file_name}]}
//   error     {title_id, message}                       (update query failed)
//   start     {title_id, index, id, size}               (download of one package begins)
//   progress  {title_id, index, downloaded, total}
//   verifying {title_id, index}
//   finished  {title_id, index, path}
//   failed    {title_id, index, message}
//   done      {}

use std::collections::HashMap;
use std::io::Write;
use std::path::PathBuf;
use std::time::{Duration, Instant};

use serde_json::{json, Value};
use tokio::runtime::Runtime;

use crate::psn::*;
use crate::utils::create_new_pkg_path;

fn emit(value: Value) {
    let mut out = std::io::stdout().lock();
    let _ = writeln!(out, "{value}");
    let _ = out.flush();
}

fn update_error_message(e: &UpdateError) -> String {
    match e {
        UpdateError::UnhandledErrorResponse(e) => format!("PSN returned an unexpected error: {e}"),
        UpdateError::InvalidSerial => String::from("The serial didn't give any results, double-check your input."),
        UpdateError::NoUpdatesAvailable => String::from("No updates are available for this title."),
        UpdateError::Reqwest(e) => format!("Request failed: {e}"),
        UpdateError::XmlParsing(e) => format!("Error parsing the response from PSN: {e}"),
        UpdateError::XmlEncodingError(e) => format!("Error parsing the response from PSN: {e}"),
        UpdateError::ManifestParsing(e) => format!("Error parsing the manifest from PSN: {e}"),
    }
}

fn download_error_message(e: &DownloadError) -> String {
    match e {
        DownloadError::HashMismatch(true) => {
            String::from("Hash mismatch: the server sent less data than expected. Sony's servers can be unreliable, try again later.")
        }
        DownloadError::HashMismatch(false) => String::from("Hash mismatch on the downloaded file."),
        DownloadError::Tokio(e) => format!("File error: {e}"),
        DownloadError::Reqwest(e) => format!("Download failed: {e}"),
    }
}

// Selections look like "BLUS30443:0,2". Titles without a selection download everything.
fn parse_selection(selection: &[String]) -> HashMap<String, Vec<usize>> {
    let mut map = HashMap::new();

    for entry in selection.iter().flat_map(|s| s.split_whitespace()) {
        if let Some((serial, indices)) = entry.split_once(':') {
            let indices = indices.split(',').filter_map(|i| i.trim().parse::<usize>().ok()).collect();
            map.insert(parse_title_id(serial), indices);
        }
    }

    map
}

pub fn run(titles: Vec<String>, destination_path: PathBuf, list_only: bool, selection: Vec<String>) {
    let runtime = Runtime::new().unwrap();
    let selection = parse_selection(&selection);

    runtime.block_on(async move {
        let serials: Vec<String> = titles.iter().flat_map(|t| t.split_whitespace()).map(parse_title_id).collect();

        let queries: Vec<_> = serials
            .iter()
            .map(|serial| (serial.clone(), tokio::spawn(UpdateInfo::get_info(serial.clone()))))
            .collect();

        let mut updates = Vec::new();

        for (serial, query) in queries {
            match query.await {
                Ok(Ok(info)) => {
                    let packages: Vec<Value> = info
                        .packages
                        .iter()
                        .enumerate()
                        .map(|(index, pkg)| {
                            json!({
                                "index": index,
                                "id": pkg.id(),
                                "version": pkg.version,
                                "size": pkg.size,
                                "system_ver": pkg.system_ver,
                                "file_name": pkg.file_name().unwrap_or_default(),
                            })
                        })
                        .collect();

                    emit(json!({
                        "event": "title",
                        "title_id": info.title_id,
                        "title": info.title(),
                        "platform": info.platform_variant.to_string(),
                        "packages": packages,
                    }));

                    updates.push(info);
                }
                Ok(Err(e)) => {
                    error!("Update query for {serial} failed: {e:?}");
                    emit(json!({ "event": "error", "title_id": serial, "message": update_error_message(&e) }));
                }
                Err(e) => {
                    emit(json!({ "event": "error", "title_id": serial, "message": e.to_string() }));
                }
            }
        }

        if !list_only {
            for update in updates {
                let title = {
                    let title = update.title();
                    if title.is_empty() { String::from("Untitled") } else { title }
                };

                let wanted = selection.get(&update.title_id);

                for (index, pkg) in update.packages.iter().enumerate() {
                    if wanted.is_some_and(|w| !w.contains(&index)) {
                        continue;
                    }

                    download(&update.title_id, &title, index, pkg, &destination_path).await;
                }
            }
        }

        emit(json!({ "event": "done" }));
    });
}

async fn download(serial: &str, title: &str, index: usize, pkg: &PackageInfo, destination_path: &PathBuf) {
    emit(json!({ "event": "start", "title_id": serial, "index": index, "id": pkg.id(), "size": pkg.size }));

    let (tx, mut rx) = tokio::sync::mpsc::channel(64);
    let task = {
        let pkg = pkg.clone();
        let destination_path = destination_path.clone();
        let serial = serial.to_string();
        let title = title.to_string();
        tokio::spawn(async move { pkg.start_download(tx, destination_path, serial, title).await })
    };

    let mut downloaded = 0u64;
    let mut last_report = Instant::now() - Duration::from_secs(1);

    // The channel closes once start_download returns
    while let Some(status) = rx.recv().await {
        match status {
            DownloadStatus::Progress(bytes) => {
                downloaded += bytes;

                if last_report.elapsed() >= Duration::from_millis(150) {
                    last_report = Instant::now();
                    emit(json!({ "event": "progress", "title_id": serial, "index": index, "downloaded": downloaded, "total": pkg.size }));
                }
            }
            DownloadStatus::Verifying => {
                if downloaded > 0 {
                    emit(json!({ "event": "progress", "title_id": serial, "index": index, "downloaded": downloaded, "total": pkg.size }));
                }
                emit(json!({ "event": "verifying", "title_id": serial, "index": index }));
            }
            DownloadStatus::DownloadSuccess | DownloadStatus::DownloadFailure => {}
        }
    }

    match task.await {
        Ok(Ok(())) => {
            let file_name = pkg.file_name().unwrap_or_else(|| String::from("update.pkg"));
            let path = create_new_pkg_path(destination_path, serial, title).join(file_name);
            emit(json!({ "event": "finished", "title_id": serial, "index": index, "path": path.to_string_lossy() }));
        }
        Ok(Err(e)) => {
            error!("Download of {serial} {} failed: {e:?}", pkg.id());
            emit(json!({ "event": "failed", "title_id": serial, "index": index, "message": download_error_message(&e) }));
        }
        Err(e) => {
            emit(json!({ "event": "failed", "title_id": serial, "index": index, "message": e.to_string() }));
        }
    }
}
