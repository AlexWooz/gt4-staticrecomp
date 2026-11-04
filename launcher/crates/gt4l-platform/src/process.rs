//! Windows process rules for the launcher: one launcher at a time, and the game ends with the launcher.
//!
//! The launcher puts itself in a Job Object with kill-on-close. The game is started from the launcher,
//! so it joins the same job; when the launcher exits, the job's last handle closes and Windows ends
//! every process in it. The job handle is intentionally never closed before exit.
//!
//! The job allows breakaway: a process the launcher must outlive on purpose (the GT4Recomp setup program of a
//! self-update) is started with `spawn_outside_job`. Without it, the setup joined the job and Windows ended it the
//! moment the launcher exited to let it replace the program files (launcher 1.0.0 bug, 2026-10-21).
//!
//! From 1.2.1 the setup is per-machine and needs administrator rights (TASK-091). `CreateProcess` cannot start such
//! a program (`ERROR_ELEVATION_REQUIRED`, 740), so a self-update first clears kill-on-close on the launcher's job
//! ([`release_kill_on_close`]; the game is not running at that point) and then asks Windows to start the setup
//! elevated (`ShellExecuteExW` with the `runas` verb, which shows the UAC prompt). [`start_setup`] keeps the
//! breakaway start and the Explorer fallback behind it.

#[cfg(windows)]
mod windows_impl {
    use std::ffi::c_void;
    use std::mem::size_of;

    use windows::core::{w, HSTRING, PCWSTR};
    use windows::Win32::Foundation::{CloseHandle, GetLastError, ERROR_ALREADY_EXISTS};
    use windows::Win32::System::JobObjects::{
        AssignProcessToJobObject, CreateJobObjectW, JobObjectExtendedLimitInformation, SetInformationJobObject,
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION, JOB_OBJECT_LIMIT_BREAKAWAY_OK, JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE,
    };
    use windows::Win32::System::Threading::{CreateMutexW, GetCurrentProcess};
    use std::path::{Path, PathBuf};
    use std::sync::OnceLock;

    /// The folder the single-instance lock belongs to (the portable build, TASK-098). Unset: one lock for the PC user.
    static SCOPE: OnceLock<PathBuf> = OnceLock::new();

    /// Makes the single-instance lock belong to `folder`, so two copies in different folders do not block each other
    /// but one copy cannot start twice. Call once, before the first claim.
    pub fn set_single_instance_scope(folder: &Path) {
        let _ = SCOPE.set(folder.to_path_buf());
    }

    /// The launcher's job handle (as an integer, so it can live in a static). Kept open until the process exits.
    static JOB: OnceLock<isize> = OnceLock::new();
    use windows::Win32::UI::WindowsAndMessaging::{FindWindowW, IsWindowVisible, SetForegroundWindow, ShowWindow, SW_RESTORE};

    /// Title of the launcher window (the window title set by the UI shell). Not the game title: the game window is
    /// "Gran Turismo 4", and focusing it from a second launcher would bring the game forward.
    const WINDOW_TITLE: PCWSTR = w!("GT4Recomp");

    /// One attempt at the launcher mutex. `true`: this is the first launcher (the handle is kept until exit). `false`:
    /// another launcher holds it; the handle is closed here, so a later attempt can succeed once that one has exited.
    fn attempt_claim() -> bool {
        claim_named(&super::launcher_mutex_name(SCOPE.get().map(PathBuf::as_path)))
    }

    /// One attempt at the named mutex (see [`attempt_claim`]).
    pub(super) fn claim_named(name: &str) -> bool {
        // SAFETY: the name is a wide string that lives through the call. The handle is kept open until the process exits
        // when it is ours.
        let name = HSTRING::from(name);
        let created = unsafe { CreateMutexW(None, true, &name) };
        let Ok(handle) = created else {
            return true; // cannot tell; start anyway
        };
        // SAFETY: no arguments; reads the error of the call above on this thread.
        if unsafe { GetLastError() } != ERROR_ALREADY_EXISTS {
            return true;
        }
        // SAFETY: `handle` was returned by CreateMutexW above and is closed exactly once.
        unsafe {
            let _ = CloseHandle(handle);
        }
        false
    }

