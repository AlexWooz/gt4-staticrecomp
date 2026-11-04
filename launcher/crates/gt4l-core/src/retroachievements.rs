//! RetroAchievements configuration, credential persistence, and `dorequest.php` client.

//!

//! Credentials (`username`, `token`) and options (`enabled`, `hardcore`, `encore`) are stored in

//! `%APPDATA%\GT4Recomp\config\retroachievements.toml` ([`crate::paths::UserDirs::retroachievements_file`]).

//! At launch, [`RaConfig::launch_environment`] passes `GT4_RA_*` environment variables to `gt4vulkan.exe`.



use std::collections::HashSet;

use std::ffi::OsString;

use std::fs;
use std::sync::atomic::{AtomicBool, AtomicU64, AtomicUsize, Ordering};
use std::sync::Arc;

use std::path::Path;

use std::time::Duration;



use serde::{Deserialize, Serialize};



/// Official RetroAchievements MD5 hash for Gran Turismo 4 (NTSC-U, SCUS-97328 v2.00).

pub const GT4_NTSC_U_RA_HASH: &str = "b7e2e47572f4163a216ea85eed3c6c63";



/// Official RetroAchievements Game ID for Gran Turismo 4 (PlayStation 2).

pub const GT4_RA_GAME_ID: u32 = 20580;



/// Default RetroAchievements server base URL.

pub const DEFAULT_RA_BASE_URL: &str = "https://retroachievements.org";



const REQUEST_TIMEOUT: Duration = Duration::from_secs(12);



fn default_true() -> bool {

    true

}



/// Category of token error to distinguish migration/storage failures from inaccessible sessions.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub enum TokenErrorKind {
    /// An encrypted token envelope exists on disk but could not be decrypted (e.g. wrong user, wrong machine, corrupt).
    Inaccessible,
    /// A legacy plaintext token exists on disk but could not be protected and migrated.
    MigrationFailed,
    /// Saving or protecting updated credentials failed.
    SaveFailed,
}

/// Persistent RetroAchievements configuration saved in `retroachievements.toml`.
#[derive(Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct RaConfig {
    #[serde(default = "default_true")]
    pub enabled: bool,

    #[serde(default)]
    pub hardcore: bool,

    #[serde(default)]
    pub encore: bool,

    #[serde(default)]
    pub username: String,

    #[serde(default, skip_serializing)]
    pub token: String,

    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub token_protected: Option<String>,

    #[serde(default)]
    pub display_name: String,

    #[serde(default)]
    pub score: u32,

    #[serde(default)]
    pub score_softcore: u32,

    #[serde(skip)]
    pub token_error: Option<TokenErrorKind>,

    #[serde(skip)]
    pub unmigrated_plaintext: Option<String>,
}

impl std::fmt::Debug for RaConfig {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("RaConfig")
            .field("enabled", &self.enabled)
            .field("hardcore", &self.hardcore)
            .field("encore", &self.encore)
            .field("username", &self.username)
            .field("token", if self.token.is_empty() { &"[EMPTY]" } else { &"[REDACTED]" })
            .field("token_protected", &self.token_protected.as_ref().map(|s| {
                if let Some(prefix_end) = s.find(':').and_then(|p| s[p + 1..].find(':').map(|p2| p + 1 + p2 + 1)) {
                    format!("{}[{} chars]", &s[..prefix_end], s.len() - prefix_end)
                } else {
                    "[PROTECTED]".to_string()
                }
            }))
            .field("display_name", &self.display_name)
            .field("score", &self.score)
            .field("score_softcore", &self.score_softcore)
            .field("token_error", &self.token_error)
            .field("unmigrated_plaintext", if self.unmigrated_plaintext.is_some() { &"[PRESENT]" } else { &"[NONE]" })
            .finish()
    }
}

impl Default for RaConfig {
    fn default() -> Self {
        Self {
            enabled: true,
            hardcore: false,
            encore: false,
            username: String::new(),
            token: String::new(),
            token_protected: None,
            display_name: String::new(),
            score: 0,
            score_softcore: 0,
            token_error: None,
            unmigrated_plaintext: None,
        }
    }
}

static TMP_FILE_COUNTER: AtomicU64 = AtomicU64::new(1);

/// Internal helper for tests to inspect the upcoming temporary file counter.
#[doc(hidden)]
pub fn next_temp_counter_for_test() -> u64 {
    TMP_FILE_COUNTER.load(Ordering::Relaxed)
}

struct TempFileGuard<'a> {
    path: &'a Path,
    disarmed: bool,
}

