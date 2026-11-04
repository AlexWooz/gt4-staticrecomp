//! The launcher's session log (TASK-112, release 1.3.0): one file per launcher start, `latest.log` for an issue report.
//!
//! [`start`] runs once at launch: it renames a 1.2.x `launcher.log`, keeps the last 20 session logs, writes `latest.log`
//! with the header and the `ERROR` lines of the earlier sessions, and opens this session's file. Every later line goes to
//! the session file and to `latest.log`. The rules (names, rotation, the error section) are in `gt4l_core::logs`.

use std::fs::{self, OpenOptions};
use std::io::Write;
use std::path::{Path, PathBuf};
use std::sync::{Mutex, MutexGuard};

use gt4l_core::logs::{self, Level, KEPT_SESSIONS, LATEST_NAME, LEGACY_NAME};
use gt4l_core::paths::DataDirs;

/// The open session: its own file and `latest.log`.
struct Session {
    file: PathBuf,
    latest: PathBuf,
}

static SESSION: Mutex<Option<Session>> = Mutex::new(None);

fn session() -> MutexGuard<'static, Option<Session>> {
    match SESSION.lock() {
        Ok(guard) => guard,
        Err(poisoned) => poisoned.into_inner(),
    }
}

/// Opens this launch's session. `header` is written at the top of the session file and of `latest.log` (version, install
/// kind, Windows version, date). Returns the session file. Errors are returned, not logged: nothing is open yet.
pub fn start(dirs: &DataDirs, header: &[String]) -> Result<PathBuf, String> {
    let dir = dirs.logs();
    fs::create_dir_all(&dir).map_err(|error| format!("{}: {error}", dir.display()))?;
    let now = gt4l_platform::system::local_time();
    let legacy = dir.join(LEGACY_NAME);
    if legacy.is_file() {
        let changed = fs::metadata(&legacy).and_then(|metadata| metadata.modified()).unwrap_or(std::time::SystemTime::now());
        let date = gt4l_platform::system::utc_time_of(changed).date_stamp();
        logs::migrate_legacy(&dir, &date).map_err(|error| format!("{}: {error}", legacy.display()))?;
    }
    // Room for this session: the oldest logs go until KEPT_SESSIONS - 1 remain (the new file makes KEPT_SESSIONS).
    let placeholder = dir.join("launcher-new.log");
    logs::prune_logs(&dir, KEPT_SESSIONS - 1, &placeholder).map_err(|error| format!("{}: {error}", dir.display()))?;
    let name = logs::session_file_name(&now, |name| dir.join(name).exists());
    let file = dir.join(&name);
    let latest = dir.join(LATEST_NAME);
    let earlier = logs::earlier_errors(&dir, &file).map_err(|error| format!("{}: {error}", dir.display()))?;
    let head = logs::latest_header(header, &earlier);
    fs::write(&latest, head).map_err(|error| format!("{}: {error}", latest.display()))?;
    let mut session_head = header.join("\n");
    session_head.push('\n');
    fs::write(&file, session_head).map_err(|error| format!("{}: {error}", file.display()))?;
    *session() = Some(Session { file: file.clone(), latest });
    Ok(file)
}

/// Appends one line at `level`. Before [`start`] the line goes to the 1.2.x `launcher.log` of the data folder.
pub fn write(dirs: &DataDirs, level: Level, message: &str) {
    let text = format!("{}\n", logs::line(&gt4l_platform::system::local_time(), level, message));
    let guard = session();
    match guard.as_ref() {
        Some(open) => {
            append(&open.file, &text);
            append(&open.latest, &text);
        }
        None => {
            let _ = fs::create_dir_all(dirs.logs());
            append(&dirs.logs().join(LEGACY_NAME), &text);
        }
    }
}

/// `latest.log` of the data folder: the file a player attaches to an issue.
pub fn latest_path(dirs: &DataDirs) -> PathBuf {
    dirs.logs().join(LATEST_NAME)
}

fn append(path: &Path, text: &str) {
    if let Ok(mut file) = OpenOptions::new().create(true).append(true).open(path) {
        let _ = file.write_all(text.as_bytes());
    }
}