    /// Brings the running launcher's window forward, only when that window is already shown. The window starts hidden
    /// and shows itself when its page is ready (`ui_ready`); a second start must not show it half ready (TASK-091,
    /// 1.2.1). When it is still hidden, nothing is shown here: the first launcher shows itself.
    fn focus_running_launcher() {
        // SAFETY: FindWindowW with a null class name looks up the title only.
        let window = unsafe { FindWindowW(PCWSTR::null(), WINDOW_TITLE) }.ok();
        // SAFETY: a found handle is a window handle returned by FindWindowW; IsWindowVisible only reads its state.
        let visible = window.map(|window| unsafe { IsWindowVisible(window).as_bool() }).unwrap_or(false);
        if !super::should_bring_forward(window.is_some(), visible) {
            return;
        }
        if let Some(window) = window {
            // SAFETY: `window` is a window handle returned by FindWindowW.
            unsafe {
                let _ = ShowWindow(window, SW_RESTORE);
                let _ = SetForegroundWindow(window);
            }
        }
    }

    /// Returns `false` when another launcher already runs; that window is brought forward instead.
    pub fn claim_single_instance() -> bool {
        if attempt_claim() {
            return true;
        }
        focus_running_launcher();
        false
    }

    /// Like [`claim_single_instance`], but keeps trying every `step` for up to `timeout` while another launcher
    /// exits (a restart in safe mode, TASK-091). It does not bring the other window forward.
    pub fn claim_single_instance_retrying(timeout: std::time::Duration, step: std::time::Duration) -> bool {
        let deadline = std::time::Instant::now() + timeout;
        loop {
            if attempt_claim() {
                return true;
            }
            if std::time::Instant::now() >= deadline {
                return false;
            }
            std::thread::sleep(step);
        }
    }

    /// Puts this process in a job that ends its members when the launcher exits.
    pub fn join_kill_on_close_job() -> Result<(), String> {
        // SAFETY: all pointers passed are to locals that live for the duration of each call.
        unsafe {
            let job = CreateJobObjectW(None, PCWSTR::null()).map_err(|error| format!("job object: {error}"))?;
            let mut limits = JOBOBJECT_EXTENDED_LIMIT_INFORMATION::default();
            // Children stay in the job unless they ask to leave it (CREATE_BREAKAWAY_FROM_JOB); the game never does.
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_BREAKAWAY_OK;
            let set = SetInformationJobObject(
                job,
                JobObjectExtendedLimitInformation,
                &limits as *const _ as *const c_void,
                size_of::<JOBOBJECT_EXTENDED_LIMIT_INFORMATION>() as u32,
            );
            if let Err(error) = set {
                let _ = CloseHandle(job);
                return Err(format!("job limits: {error}"));
            }
            if let Err(error) = AssignProcessToJobObject(job, GetCurrentProcess()) {
                let _ = CloseHandle(job);
                return Err(format!("job assignment: {error}"));
            }
            // The handle is not closed: it closes at exit, and then Windows ends the game.
            let _ = JOB.set(job.0 as isize);
        }
        Ok(())
    }

    /// Clears kill-on-close on the launcher's job, so every process it started keeps running after it exits. Used
    /// only right before a self-update hands over to the setup (the game is not running then). No job: nothing to do.
    pub fn release_kill_on_close() -> Result<(), String> {
        let Some(raw) = JOB.get() else {
            return Ok(());
        };
        let job = windows::Win32::Foundation::HANDLE(*raw as *mut c_void);
        let mut limits = JOBOBJECT_EXTENDED_LIMIT_INFORMATION::default();
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_BREAKAWAY_OK;
        // SAFETY: `job` is the job handle created above and never closed; `limits` lives for the call.
        unsafe {
            SetInformationJobObject(
                job,
                JobObjectExtendedLimitInformation,
                &limits as *const _ as *const c_void,
                size_of::<JOBOBJECT_EXTENDED_LIMIT_INFORMATION>() as u32,
            )
        }
        .map_err(|error| format!("job limits: {error}"))
    }