impl<'a> TempFileGuard<'a> {
    fn arm(path: &'a Path) -> Self {
        Self {
            path,
            disarmed: false,
        }
    }

    fn disarm(&mut self) {
        self.disarmed = true;
    }
}

impl<'a> Drop for TempFileGuard<'a> {
    fn drop(&mut self) {
        if !self.disarmed {
            let _ = fs::remove_file(self.path);
        }
    }
}

/// Injectable filesystem seam for atomic file writes and replacements.
pub trait AtomicFileOps: Send + Sync {
    /// Opens/creates a temporary file exclusively (fails if file already exists).
    fn open_exclusive(&self, temp_path: &Path) -> Result<fs::File, std::io::Error>;
    /// Writes content and flushes/syncs the temporary file. Closes the file handle before returning.
    fn write_and_flush(&self, file: fs::File, temp_path: &Path, content: &[u8]) -> Result<(), std::io::Error>;
    /// Replaces the target file with the temporary file atomically.
    fn rename_replace(&self, temp_path: &Path, target: &Path) -> Result<(), std::io::Error>;
}

/// Standard production implementation of [`AtomicFileOps`] using `std::fs`.
#[derive(Clone, Copy, Debug, Default)]
pub struct StdAtomicFileOps;

impl AtomicFileOps for StdAtomicFileOps {
    fn open_exclusive(&self, temp_path: &Path) -> Result<fs::File, std::io::Error> {
        fs::OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(temp_path)
    }

    fn write_and_flush(&self, mut file: fs::File, _temp_path: &Path, content: &[u8]) -> Result<(), std::io::Error> {
        use std::io::Write;
        file.write_all(content)?;
        file.sync_all()?;
        drop(file);
        Ok(())
    }

    fn rename_replace(&self, temp_path: &Path, target: &Path) -> Result<(), std::io::Error> {
        fs::rename(temp_path, target)
    }
}

/// Mock implementation of [`AtomicFileOps`] for deterministic failure injection in tests.
#[derive(Clone, Debug, Default)]
pub struct MockAtomicFileOps {
    pub fail_open: bool,
    pub collision_remaining: Arc<AtomicUsize>,
    pub fail_during_write: Option<usize>,
    pub fail_flush: bool,
    pub fail_replace: bool,
    pub partial_write_reached: Arc<AtomicBool>,
    pub collision_retries_reached: Arc<AtomicUsize>,
}

impl MockAtomicFileOps {
    pub fn with_collisions(collisions: usize) -> Self {
        Self {
            collision_remaining: Arc::new(AtomicUsize::new(collisions)),
            ..Default::default()
        }
    }
}

impl AtomicFileOps for MockAtomicFileOps {
    fn open_exclusive(&self, temp_path: &Path) -> Result<fs::File, std::io::Error> {
        if self.collision_remaining.load(Ordering::SeqCst) > 0 {
            self.collision_remaining.fetch_sub(1, Ordering::SeqCst);
            self.collision_retries_reached.fetch_add(1, Ordering::SeqCst);
            return Err(std::io::Error::new(
                std::io::ErrorKind::AlreadyExists,
                "simulated name collision in open_exclusive",
            ));
        }
        if self.fail_open {
            return Err(std::io::Error::new(
                std::io::ErrorKind::PermissionDenied,
                "injected fail_open",
            ));
        }
        StdAtomicFileOps.open_exclusive(temp_path)
    }

    fn write_and_flush(&self, mut file: fs::File, temp_path: &Path, content: &[u8]) -> Result<(), std::io::Error> {
        use std::io::Write;
        if let Some(partial_len) = self.fail_during_write {
            self.partial_write_reached.store(true, Ordering::SeqCst);
            let slice = &content[..partial_len.min(content.len())];
            file.write_all(slice)?;
            file.flush()?;
            drop(file);
            return Err(std::io::Error::new(
                std::io::ErrorKind::WriteZero,
                "injected partial write failure",
            ));
        }
        if self.fail_flush {
            file.write_all(content)?;
            drop(file);
            return Err(std::io::Error::new(
                std::io::ErrorKind::Other,
                "injected flush failure",
            ));
        }
        StdAtomicFileOps.write_and_flush(file, temp_path, content)
    }

    fn rename_replace(&self, temp_path: &Path, target: &Path) -> Result<(), std::io::Error> {
        if self.fail_replace {
            return Err(std::io::Error::new(
                std::io::ErrorKind::PermissionDenied,
                "injected fail_replace",
            ));
        }
        StdAtomicFileOps.rename_replace(temp_path, target)
    }
}

