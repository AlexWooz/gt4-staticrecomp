//! Starting, watching and stopping one game session. The launch line itself comes from
//! `gt4l_core::launch`; this module only runs it and turns the result into a classified outcome.

use std::cell::Cell;
use std::fs::{self, File, OpenOptions};
use std::io::{BufRead, BufReader, Write};
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, SystemTime, UNIX_EPOCH};

use gt4l_core::display::DisplayTarget;
use gt4l_core::exit::{self, Outcome};
use gt4l_core::gs_release::Installed;
use gt4l_core::gs_table;
use gt4l_core::launch::{self, LaunchInputs, LaunchPlan, Pcsx2Paths, ShaderPrepareLine};
use gt4l_core::lock::GameLock;
use gt4l_core::textures::TextureLocation;
use gt4l_core::paths::DataDirs;
use gt4l_core::shader_prepare::{self, AttemptReport, GuardSnapshot, PrepareOutcome};
use gt4l_core::Settings;

#[cfg(windows)]
const CREATE_NO_WINDOW: u32 = 0x0800_0000;
/// Sessions kept in the sessions folder; older ones are removed when a new one starts.
pub const KEEP_SESSIONS: usize = 20;

/// A running game: the child process (shared with the stop button) and where its log goes.
pub struct Running {
    pub child: Arc<Mutex<Option<Child>>>,
    pub session_dir: PathBuf,
    pub log_path: PathBuf,
    /// Held for the whole session so that a second launch cannot share the card (dropped with `Running`).
    #[allow(dead_code)]
    pub lock: GameLock,
}

/// What the prepare reports to the window: `done` of `total` variants, in attempt number `attempt` (1 is the first; a higher
/// one is a retry after the program stopped, TASK-170b).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct PrepareProgress {
    pub done: u32,
    pub total: u32,
    pub attempt: u32,
}

/// Compiles what the CoreGS 2.0 shader cache lacks with the game program (IN-507), on the card of the session: `child` holds
/// the process so that Cancel can stop it (`cancelled` is the flag Cancel sets), and `progress` gets the count only when there
/// is work, and once more as a retry begins. When the program ends without finishing (a driver that ends the process inside a
/// pipeline compile), it runs again while each attempt gets further, up to [`shader_prepare::MAX_PREPARE_ATTEMPTS`]
/// (TASK-170b). Returns the outcome; every attempt's stderr goes to `logs\\shaders.log` under a header.
#[allow(clippy::too_many_arguments)]
pub fn prepare_shaders(
    dirs: &DataDirs,
    game_executable: &Path,
    settings: &Settings,
    display: Option<&DisplayTarget>,
    textures: &TextureLocation,
    child: &Mutex<Option<Child>>,
    cancelled: &AtomicBool,
    progress: &mut dyn FnMut(PrepareProgress),
) -> Result<PrepareOutcome, String> {
    let guard_folder = textures.shader_cache.clone();
    let snapshot = || guard_snapshot(guard_folder.as_deref());
    let last_total = Cell::new(0u32);
    let outcome = shader_prepare::run_attempts(
        shader_prepare::MAX_PREPARE_ATTEMPTS,
        &|| cancelled.load(Ordering::SeqCst),
        &snapshot,
        &mut |attempt| {
            if attempt > 1 {
                progress(PrepareProgress { done: 0, total: last_total.get(), attempt });
            }
            prepare_shaders_once(dirs, game_executable, settings, display, textures, child, attempt, &mut |done, total| {
                last_total.set(total);
                progress(PrepareProgress { done, total, attempt });
            })
        },
    );
    match outcome {
        PrepareOutcome::Failed { message, .. } => Err(message),
        outcome => Ok(outcome),
    }
}

/// The guard files of the shader cache folder, read now.
fn guard_snapshot(folder: Option<&Path>) -> GuardSnapshot {
    let read = |name: &str| folder.and_then(|folder| fs::read(folder.join(name)).ok());
    shader_prepare::snapshot_of(
        read(shader_prepare::QUARANTINE_FILE).as_deref(),
        read(shader_prepare::COMPILING_FILE).as_deref(),
        read(shader_prepare::SUSPECTS_FILE).as_deref(),
    )
}