    /// Sets kill-on-close again on the launcher's job, after a self-update that did not start its setup. Without it
    /// the launcher would stay open with a game that no longer ends with it. No job: nothing to do.
    pub fn restore_kill_on_close() -> Result<(), String> {
        let Some(raw) = JOB.get() else {
            return Ok(());
        };
        let job = windows::Win32::Foundation::HANDLE(*raw as *mut c_void);
        let mut limits = JOBOBJECT_EXTENDED_LIMIT_INFORMATION::default();
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_BREAKAWAY_OK;
        // SAFETY: `job` is the job handle created above and never closed; `limits` lives for the call.
        unsafe {
            SetInformationJobObject(
                job,
                JobObjectExtendedLimitInformation,
                &limits as *const _ as *const c_void,
                size_of::<JOBOBJECT_EXTENDED_LIMIT_INFORMATION>() as u32,
            )
        }
        .map_err(|error| format!("job limits: {error}"))
    }

    /// Whether a process with one of these executable names runs (case-insensitive). A failed snapshot counts as
    /// "running", so a move never starts blind.
    pub fn process_running(names: &[&str]) -> bool {
        use windows::Win32::System::Diagnostics::ToolHelp::{
            CreateToolhelp32Snapshot, Process32FirstW, Process32NextW, PROCESSENTRY32W, TH32CS_SNAPPROCESS,
        };
        let wanted: Vec<String> = names.iter().map(|name| name.to_ascii_lowercase()).collect();
        // SAFETY: the snapshot handle is closed below; `entry` lives for each call and has its size set.
        unsafe {
            let Ok(snapshot) = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0) else {
                return true;
            };
            let mut entry = PROCESSENTRY32W { dwSize: size_of::<PROCESSENTRY32W>() as u32, ..Default::default() };
            let mut found = false;
            let mut more = Process32FirstW(snapshot, &mut entry).is_ok();
            while more {
                let length = entry.szExeFile.iter().position(|unit| *unit == 0).unwrap_or(entry.szExeFile.len());
                let name = String::from_utf16_lossy(&entry.szExeFile[..length]).to_ascii_lowercase();
                if wanted.contains(&name) {
                    found = true;
                    break;
                }
                more = Process32NextW(snapshot, &mut entry).is_ok();
            }
            let _ = CloseHandle(snapshot);
            found
        }
    }

    /// `ShellExecuteExW` with the `runas` verb: Windows shows the UAC prompt and starts `program` elevated, with
    /// `arguments` as its command line when given (`lpParameters`, passed unchanged). Waits until the start is
    /// handed over (`SEE_MASK_NOASYNC`), as the launcher exits right after. `Err` is the Win32 error
    /// (`ERROR_CANCELLED` when the prompt was declined).
    pub fn shell_run_as_admin(program: &std::path::Path, arguments: Option<&str>) -> Result<(), u32> {
        use std::os::windows::ffi::OsStrExt;
        use windows::Win32::System::Com::{CoInitializeEx, COINIT_APARTMENTTHREADED, COINIT_DISABLE_OLE1DDE};
        use windows::Win32::UI::Shell::{ShellExecuteExW, SEE_MASK_NOASYNC, SHELLEXECUTEINFOW};
        use windows::Win32::UI::WindowsAndMessaging::SW_SHOWNORMAL;
        let file: Vec<u16> = program.as_os_str().encode_wide().chain(std::iter::once(0)).collect();
        let directory: Vec<u16> = program
            .parent()
            .map(|folder| folder.as_os_str().encode_wide().chain(std::iter::once(0)).collect())
            .unwrap_or_else(|| vec![0]);
        let parameters: Vec<u16> = arguments.unwrap_or("").encode_utf16().chain(std::iter::once(0)).collect();
        let parameters_ptr = if arguments.is_some() { PCWSTR(parameters.as_ptr()) } else { PCWSTR::null() };
        let mut info = SHELLEXECUTEINFOW {
            cbSize: size_of::<SHELLEXECUTEINFOW>() as u32,
            fMask: SEE_MASK_NOASYNC,
            lpVerb: w!("runas"),
            lpFile: PCWSTR(file.as_ptr()),
            lpParameters: parameters_ptr,
            lpDirectory: PCWSTR(directory.as_ptr()),
            nShow: SW_SHOWNORMAL.0,
            ..Default::default()
        };
        // SAFETY: COM is initialised for this thread as ShellExecuteEx asks (a second call is harmless); `info` and
        // the wide strings it points at live for the call.
        unsafe {
            let _ = CoInitializeEx(None, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
            ShellExecuteExW(&mut info).map_err(|error| win32_code(error.code().0))
        }
    }

    /// The Win32 error inside an HRESULT_FROM_WIN32 value (other HRESULTs are returned as they are).
    fn win32_code(hresult: i32) -> u32 {
        let value = hresult as u32;
        if value & 0xFFFF_0000 == 0x8007_0000 {
            value & 0xFFFF
        } else {
            value
        }
    }
}