impl RaConfig {
    /// Loads `retroachievements.toml` from disk using the default system secret protector (DPAPI on Windows).
    pub fn load(path: &Path) -> Self {
        Self::load_with_custom(path, &crate::secret::default_protector(), &StdAtomicFileOps)
    }

    /// Loads `retroachievements.toml` from disk, unprotecting tokens or migrating legacy plaintext configurations
    /// using the provided [`crate::secret::SecretProtector`].
    pub fn load_with(path: &Path, protector: &dyn crate::secret::SecretProtector) -> Self {
        Self::load_with_custom(path, protector, &StdAtomicFileOps)
    }

    /// Loads `retroachievements.toml` from disk with custom protector and filesystem seam for deterministic tests.
    pub fn load_with_custom(
        path: &Path,
        protector: &dyn crate::secret::SecretProtector,
        ops: &dyn AtomicFileOps,
    ) -> Self {
        let Ok(contents) = fs::read_to_string(path) else {
            return Self::default();
        };

        let mut cfg: RaConfig = match toml::from_str(&contents) {
            Ok(c) => c,
            Err(_) => return Self::default(),
        };

        // 1. Protected token envelope exists on disk
        if let Some(ref envelope) = cfg.token_protected {
            if !envelope.trim().is_empty() {
                match protector.unprotect_secret(envelope) {
                    Ok(unprotected) => {
                        cfg.token = unprotected;
                        cfg.token_error = None;
                        cfg.unmigrated_plaintext = None;
                    }
                    Err(_) => {
                        cfg.token = String::new();
                        cfg.token_error = Some(TokenErrorKind::Inaccessible);
                        cfg.unmigrated_plaintext = None;
                    }
                }
                return cfg;
            }
        }

        // 2. Legacy plaintext token exists on disk (migration from 1.2.4/1.2.5/rc7)
        if !cfg.token.trim().is_empty() {
            let plaintext = cfg.token.trim().to_string();
            match protector.protect_secret(&plaintext) {
                Ok(envelope) => {
                    match protector.unprotect_secret(&envelope) {
                        Ok(verified) if verified == plaintext => {
                            cfg.token_protected = Some(envelope);
                            let mut disk_cfg = cfg.clone();
                            disk_cfg.token = String::new();
                            disk_cfg.unmigrated_plaintext = None;
                            if let Err(_) = disk_cfg.write_disk_atomically_with(path, ops) {
                                cfg.token = String::new();
                                cfg.token_protected = None;
                                cfg.token_error = Some(TokenErrorKind::MigrationFailed);
                                cfg.unmigrated_plaintext = Some(plaintext);
                            } else {
                                cfg.token_error = None;
                                cfg.unmigrated_plaintext = None;
                            }
                        }
                        _ => {
                            cfg.token = String::new();
                            cfg.token_protected = None;
                            cfg.token_error = Some(TokenErrorKind::MigrationFailed);
                            cfg.unmigrated_plaintext = Some(plaintext);
                        }
                    }
                }
                Err(_) => {
                    cfg.token = String::new();
                    cfg.token_protected = None;
                    cfg.token_error = Some(TokenErrorKind::MigrationFailed);
                    cfg.unmigrated_plaintext = Some(plaintext);
                }
            }
        }

        cfg
    }

    pub fn write_disk_atomically(&self, path: &Path) -> Result<(), String> {
        self.write_disk_atomically_with(path, &StdAtomicFileOps)
    }