/// The line that opens an attempt's part of `shaders.log`.
fn attempt_header(attempt: u32) -> String {
    format!("=== shader prepare, attempt {attempt} ===\n")
}

/// Opens `shaders.log` for an attempt: the first attempt of a Play starts the file anew, the others append to it.
fn open_attempt_log(path: &Path, attempt: u32) -> Result<File, String> {
    if attempt <= 1 {
        File::create(path).map_err(|error| format!("shaders.log: {error}"))?;
    }
    let mut log = OpenOptions::new().create(true).append(true).open(path).map_err(|error| format!("shaders.log: {error}"))?;
    log.write_all(attempt_header(attempt).as_bytes()).map_err(|error| format!("shaders.log: {error}"))?;
    Ok(log)
}

/// One run of `gt4.exe --prepare-shaders`: the report says whether it finished and how far it got.
fn prepare_shaders_once(
    dirs: &DataDirs,
    game_executable: &Path,
    settings: &Settings,
    display: Option<&DisplayTarget>,
    textures: &TextureLocation,
    child: &Mutex<Option<Child>>,
    attempt: u32,
    progress: &mut dyn FnMut(u32, u32),
) -> Result<AttemptReport, String> {
    let logs = dirs.logs();
    fs::create_dir_all(&logs).map_err(|error| format!("logs folder: {error}"))?;
    let log_path = logs.join("shaders.log");
    let log = open_attempt_log(&log_path, attempt)?;
    let inherited: Vec<String> = std::env::vars_os().filter_map(|(name, _)| name.into_string().ok()).collect();
    // Only the program and the environment of this plan are used: nothing here reads the disc or the card.
    let inputs = LaunchInputs { game_executable, elf: game_executable, disc: game_executable, card: game_executable, session_dir: &logs };
    let integrated = crate::gpu::integrated_gpu_name(settings.get("graphics_card")).is_some();
    let plan = launch::plan_coregs20_launch(&inputs, settings, display, integrated, textures, &inherited);
    let mut command = Command::new(&plan.program);
    command.arg(launch::PREPARE_SHADERS_ARGUMENT).stdin(Stdio::null()).stdout(Stdio::piped()).stderr(Stdio::from(log));
    if let Some(folder) = game_executable.parent() {
        command.current_dir(folder);
    }
    for name in &plan.removed_inherited {
        command.env_remove(name);
    }
    for (name, value) in &plan.environment {
        command.env(name, value);
    }
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        command.creation_flags(CREATE_NO_WINDOW);
    }
    let mut process = command.spawn().map_err(|error| format!("could not start {}: {error}", plan.program.display()))?;
    let stdout = process.stdout.take();
    *child.lock().unwrap_or_else(|poisoned| poisoned.into_inner()) = Some(process);
    let mut best_done = 0;
    if let Some(stdout) = stdout {
        for line in BufReader::new(stdout).lines().map_while(Result::ok) {
            if let Some(ShaderPrepareLine::Progress { done, total }) = launch::parse_shader_prepare_line(&line) {
                best_done = best_done.max(done);
                progress(done, total);
            }
        }
    }
    let process = child.lock().unwrap_or_else(|poisoned| poisoned.into_inner()).take();
    let status = process.map(|mut process| process.wait()).transpose().map_err(|error| error.to_string())?;
    let finished = status.is_some_and(|status| status.success());
    // The end of the attempt, in the log beside what the program wrote.
    if let Ok(mut log) = OpenOptions::new().append(true).open(&log_path) {
        let ended = match status.and_then(|status| status.code()) {
            Some(code) => format!("=== attempt {attempt} ended with exit code {code}{} ===\n", if finished { " (finished)" } else { "" }),
            None => format!("=== attempt {attempt} ended without an exit code ===\n"),
        };
        let _ = log.write_all(ended.as_bytes());
    }
    Ok(AttemptReport { finished, best_done })
}

