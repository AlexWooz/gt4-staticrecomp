//! The launcher's log files (TASK-112, release 1.3.0; owner design 2026-10-23).
//!
//! - One file per launcher session: `logs\launcher-YYYYMMDD-HHMMSS.log` (local time; `-2`, `-3`... after the stamp when
//!   two starts fall in the same second). Every line has a level: `INFO`, `WARN` or `ERROR`.
//! - The last [`KEPT_SESSIONS`] session logs are kept; the oldest go when a new session starts. The running session's
//!   file is never removed.
//! - `logs\latest.log` is the one file a player attaches to an issue: a header, the `ERROR` lines of the earlier kept
//!   sessions (oldest first, each prefixed with its file name), then the current session's lines as they are written.
//! - A 1.2.x `launcher.log` is renamed once to `launcher-legacy-YYYYMMDD.log` and counts as one of the kept logs.
//! - Game session folders (`data\sessions\session-*`) are capped the same way ([`prune_sessions`]).
//!
//! The rules are pure file-name and file-text functions, tested here. The launcher's writer (`gt4l-tauri`) owns the
//! open session and calls them.

use std::fs;
use std::io;
use std::path::{Path, PathBuf};
use std::time::SystemTime;

/// How many launcher session logs (and game session folders) are kept.
pub const KEPT_SESSIONS: usize = 20;
/// The file a player attaches to an issue.
pub const LATEST_NAME: &str = "latest.log";
/// The 1.2.x log, renamed once (see [`migrate_legacy`]).
pub const LEGACY_NAME: &str = "launcher.log";
const SESSION_PREFIX: &str = "launcher-";
const LEGACY_PREFIX: &str = "launcher-legacy-";
const LOG_EXTENSION: &str = ".log";

/// The level of a line. Real failures are `Error`, soft problems `Warn`, everything else `Info`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Level {
    Info,
    Warn,
    Error,
}

impl Level {
    /// The word on each line.
    pub fn label(self) -> &'static str {
        match self {
            Level::Info => "INFO",
            Level::Warn => "WARN",
            Level::Error => "ERROR",
        }
    }
}

/// A local date and time, as the launcher's clock reads it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct LocalTime {
    pub year: u32,
    pub month: u32,
    pub day: u32,
    pub hour: u32,
    pub minute: u32,
    pub second: u32,
}

impl LocalTime {
    /// `2026-10-23 21:03:40`.
    pub fn stamp(&self) -> String {
        format!("{:04}-{:02}-{:02} {:02}:{:02}:{:02}", self.year, self.month, self.day, self.hour, self.minute, self.second)
    }

    /// `20261023-210340`.
    pub fn file_stamp(&self) -> String {
        format!("{:04}{:02}{:02}-{:02}{:02}{:02}", self.year, self.month, self.day, self.hour, self.minute, self.second)
    }

    /// `20261023`.
    pub fn date_stamp(&self) -> String {
        format!("{:04}{:02}{:02}", self.year, self.month, self.day)
    }
}

/// One line of a log: `[2026-10-23 21:03:40] ERROR <message>`.
pub fn line(time: &LocalTime, level: Level, message: &str) -> String {
    format!("[{}] {} {message}", time.stamp(), level.label())
}

/// The name of a new session log: `launcher-YYYYMMDD-HHMMSS.log`, or with `-2`, `-3`... before `.log` when `taken` says the
/// name is already there.
pub fn session_file_name(time: &LocalTime, taken: impl Fn(&str) -> bool) -> String {
    let base = format!("{SESSION_PREFIX}{}", time.file_stamp());
    let first = format!("{base}{LOG_EXTENSION}");
    if !taken(&first) {
        return first;
    }
    (2..).map(|counter| format!("{base}-{counter}{LOG_EXTENSION}")).find(|name| !taken(name.as_str())).unwrap_or(first)
}

/// True for a launcher log that counts toward the kept logs: a session log or the legacy one. `latest.log` is not one.
pub fn is_session_log(name: &str) -> bool {
    name.starts_with(SESSION_PREFIX) && name.ends_with(LOG_EXTENSION)
}