    /// Writes `retroachievements.toml` atomically with exclusive temporary file creation,
    /// RAII cleanup guard armed strictly on owned temporary files, and injectable filesystem seam.
    ///
    /// ## Concurrency Notice
    /// Unique temporary files prevent collisions and ensure that one process never overwrites or
    /// deletes another process's in-progress write. However, separate running launcher instances
    /// mutating the same configuration file are serialized at the file-system replacement boundary
    /// under last-write-wins semantics.
    pub fn write_disk_atomically_with(&self, path: &Path, ops: &dyn AtomicFileOps) -> Result<(), String> {
        let parent = path.parent().unwrap_or_else(|| Path::new("."));
        fs::create_dir_all(parent).map_err(|e| format!("cannot create config folder: {e}"))?;

        let file_name = path.file_name().and_then(|n| n.to_str()).unwrap_or("retroachievements.toml");
        let body = toml::to_string_pretty(self).map_err(|e| format!("cannot format retroachievements.toml: {e}"))?;

        const MAX_TEMP_ATTEMPTS: usize = 10;
        let mut last_collision_err = None;

        let (file, tmp) = {
            let mut chosen = None;
            for _ in 0..MAX_TEMP_ATTEMPTS {
                let tmp_name = format!(
                    ".{}.tmp-{}-{}",
                    file_name,
                    std::process::id(),
                    TMP_FILE_COUNTER.fetch_add(1, Ordering::Relaxed)
                );
                let cand = parent.join(tmp_name);
                match ops.open_exclusive(&cand) {
                    Ok(f) => {
                        chosen = Some((f, cand));
                        break;
                    }
                    Err(e) if e.kind() == std::io::ErrorKind::AlreadyExists => {
                        last_collision_err = Some(e);
                        continue;
                    }
                    Err(e) => return Err(format!("cannot create temporary file {}: {e}", cand.display())),
                }
            }
            match chosen {
                Some(pair) => pair,
                None => {
                    return Err(format!(
                        "cannot allocate unique temporary file in {} after {MAX_TEMP_ATTEMPTS} attempts: {}",
                        parent.display(),
                        last_collision_err.map(|e| e.to_string()).unwrap_or_else(|| "collision".to_string())
                    ));
                }
            }
        };

        // Guard is armed ONLY AFTER exclusive creation of `tmp` succeeds.
        // It ensures only the file created by this attempt is cleaned up if writing fails.
        let mut guard = TempFileGuard::arm(&tmp);

        ops.write_and_flush(file, &tmp, body.as_bytes())
            .map_err(|e| format!("cannot write {}: {e}", tmp.display()))?;

        ops.rename_replace(&tmp, path)
            .map_err(|e| format!("cannot save {}: {e}", path.display()))?;

        guard.disarm();
        Ok(())
    }

    /// Writes `retroachievements.toml` atomically using the default system secret protector (DPAPI on Windows).
    pub fn save(&self, path: &Path) -> Result<(), String> {
        self.save_with_custom(path, &crate::secret::default_protector(), &StdAtomicFileOps)
    }

    /// Writes `retroachievements.toml` atomically, protecting any non-empty in-memory token using
    /// the provided [`crate::secret::SecretProtector`].
    pub fn save_with(&self, path: &Path, protector: &dyn crate::secret::SecretProtector) -> Result<(), String> {
        self.save_with_custom(path, protector, &StdAtomicFileOps)
    }

    /// Writes `retroachievements.toml` atomically with custom secret protector and filesystem seam.
    pub fn save_with_custom(
        &self,
        path: &Path,
        protector: &dyn crate::secret::SecretProtector,
        ops: &dyn AtomicFileOps,
    ) -> Result<(), String> {
        let mut disk_cfg = self.clone();
        disk_cfg.token = String::new();

        // If an unmigrated legacy plaintext token previously failed protection:
        if let Some(ref legacy) = self.unmigrated_plaintext {
            if self.token.trim().is_empty() {
                match protector.protect_secret(legacy) {
                    Ok(envelope) => {
                        match protector.unprotect_secret(&envelope) {
                            Ok(verified) if verified == *legacy => {
                                disk_cfg.token_protected = Some(envelope);
                                disk_cfg.unmigrated_plaintext = None;
                            }
                            _ => {
                                return Err("cannot save settings: existing legacy credentials could not be protected, preserving original configuration file".into());
                            }
                        }
                    }
                    Err(e) => {
                        return Err(format!("cannot save settings: existing legacy credentials could not be protected ({e}), preserving original configuration file"));
                    }
                }
            }
        }

        if !self.token.trim().is_empty() {
            let envelope = protector.protect_secret(self.token.trim())
                .map_err(|e| format!("cannot protect RetroAchievements token: {e}"))?;
            let roundtrip = protector.unprotect_secret(&envelope)
                .map_err(|e| format!("cannot verify protected RetroAchievements token: {e}"))?;
            if roundtrip != self.token.trim() {
                return Err("roundtrip verification of protected RetroAchievements token failed".into());
            }
            disk_cfg.token_protected = Some(envelope);
            disk_cfg.unmigrated_plaintext = None;
        } else if self.token_protected.is_none() && self.unmigrated_plaintext.is_none() {
            disk_cfg.token_protected = None;
        }

        disk_cfg.write_disk_atomically_with(path, ops)
    }

    /// Whether an authenticated username and API token are stored.
    pub fn is_signed_in(&self) -> bool {
        !self.username.trim().is_empty() && !self.token.trim().is_empty() && self.token_error.is_none()
    }

    /// Clears stored credentials while preserving the user's mode toggles.
    pub fn clear_credentials(&mut self) {
        self.username.clear();
        self.token.clear();
        self.token_protected = None;
        self.token_error = None;
        self.unmigrated_plaintext = None;
        self.display_name.clear();
        self.score = 0;
        self.score_softcore = 0;
    }



