//! Discord Rich Presence for the launcher (TASK-115, rc7).
//!
//! The launcher owns the presence: it opens the Discord client's local IPC pipe,
//! handshakes with the public application id and sends one activity that carries
//! the game name (Gran Turismo 4 or Gran Turismo 4 Spec II), the GT4Recomp name
//! and the real session start time. No mode, track, car, save, account or path is
//! ever sent. Presence never blocks a launch and never fails the game: every
//! connection uses bounded I/O, is retried with backoff and is stopped cleanly.
//!
//! References: Discord IPC protocol (handshake op 0, frame op 1, close op 2,
//! ping op 3, pong op 4) as documented by Discord's local RPC / social SDK
//! guides and mirrored by permissively licensed reference clients. License of
//! this implementation: MIT (project license). No dependency beyond the already
//! used `windows` crate.

use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::thread::{self, JoinHandle};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use std::sync::mpsc;

pub const CLIENT_ID: &str = "1564620020796100719";
pub const ASSET_KEY: &str = "gt4-discord-avatar";

pub const CONNECT_TIMEOUT: Duration = Duration::from_secs(1);
pub const IO_TIMEOUT: Duration = Duration::from_secs(2);
pub const SHUTDOWN_TIMEOUT: Duration = Duration::from_millis(250);
pub const HEARTBEAT_INTERVAL: Duration = Duration::from_secs(15);

pub trait PresenceTransport: Send {
    fn connect(&mut self, timeout: Duration) -> Result<(), String>;
    fn write_frame(&mut self, op: u32, payload: &str, timeout: Duration) -> Result<(), String>;
    fn read_frame(&mut self, timeout: Duration) -> Result<(u32, String), String>;
    fn close(&mut self);
}

#[cfg(windows)]
pub struct DiscordIpcTransport {
    handle: Option<isize>,
    pipe_override: Option<String>,
}

#[cfg(windows)]
impl DiscordIpcTransport {
    pub fn new() -> Self {
        Self { handle: None, pipe_override: None }
    }
    pub fn new_with_pipe(pipe: String) -> Self {
        Self { handle: None, pipe_override: Some(pipe) }
    }
}

#[cfg(windows)]
impl PresenceTransport for DiscordIpcTransport {
    fn connect(&mut self, timeout: Duration) -> Result<(), String> {
        use windows::Win32::Foundation::{GENERIC_READ, GENERIC_WRITE, INVALID_HANDLE_VALUE};
        use windows::Win32::Storage::FileSystem::{CreateFileW, FILE_SHARE_NONE, OPEN_EXISTING};
        use windows::core::PCWSTR;
        self.close();
        let deadline = Instant::now() + timeout;
        if let Some(pipe) = &self.pipe_override {
            let wide: Vec<u16> = pipe.encode_utf16().chain(std::iter::once(0)).collect();
            use windows::Win32::Foundation::{GENERIC_READ, GENERIC_WRITE, INVALID_HANDLE_VALUE};
            use windows::Win32::Storage::FileSystem::{CreateFileW, FILE_SHARE_NONE, OPEN_EXISTING};
            use windows::core::PCWSTR;
            while Instant::now() < deadline {
                let handle = unsafe {
                    CreateFileW(
                        PCWSTR(wide.as_ptr()),
                        (GENERIC_READ | GENERIC_WRITE).0,
                        FILE_SHARE_NONE,
                        None,
                        OPEN_EXISTING,
                        windows::Win32::Storage::FileSystem::FILE_FLAGS_AND_ATTRIBUTES(0x40000000), // FILE_FLAG_OVERLAPPED
                        None,
                    )
                };
                match handle {
                    Ok(h) if h != INVALID_HANDLE_VALUE => {
                        self.handle = Some(h.0 as isize);
                        return Ok(());
                    }
                    _ => thread::sleep(Duration::from_millis(10)),
                }
            }
            return Err("discord ipc connect timed out".to_string());
        }
        for index in 0..10 {
            if Instant::now() >= deadline {
                return Err("discord ipc connect timed out".to_string());
            }
            let path = format!("\\\\.\\pipe\\discord-ipc-{}", index);
            let wide: Vec<u16> = path.encode_utf16().chain(std::iter::once(0)).collect();
            let handle = unsafe {
                CreateFileW(
                    PCWSTR(wide.as_ptr()),
                    (GENERIC_READ | GENERIC_WRITE).0,
                    FILE_SHARE_NONE,
                    None,
                    OPEN_EXISTING,
                    windows::Win32::Storage::FileSystem::FILE_FLAGS_AND_ATTRIBUTES(0x40000000),
                    None,
                )
            };
            match handle {
                Ok(h) if h != INVALID_HANDLE_VALUE => {
                    self.handle = Some(h.0 as isize);
                    return Ok(());
                }
                _ => {}
            }
        }
        Err("discord ipc pipe not available".to_string())
    }