#[cfg(windows)]
pub use windows_impl::{
    set_single_instance_scope, claim_single_instance, claim_single_instance_retrying, join_kill_on_close_job, process_running, release_kill_on_close, restore_kill_on_close,
    shell_run_as_admin,
};

/// `CREATE_BREAKAWAY_FROM_JOB`: the new process is not a member of the launcher's kill-on-close job.
#[cfg(windows)]
const CREATE_BREAKAWAY_FROM_JOB: u32 = 0x0100_0000;

/// Starts a program that must keep running after the launcher exits (the setup program of a self-update). On
/// Windows it leaves the launcher's kill-on-close job; any other child (the game) stays in it.
pub fn spawn_outside_job(command: &mut std::process::Command) -> std::io::Result<std::process::Child> {
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        command.creation_flags(CREATE_BREAKAWAY_FROM_JOB);
    }
    command.spawn()
}

/// The command to start `program` with `arguments` given as one string, passed to the command line unchanged. NSIS
/// needs `/D=` last and unquoted, so the text is not split into arguments and quoted one by one. None adds nothing.
pub fn command_with_arguments(program: &std::path::Path, arguments: Option<&str>) -> std::process::Command {
    let mut command = std::process::Command::new(program);
    if let Some(arguments) = arguments {
        #[cfg(windows)]
        {
            use std::os::windows::process::CommandExt;
            command.raw_arg(arguments);
        }
        #[cfg(not(windows))]
        {
            command.arg(arguments);
        }
    }
    command
}

/// The name of the launcher mutex. Without a scope it is the one name of the installer build (one launcher per Windows
/// session); with a folder (the portable build) the name carries the folder's key, so the lock is per folder.
pub fn launcher_mutex_name(scope: Option<&std::path::Path>) -> String {
    match scope {
        None => "Local\\GT4Recomp.Launcher".to_string(),
        Some(folder) => format!("Local\\GT4Recomp.Launcher.{}", gt4l_core::paths::folder_key(folder)),
    }
}

/// The decision of a second launcher start: bring the running launcher's window forward only when that window exists
/// and is already shown. A hidden window (not ready yet) is left to the first launcher, which shows it on `ui_ready`.
pub fn should_bring_forward(window_found: bool, window_visible: bool) -> bool {
    window_found && window_visible
}

#[cfg(test)]
mod single_instance_tests {
    use super::should_bring_forward;