    /// Environment variables passed to `gt4vulkan.exe` when RetroAchievements is enabled and signed in.

    pub fn launch_environment(&self, cache_dir: &Path, language: crate::language::Language) -> Vec<(String, OsString)> {

        if !self.enabled || !self.is_signed_in() {

            return Vec::new();

        }

        let mut vars = vec![

            ("GT4_RA_ENABLED".to_string(), OsString::from("1")),

            (

                "GT4_RA_HARDCORE".to_string(),

                OsString::from(if self.hardcore { "1" } else { "0" }),

            ),

            (

                "GT4_RA_ENCORE".to_string(),

                OsString::from(if self.encore { "1" } else { "0" }),

            ),

            ("GT4_RA_USERNAME".to_string(), OsString::from(self.username.trim())),

            ("GT4_RA_TOKEN".to_string(), OsString::from(self.token.trim())),

            ("GT4_RA_HASH".to_string(), OsString::from(GT4_NTSC_U_RA_HASH)),

            ("GT4_RA_CACHE_DIR".to_string(), cache_dir.as_os_str().to_os_string()),

            (

                "GT4_RA_LANGUAGE".to_string(),

                OsString::from(if language.tag() == "pt-BR" { "pt-BR" } else { "en-US" }),

            ),

        ];

        if let Ok(custom_host) = std::env::var("GT4_RA_HOST") {

            let trimmed = custom_host.trim();

            if !trimmed.is_empty() {

                vars.push(("GT4_RA_HOST".to_string(), OsString::from(trimmed)));

            }

        }

        vars

    }

}



/// Summary of an individual achievement in the Gran Turismo 4 set.

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]

pub struct RaAchievementSummary {

    pub id: u32,

    pub title: String,

    pub description: String,

    pub points: u32,

    pub unlocked: bool,

    pub unlocked_hardcore: bool,

    pub badge_name: String,

    pub type_label: String,

}



/// Overview of the player's Gran Turismo 4 progress on RetroAchievements.

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]

pub struct RaGameOverview {

    pub game_id: u32,

    pub title: String,

    pub total_achievements: u32,

    pub unlocked_achievements: u32,

    pub total_points: u32,

    pub unlocked_points: u32,

    pub achievements: Vec<RaAchievementSummary>,

}



/// Account details returned by `r=login2`.

#[derive(Clone, PartialEq, Eq)]
pub struct RaAccountSession {
    pub username: String,
    pub display_name: String,
    pub token: String,
    pub score: u32,
    pub score_softcore: u32,
}

impl std::fmt::Debug for RaAccountSession {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("RaAccountSession")
            .field("username", &self.username)
            .field("display_name", &self.display_name)
            .field("token", if self.token.is_empty() { &"[EMPTY]" } else { &"[REDACTED]" })
            .field("score", &self.score)
            .field("score_softcore", &self.score_softcore)
            .finish()
    }
}



/// Percent-encodes a form parameter value according to RFC 3986.

pub fn url_encode(input: &str) -> String {

    let mut out = String::with_capacity(input.len());

    for byte in input.bytes() {

        match byte {

            b'A'..=b'Z' | b'a'..=b'z' | b'0'..=b'9' | b'-' | b'_' | b'.' | b'~' => {

                out.push(byte as char);

            }

            b' ' => out.push_str("%20"),

            _ => {

                out.push_str(&format!("%{byte:02X}"));

            }

        }

    }

    out

}



fn dorequest_post(base_url: &str, form_body: &str) -> Result<String, String> {

    let url = format!("{}/dorequest.php", base_url.trim_end_matches('/'));

    let agent = ureq::Agent::new_with_config(

        ureq::config::Config::builder()

            .timeout_global(Some(REQUEST_TIMEOUT))

            .http_status_as_error(false)

            .build(),

    );

    let mut response = agent

        .post(&url)

        .header("User-Agent", &format!("GT4Recomp/{}", crate::LAUNCHER_VERSION))

        .header("Content-Type", "application/x-www-form-urlencoded")

        .send(form_body.as_bytes())

        .map_err(|e| format!("Could not reach RetroAchievements: {e}"))?;

    response

        .body_mut()

        .read_to_string()

        .map_err(|e| format!("Invalid response from RetroAchievements: {e}"))

}



#[derive(Deserialize)]

struct RawLoginResponse {

    #[serde(rename = "Success", default)]

    success: bool,

    #[serde(rename = "User", default)]

    user: Option<String>,