/// The name a 1.2.x `launcher.log` gets when it is moved: `launcher-legacy-YYYYMMDD.log` (the date of its last change),
/// with a counter when that name is taken.
pub fn legacy_file_name(date: &str, taken: impl Fn(&str) -> bool) -> String {
    let first = format!("{LEGACY_PREFIX}{date}{LOG_EXTENSION}");
    if !taken(&first) {
        return first;
    }
    (2..)
        .map(|counter| format!("{LEGACY_PREFIX}{date}-{counter}{LOG_EXTENSION}"))
        .find(|name| !taken(name.as_str()))
        .unwrap_or(first)
}

/// Renames `logs\launcher.log` (a 1.2.x file) to `launcher-legacy-<date>.log` once. `date` is the date of the file's last
/// change (the caller reads the clock and the file). Returns the new name, or `None` when there is no such file.
pub fn migrate_legacy(dir: &Path, date: &str) -> io::Result<Option<String>> {
    let legacy = dir.join(LEGACY_NAME);
    if !legacy.is_file() {
        return Ok(None);
    }
    let name = legacy_file_name(date, |name| dir.join(name).exists());
    fs::rename(&legacy, dir.join(&name))?;
    Ok(Some(name))
}

/// The session logs of `dir` (not `latest.log`), oldest first by their last change.
pub fn session_logs(dir: &Path) -> io::Result<Vec<PathBuf>> {
    let mut found: Vec<(SystemTime, PathBuf)> = fs::read_dir(dir)?
        .filter_map(|entry| entry.ok())
        .map(|entry| entry.path())
        .filter(|path| path.is_file())
        .filter(|path| path.file_name().and_then(|name| name.to_str()).is_some_and(is_session_log))
        .map(|path| (modified(&path), path))
        .collect();
    found.sort();
    Ok(found.into_iter().map(|(_, path)| path).collect())
}

/// Deletes the oldest session logs of `dir` until `keep` remain (the running session's file counts toward `keep` but is
/// never deleted). Returns the names removed.
pub fn prune_logs(dir: &Path, keep: usize, running: &Path) -> io::Result<Vec<String>> {
    prune_oldest(session_logs(dir)?, keep, running)
}

/// Deletes the oldest game session folders (`session-*`) of `sessions` until `keep` remain; `running` is never deleted.
/// Returns the names removed.
pub fn prune_sessions(sessions: &Path, keep: usize, running: &Path) -> io::Result<Vec<String>> {
    if !sessions.is_dir() {
        return Ok(Vec::new());
    }
    let mut found: Vec<(SystemTime, PathBuf)> = fs::read_dir(sessions)?
        .filter_map(|entry| entry.ok())
        .map(|entry| entry.path())
        .filter(|path| path.is_dir())
        .filter(|path| path.file_name().and_then(|name| name.to_str()).is_some_and(|name| name.starts_with("session-")))
        .map(|path| (modified(&path), path))
        .collect();
    found.sort();
    prune_oldest(found.into_iter().map(|(_, path)| path).collect(), keep, running)
}

/// Removes the oldest of `oldest_first` while more than `keep` remain; `running` stays. Returns the names removed.
fn prune_oldest(oldest_first: Vec<PathBuf>, keep: usize, running: &Path) -> io::Result<Vec<String>> {
    let mut remaining = oldest_first.len();
    let mut removed = Vec::new();
    for path in oldest_first {
        if remaining <= keep {
            break;
        }
        if path == running {
            continue;
        }
        if path.is_dir() {
            fs::remove_dir_all(&path)?;
        } else {
            fs::remove_file(&path)?;
        }
        removed.push(path.file_name().map(|name| name.to_string_lossy().into_owned()).unwrap_or_default());
        remaining -= 1;
    }
    Ok(removed)
}

/// The `ERROR` lines of the kept session logs other than `running`, oldest first. Each is prefixed with its file name.
pub fn earlier_errors(dir: &Path, running: &Path) -> io::Result<Vec<String>> {
    let mut lines = Vec::new();
    for path in session_logs(dir)? {
        if path == running {
            continue;
        }
        let name = path.file_name().map(|name| name.to_string_lossy().into_owned()).unwrap_or_default();
        let text = fs::read_to_string(&path).unwrap_or_default();
        for line in text.lines().filter(|line| line.contains(" ERROR ")) {
            lines.push(format!("{name}: {line}"));
        }
    }
    Ok(lines)
}