    #[test]
    fn the_lock_is_one_name_without_a_scope_and_one_per_folder_with_one() {
        use super::launcher_mutex_name;
        use std::path::Path;
        assert_eq!(launcher_mutex_name(None), "Local\\GT4Recomp.Launcher", "the installer build keeps its name");
        let first = launcher_mutex_name(Some(Path::new(r"E:\Portable\GT4Recomp")));
        let second = launcher_mutex_name(Some(Path::new(r"F:\Other\GT4Recomp")));
        assert_ne!(first, second, "two copies do not block each other");
        assert_ne!(first, launcher_mutex_name(None), "a portable copy and the installed one do not block each other");
        assert_eq!(first, launcher_mutex_name(Some(Path::new(r"e:\portable\gt4recomp\"))), "the same folder is the same lock");
        assert!(first.starts_with("Local\\GT4Recomp.Launcher."));
    }

    #[cfg(windows)]
    #[test]
    fn a_second_claim_of_the_same_folder_fails_and_another_folder_succeeds() {
        use super::launcher_mutex_name;
        use std::path::Path;
        let unique = std::process::id();
        let first = launcher_mutex_name(Some(Path::new(&format!(r"Z:\lock-test-{unique}\one"))));
        let other = launcher_mutex_name(Some(Path::new(&format!(r"Z:\lock-test-{unique}\two"))));
        assert!(super::windows_impl::claim_named(&first), "the first claim of a folder succeeds");
        assert!(!super::windows_impl::claim_named(&first), "the same folder cannot start twice");
        assert!(super::windows_impl::claim_named(&other), "another folder does not block");
    }

    #[test]
    fn a_shown_window_is_brought_forward() {
        assert!(should_bring_forward(true, true));
    }

    #[test]
    fn a_hidden_window_is_not_shown_by_a_second_start() {
        assert!(!should_bring_forward(true, false), "the page is not ready yet: the first launcher shows it");
    }

    #[test]
    fn no_window_means_nothing_to_bring_forward() {
        assert!(!should_bring_forward(false, false));
        assert!(!should_bring_forward(false, true));
    }
}

#[cfg(not(windows))]
pub fn set_single_instance_scope(_folder: &std::path::Path) {}

#[cfg(not(windows))]
pub fn claim_single_instance() -> bool {
    true
}

#[cfg(not(windows))]
pub fn claim_single_instance_retrying(_timeout: std::time::Duration, _step: std::time::Duration) -> bool {
    true
}

#[cfg(not(windows))]
pub fn join_kill_on_close_job() -> Result<(), String> {
    Ok(())
}

#[cfg(not(windows))]
pub fn release_kill_on_close() -> Result<(), String> {
    Ok(())
}

#[cfg(not(windows))]
pub fn restore_kill_on_close() -> Result<(), String> {
    Ok(())
}

#[cfg(not(windows))]
pub fn process_running(_names: &[&str]) -> bool {
    false
}

#[cfg(not(windows))]
pub fn shell_run_as_admin(_program: &std::path::Path, _arguments: Option<&str>) -> Result<(), u32> {
    Err(ERROR_ELEVATION_REQUIRED)
}

/// Executable names of the game and its renderer process: neither may run while files move.
/// The old names (1.2.x and before) are listed too: a 1.2.x game may still run while the update starts.
pub const GAME_PROCESS_NAMES: &[&str] =
    &["gt4.exe", "gt4-spec2.exe", "gt4vulkan.exe", "gt4vulkan-spec2.exe", "gt4-gs.exe"];

/// `ERROR_CANCELLED`: the UAC prompt was declined.
pub const ERROR_CANCELLED: u32 = 1223;

/// `ERROR_ELEVATION_REQUIRED`: `CreateProcess` refused a program whose manifest asks for administrator rights.
pub const ERROR_ELEVATION_REQUIRED: u32 = 740;

/// How the setup program of a self-update was started.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SetupStart {
    /// Elevated through the UAC prompt (`runas`): the 1.2.1 path.
    Elevated,
    /// Started directly outside the job (a setup that needs no administrator rights).
    OutsideJob,
    /// Started by the Windows shell (Explorer), which shows the UAC prompt itself when the setup needs it.
    Explorer,
}

/// The three ways to start the setup, so the order and the error rules are testable without a UAC prompt.
/// `arguments` is the command line after the program (a self-update passes `/S /UPDATE /D=<folder>`). The Explorer
/// way cannot pass arguments, so it does not get them.
pub trait SetupStarter {
    /// `Err` is the Win32 error of the elevated start.
    fn run_as_admin(&self, program: &std::path::Path, arguments: Option<&str>) -> Result<(), u32>;
    fn spawn_outside_job(&self, program: &std::path::Path, arguments: Option<&str>) -> std::io::Result<()>;
    fn via_explorer(&self, program: &std::path::Path) -> std::io::Result<()>;
}

/// The starter of the launcher.
pub struct WindowsSetupStarter;

impl SetupStarter for WindowsSetupStarter {
    fn run_as_admin(&self, program: &std::path::Path, arguments: Option<&str>) -> Result<(), u32> {
        shell_run_as_admin(program, arguments)
    }

    fn spawn_outside_job(&self, program: &std::path::Path, arguments: Option<&str>) -> std::io::Result<()> {
        spawn_outside_job(&mut command_with_arguments(program, arguments)).map(|_| ())
    }

    fn via_explorer(&self, program: &std::path::Path) -> std::io::Result<()> {
        std::process::Command::new("explorer.exe").arg(program).spawn().map(|_| ())
    }
}

/// Starts the setup program with `arguments` (None: none): elevated first; when the elevated start fails for another
/// reason than a declined prompt, a direct start outside the job, then Explorer. Explorer cannot pass the arguments,
/// so that update shows the setup pages (the log says so). A declined prompt is final (the player said no).
/// `log` gets one line per failed attempt.
pub fn start_setup(
    program: &std::path::Path,
    arguments: Option<&str>,
    starter: &dyn SetupStarter,
    log: &mut dyn FnMut(String),
) -> Result<SetupStart, String> {
    match starter.run_as_admin(program, arguments) {
        Ok(()) => return Ok(SetupStart::Elevated),
        Err(ERROR_CANCELLED) => {
            return Err("The administrator prompt was declined, so GT4Recomp was not updated.".to_string());
        }
        Err(code) => log(format!("the elevated start of the setup failed (Win32 error {code}); trying a direct start")),
    }
    match starter.spawn_outside_job(program, arguments) {
        Ok(()) => return Ok(SetupStart::OutsideJob),
        Err(error) => log(format!("setup could not start directly ({error}); asking Explorer to start it")),
    }
    if arguments.is_some() {
        log("Explorer cannot pass the setup arguments, so this update shows the setup pages".to_string());
    }
    starter
        .via_explorer(program)
        .map(|_| SetupStart::Explorer)
        .map_err(|error| format!("could not start the setup: {error}"))
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::cell::RefCell;
    use std::path::Path;

    /// Records the calls and the arguments each way got; each way succeeds or fails as told.
    struct FakeStarter {
        admin: Result<(), u32>,
        direct: bool,
        explorer: bool,
        calls: RefCell<Vec<String>>,
    }

    /// The arguments as a starter recorded them (`-` for none).
    fn args_text(arguments: Option<&str>) -> &str {
        arguments.unwrap_or("-")
    }

    impl SetupStarter for FakeStarter {
        fn run_as_admin(&self, _: &Path, arguments: Option<&str>) -> Result<(), u32> {
            self.calls.borrow_mut().push(format!("runas {}", args_text(arguments)));
            self.admin
        }
        fn spawn_outside_job(&self, _: &Path, arguments: Option<&str>) -> std::io::Result<()> {
            self.calls.borrow_mut().push(format!("direct {}", args_text(arguments)));
            if self.direct {
                Ok(())
            } else {
                Err(std::io::Error::from_raw_os_error(ERROR_ELEVATION_REQUIRED as i32))
            }
        }
        fn via_explorer(&self, _: &Path) -> std::io::Result<()> {
            self.calls.borrow_mut().push("explorer".to_string());
            if self.explorer {
                Ok(())
            } else {
                Err(std::io::Error::other("no shell"))
            }
        }
    }

    fn fake(admin: Result<(), u32>, direct: bool, explorer: bool) -> FakeStarter {
        FakeStarter { admin, direct, explorer, calls: RefCell::new(Vec::new()) }
    }

    const SILENT: &str = "/S /UPDATE /D=C:\\Program Files\\GT4Recomp";

    #[test]
    fn the_setup_starts_elevated_first() {
        let starter = fake(Ok(()), true, true);
        assert_eq!(start_setup(Path::new("setup.exe"), None, &starter, &mut |_| {}), Ok(SetupStart::Elevated));
        assert_eq!(*starter.calls.borrow(), vec!["runas -"]);
    }

    #[test]
    fn the_arguments_reach_the_elevated_start() {
        let starter = fake(Ok(()), true, true);
        assert_eq!(start_setup(Path::new("setup.exe"), Some(SILENT), &starter, &mut |_| {}), Ok(SetupStart::Elevated));
        assert_eq!(*starter.calls.borrow(), vec![format!("runas {SILENT}")]);
    }

    #[test]
    fn a_declined_prompt_is_final() {
        let starter = fake(Err(ERROR_CANCELLED), true, true);
        let result = start_setup(Path::new("setup.exe"), Some(SILENT), &starter, &mut |_| {});
        assert!(result.unwrap_err().contains("declined"));
        assert_eq!(*starter.calls.borrow(), vec![format!("runas {SILENT}")], "no other start after the player said no");
    }

    #[test]
    fn other_failures_fall_back_to_a_direct_start_then_explorer() {
        let mut lines = Vec::new();
        let starter = fake(Err(2), false, true);
        assert_eq!(start_setup(Path::new("setup.exe"), None, &starter, &mut |line| lines.push(line)), Ok(SetupStart::Explorer));
        assert_eq!(*starter.calls.borrow(), vec!["runas -", "direct -", "explorer"]);
        assert_eq!(lines.len(), 2);
        let starter = fake(Err(2), true, true);
        assert_eq!(start_setup(Path::new("setup.exe"), Some(SILENT), &starter, &mut |_| {}), Ok(SetupStart::OutsideJob));
        assert_eq!(*starter.calls.borrow(), vec![format!("runas {SILENT}"), format!("direct {SILENT}")]);
        let starter = fake(Err(2), false, false);
        assert!(start_setup(Path::new("setup.exe"), None, &starter, &mut |_| {}).is_err());
    }

    #[test]
    fn explorer_starts_without_arguments_and_the_log_says_the_update_is_interactive() {
        let mut lines = Vec::new();
        let starter = fake(Err(2), false, true);
        let started = start_setup(Path::new("setup.exe"), Some(SILENT), &starter, &mut |line| lines.push(line));
        assert_eq!(started, Ok(SetupStart::Explorer));
        assert_eq!(*starter.calls.borrow(), vec![format!("runas {SILENT}"), format!("direct {SILENT}"), "explorer".to_string()]);
        assert!(lines.iter().any(|line| line.contains("cannot pass the setup arguments")), "{lines:?}");
    }

    #[test]
    #[cfg(windows)]
    fn the_arguments_are_one_command_line_text_not_split_arguments() {
        let command = command_with_arguments(Path::new("setup.exe"), Some(SILENT));
        assert_eq!(command.get_program(), "setup.exe");
        let args: Vec<_> = command.get_args().collect();
        assert_eq!(args, vec![std::ffi::OsStr::new(SILENT)], "one raw item: the text, unquoted, with /D= last");
        assert_eq!(command_with_arguments(Path::new("setup.exe"), None).get_args().count(), 0);
    }

    #[test]
    #[cfg(windows)]
    fn this_test_process_is_seen_and_a_made_up_name_is_not() {
        let me = std::env::current_exe().unwrap();
        let name = me.file_name().unwrap().to_string_lossy().into_owned();
        assert!(process_running(&[&name.to_uppercase()]));
        assert!(!process_running(&["gt4-no-such-process-091.exe"]));
    }
}