    #[serde(rename = "Token", default)]

    token: Option<String>,

    #[serde(rename = "Score", default)]

    score: u32,

    #[serde(rename = "SoftcoreScore", default)]

    softcore_score: u32,

    #[serde(rename = "Error", default)]

    error: Option<String>,

}



/// Parses a `r=login2` JSON response from `dorequest.php`.

pub fn parse_login_response(json_text: &str, fallback_user: &str) -> Result<RaAccountSession, String> {

    let parsed: RawLoginResponse = serde_json::from_str(json_text)

        .map_err(|_| "Unexpected response from RetroAchievements.".to_string())?;

    if !parsed.success {

        return Err(parsed

            .error

            .filter(|msg| !msg.trim().is_empty())

            .unwrap_or_else(|| "Invalid RetroAchievements username or password.".to_string()));

    }

    let token = parsed

        .token

        .filter(|t| !t.trim().is_empty())

        .ok_or_else(|| "RetroAchievements did not return a session token.".to_string())?;

    let display_name = parsed

        .user

        .filter(|u| !u.trim().is_empty())

        .unwrap_or_else(|| fallback_user.trim().to_string());

    Ok(RaAccountSession {

        username: fallback_user.trim().to_string(),

        display_name,

        token,

        score: parsed.score,

        score_softcore: parsed.softcore_score,

    })

}



/// Signs in with a RetroAchievements username and password via `r=login2`.

pub fn login_with_password(base_url: &str, username: &str, password: &str) -> Result<RaAccountSession, String> {

    let user_trimmed = username.trim();

    if user_trimmed.is_empty() {

        return Err("Enter your RetroAchievements username.".to_string());

    }

    if password.is_empty() {

        return Err("Enter your RetroAchievements password.".to_string());

    }

    let body = format!(

        "r=login2&u={}&p={}",

        url_encode(user_trimmed),

        url_encode(password)

    );

    let json = dorequest_post(base_url, &body)?;

    parse_login_response(&json, user_trimmed)

}



/// Refreshes user score and verifies the stored token via `r=login2`.

pub fn login_with_token(base_url: &str, username: &str, token: &str) -> Result<RaAccountSession, String> {

    let user_trimmed = username.trim();

    let token_trimmed = token.trim();

    if user_trimmed.is_empty() || token_trimmed.is_empty() {

        return Err("Not signed in to RetroAchievements.".to_string());

    }

    let body = format!(

        "r=login2&u={}&t={}",

        url_encode(user_trimmed),

        url_encode(token_trimmed)

    );

    let json = dorequest_post(base_url, &body)?;

    parse_login_response(&json, user_trimmed)

}



#[derive(Deserialize)]

struct RawPatchEnvelope {

    #[serde(rename = "Success", default)]

    success: bool,

    #[serde(rename = "Error", default)]

    error: Option<String>,

    #[serde(rename = "PatchData", default)]

    patch_data: Option<RawPatchData>,

}



#[derive(Deserialize)]

struct RawPatchData {

    #[serde(rename = "ID", default)]

    id: u32,

    #[serde(rename = "Title", default)]

    title: String,

    #[serde(rename = "Achievements", default)]

    achievements: Vec<RawAchievementDef>,

}



#[derive(Deserialize)]

struct RawAchievementDef {

    #[serde(rename = "ID", default)]

    id: u32,

    #[serde(rename = "Title", default)]

    title: String,

    #[serde(rename = "Description", default)]

    description: String,

    #[serde(rename = "Points", default)]

    points: u32,

    #[serde(rename = "Flags", default)]

    flags: u32,

    #[serde(rename = "BadgeName", default)]

    badge_name: String,

    #[serde(rename = "Type", default)]

    ach_type: Option<String>,

}



#[derive(Deserialize)]

struct RawStartSessionResponse {

    #[serde(rename = "Success", default)]

    success: bool,

    #[serde(rename = "Unlocks", default)]

    unlocks: Vec<RawUnlockEntry>,

    #[serde(rename = "HardcoreUnlocks", default)]

    hardcore_unlocks: Vec<RawUnlockEntry>,

}



#[derive(Deserialize)]

struct RawUnlockEntry {

    #[serde(rename = "ID", default)]

    id: u32,

}



/// Combines `r=patch` and `r=startsession` JSON responses into a [`RaGameOverview`].