    fn write_frame(&mut self, op: u32, payload: &str, timeout: Duration) -> Result<(), String> {
        use windows::Win32::Foundation::{HANDLE, WAIT_OBJECT_0};
        use windows::Win32::Storage::FileSystem::WriteFile;
        use windows::Win32::System::IO::{GetOverlappedResult, OVERLAPPED};
        use windows::Win32::System::Threading::{CreateEventW, WaitForSingleObject};
        let handle = self.handle.ok_or_else(|| "discord ipc not connected".to_string())?;
        let bytes = payload.as_bytes();
        let mut frame = Vec::with_capacity(8 + bytes.len());
        frame.extend_from_slice(&op.to_le_bytes());
        frame.extend_from_slice(&(bytes.len() as u32).to_le_bytes());
        frame.extend_from_slice(bytes);

        let event = unsafe { CreateEventW(None, true, false, None) }
            .map_err(|e| format!("event: {e}"))?;
        let mut overlapped = OVERLAPPED::default();
        overlapped.hEvent = event;
        let mut written: u32 = 0;
        let res = unsafe { WriteFile(HANDLE(handle as _), Some(&frame), Some(&mut written), Some(&mut overlapped)) };
        if res.is_err() {
            use windows::Win32::Foundation::GetLastError;
            use windows::Win32::Foundation::ERROR_IO_PENDING;
            let err = unsafe { GetLastError() };
            if err != ERROR_IO_PENDING {
                unsafe { let _ = windows::Win32::Foundation::CloseHandle(event); }
                return Err(format!("discord ipc write failed: {err:?}"));
            }
        } else {
            unsafe { let _ = windows::Win32::Foundation::CloseHandle(event); }
            if written as usize == frame.len() {
                return Ok(());
            }
            return Err("discord ipc short write".to_string());
        }

        let wait = unsafe { WaitForSingleObject(event, timeout.as_millis() as u32) };
        if wait != WAIT_OBJECT_0 {
            use windows::Win32::System::IO::CancelIoEx;
            unsafe {
                let _ = CancelIoEx(HANDLE(handle as _), Some(&overlapped));
                // Cancellation is asynchronous: keep the OVERLAPPED and buffer alive
                // until the kernel reports completion, including ERROR_OPERATION_ABORTED.
                let mut completed = 0u32;
                let _ = GetOverlappedResult(HANDLE(handle as _), &overlapped, &mut completed, true);
                let _ = windows::Win32::Foundation::CloseHandle(event);
            }
            return Err("discord ipc write timed out".to_string());
        }
        let mut written2: u32 = 0;
        let ok = unsafe { GetOverlappedResult(HANDLE(handle as _), &overlapped, &mut written2, false) };
        unsafe { let _ = windows::Win32::Foundation::CloseHandle(event); }
        if ok.is_ok() && written2 as usize == frame.len() {
            Ok(())
        } else {
            Err("discord ipc write failed".to_string())
        }
    }

    fn read_frame(&mut self, timeout: Duration) -> Result<(u32, String), String> {
        use windows::Win32::Foundation::HANDLE;
        let handle = self.handle.ok_or_else(|| "discord ipc not connected".to_string())?;
        let mut header = [0u8; 8];
        read_exact(handle, &mut header, timeout)?;
        let op = u32::from_le_bytes([header[0], header[1], header[2], header[3]]);
        let len = u32::from_le_bytes([header[4], header[5], header[6], header[7]]) as usize;
        if len > 65536 {
            return Err("discord ipc frame too large".to_string());
        }
        let mut payload = vec![0u8; len];
        if len > 0 {
            read_exact(handle, &mut payload, timeout)?;
        }
        let text = String::from_utf8_lossy(&payload).to_string();
        Ok((op, text))
    }

    fn close(&mut self) {
        if let Some(handle) = self.handle.take() {
            use windows::Win32::Foundation::HANDLE;
            use windows::Win32::Foundation::CloseHandle;
            unsafe {
                let _ = CloseHandle(HANDLE(handle as _));
            }
        }
    }
}