/// Starts a session. The disc and the prepared ELF must already be verified by the caller.
/// `pcsx2` is the installed PCSX2 GS (decision 0070): the session runs with `GT4_RENDERER=pcsx2` and writes its own
/// gt4-gs.ini. `None` is a CoreGS 2.0 session (`launch::uses_coregs20`, TASK-027): `GT4_RENDERER=hw`, nothing to write.
/// `display` is the resolved Monitor and Resolution (TASK-080 addendum): `GT4_MONITOR`, `GT4_OUTPUT_SIZE` and the
/// gt4-gs internal resolution follow it; `None` when no monitor could be read.
#[allow(clippy::too_many_arguments)]
pub fn start(
    dirs: &DataDirs,
    game_executable: &Path,
    elf: &Path,
    disc: &Path,
    settings: &Settings,
    pcsx2: Option<&Installed>,
    display: Option<&DisplayTarget>,
    textures: &TextureLocation,
    extra_environment: &[(String, String)],
) -> Result<Running, String> {
    let lock = GameLock::acquire(&dirs.runtime_card().with_file_name("launcher.lock"))
        .map_err(|error| error.to_string())?;
    let card = dirs.runtime_card();
    if let Some(parent) = card.parent() {
        fs::create_dir_all(parent).map_err(|error| format!("runtime folder: {error}"))?;
    }
    let session_dir = dirs.session(&session_name());
    fs::create_dir_all(&session_dir).map_err(|error| format!("session folder: {error}"))?;
    prune_sessions(&dirs.sessions(), KEEP_SESSIONS);

    let inherited: Vec<String> = std::env::vars_os()
        .filter_map(|(name, _)| name.into_string().ok())
        .collect();
    let inputs = LaunchInputs {
        game_executable,
        elf,
        disc,
        card: &card,
        session_dir: &session_dir,
    };
    let integrated = crate::gpu::integrated_gpu_name(settings.get("graphics_card")).is_some();
    let mut plan = match pcsx2 {
        Some(pcsx2) => {
            // TASK-155: an install made before the marker existed gets it here (best effort: gt4-gs still starts without it
            // where Documents is local).
            if let Some(folder) = pcsx2.executable.parent() {
                if let Err(error) = gt4l_core::gs_release::ensure_portable_marker(folder) {
                    eprintln!("gt4-gs: {} not written: {error}", gt4l_core::gs_release::PORTABLE_MARKER);
                }
            }
            let paths = Pcsx2Paths::for_session(pcsx2, &dirs.root().join("gs"), &session_dir, &textures.folder);
            let text = launch::gs_config_text(settings, &paths, display, integrated)?;
            gs_table::write_ini_atomic(&paths.config, &text).map_err(|error| format!("gt4-gs.ini: {error}"))?;
            launch::plan_pcsx2_launch(&inputs, settings, &paths, display, &inherited)
        }
        None => launch::plan_coregs20_launch(&inputs, settings, display, integrated, textures, &inherited),
    };
    plan.environment.extend(extra_environment.iter().cloned());
    let child = spawn(&plan)?;
    Ok(Running {
        child: Arc::new(Mutex::new(Some(child))),
        session_dir,
        log_path: plan.log_path.clone(),
        lock,
    })
}

fn spawn(plan: &LaunchPlan) -> Result<Child, String> {
    let log = File::create(&plan.log_path).map_err(|error| format!("game log: {error}"))?;
    let log_copy = log.try_clone().map_err(|error| format!("game log: {error}"))?;
    let mut command = Command::new(&plan.program);
    command
        .args(&plan.arguments)
        .current_dir(&plan.working_directory)
        .stdin(Stdio::null())
        .stdout(Stdio::from(log))
        .stderr(Stdio::from(log_copy));
    for name in &plan.removed_inherited {
        command.env_remove(name);
    }
    for (name, value) in &plan.environment {
        command.env(name, value);
    }
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        command.creation_flags(CREATE_NO_WINDOW);
    }
    command
        .spawn()
        .map_err(|error| format!("could not start {}: {error}", plan.program.display()))
}