pub fn parse_game_overview(patch_json: &str, session_json: &str) -> Result<RaGameOverview, String> {

    let patch_env: RawPatchEnvelope = serde_json::from_str(patch_json)

        .map_err(|_| "Could not parse achievement data from RetroAchievements.".to_string())?;

    if !patch_env.success {

        return Err(patch_env

            .error

            .unwrap_or_else(|| "RetroAchievements rejected the achievement set request.".to_string()));

    }

    let patch = patch_env

        .patch_data

        .ok_or_else(|| "RetroAchievements returned no achievement data for Gran Turismo 4.".to_string())?;



    let mut softcore_ids = HashSet::new();

    let mut hardcore_ids = HashSet::new();

    if let Ok(sess) = serde_json::from_str::<RawStartSessionResponse>(session_json) {

        if sess.success {

            for entry in sess.unlocks {

                softcore_ids.insert(entry.id);

            }

            for entry in sess.hardcore_unlocks {

                hardcore_ids.insert(entry.id);

                softcore_ids.insert(entry.id);

            }

        }

    }



    let mut total_achievements = 0u32;

    let mut unlocked_achievements = 0u32;

    let mut total_points = 0u32;

    let mut unlocked_points = 0u32;

    let mut achievements = Vec::new();



    for def in patch.achievements {

        // Flags == 3 is Core (promoted) achievements in rcheevos.

        if def.flags != 3 {

            continue;

        }

        let unlocked_hc = hardcore_ids.contains(&def.id);

        let unlocked = unlocked_hc || softcore_ids.contains(&def.id);

        total_achievements += 1;

        total_points += def.points;

        if unlocked {

            unlocked_achievements += 1;

            unlocked_points += def.points;

        }

        let type_label = match def.ach_type.as_deref().unwrap_or("") {

            "progression" => "Progression",

            "win_condition" => "Win Condition",

            "missable" => "Missable",

            _ => "",

        }

        .to_string();



        achievements.push(RaAchievementSummary {

            id: def.id,

            title: def.title,

            description: def.description,

            points: def.points,

            unlocked,

            unlocked_hardcore: unlocked_hc,

            badge_name: def.badge_name,

            type_label,

        });

    }



    Ok(RaGameOverview {

        game_id: if patch.id != 0 { patch.id } else { GT4_RA_GAME_ID },

        title: if patch.title.is_empty() {

            "Gran Turismo 4".to_string()

        } else {

            patch.title

        },

        total_achievements,

        unlocked_achievements,

        total_points,

        unlocked_points,

        achievements,

    })

}



/// Fetches the player's Gran Turismo 4 achievement list and unlock progress from RetroAchievements.

pub fn fetch_game_overview(base_url: &str, username: &str, token: &str) -> Result<RaGameOverview, String> {

    let u_enc = url_encode(username.trim());

    let t_enc = url_encode(token.trim());

    let patch_body = format!("r=patch&u={u_enc}&t={t_enc}&g={GT4_RA_GAME_ID}");

    let session_body = format!("r=startsession&u={u_enc}&t={t_enc}&g={GT4_RA_GAME_ID}&h=1&m={GT4_NTSC_U_RA_HASH}");

    let patch_json = dorequest_post(base_url, &patch_body)?;

    let session_json = dorequest_post(base_url, &session_body).unwrap_or_default();

    parse_game_overview(&patch_json, &session_json)

}



#[cfg(test)]

mod tests {

    use super::*;

    use std::path::PathBuf;



    fn scratch_file(name: &str) -> PathBuf {

        let dir = std::env::temp_dir().join(format!("gt4l-ra-{}-{name}", std::process::id()));

        let _ = fs::remove_dir_all(&dir);

        fs::create_dir_all(&dir).unwrap();

        dir.join("retroachievements.toml")

    }