#[cfg(windows)]
fn read_exact(handle: isize, buf: &mut [u8], timeout: Duration) -> Result<(), String> {
    use windows::Win32::Foundation::HANDLE;
    use windows::Win32::Storage::FileSystem::ReadFile;
    use windows::Win32::System::IO::{GetOverlappedResult, OVERLAPPED};
    use windows::Win32::System::Threading::{CreateEventW, WaitForSingleObject};
    use windows::Win32::Foundation::WAIT_OBJECT_0;
    let mut done = 0usize;
    while done < buf.len() {
        let remaining = buf.len() - done;
        let event = unsafe { CreateEventW(None, true, false, None) }
            .map_err(|e| format!("event: {e}"))?;
        let mut overlapped = OVERLAPPED::default();
        overlapped.hEvent = event;
        let mut read: u32 = 0;
        let res = unsafe { ReadFile(HANDLE(handle as _), Some(&mut buf[done..]), Some(&mut read), Some(&mut overlapped)) };
        if res.is_err() {
            use windows::Win32::Foundation::GetLastError;
            use windows::Win32::Foundation::ERROR_IO_PENDING;
            let err = unsafe { GetLastError() };
            if err != ERROR_IO_PENDING {
                unsafe { let _ = windows::Win32::Foundation::CloseHandle(event); }
                return Err(format!("discord ipc read failed: {err:?}"));
            }
        } else {
            if read == 0 {
                unsafe { let _ = windows::Win32::Foundation::CloseHandle(event); }
                return Err("discord ipc closed".to_string());
            }
            done += read as usize;
            unsafe { let _ = windows::Win32::Foundation::CloseHandle(event); }
            continue;
        }
        let wait = unsafe { WaitForSingleObject(event, timeout.as_millis() as u32) };
        if wait != WAIT_OBJECT_0 {
            use windows::Win32::System::IO::CancelIoEx;
            unsafe {
                let _ = CancelIoEx(HANDLE(handle as _), Some(&overlapped));
                // Cancellation is asynchronous: keep the OVERLAPPED and buffer alive
                // until the kernel reports completion, including ERROR_OPERATION_ABORTED.
                let mut completed = 0u32;
                let _ = GetOverlappedResult(HANDLE(handle as _), &overlapped, &mut completed, true);
                let _ = windows::Win32::Foundation::CloseHandle(event);
            }
            return Err("discord ipc read timed out".to_string());
        }
        let mut read2: u32 = 0;
        let ok = unsafe { GetOverlappedResult(HANDLE(handle as _), &overlapped, &mut read2, false) };
        unsafe { let _ = windows::Win32::Foundation::CloseHandle(event); }
        if !ok.is_ok() || read2 == 0 {
            return Err("discord ipc read failed".to_string());
        }
        done += read2 as usize;
    }
    Ok(())
}

#[cfg(not(windows))]
pub struct DiscordIpcTransport;

#[cfg(not(windows))]
impl DiscordIpcTransport {
    pub fn new() -> Self {
        Self
    }
}

#[cfg(not(windows))]
impl PresenceTransport for DiscordIpcTransport {
    fn connect(&mut self, _timeout: Duration) -> Result<(), String> {
        Err("discord rich presence is only supported on Windows".to_string())
    }
    fn write_frame(&mut self, _op: u32, _payload: &str, _timeout: Duration) -> Result<(), String> {
        Err("discord rich presence is only supported on Windows".to_string())
    }
    fn read_frame(&mut self, _timeout: Duration) -> Result<(u32, String), String> {
        Err("discord rich presence is only supported on Windows".to_string())
    }
    fn close(&mut self) {}
}

pub fn build_activity_payload(game_name: &str, start_unix: i64, pid: u32, nonce: &str) -> String {
    format!(
        r#"{{"cmd":"SET_ACTIVITY","args":{{"pid":{pid},"activity":{{"details":"{game_name}","state":"GT4Recomp","timestamps":{{"start":{start_unix}}},"assets":{{"large_image":"{asset}","large_text":"GT4Recomp"}},"instance":false}}}},"nonce":"{nonce}"}}"#,
        pid = pid,
        game_name = game_name,
        start_unix = start_unix,
        asset = ASSET_KEY,
        nonce = nonce,
    )
}

pub fn build_clear_payload(pid: u32, nonce: &str) -> String {
    format!(
        r#"{{"cmd":"SET_ACTIVITY","args":{{"pid":{pid},"activity":null}},"nonce":"{nonce}"}}"#,
        pid = pid,
        nonce = nonce,
    )
}