/// The text `latest.log` starts with: the header lines, then the errors of the earlier sessions. The current session's
/// lines follow as they are written.
pub fn latest_header(header: &[String], earlier: &[String]) -> String {
    let mut text = String::new();
    for line in header {
        text.push_str(line);
        text.push('\n');
    }
    text.push('\n');
    text.push_str("== Errors from earlier sessions ==\n");
    if earlier.is_empty() {
        text.push_str("(none)\n");
    }
    for line in earlier {
        text.push_str(line);
        text.push('\n');
    }
    text.push('\n');
    text.push_str("== This session ==\n");
    text
}

fn modified(path: &Path) -> SystemTime {
    fs::metadata(path).and_then(|metadata| metadata.modified()).unwrap_or(SystemTime::UNIX_EPOCH)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::time::{Duration, SystemTime};

    const TIME: LocalTime = LocalTime { year: 2026, month: 10, day: 23, hour: 21, minute: 3, second: 40 };

    fn scratch(name: &str) -> PathBuf {
        let path = std::env::temp_dir().join(format!("gt4l-logs-{name}-{}", std::process::id()));
        let _ = fs::remove_dir_all(&path);
        fs::create_dir_all(&path).unwrap();
        path
    }

    /// Writes `path` and gives it a last-change time `age_seconds` before `base` (older files have larger ages).
    fn file_aged(path: &Path, text: &str, base: SystemTime, age_seconds: u64) {
        fs::write(path, text).unwrap();
        let time = base - Duration::from_secs(age_seconds);
        fs::File::options().write(true).open(path).unwrap().set_modified(time).unwrap();
    }

    #[test]
    fn a_line_has_the_time_the_level_and_the_text() {
        assert_eq!(line(&TIME, Level::Error, "game crashed: exit 1"), "[2026-10-23 21:03:40] ERROR game crashed: exit 1");
        assert_eq!(line(&TIME, Level::Info, "x"), "[2026-10-23 21:03:40] INFO x");
        assert_eq!(Level::Warn.label(), "WARN");
    }

    #[test]
    fn session_names_use_local_time_and_a_counter_for_the_same_second() {
        assert_eq!(session_file_name(&TIME, |_| false), "launcher-20261023-210340.log");
        let taken = |name: &str| name == "launcher-20261023-210340.log";
        assert_eq!(session_file_name(&TIME, taken), "launcher-20261023-210340-2.log");
        let taken_twice = |name: &str| name == "launcher-20261023-210340.log" || name == "launcher-20261023-210340-2.log";
        assert_eq!(session_file_name(&TIME, taken_twice), "launcher-20261023-210340-3.log");
        assert!(is_session_log("launcher-20261023-210340.log"));
        assert!(is_session_log("launcher-legacy-20261021.log"), "the legacy file counts");
        assert!(!is_session_log("latest.log"), "latest.log is not a session log");
        assert!(!is_session_log("launcher.log"), "the 1.2.x name is migrated, not counted as a session");
    }

    #[test]
    fn the_legacy_log_is_renamed_once_and_counts_as_one_kept_log() {
        let dir = scratch("legacy");
        fs::write(dir.join("launcher.log"), "[1] old line\n").unwrap();
        let moved = migrate_legacy(&dir, "20261021").unwrap();
        assert_eq!(moved.as_deref(), Some("launcher-legacy-20261021.log"));
        assert!(!dir.join("launcher.log").exists());
        assert_eq!(fs::read_to_string(dir.join("launcher-legacy-20261021.log")).unwrap(), "[1] old line\n");
        // A second run finds nothing to move.
        assert_eq!(migrate_legacy(&dir, "20261021").unwrap(), None);
        assert_eq!(legacy_file_name("20261021", |name| name == "launcher-legacy-20261021.log"), "launcher-legacy-20261021-2.log");
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn the_oldest_session_logs_go_when_more_than_twenty_exist() {
        let dir = scratch("rotate");
        let base = SystemTime::now();
        // 21 older logs plus the running one: ages 1..=21 seconds, and the running file is the newest.
        for age in 1..=21u64 {
            file_aged(&dir.join(format!("launcher-old-{age:02}.log")), "x", base, age * 10);
        }
        let running = dir.join("launcher-running.log");
        file_aged(&running, "x", base, 0);
        fs::write(dir.join("latest.log"), "x").unwrap();
        // A new session starts: keep 19 others plus the new one = 20.
        let removed = prune_logs(&dir, KEPT_SESSIONS - 1, &dir.join("launcher-new.log")).unwrap();
        assert_eq!(removed.len(), 3, "22 candidates down to 19 kept: {removed:?}");
        assert_eq!(removed[0], "launcher-old-21.log", "the oldest goes first");
        assert!(dir.join("latest.log").exists(), "latest.log is never a session log");
        let left = session_logs(&dir).unwrap();
        assert_eq!(left.len(), 19, "{left:?}");
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn the_running_session_is_never_removed() {
        let dir = scratch("running");
        let base = SystemTime::now();
        let running = dir.join("launcher-running.log");
        file_aged(&running, "x", base, 500);
        for age in 1..=3u64 {
            file_aged(&dir.join(format!("launcher-b-{age}.log")), "x", base, age);
        }
        // Keep 1: the running file is the oldest, but it stays; the others go.
        let removed = prune_logs(&dir, 1, &running).unwrap();
        assert_eq!(removed.len(), 3);
        assert!(running.exists());
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn latest_collects_only_the_errors_of_the_earlier_sessions_oldest_first() {
        let dir = scratch("earlier");
        let base = SystemTime::now();
        file_aged(
            &dir.join("launcher-20261020-090000.log"),
            "[2026-10-20 09:00:00] INFO started\n[2026-10-20 09:01:00] ERROR game crashed: exit 1\n",
            base,
            3600,
        );
        file_aged(
            &dir.join("launcher-20261021-090000.log"),
            "[2026-10-21 09:00:00] WARN soft problem\n[2026-10-21 09:05:00] ERROR GPU lost\n",
            base,
            60,
        );
        let running = dir.join("launcher-20261023-210340.log");
        file_aged(&running, "[2026-10-23 21:03:40] ERROR this session is not earlier\n", base, 0);
        let earlier = earlier_errors(&dir, &running).unwrap();
        assert_eq!(
            earlier,
            vec![
                "launcher-20261020-090000.log: [2026-10-20 09:01:00] ERROR game crashed: exit 1".to_string(),
                "launcher-20261021-090000.log: [2026-10-21 09:05:00] ERROR GPU lost".to_string(),
            ]
        );
        let text = latest_header(&["GT4Recomp 1.3.0".to_string(), "Install: portable".to_string()], &earlier);
        assert!(text.starts_with("GT4Recomp 1.3.0\nInstall: portable\n"));
        assert!(text.contains("== Errors from earlier sessions ==\nlauncher-20261020-090000.log: [2026-10-20 09:01:00] ERROR"));
        assert!(!text.contains("this session is not earlier"), "the running file is not an earlier session");
        assert!(text.ends_with("== This session ==\n"));
        let none = latest_header(&[], &[]);
        assert!(none.contains("(none)"));
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn the_game_session_folders_are_capped_and_the_running_one_stays() {
        let sessions = scratch("sessions");
        for age in 1..=22u64 {
            // Folder times are not set here (Windows cannot open a folder for a time change); the test checks counts only.
            let folder = sessions.join(format!("session-{age:02}"));
            fs::create_dir_all(&folder).unwrap();
        }
        fs::create_dir_all(sessions.join("other")).unwrap();
        let running = sessions.join("session-running");
        fs::create_dir_all(&running).unwrap();
        let removed = prune_sessions(&sessions, KEPT_SESSIONS, &running).unwrap();
        assert_eq!(removed.len(), 3, "23 session folders down to 20: {removed:?}");
        assert!(running.is_dir());
        assert!(sessions.join("other").is_dir(), "a folder that is not a game session is left alone");
        let _ = fs::remove_dir_all(&sessions);
    }
}