    #[test]
    fn config_round_trips_and_clears_credentials_cleanly() {
        let file = scratch_file("roundtrip");
        let mut cfg = RaConfig::default();
        assert!(cfg.enabled);
        assert!(!cfg.is_signed_in());

        cfg.username = "DriverOne".to_string();
        cfg.display_name = "DriverOne".to_string();
        cfg.token = "tok_secret_123".to_string();
        cfg.hardcore = true;
        cfg.encore = true;
        cfg.score = 1500;
        cfg.score_softcore = 300;
        cfg.save(&file).unwrap();

        let loaded = RaConfig::load(&file);
        assert_eq!(loaded.username, cfg.username);
        assert_eq!(loaded.token, cfg.token);
        assert_eq!(loaded.display_name, cfg.display_name);
        assert_eq!(loaded.score, cfg.score);
        assert_eq!(loaded.score_softcore, cfg.score_softcore);
        assert_eq!(loaded.hardcore, cfg.hardcore);
        assert_eq!(loaded.encore, cfg.encore);
        assert!(loaded.token_protected.is_some());
        assert!(loaded.is_signed_in());

        let raw_disk = fs::read_to_string(&file).unwrap();
        assert!(!raw_disk.contains("tok_secret_123"), "plaintext token must not be written to disk");
        assert!(raw_disk.contains("token_protected = \"dpapi:v1:"));



        let env_vars = loaded.launch_environment(Path::new("C:\\GT4\\data\\runtime\\ra_cache"), crate::language::Language::fallback());

        assert!(env_vars.contains(&("GT4_RA_ENABLED".to_string(), OsString::from("1"))));

        assert!(env_vars.contains(&("GT4_RA_HARDCORE".to_string(), OsString::from("1"))));

        assert!(env_vars.contains(&("GT4_RA_ENCORE".to_string(), OsString::from("1"))));

        assert!(env_vars.contains(&("GT4_RA_USERNAME".to_string(), OsString::from("DriverOne"))));

        assert!(env_vars.contains(&("GT4_RA_TOKEN".to_string(), OsString::from("tok_secret_123"))));

        assert!(env_vars.contains(&("GT4_RA_HASH".to_string(), OsString::from(GT4_NTSC_U_RA_HASH))));

        assert!(env_vars.contains(&("GT4_RA_LANGUAGE".to_string(), OsString::from("en-US"))));



        let pt_lang = crate::language::Language::from_tag("pt-BR").unwrap();

        let pt_vars = loaded.launch_environment(Path::new("C:\\GT4\\data\\runtime\\ra_cache"), pt_lang);

        assert!(pt_vars.contains(&("GT4_RA_LANGUAGE".to_string(), OsString::from("pt-BR"))));



        let mut cleared = loaded;

        cleared.clear_credentials();

        assert!(!cleared.is_signed_in());

        assert!(cleared.hardcore, "mode toggles stay intact on sign-out");

        assert!(cleared.launch_environment(Path::new("C:\\cache"), crate::language::Language::fallback()).is_empty());



        let _ = fs::remove_dir_all(file.parent().unwrap());

    }



    #[test]

    fn parse_login_and_game_overview_handle_real_dorequest_payloads() {

        let login_json = r#"{"Success":true,"User":"BSpecBob","Token":"ra_tok_777","Score":4200,"SoftcoreScore":850,"Messages":1}"#;

        let session = parse_login_response(login_json, "bspecbob").unwrap();

        assert_eq!(session.display_name, "BSpecBob");

        assert_eq!(session.token, "ra_tok_777");

        assert_eq!(session.score, 4200);

        assert_eq!(session.score_softcore, 850);



        let err_json = r#"{"Success":false,"Error":"Invalid User/Password combination."}"#;

        assert_eq!(

            parse_login_response(err_json, "bspecbob").unwrap_err(),

            "Invalid User/Password combination."

        );



        let patch_json = r#"{

            "Success": true,

            "PatchData": {

                "ID": 20580,

                "Title": "Gran Turismo 4",

                "Achievements": [

                    {

                        "ID": 1001,

                        "Title": "National B License",

                        "Description": "Acquire the National B License.",

                        "Points": 10,

                        "Flags": 3,

                        "BadgeName": "11001",

                        "Type": "progression"

                    },

                    {

                        "ID": 1002,

                        "Title": "GT World Championship",

                        "Description": "Win the GT World Championship.",

                        "Points": 50,

                        "Flags": 3,

                        "BadgeName": "11002",

                        "Type": "win_condition"

                    },

                    {

                        "ID": 9999,

                        "Title": "Unofficial Test",

                        "Description": "Should be skipped.",

                        "Points": 5,

                        "Flags": 5,

                        "BadgeName": "99999"

                    }

                ]

            }

        }"#;

        let startsession_json = r#"{

            "Success": true,

            "HardcoreUnlocks": [{"ID": 1001, "When": 1700000100}],

            "Unlocks": []

        }"#;



        let overview = parse_game_overview(patch_json, startsession_json).unwrap();

        assert_eq!(overview.game_id, 20580);

        assert_eq!(overview.title, "Gran Turismo 4");

        assert_eq!(overview.total_achievements, 2);

        assert_eq!(overview.unlocked_achievements, 1);

        assert_eq!(overview.total_points, 60);

        assert_eq!(overview.unlocked_points, 10);

        assert_eq!(overview.achievements.len(), 2);

        assert!(overview.achievements[0].unlocked);

        assert!(overview.achievements[0].unlocked_hardcore);

        assert_eq!(overview.achievements[0].type_label, "Progression");

        assert!(!overview.achievements[1].unlocked);

    }

}