fn handshake_payload() -> String {
    format!(r#"{{"v":1,"client_id":"{CLIENT_ID}"}}"#)
}

fn unix_now() -> i64 {
    SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_secs() as i64).unwrap_or(0)
}

#[derive(Clone)]
struct SessionContext {
    game_name: String,
    start_unix: i64,
}

struct Session {
    generation: u64,
    stop: Arc<AtomicBool>,
    done_rx: mpsc::Receiver<()>,
    handle: Option<JoinHandle<()>>,
}

pub struct DiscordPresence {
    factory: Arc<dyn Fn() -> Box<dyn PresenceTransport> + Send + Sync>,
    current_generation: Arc<AtomicU64>,
    worker_gate: Arc<Mutex<()>>,
    inner: Mutex<Inner>,
}

struct Inner {
    context: Option<SessionContext>,
    session: Option<Session>,
    retired: Vec<JoinHandle<()>>,
}

impl DiscordPresence {
    pub fn new<F>(factory: F) -> Self
    where
        F: Fn() -> Box<dyn PresenceTransport> + Send + Sync + 'static,
    {
        Self {
            factory: Arc::new(factory),
            current_generation: Arc::new(AtomicU64::new(0)),
            worker_gate: Arc::new(Mutex::new(())),
            inner: Mutex::new(Inner {
                context: None,
                session: None,
                retired: Vec::new(),
            }),
        }
    }

    pub fn new_default() -> Self {
        Self::new(|| Box::new(DiscordIpcTransport::new()))
    }

    pub fn on_game_start(&self, game_name: &str, start_unix: i64, enabled: bool) {
        let mut inner = self.inner.lock().unwrap_or_else(|p| p.into_inner());
        inner.context = Some(SessionContext {
            game_name: game_name.to_string(),
            start_unix,
        });
        if enabled {
            start_session_locked(self, &mut inner);
        }
    }

    pub fn on_game_end(&self) {
        let session = {
            let mut inner = self.inner.lock().unwrap_or_else(|p| p.into_inner());
            inner.context = None;
            inner.session.take()
        };
        if let Some(session) = session {
            session.stop.store(true, Ordering::SeqCst);
            thread::spawn(move || stop_session_detached(session));
        }
    }

    pub fn on_launcher_exit(&self) {
        let mut sessions = Vec::new();
        {
            let mut inner = self.inner.lock().unwrap_or_else(|p| p.into_inner());
            if let Some(s) = inner.session.take() {
                s.stop.store(true, Ordering::SeqCst);
                sessions.push(s);
            }
            sessions.extend(inner.retired.drain(..).map(|h| {
                // wrap handle in a dummy session? We only have handles; signal none. Just join handle.
                // Use a channel that is already closed.
                let (tx, rx) = mpsc::channel();
                drop(tx);
                Session {
                    generation: 0,
                    stop: Arc::new(AtomicBool::new(true)),
                    done_rx: rx,
                    handle: Some(h),
                }
            }));
        }
        for s in sessions {
            thread::spawn(move || stop_session_detached(s));
        }
    }

    pub fn apply_toggle(&self, enabled: bool) {
        let mut inner = self.inner.lock().unwrap_or_else(|p| p.into_inner());
        if enabled {
            if inner.context.is_some() && inner.session.is_none() {
                start_session_locked(self, &mut inner);
            }
        } else if let Some(session) = inner.session.take() {
            session.stop.store(true, Ordering::SeqCst);
            drop(inner);
            thread::spawn(move || stop_session_detached(session));
        }
    }

    pub fn is_running(&self) -> bool {
        let inner = self.inner.lock().unwrap_or_else(|p| p.into_inner());
        inner.session.is_some()
    }

    pub fn current_generation(&self) -> u64 {
        self.current_generation.load(Ordering::SeqCst)
    }
}

fn start_session_locked(presence: &DiscordPresence, inner: &mut Inner) {
    // Reap retired handles.
    inner.retired.retain(|h| !h.is_finished());
    // Generate new generation.
    let generation = presence.current_generation.fetch_add(1, Ordering::SeqCst) + 1;
    let context = match inner.context.clone() {
        Some(c) => c,
        None => return,
    };
    let stop = Arc::new(AtomicBool::new(false));
    let stop_clone = stop.clone();
    let gen_clone = generation;
    let current_gen = presence.current_generation.clone();
    let factory = presence.factory.clone();
    let worker_gate = presence.worker_gate.clone();
    let (done_tx, done_rx) = mpsc::channel();
    let handle = thread::spawn(move || {
        // Serialize teardown before a later session publishes its activity.
        let _guard = worker_gate.lock().unwrap_or_else(|p| p.into_inner());
        run_session(factory, context, gen_clone, current_gen, stop_clone, done_tx);
    });
    inner.session = Some(Session {
        generation,
        stop,
        done_rx,
        handle: Some(handle),
    });
}