/// Waits for the child to end. Returns the outcome once the process has exited and the log is read.
pub fn wait_for_exit(running: &Running) -> Outcome {
    let code = loop {
        let status = {
            let mut guard = match running.child.lock() {
                Ok(guard) => guard,
                Err(poisoned) => poisoned.into_inner(),
            };
            match guard.as_mut() {
                None => break None,
                Some(child) => child.try_wait(),
            }
        };
        match status {
            Ok(Some(status)) => break status.code(),
            Ok(None) => std::thread::sleep(Duration::from_millis(250)),
            Err(_) => break None,
        }
    };
    let log = fs::read_to_string(&running.log_path).unwrap_or_default();
    exit::classify(&log, code)
}

/// `session-<unix seconds>`; the seconds keep names unique for sessions started a second apart.
fn session_name() -> String {
    let seconds = SystemTime::now().duration_since(UNIX_EPOCH).map(|value| value.as_secs()).unwrap_or(0);
    format!("session-{seconds}")
}

fn prune_sessions(sessions: &Path, keep: usize) {
    let Ok(entries) = fs::read_dir(sessions) else {
        return;
    };
    let mut names: Vec<PathBuf> = entries.filter_map(|entry| entry.ok().map(|entry| entry.path())).collect();
    names.sort();
    if names.len() > keep {
        for old in &names[..names.len() - keep] {
            let _ = fs::remove_dir_all(old);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn scratch(name: &str) -> PathBuf {
        let folder = std::env::temp_dir().join(format!("gt4l-game-{name}-{}", std::process::id()));
        let _ = fs::remove_dir_all(&folder);
        fs::create_dir_all(&folder).unwrap();
        folder
    }

    #[test]
    fn the_shader_log_starts_anew_with_attempt_one_and_appends_the_retries() {
        let folder = scratch("shader-log");
        let path = folder.join("shaders.log");
        fs::write(&path, "an old Play\n").unwrap();
        let mut first = open_attempt_log(&path, 1).unwrap();
        first.write_all(b"[core2] stopped inside a compile\n").unwrap();
        drop(first);
        let mut second = open_attempt_log(&path, 2).unwrap();
        second.write_all(b"[core2] shader guard: 1 variants quarantined\n").unwrap();
        drop(second);
        drop(open_attempt_log(&path, 3).unwrap());
        let text = fs::read_to_string(&path).unwrap();
        assert!(!text.contains("an old Play"), "attempt 1 starts the file anew");
        let order: Vec<usize> = ["attempt 1 ===", "stopped inside a compile", "attempt 2 ===", "1 variants quarantined", "attempt 3 ==="]
            .iter()
            .map(|part| text.find(part).unwrap_or_else(|| panic!("{part} missing in {text}")))
            .collect();
        assert!(order.windows(2).all(|pair| pair[0] < pair[1]), "attempts stay in order: {text}");
        let _ = fs::remove_dir_all(&folder);
    }

    #[test]
    fn the_guard_snapshot_reads_the_cache_files_and_is_empty_without_them() {
        let folder = scratch("guard-snapshot");
        assert_eq!(guard_snapshot(Some(&folder)), GuardSnapshot::default());
        assert_eq!(guard_snapshot(None), GuardSnapshot::default());
        // One key of three words in compiling.bin: a run stopped inside that compile.
        let mut file = b"GT4GRD1\0".to_vec();
        file.extend_from_slice(&1u64.to_le_bytes());
        for value in [0x1002u32, 0x73df, 1, 3, 0, 1, 10, 20, 30] {
            file.extend_from_slice(&value.to_le_bytes());
        }
        fs::write(folder.join(shader_prepare::COMPILING_FILE), &file).unwrap();
        let snapshot = guard_snapshot(Some(&folder));
        assert_eq!((snapshot.compiling.clone(), snapshot.quarantine.is_empty(), snapshot.suspects.is_empty()), (file.clone(), true, true));
        // A suspects.bin written later is another state: the first crash of a key is progress.
        fs::write(folder.join(shader_prepare::SUSPECTS_FILE), &file).unwrap();
        assert_ne!(guard_snapshot(Some(&folder)), snapshot);
        let _ = fs::remove_dir_all(&folder);
    }
}