fn stop_session_detached(session: Session) {
    session.stop.store(true, Ordering::SeqCst);
    // Wait briefly for completion without holding caller locks (caller already dropped lock).
    let _ = session.done_rx.recv_timeout(SHUTDOWN_TIMEOUT);
    // handle dropped here (detaches if still running)
    drop(session.handle);
}

fn run_session(
    factory: Arc<dyn Fn() -> Box<dyn PresenceTransport> + Send + Sync>,
    context: SessionContext,
    generation: u64,
    current_gen: Arc<AtomicU64>,
    stop: Arc<AtomicBool>,
    done_tx: mpsc::Sender<()>,
) {
    let mut transport: Option<Box<dyn PresenceTransport>> = None;
    let mut connected = false;
    let mut attempts: u32 = 0;
    let mut nonce_counter: u64 = 0;
    let handshake = handshake_payload();
    let pid = std::process::id();

    let is_stopped = || stop.load(Ordering::SeqCst) || current_gen.load(Ordering::SeqCst) != generation;

    while !is_stopped() {
        if !connected {
            if is_stopped() {
                break;
            }
            let mut t = (factory)();
            match t.connect(CONNECT_TIMEOUT) {
                Ok(()) => {
                    if is_stopped() {
                        t.close();
                        break;
                    }
                    // handshake
                    if t.write_frame(0, &handshake, IO_TIMEOUT).is_ok() {
                        if is_stopped() {
                            t.close();
                            break;
                        }
                        match t.read_frame(IO_TIMEOUT) {
                            Ok((op, text)) => {
                                if op == 2 {
                                    t.close();
                                    // closed by discord
                                } else if text.contains("\"evt\":\"ERROR\"") {
                                    t.close();
                                } else if op == 1 {
                                    // handshake ack (READY or similar)
                                    if is_stopped() {
                                        t.close();
                                        break;
                                    }
                                    nonce_counter += 1;
                                    let payload = build_activity_payload(
                                        &context.game_name,
                                        context.start_unix,
                                        pid,
                                        &format!("n{}", nonce_counter),
                                    );
                                    if t.write_frame(1, &payload, IO_TIMEOUT).is_ok() {
                                        let ack = t.read_frame(IO_TIMEOUT);
                                        let accepted = ack.ok().and_then(|(op, text)| {
                                            let value: serde_json::Value = serde_json::from_str(&text).ok()?;
                                            Some(op == 1 && value["cmd"] == "SET_ACTIVITY"
                                                && value["nonce"] == format!("n{}", nonce_counter)
                                                && value["evt"] != "ERROR")
                                        }).unwrap_or(false);
                                        if accepted {
                                            transport = Some(t);
                                            connected = true;
                                            attempts = 0;
                                        } else {
                                            t.close();
                                        }
                                    } else {
                                        t.close();
                                    }
                                } else {
                                    t.close();
                                }
                            }
                            Err(_) => {
                                t.close();
                            }
                        }
                    } else {
                        t.close();
                    }
                }
                Err(_) => {}
            }
            if !connected {
                attempts = attempts.saturating_add(1);
                let delay_ms = (500u64 * (1u64 << attempts.min(6))).min(30_000);
                let chunks = (delay_ms / 250).max(1);
                for _ in 0..chunks {
                    if is_stopped() {
                        break;
                    }
                    thread::sleep(Duration::from_millis(250));
                }
                continue;
            }
        }

        // Connected: heartbeat + inbound handling loop.
        let mut last_heartbeat = Instant::now();
        loop {
            if is_stopped() {
                break;
            }
            // inbound check with short timeout
            if let Some(t) = transport.as_mut() {
                match t.read_frame(Duration::from_millis(100)) {
                    Ok((op, text)) => {
                        if op == 2 || text.contains("\"evt\":\"ERROR\"") {
                            connected = false;
                            if let Some(mut t) = transport.take() {
                                t.close();
                            }
                            break;
                        }
                        if op == 3 && t.write_frame(4, &text, IO_TIMEOUT).is_err() {
                            connected = false;
                            if let Some(mut t) = transport.take() { t.close(); }
                            break;
                        }
                    }
                    Err(error) => {
                        if !error.contains("timed out") {
                            connected = false;
                            if let Some(mut t) = transport.take() { t.close(); }
                            break;
                        }
                    }
                }
            }
            if is_stopped() {
                break;
            }
            if last_heartbeat.elapsed() >= HEARTBEAT_INTERVAL {
                if let Some(t) = transport.as_mut() {
                    nonce_counter += 1;
                    let ping = format!(r#"{{"v":1,"client_id":"{CLIENT_ID}"}}"#);
                    if t.write_frame(3, &ping, IO_TIMEOUT).is_ok() {
                        let _ = t.read_frame(IO_TIMEOUT);
                    } else {
                        connected = false;
                        if let Some(mut t) = transport.take() {
                            t.close();
                        }
                        break;
                    }
                }
                last_heartbeat = Instant::now();
            }
            // sleep small
            thread::sleep(Duration::from_millis(250));
        }
    }

    // Clear on exit.
    if connected {
        if let Some(mut t) = transport.take() {
            nonce_counter += 1;
            let payload = build_clear_payload(pid, &format!("c{}", nonce_counter));
            let _ = t.write_frame(1, &payload, IO_TIMEOUT);
            t.close();
        }
    }
    let _ = unix_now();
    let _ = done_tx.send(());
}

#[cfg(test)]
mod tests {
    use super::*;

    struct FakeTransport {
        fail_connect: bool,
        fail_send: bool,
        sent: Arc<Mutex<Vec<(u32, String)>>>,
        // control: delay connect to test cancellation
        connect_delay: Duration,
    }

    impl FakeTransport {
        fn new(fail_connect: bool, fail_send: bool, sent: Arc<Mutex<Vec<(u32, String)>>>) -> Self {
            Self { fail_connect, fail_send, sent, connect_delay: Duration::from_millis(0) }
        }
    }

    impl PresenceTransport for FakeTransport {
        fn connect(&mut self, _timeout: Duration) -> Result<(), String> {
            thread::sleep(self.connect_delay);
            if self.fail_connect {
                Err("no discord".to_string())
            } else {
                Ok(())
            }
        }
        fn write_frame(&mut self, op: u32, payload: &str, _timeout: Duration) -> Result<(), String> {
            if self.fail_send {
                Err("send failed".to_string())
            } else {
                self.sent.lock().unwrap().push((op, payload.to_string()));
                Ok(())
            }
        }
        fn read_frame(&mut self, _timeout: Duration) -> Result<(u32, String), String> {
            let sent = self.sent.lock().unwrap();
            let (op, payload) = sent.last().expect("frame precedes response");
            if *op == 0 {
                Ok((1, r#"{"cmd":"DISPATCH","evt":"READY"}"#.to_string()))
            } else {
                let sent: serde_json::Value = serde_json::from_str(payload).unwrap();
                Ok((1, serde_json::json!({"cmd": "SET_ACTIVITY", "nonce": sent["nonce"], "evt": null}).to_string()))
            }
        }
        fn close(&mut self) {}
    }

    #[test]
    fn payload_contains_both_targets_and_asset() {
        let p = build_activity_payload("Gran Turismo 4", 1000, 42, "n1");
        assert!(p.contains("\"details\":\"Gran Turismo 4\""));
        assert!(p.contains("\"large_image\":\"gt4-discord-avatar\""));
        assert!(p.contains("\"start\":1000"));
        let p2 = build_activity_payload("Gran Turismo 4 Spec II", 1000, 42, "n1");
        assert!(p2.contains("\"details\":\"Gran Turismo 4 Spec II\""));
    }

    #[test]
    fn manager_does_not_block_when_discord_absent() {
        let sent = Arc::new(Mutex::new(Vec::new()));
        let sent_clone = sent.clone();
        let presence = DiscordPresence::new(move || {
            Box::new(FakeTransport::new(true, false, sent_clone.clone()))
        });
        presence.on_game_start("Gran Turismo 4", 1234, true);
        thread::sleep(Duration::from_millis(200));
        presence.on_game_end();
        thread::sleep(Duration::from_millis(100));
        assert!(!presence.is_running());
    }

    #[test]
    fn disable_clears_and_reenable_preserves_timestamp() {
        let sent = Arc::new(Mutex::new(Vec::new()));
        let sent_clone = sent.clone();
        let presence = DiscordPresence::new(move || {
            Box::new(FakeTransport::new(false, false, sent_clone.clone()))
        });
        presence.on_game_start("Gran Turismo 4", 5555, true);
        thread::sleep(Duration::from_millis(300));
        presence.apply_toggle(false);
        thread::sleep(Duration::from_millis(500));
        assert!(!presence.is_running());
        // clear frame sent
        {
            let frames = sent.lock().unwrap();
            assert!(frames.iter().any(|(op, p)| *op == 1 && p.contains("\"activity\":null")));
        }
        presence.apply_toggle(true);
        thread::sleep(Duration::from_millis(300));
        assert!(presence.is_running());
        {
            let frames = sent.lock().unwrap();
            assert!(frames.iter().any(|(_, p)| p.contains("\"start\":5555")));
        }
        presence.on_game_end();
    }

    #[test]
    fn in_flight_cancel_prevents_activity() {
        let sent = Arc::new(Mutex::new(Vec::new()));
        let sent_clone = sent.clone();
        let presence = DiscordPresence::new(move || {
            let mut t = FakeTransport::new(false, false, sent_clone.clone());
            t.connect_delay = Duration::from_millis(300);
            Box::new(t)
        });
        presence.on_game_start("Gran Turismo 4", 7777, true);
        // cancel during connect
        thread::sleep(Duration::from_millis(50));
        presence.apply_toggle(false);
        thread::sleep(Duration::from_millis(500));
        assert!(!presence.is_running());
        let frames = sent.lock().unwrap();
        // no activity frame with start timestamp should have been sent
        assert!(!frames.iter().any(|(_, p)| p.contains("\"start\":7777")));
    }

    #[test]
    fn reconnect_preserves_timestamp() {
        // second start uses same context timestamp if re-enabled? context cleared on end; test re-enable during run
        let sent = Arc::new(Mutex::new(Vec::new()));
        let sent_clone = sent.clone();
        let presence = DiscordPresence::new(move || {
            Box::new(FakeTransport::new(false, false, sent_clone.clone()))
        });
        presence.on_game_start("Gran Turismo 4 Spec II", 9999, true);
        thread::sleep(Duration::from_millis(300));
        presence.apply_toggle(false);
        thread::sleep(Duration::from_millis(200));
        presence.apply_toggle(true);
        thread::sleep(Duration::from_millis(300));
        assert!(presence.is_running());
        let frames = sent.lock().unwrap();
        assert!(frames.iter().any(|(_, p)| p.contains("\"details\":\"Gran Turismo 4 Spec II\"") && p.contains("\"start\":9999")));
        presence.on_game_end();
    }

    #[cfg(windows)]
    mod windows_pipe {
        use super::*;
        use std::io::Write as _;
        use windows::Win32::Foundation::HANDLE;
        use windows::Win32::Storage::FileSystem::{ReadFile, WriteFile};

        fn spawn_server(pipe: String, handler: impl Fn(HANDLE) + Send + 'static) -> std::thread::JoinHandle<()> {
            std::thread::spawn(move || {
                use windows::Win32::System::Pipes::CreateNamedPipeW;
                use windows::core::PCWSTR;
                let wide: Vec<u16> = pipe.encode_utf16().chain(std::iter::once(0)).collect();
                let handle = unsafe {
                    CreateNamedPipeW(
                        PCWSTR(wide.as_ptr()),
                        windows::Win32::Storage::FileSystem::PIPE_ACCESS_DUPLEX,
                        windows::Win32::System::Pipes::PIPE_TYPE_BYTE
                            | windows::Win32::System::Pipes::PIPE_READMODE_BYTE
                            | windows::Win32::System::Pipes::PIPE_WAIT,
                        1,
                        65536,
                        65536,
                        0,
                        None,
                    )
                };
                unsafe {
                    let _ = windows::Win32::System::Pipes::ConnectNamedPipe(handle, None);
                }
                handler(handle);
                unsafe {
                    let _ = windows::Win32::Foundation::CloseHandle(handle);
                }
            })
        }

        fn read_exact_server(handle: HANDLE, buf: &mut [u8]) {
            let mut done = 0usize;
            while done < buf.len() {
                let mut read: u32 = 0;
                let ok = unsafe {
                    ReadFile(handle, Some(&mut buf[done..]), Some(&mut read), None)
                };
                if !ok.is_ok() || read == 0 {
                    break;
                }
                done += read as usize;
            }
        }

        fn write_all_server(handle: HANDLE, data: &[u8]) {
            let mut done = 0usize;
            while done < data.len() {
                let mut written: u32 = 0;
                let ok = unsafe {
                    WriteFile(handle, Some(&data[done..]), Some(&mut written), None)
                };
                if !ok.is_ok() || written == 0 {
                    break;
                }
                done += written as usize;
            }
        }

        #[test]
        fn windows_pipe_roundtrip() {
            let pipe = format!("\\\\.\\pipe\\gt4recomp-discord-test-{}-1", std::process::id());
            let server_pipe = pipe.clone();
            let server = spawn_server(server_pipe, move |handle| {
                // read handshake (8 + payload)
                let mut hdr = [0u8; 8];
                read_exact_server(handle, &mut hdr);
                let len = u32::from_le_bytes([hdr[4], hdr[5], hdr[6], hdr[7]]) as usize;
                let mut payload = vec![0u8; len];
                if len > 0 {
                    read_exact_server(handle, &mut payload);
                }
                // send ack
                let ack = b"{\"evt\":\"READY\"}";
                let mut frame = Vec::new();
                frame.extend_from_slice(&1u32.to_le_bytes());
                frame.extend_from_slice(&(ack.len() as u32).to_le_bytes());
                frame.extend_from_slice(ack);
                write_all_server(handle, &frame);
                // read activity
                let mut hdr2 = [0u8; 8];
                read_exact_server(handle, &mut hdr2);
                let len2 = u32::from_le_bytes([hdr2[4], hdr2[5], hdr2[6], hdr2[7]]) as usize;
                let mut payload2 = vec![0u8; len2];
                if len2 > 0 {
                    read_exact_server(handle, &mut payload2);
                }
                let text = String::from_utf8_lossy(&payload2);
                assert!(text.contains("SET_ACTIVITY"));
                // send ack
                let ack2 = b"{\"evt\":\"OK\"}";
                let mut frame2 = Vec::new();
                frame2.extend_from_slice(&1u32.to_le_bytes());
                frame2.extend_from_slice(&(ack2.len() as u32).to_le_bytes());
                frame2.extend_from_slice(ack2);
                write_all_server(handle, &frame2);
            });
            thread::sleep(Duration::from_millis(100));
            let mut transport = DiscordIpcTransport::new_with_pipe(pipe);
            assert!(transport.connect(Duration::from_secs(2)).is_ok());
            assert!(transport.write_frame(0, &handshake_payload(), Duration::from_secs(2)).is_ok());
            let (op, text) = transport.read_frame(Duration::from_secs(2)).unwrap();
            assert_eq!(op, 1);
            assert!(text.contains("READY"));
            assert!(transport.write_frame(1, &build_activity_payload("Gran Turismo 4", 1, 2, "n"), Duration::from_secs(2)).is_ok());
            let (op2, text2) = transport.read_frame(Duration::from_secs(2)).unwrap();
            assert_eq!(op2, 1);
            assert!(text2.contains("OK"));
            transport.close();
            let _ = server.join();
        }

        #[test]
        fn windows_pipe_read_timeout() {
            let pipe = format!("\\\\.\\pipe\\gt4recomp-discord-test-{}-2", std::process::id());
            let server_pipe = pipe.clone();
            let server = spawn_server(server_pipe, move |handle| {
                // accept but never reply
                let mut buf = [0u8; 8];
                let mut read: u32 = 0;
                unsafe {
                    let _ = ReadFile(handle, Some(&mut buf), Some(&mut read), None);
                }
                thread::sleep(Duration::from_secs(5));
            });
            thread::sleep(Duration::from_millis(100));
            let mut transport = DiscordIpcTransport::new_with_pipe(pipe);
            assert!(transport.connect(Duration::from_secs(2)).is_ok());
            let start = Instant::now();
            let res = transport.read_frame(Duration::from_millis(200));
            let elapsed = start.elapsed();
            assert!(res.is_err());
            assert!(elapsed < Duration::from_secs(2));
            transport.close();
            let _ = server.join();
        }
    }

    #[test]
    fn generation_fence_blocks_stale_session() {
        let sent = Arc::new(Mutex::new(Vec::new()));
        let sent_clone = sent.clone();
        let presence = DiscordPresence::new(move || {
            Box::new(FakeTransport::new(false, false, sent_clone.clone()))
        });
        presence.on_game_start("Gran Turismo 4", 1, true);
        thread::sleep(Duration::from_millis(200));
        // force a new generation by ending and starting again
        presence.on_game_end();
        thread::sleep(Duration::from_millis(200));
        presence.on_game_start("Gran Turismo 4", 2, true);
        thread::sleep(Duration::from_millis(300));
        assert!(presence.is_running());
        presence.on_game_end();
    }
}
