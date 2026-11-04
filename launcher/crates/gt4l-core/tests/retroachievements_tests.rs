use std::fs;
use std::path::{Path, PathBuf};
use std::sync::atomic::Ordering;

use gt4l_core::games::{GT4, SPEC2};
use gt4l_core::language::Language;
use gt4l_core::launch;
use gt4l_core::log_export;
use gt4l_core::paths::{DataDirs, UserDirs, UserRootSource};
use gt4l_core::retroachievements::{RaConfig, TokenErrorKind, MockAtomicFileOps};
use gt4l_core::secret::{self, DpapiProtector, FakeProtector, SecretError, SecretProtector};

fn temp_dir(name: &str) -> PathBuf {
    let dir = std::env::temp_dir().join(format!("gt4-core-ra-test-{}-{name}", std::process::id()));
    let _ = fs::remove_dir_all(&dir);
    fs::create_dir_all(&dir).unwrap();
    dir
}

#[test]
fn retroachievements_config_roundtrips_in_installed_and_portable_layouts() {
    let root = temp_dir("layouts");

    // 1. Installed layout (%APPDATA%\GT4Recomp\config)
    let appdata = root.join("AppData").join("Roaming").join("GT4Recomp");
    let user_installed = UserDirs::new(appdata.clone(), UserRootSource::AppData);
    let ra_file_installed = user_installed.retroachievements_file();
    assert_eq!(ra_file_installed, appdata.join("config").join("retroachievements.toml"));

    let mut cfg = RaConfig::default();
    cfg.username = "Driver99".to_string();
    cfg.token = "tok_installed_abc".to_string();
    cfg.hardcore = true;
    cfg.save(&ra_file_installed).unwrap();

    let loaded = RaConfig::load(&ra_file_installed);
    assert_eq!(loaded.username, "Driver99");
    assert_eq!(loaded.token, "tok_installed_abc");
    assert!(loaded.hardcore);

    // 2. Portable layout (<install>\user\config)
    let portable_user = root.join("portable").join("user");
    let user_portable = UserDirs::new(portable_user.clone(), UserRootSource::Override);
    let ra_file_portable = user_portable.retroachievements_file();
    assert_eq!(ra_file_portable, portable_user.join("config").join("retroachievements.toml"));

    cfg.username = "PortableDriver".to_string();
    cfg.token = "tok_portable_xyz".to_string();
    cfg.hardcore = false;
    cfg.encore = true;
    cfg.save(&ra_file_portable).unwrap();

    let loaded_portable = RaConfig::load(&ra_file_portable);
    assert_eq!(loaded_portable.username, "PortableDriver");
    assert_eq!(loaded_portable.token, "tok_portable_xyz");
    assert!(!loaded_portable.hardcore);
    assert!(loaded_portable.encore);

    let _ = fs::remove_dir_all(root);
}

#[test]
fn retroachievements_cache_explicitly_resolves_under_gt4_in_modern_layout() {
    let root = temp_dir("cache");
    let data_root = root.join("data");
    let dirs = DataDirs::new(&data_root);

    // Default dirs is GT4
    let cache_gt4 = dirs.retroachievements_cache();
    assert_eq!(cache_gt4, data_root.join("games").join("gt4").join("runtime").join("ra_cache"));

    // dirs switched to SPEC2 still resolves cache explicitly to GT4
    let dirs_spec2 = dirs.for_game(&SPEC2);
    let cache_spec2 = dirs_spec2.retroachievements_cache();
    assert_eq!(cache_spec2, data_root.join("games").join("gt4").join("runtime").join("ra_cache"));

    let _ = fs::remove_dir_all(root);
}

#[test]
fn switching_games_preserves_shared_retroachievements_file() {
    let root = temp_dir("shared_ra_file");
    let user = UserDirs::new(root.join("user"), UserRootSource::Override);

    let file_gt4 = user.for_game(&GT4).retroachievements_file();
    let file_spec2 = user.for_game(&SPEC2).retroachievements_file();

    assert_eq!(file_gt4, file_spec2, "retroachievements.toml path must be shared across games");
    assert_eq!(file_gt4, root.join("user").join("config").join("retroachievements.toml"));

    let _ = fs::remove_dir_all(root);
}

#[test]
fn hostile_inherited_retroachievements_environment_is_scrubbed() {
    let hostile_keys = [
        "GT4_RA_ENABLED",
        "GT4_RA_HARDCORE",
        "GT4_RA_ENCORE",
        "GT4_RA_USERNAME",
        "GT4_RA_TOKEN",
        "GT4_RA_PASSWORD",
        "GT4_RA_HASH",
        "GT4_RA_HOST",
        "GT4_RA_CACHE_DIR",
        "GT4_RA_LANGUAGE",
    ];

    for key in hostile_keys {
        assert!(launch::is_scrubbed(key), "Hostile key {key} must be scrubbed");
    }

    // Verify plan_pcsx2_launch removes all hostile keys into removed_inherited
    let root = temp_dir("launch_scrub");
    let game_dir = root.join("game");
    fs::create_dir_all(&game_dir).unwrap();

    let exe = game_dir.join("gt4vulkan.exe");
    let elf = game_dir.join("SCUS_973.28");
    let disc = root.join("disc.iso");
    let card = root.join("card.ps2");
    let session = root.join("session");
    fs::create_dir_all(&session).unwrap();

    let inputs = launch::LaunchInputs {
        game_executable: &exe,
        elf: &elf,
        disc: &disc,
        card: &card,
        session_dir: &session,
    };
    let settings = gt4l_core::settings::Settings::default();
    let textures = root.join("textures");
    let gs_exe = root.join("gs").join("v1.0.0").join("gt4-gs.exe");
    let installed = gt4l_core::gs_release::Installed {
        version: gt4l_core::gs_release::MINIMUM_VERSION,
        executable: gs_exe,
        executable_sha256: "0".repeat(64),
    };
    let pcsx2_paths = launch::Pcsx2Paths::for_session(&installed, &root.join("gs"), &session, &textures);

    let inherited: Vec<String> = hostile_keys.iter().map(|s| s.to_string()).collect();
    let plan = launch::plan_pcsx2_launch(&inputs, &settings, &pcsx2_paths, None, &inherited);

    for key in hostile_keys {
        assert!(plan.removed_inherited.contains(&key.to_string()), "{key} was not removed from inherited env");
        assert!(!plan.environment.iter().any(|(k, _)| k == key), "{key} should not be in pure launch plan env");
    }

    let _ = fs::remove_dir_all(root);
}

#[test]
fn diagnostic_export_forbids_retroachievements_file() {
    assert!(log_export::is_forbidden("retroachievements.toml"));
    assert!(log_export::is_forbidden("C:\\Users\\User\\AppData\\Roaming\\GT4Recomp\\config\\retroachievements.toml"));
    assert!(log_export::is_forbidden("user/config/retroachievements.toml"));
    assert!(log_export::is_forbidden("retroachievements.toml.tmp"));
}

#[test]
fn launch_environment_includes_language_tag() {
    let mut cfg = RaConfig::default();
    cfg.enabled = true;
    cfg.username = "Driver".to_string();
    cfg.token = "tok".to_string();

    let cache = Path::new("C:\\cache");
    let en_vars = cfg.launch_environment(cache, Language::fallback());
    let lang_en = en_vars.iter().find(|(k, _)| k == "GT4_RA_LANGUAGE").unwrap();
    assert_eq!(lang_en.1, "en-US");

    let pt = Language::from_tag("pt-BR").unwrap();
    let pt_vars = cfg.launch_environment(cache, pt);
    let lang_pt = pt_vars.iter().find(|(k, _)| k == "GT4_RA_LANGUAGE").unwrap();
    assert_eq!(lang_pt.1, "pt-BR");
}
#[test]
fn hostile_inherited_retroachievements_environment_with_case_variations_is_scrubbed_and_redacted() {
    let case_variations = [
        "gt4_ra_token",
        "GT4_RA_TOKEN",
        "Gt4_Ra_Token",
        "gt4_ra_password",
        "GT4_RA_PASSWORD",
        "Gt4_Ra_Password",
        "gt4_ra_enabled",
        "GT4_RA_ENABLED",
        "Gt4_Ra_Enabled",
        "gt4_ra_hash",
        "GT4_RA_HASH",
        "gt4_ra_host",
        "GT4_RA_HOST",
        "ps2_disc",
        "PS2_DISC",
        "Ps2_Disc",
    ];

    for key in case_variations {
        assert!(launch::is_scrubbed(key), "Case-variant hostile key {key} must be scrubbed");
    }

    // Passed-through keys stay passed through regardless of case
    assert!(!launch::is_scrubbed("GT4_ASPECT_FOV_EXTRA"));
    assert!(!launch::is_scrubbed("gt4_aspect_fov_extra"));

    let root = temp_dir("case_scrub");
    let game_dir = root.join("game");
    fs::create_dir_all(&game_dir).unwrap();
    let exe = game_dir.join("gt4vulkan.exe");
    let elf = game_dir.join("SCUS_973.28");
    let disc = root.join("disc.iso");
    let card = root.join("card.ps2");
    let session = root.join("session");
    fs::create_dir_all(&session).unwrap();

    let inputs = launch::LaunchInputs {
        game_executable: &exe,
        elf: &elf,
        disc: &disc,
        card: &card,
        session_dir: &session,
    };
    let settings = gt4l_core::settings::Settings::default();
    let textures = root.join("textures");
    let gs_exe = root.join("gs").join("v1.0.0").join("gt4-gs.exe");
    let installed = gt4l_core::gs_release::Installed {
        version: gt4l_core::gs_release::MINIMUM_VERSION,
        executable: gs_exe,
        executable_sha256: "0".repeat(64),
    };
    let pcsx2_paths = launch::Pcsx2Paths::for_session(&installed, &root.join("gs"), &session, &textures);
    let inherited: Vec<String> = case_variations.iter().map(|s| s.to_string()).collect();

    let mut plan = launch::plan_pcsx2_launch(&inputs, &settings, &pcsx2_paths, None, &inherited);

    // Verify all case-variant hostile keys were placed into removed_inherited
    for key in case_variations {
        assert!(plan.removed_inherited.contains(&key.to_string()), "{key} was not removed from inherited env");
        assert!(!plan.environment.iter().any(|(k, _)| k == key), "{key} should not be in launch plan env");
    }

    // Now inject authorized RA environment into plan and test Debug redaction
    plan.environment.push(("GT4_RA_ENABLED".to_string(), "1".to_string()));
    plan.environment.push(("GT4_RA_USERNAME".to_string(), "Pilot99".to_string()));
    plan.environment.push(("GT4_RA_TOKEN".to_string(), "super_secret_auth_token_xyz".to_string()));

    let debug_output = format!("{:?}", plan);
    assert!(debug_output.contains("[REDACTED]"), "Debug output of LaunchPlan must redact token");
    assert!(!debug_output.contains("super_secret_auth_token_xyz"), "Debug output must NEVER leak secret token!");

    let _ = fs::remove_dir_all(root);
}

#[test]
fn fake_server_http_login_refresh_and_failure_scenarios() {
    use std::io::{Read, Write};
    use std::net::TcpListener;
    use std::sync::atomic::{AtomicBool, Ordering};
    use std::sync::Arc;
    use std::thread;

    let listener = TcpListener::bind("127.0.0.1:0").expect("bind ephemeral port");
    let port = listener.local_addr().unwrap().port();
    let base_url = format!("http://127.0.0.1:{port}");
    let stop = Arc::new(AtomicBool::new(false));
    let stop_clone = stop.clone();

    let server_thread = thread::spawn(move || {
        listener.set_nonblocking(true).unwrap();
        while !stop_clone.load(Ordering::Relaxed) {
            match listener.accept() {
                Ok((mut stream, _)) => {
                    let mut buf = [0u8; 4096];
                    let mut read_bytes = 0;
                    while read_bytes < buf.len() {
                        match stream.read(&mut buf[read_bytes..]) {
                            Ok(0) => break,
                            Ok(n) => {
                                read_bytes += n;
                                let text = String::from_utf8_lossy(&buf[..read_bytes]);
                                if let Some(pos) = text.find("\r\n\r\n") {
                                    let cl = text.lines()
                                        .find(|l| l.to_ascii_lowercase().starts_with("content-length:"))
                                        .and_then(|l| l.split(':').nth(1))
                                        .and_then(|s| s.trim().parse::<usize>().ok())
                                        .unwrap_or(0);
                                    if read_bytes >= pos + 4 + cl {
                                        break;
                                    }
                                }
                            }
                            Err(_) => break,
                        }
                    }
                    let req = String::from_utf8_lossy(&buf[..read_bytes]);
                    if req.contains("mode_delay") {
                        thread::sleep(std::time::Duration::from_millis(60));
                    }
                    if req.contains("mode_fail") {
                        let body = r#"{"Success":false,"Error":"Invalid user or password."}"#;
                        let resp = format!("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}", body.len(), body);
                        let _ = stream.write_all(resp.as_bytes());
                        let _ = stream.flush();
                        let _ = stream.shutdown(std::net::Shutdown::Write);
                        thread::sleep(std::time::Duration::from_millis(15));
                    } else if req.contains("mode_abort") {
                        // Drop stream immediately simulating network disconnect
                        drop(stream);
                    } else {
                        // Success response
                        let body = r#"{"Success":true,"User":"FakeDriver","Token":"tok_fake_789","Score":1500,"SoftcoreScore":200}"#;
                        let resp = format!("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}", body.len(), body);
                        let _ = stream.write_all(resp.as_bytes());
                        let _ = stream.flush();
                        let _ = stream.shutdown(std::net::Shutdown::Write);
                        thread::sleep(std::time::Duration::from_millis(15));
                    }
                }
                Err(ref e) if e.kind() == std::io::ErrorKind::WouldBlock => {
                    thread::sleep(std::time::Duration::from_millis(5));
                }
                Err(_) => break,
            }
        }
    });

    // 1. Password login success
    let session = gt4l_core::retroachievements::login_with_password(&base_url, "FakeDriver", "pass123")
        .expect("login with password succeeds against fake server");
    assert_eq!(session.username, "FakeDriver");
    assert_eq!(session.token, "tok_fake_789");
    assert_eq!(session.score, 1500);

    // 2. Token login success
    let token_session = gt4l_core::retroachievements::login_with_token(&base_url, "FakeDriver", "tok_fake_789")
        .expect("token login succeeds against fake server");
    assert_eq!(token_session.username, "FakeDriver");
    assert_eq!(token_session.token, "tok_fake_789");

    // 3. Delayed login completes cleanly
    let delayed = gt4l_core::retroachievements::login_with_password(&base_url, "FakeDriver", "pass_mode_delay")
        .expect("delayed login succeeds");
    assert_eq!(delayed.username, "FakeDriver");

    // 4. Failed login returns explicit server error
    let fail_res = gt4l_core::retroachievements::login_with_password(&base_url, "FakeDriver", "badpass_mode_fail");
    assert!(fail_res.is_err(), "Invalid credentials must return Err");
    assert!(fail_res.unwrap_err().contains("Invalid user or password"));

    // 5. Aborted connection returns clean error without panic
    let abort_res = gt4l_core::retroachievements::login_with_password(&base_url, "FakeDriver", "pass_mode_abort");
    assert!(abort_res.is_err(), "Aborted connection must return Err");

    stop.store(true, Ordering::Relaxed);
    let _ = server_thread.join();
}

#[test]
fn retroachievements_config_125_compatibility_and_malformed_handling() {
    let root = temp_dir("config_compat");

    // 1. Valid 1.2.5 format with comments and future unknown fields
    let toml_125 = r#"
# RetroAchievements configuration for GT4Recomp 1.2.5
username = "VeteranPilot"
token = "tok_compat_abc"
enabled = true
hardcore = true
encore = false
unknown_future_field = "ignored_safely"

[future_section]
feature_x = 42
"#;
    let cfg_file = root.join("retroachievements.toml");
    fs::write(&cfg_file, toml_125).unwrap();

    let loaded = RaConfig::load(&cfg_file);
    assert_eq!(loaded.username, "VeteranPilot");
    assert_eq!(loaded.token, "tok_compat_abc");
    assert!(loaded.enabled);
    assert!(loaded.hardcore);
    assert!(!loaded.encore);

    // 2. Malformed / corrupted TOML falls back to safe defaults without crashing
    let corrupt_file = root.join("corrupt.toml");
    fs::write(&corrupt_file, b"this is not valid toml = [[{\x00\xFF").unwrap();
    let corrupt_loaded = RaConfig::load(&corrupt_file);
    assert_eq!(corrupt_loaded.username, "");
    assert_eq!(corrupt_loaded.token, "");
    assert!(corrupt_loaded.enabled); // default enabled is true
    assert!(!corrupt_loaded.hardcore);

    // 3. Empty file falls back to defaults
    let empty_file = root.join("empty.toml");
    fs::write(&empty_file, b"").unwrap();
    let empty_loaded = RaConfig::load(&empty_file);
    assert_eq!(empty_loaded.username, "");
    assert_eq!(empty_loaded.token, "");

    let _ = fs::remove_dir_all(root);
}

#[test]
fn diagnostic_export_exclusions_comprehensive() {
    // Explicitly forbidden paths
    assert!(log_export::is_forbidden("retroachievements.toml"));
    assert!(log_export::is_forbidden("retroachievements.toml.tmp"));
    assert!(log_export::is_forbidden("retroachievements.toml.bak"));
    assert!(log_export::is_forbidden("retroachievements.toml.old"));
    assert!(log_export::is_forbidden("config/retroachievements.toml"));
    assert!(log_export::is_forbidden("user/config/retroachievements.toml"));
    assert!(log_export::is_forbidden(r"C:\Users\Pilot\AppData\Roaming\GT4Recomp\config\retroachievements.toml"));

    // Safe, non-forbidden diagnostic files
    assert!(!log_export::is_forbidden("game.log"));
    assert!(!log_export::is_forbidden("misses.txt"));
    assert!(!log_export::is_forbidden("probe.log"));
    assert!(!log_export::is_forbidden("achievements_summary.txt"));
}

#[test]
fn fake_protector_encrypt_decrypt_and_injected_failures() {
    let protector = FakeProtector::default();

    // 1. Normal round-trip
    let plaintext = "secret_session_token_12345";
    let envelope = protector.protect_secret(plaintext).expect("protect succeeds");
    assert!(envelope.starts_with(secret::FAKE_V1_PREFIX));
    let decrypted = protector.unprotect_secret(&envelope).expect("unprotect succeeds");
    assert_eq!(decrypted, plaintext);

    // 2. Injected protect failure
    let fail_protect = FakeProtector { fail_protect: true, ..FakeProtector::default() };
    let err = fail_protect.protect_secret(plaintext).unwrap_err();
    assert!(matches!(err, SecretError::BackendUnavailable(_)));

    // 3. Injected unprotect failure (simulating NTE_BAD_KEYSET / wrong user)
    let fail_unprotect = FakeProtector { fail_unprotect: true, wrong_user_code: 0x8009000B, ..FakeProtector::default() };
    let err2 = fail_unprotect.unprotect_secret(&envelope).unwrap_err();
    match err2 {
        SecretError::OsError { code, .. } => assert_eq!(code, 0x8009000B),
        other => panic!("expected OsError, got {:?}", other),
    }

    // 4. Corrupted envelopes
    assert!(matches!(
        protector.unprotect_secret("wrong_prefix:v1:010203").unwrap_err(),
        SecretError::CorruptData(_)
    ));
    assert!(matches!(
        protector.unprotect_secret("fake:v1:not_hex_xyz").unwrap_err(),
        SecretError::CorruptData(_)
    ));
    assert!(matches!(
        protector.unprotect_secret("fake:v1:123").unwrap_err(),
        SecretError::CorruptData(_)
    ));
}

#[test]
fn real_windows_dpapi_roundtrip_tampered_blob_and_empty_handling() {
    let protector = DpapiProtector;

    // 1. Normal DPAPI roundtrip
    let token = "ra_prod_token_test_abc_888";
    let envelope = protector.protect_secret(token).expect("DPAPI protect succeeds");
    assert!(envelope.starts_with(secret::DPAPI_V1_PREFIX));
    let decrypted = protector.unprotect_secret(&envelope).expect("DPAPI unprotect succeeds");
    assert_eq!(decrypted, token);

    // 2. Empty string roundtrip
    let empty_env = protector.protect_secret("").expect("protect empty");
    assert_eq!(protector.unprotect_secret(&empty_env).expect("unprotect empty"), "");

    // 3. Tampered ciphertext rejection (verifying OS integrity check)
    let hex_part = &envelope[secret::DPAPI_V1_PREFIX.len()..];
    let mut raw_bytes = secret::hex_decode(hex_part).expect("hex decode");
    if let Some(b) = raw_bytes.last_mut() {
        *b ^= 0xFF;
    }
    let tampered_hex = secret::hex_encode(&raw_bytes);
    let tampered_env = format!("{}{}", secret::DPAPI_V1_PREFIX, tampered_hex);
    let tampered_res = protector.unprotect_secret(&tampered_env);
    assert!(tampered_res.is_err(), "Tampered ciphertext must be rejected by CryptUnprotectData");
    match tampered_res.unwrap_err() {
        SecretError::OsError { .. } => {}
        other => panic!("expected OsError from tampered DPAPI blob, got {:?}", other),
    }

    // 4. Invalid envelope prefix and invalid hex
    assert!(matches!(
        protector.unprotect_secret("dpapi:v2:0102").unwrap_err(),
        SecretError::CorruptData(_)
    ));
    assert!(matches!(
        protector.unprotect_secret("dpapi:v1:invalid_non_hex").unwrap_err(),
        SecretError::CorruptData(_)
    ));
}

#[test]
fn malformed_non_ascii_and_truncated_ciphertext_never_panics() {
    let protector = DpapiProtector;
    let fake_protector = FakeProtector::default();

    // Multi-byte non-ASCII characters that would panic naive string-slicing
    let non_ascii_inputs = [
        "dpapi:v1:aéa",
        "dpapi:v1:01020304a\u{00E9}a",
        "dpapi:v1:\u{1F980}\u{1F980}",
        "fake:v1:aéa",
        "fake:v1:\u{00E9}\u{00E8}",
        "fake:v1:\u{1F600}",
        "dpapi:v1:odd_length_3",
        "fake:v1:odd",
        "dpapi:v1:",
        "fake:v1:",
        "dpapi:v1:   ",
        "not_an_envelope_at_all",
    ];

    for input in non_ascii_inputs {
        let res_dpapi = protector.unprotect_secret(input);
        assert!(res_dpapi.is_err(), "Input '{input}' must return Err without panicking");
        assert!(matches!(res_dpapi.unwrap_err(), SecretError::CorruptData(_)));

        let res_fake = fake_protector.unprotect_secret(input);
        assert!(res_fake.is_err(), "Input '{input}' must return Err without panicking");
        assert!(matches!(res_fake.unwrap_err(), SecretError::CorruptData(_)));
    }
}

#[test]
fn legacy_plaintext_migration_from_124_125_config_and_clean_disk_replacement() {
    let root = temp_dir("migration_clean");
    let cfg_path = root.join("retroachievements.toml");

    let legacy_toml = r#"
# RetroAchievements configuration from 1.2.5
username = "VeteranPilot"
token = "tok_plaintext_legacy_999"
display_name = "VeteranPilot"
score = 4500
score_softcore = 350
enabled = true
hardcore = true
encore = false
"#;
    fs::write(&cfg_path, legacy_toml).unwrap();

    let protector = FakeProtector::default();

    // First load triggers automatic migration
    let loaded = RaConfig::load_with(&cfg_path, &protector);
    assert_eq!(loaded.username, "VeteranPilot");
    assert_eq!(loaded.token, "tok_plaintext_legacy_999");
    assert!(loaded.is_signed_in());
    assert!(loaded.token_protected.is_some());
    assert_eq!(loaded.token_error, None);

    // Verify disk content: MUST NOT contain plaintext token, MUST contain protected envelope
    let disk_raw = fs::read_to_string(&cfg_path).unwrap();
    assert!(!disk_raw.contains("tok_plaintext_legacy_999"), "Plaintext token must not remain on disk after migration");
    assert!(disk_raw.contains("token_protected = \"fake:v1:"));
    assert!(!root.join("retroachievements.toml.tmp").exists(), "Temporary migration file must be cleaned");

    // Second load reads directly from the protected envelope
    let reloaded = RaConfig::load_with(&cfg_path, &protector);
    assert_eq!(reloaded.username, "VeteranPilot");
    assert_eq!(reloaded.token, "tok_plaintext_legacy_999");
    assert!(reloaded.is_signed_in());
    assert_eq!(reloaded.score, 4500);

    let _ = fs::remove_dir_all(root);
}

#[test]
fn failed_migration_leaves_original_plaintext_file_untouched() {
    let root = temp_dir("migration_fail");
    let cfg_path = root.join("retroachievements.toml");

    let legacy_toml = "username = \"SafeUser\"\ntoken = \"tok_legacy_do_not_destroy\"\nenabled = true\nhardcore = false\n";
    fs::write(&cfg_path, legacy_toml).unwrap();

    // Use a protector that fails during protection
    let fail_protector = FakeProtector { fail_protect: true, ..FakeProtector::default() };
    let loaded = RaConfig::load_with(&cfg_path, &fail_protector);

    // In-memory state: migration failed, so token is not signed in and token_error is MigrationFailed
    assert!(!loaded.is_signed_in());
    assert_eq!(loaded.token_error, Some(TokenErrorKind::MigrationFailed));

    // Crucial check: Disk file MUST be 100% untouched!
    let disk_after = fs::read_to_string(&cfg_path).unwrap();
    assert_eq!(disk_after, legacy_toml, "Disk file must remain 100% untouched when migration fails");
    assert!(!root.join("retroachievements.toml.tmp").exists());

    let _ = fs::remove_dir_all(root);
}

#[test]
fn migration_failure_prevents_option_updates_from_destroying_legacy_token() {
    let root = temp_dir("migration_fail_save");
    let cfg_path = root.join("retroachievements.toml");

    let legacy_toml = "username = \"ImportantUser\"\ntoken = \"tok_crucial_legacy_credential\"\nenabled = true\nhardcore = false\n";
    fs::write(&cfg_path, legacy_toml).unwrap();

    let fail_protector = FakeProtector { fail_protect: true, ..FakeProtector::default() };

    // 1. Initial load fails migration safely
    let mut loaded = RaConfig::load_with(&cfg_path, &fail_protector);
    assert_eq!(loaded.token_error, Some(TokenErrorKind::MigrationFailed));
    assert!(!loaded.is_signed_in());

    // 2. Unrelated option update (e.g. toggling hardcore) MUST NOT erase legacy token on disk
    loaded.hardcore = true;
    let save_err = loaded.save_with(&cfg_path, &fail_protector);
    assert!(save_err.is_err(), "Saving options must return Err when legacy token cannot be protected");

    let disk_content = fs::read_to_string(&cfg_path).unwrap();
    assert_eq!(disk_content, legacy_toml, "Legacy config must be preserved untouched across option save attempt");

    // 3. Explicit logout cleanly clears credentials
    loaded.clear_credentials();
    assert!(loaded.save_with(&cfg_path, &fail_protector).is_ok(), "Logout must succeed even with failed protector");
    let disk_logged_out = fs::read_to_string(&cfg_path).unwrap();
    assert!(!disk_logged_out.contains("tok_crucial_legacy_credential"));
    assert!(!disk_logged_out.contains("token_protected"));

    let _ = fs::remove_dir_all(root);
}

#[test]
fn unprotectable_token_preserves_ciphertext_and_non_secret_settings() {
    let root = temp_dir("unprotectable_token");
    let cfg_path = root.join("retroachievements.toml");

    let protector = FakeProtector::default();

    let mut cfg = RaConfig::default();
    cfg.username = "DriverMovingPC".to_string();
    cfg.display_name = "Driver Moving PC".to_string();
    cfg.token = "tok_valid_before_move".to_string();
    cfg.score = 3200;
    cfg.score_softcore = 400;
    cfg.hardcore = true;
    cfg.encore = true;
    cfg.save_with(&cfg_path, &protector).unwrap();

    // Verify file is saved with protected token
    let disk_before = fs::read_to_string(&cfg_path).unwrap();
    assert!(!disk_before.contains("tok_valid_before_move"));
    assert!(disk_before.contains("token_protected = \"fake:v1:"));

    // Simulate moving portable folder to another Windows machine / user account
    let wrong_user_protector = FakeProtector { fail_unprotect: true, wrong_user_code: 0x8009000B, ..FakeProtector::default() };
    let loaded = RaConfig::load_with(&cfg_path, &wrong_user_protector);

    // Account is not signed in, but non-secret fields and ciphertext are intact!
    assert!(!loaded.is_signed_in());
    assert_eq!(loaded.token, "");
    assert_eq!(loaded.token_error, Some(TokenErrorKind::Inaccessible));
    assert!(loaded.token_protected.is_some());
    assert_eq!(loaded.username, "DriverMovingPC");
    assert_eq!(loaded.display_name, "Driver Moving PC");
    assert_eq!(loaded.score, 3200);
    assert_eq!(loaded.score_softcore, 400);
    assert!(loaded.hardcore);
    assert!(loaded.encore);

    // Environment variables for game must be EMPTY because account cannot be unlocked
    let env_vars = loaded.launch_environment(Path::new("C:\\cache"), Language::fallback());
    assert!(env_vars.is_empty(), "Unprotectable token must NOT pass credentials or enable RA for game");

    // Updating a session option (e.g. toggling hardcore) preserves ciphertext on disk!
    let mut updated_options = loaded;
    updated_options.hardcore = false;
    updated_options.save_with(&cfg_path, &wrong_user_protector).unwrap();

    let disk_after = fs::read_to_string(&cfg_path).unwrap();
    assert!(disk_after.contains("token_protected = \"fake:v1:"), "Ciphertext must be preserved when saving options");
    assert!(!disk_after.contains("tok_valid_before_move"), "Plaintext must never be written");
    assert!(disk_after.contains("hardcore = false"), "Option toggle must be updated");

    let _ = fs::remove_dir_all(root);
}

#[test]
fn real_windows_dpapi_end_to_end_migration_and_persistence() {
    let root = temp_dir("dpapi_e2e");
    let cfg_path = root.join("retroachievements.toml");

    let legacy_toml = "username = \"WindowsUser99\"\ntoken = \"real_dpapi_test_tok_777\"\ndisplay_name = \"WindowsUser99\"\nscore = 800\n";
    fs::write(&cfg_path, legacy_toml).unwrap();

    // 1. Load triggers real Windows DPAPI migration
    let loaded = RaConfig::load(&cfg_path);
    assert_eq!(loaded.username, "WindowsUser99");
    assert_eq!(loaded.token, "real_dpapi_test_tok_777");
    assert!(loaded.is_signed_in());
    assert!(loaded.token_protected.is_some());
    assert_eq!(loaded.token_error, None);

    // 2. Verify disk has dpapi envelope and no plaintext
    let raw = fs::read_to_string(&cfg_path).unwrap();
    assert!(!raw.contains("real_dpapi_test_tok_777"), "DPAPI migrated file must not contain plaintext");
    assert!(raw.contains("token_protected = \"dpapi:v1:"));

    // 3. Reload directly using real DPAPI
    let reloaded = RaConfig::load(&cfg_path);
    assert_eq!(reloaded.username, "WindowsUser99");
    assert_eq!(reloaded.token, "real_dpapi_test_tok_777");
    assert!(reloaded.is_signed_in());

    // 4. Logout / clear credentials removes protected token and plaintext
    let mut logged_out = reloaded;
    logged_out.clear_credentials();
    logged_out.save(&cfg_path).unwrap();

    let disk_cleared = fs::read_to_string(&cfg_path).unwrap();
    assert!(!disk_cleared.contains("token_protected"), "Cleared credentials must not have token_protected");
    assert!(!disk_cleared.contains("token ="), "Cleared credentials must not have token");

    let reload_cleared = RaConfig::load(&cfg_path);
    assert!(!reload_cleared.is_signed_in());
    assert_eq!(reload_cleared.username, "");
    assert_eq!(reload_cleared.token, "");
    assert!(reload_cleared.token_protected.is_none());

    let _ = fs::remove_dir_all(root);
}

#[test]
fn two_portable_profiles_and_installed_profile_independence() {
    let root = temp_dir("multi_profile_independence");

    let installed_dir = root.join("installed").join("config");
    let portable_a_dir = root.join("portable_a").join("user").join("config");
    let portable_b_dir = root.join("portable_b").join("user").join("config");

    let inst_path = installed_dir.join("retroachievements.toml");
    let port_a_path = portable_a_dir.join("retroachievements.toml");
    let port_b_path = portable_b_dir.join("retroachievements.toml");

    let protector = FakeProtector::default();

    // 1. Setup Installed Profile
    let mut cfg_inst = RaConfig::default();
    cfg_inst.username = "InstPilot".to_string();
    cfg_inst.token = "tok_inst_111".to_string();
    cfg_inst.hardcore = true;
    cfg_inst.save_with(&inst_path, &protector).unwrap();

    // 2. Setup Portable A
    let mut cfg_port_a = RaConfig::default();
    cfg_port_a.username = "PortPilotA".to_string();
    cfg_port_a.token = "tok_port_a_222".to_string();
    cfg_port_a.hardcore = false;
    cfg_port_a.save_with(&port_a_path, &protector).unwrap();

    // 3. Setup Portable B
    let mut cfg_port_b = RaConfig::default();
    cfg_port_b.username = "PortPilotB".to_string();
    cfg_port_b.token = "tok_port_b_333".to_string();
    cfg_port_b.encore = true;
    cfg_port_b.save_with(&port_b_path, &protector).unwrap();

    // 4. Verify completely independent loading and no crosstalk
    let load_inst = RaConfig::load_with(&inst_path, &protector);
    let load_a = RaConfig::load_with(&port_a_path, &protector);
    let load_b = RaConfig::load_with(&port_b_path, &protector);

    assert_eq!(load_inst.username, "InstPilot");
    assert_eq!(load_inst.token, "tok_inst_111");
    assert!(load_inst.hardcore);

    assert_eq!(load_a.username, "PortPilotA");
    assert_eq!(load_a.token, "tok_port_a_222");
    assert!(!load_a.hardcore);

    assert_eq!(load_b.username, "PortPilotB");
    assert_eq!(load_b.token, "tok_port_b_333");
    assert!(load_b.encore);

    // 5. Mutating one profile does not touch the others
    let mut mutated_a = load_a;
    mutated_a.clear_credentials();
    mutated_a.save_with(&port_a_path, &protector).unwrap();

    let check_inst = RaConfig::load_with(&inst_path, &protector);
    let check_b = RaConfig::load_with(&port_b_path, &protector);
    assert_eq!(check_inst.username, "InstPilot");
    assert_eq!(check_b.username, "PortPilotB");

    let _ = fs::remove_dir_all(root);
}

#[test]
fn debug_and_serialization_paths_never_leak_plaintext_token() {
    let mut cfg = RaConfig::default();
    cfg.username = "SecretAuditUser".to_string();
    cfg.token = "synthetic_super_secret_audit_token_99999".to_string();
    cfg.token_protected = Some("dpapi:v1:010203040506".to_string());

    // 1. Debug formatting must REDACT plaintext token
    let debug_str = format!("{:?}", cfg);
    assert!(!debug_str.contains("synthetic_super_secret_audit_token_99999"), "RaConfig Debug must NEVER contain plaintext token");
    assert!(debug_str.contains("[REDACTED]"), "RaConfig Debug must print [REDACTED]");

    // 2. Serde TOML serialization must NEVER include plaintext token
    let toml_str = toml::to_string(&cfg).expect("serialize RaConfig");
    assert!(!toml_str.contains("synthetic_super_secret_audit_token_99999"), "TOML serialization must NEVER output plaintext token");
    assert!(!toml_str.contains("token ="), "TOML serialization must not have token field");
    assert!(toml_str.contains("token_protected = \"dpapi:v1:010203040506\""));

    // 3. RaAccountSession Debug must REDACT token
    let session = gt4l_core::retroachievements::RaAccountSession {
        username: "AuditUser".to_string(),
        display_name: "Audit User".to_string(),
        token: "session_secret_token_abc".to_string(),
        score: 500,
        score_softcore: 100,
    };
    let session_debug = format!("{:?}", session);
    assert!(!session_debug.contains("session_secret_token_abc"), "RaAccountSession Debug must NEVER contain plaintext token");
    assert!(session_debug.contains("[REDACTED]"));
}

#[test]
fn interrupted_and_failed_atomic_writes_are_safe() {
    let root = temp_dir("failed_write_safe");
    let target_file = root.join("retroachievements.toml");

    let protector = FakeProtector::default();
    let mut cfg = RaConfig::default();
    cfg.username = "WriteTester".to_string();
    cfg.token = "tok_write_test".to_string();

    // 1. Pre-existing unrelated files and sentinel colliding candidate file
    let unrelated_tmp = root.join("unrelated_prior.tmp");
    let static_toml_tmp = root.join("retroachievements.toml.tmp");
    fs::write(&unrelated_tmp, b"unrelated temp data").unwrap();
    fs::write(&static_toml_tmp, b"pre-existing static tmp data").unwrap();

    // Deterministic pre-existing sentinel candidate matching the upcoming counter
    let next_counter = gt4l_core::retroachievements::next_temp_counter_for_test();
    let sentinel_candidate = root.join(format!(".retroachievements.toml.tmp-{}-{}", std::process::id(), next_counter));
    let sentinel_content = b"SENTINEL PRE-EXISTING DATA MUST NEVER BE OVERWRITTEN OR DELETED";
    fs::write(&sentinel_candidate, sentinel_content).unwrap();

    // 2. Deterministic OS collision retry with pre-existing candidate file
    // StdAtomicFileOps uses OpenOptions::create_new(true), which fails on the pre-existing sentinel.
    // The retry loop catches AlreadyExists, increments the counter, and succeeds on the next candidate.
    let res_coll = cfg.save_with(&target_file, &protector);
    assert!(res_coll.is_ok(), "save_with must succeed after retrying on collision: {:?}", res_coll.err());
    assert!(target_file.exists(), "Target file must be created after retry");
    assert_eq!(fs::read(&sentinel_candidate).unwrap(), sentinel_content, "Pre-existing colliding file must remain byte-for-byte intact");
    assert!(unrelated_tmp.exists(), "Unrelated file must remain untouched");
    assert!(static_toml_tmp.exists(), "Static tmp file must remain untouched");
    let _ = fs::remove_file(&target_file);

    // 3. Multi-attempt collision handling via MockAtomicFileOps (3 collisions then success)
    let ops_multi_coll = MockAtomicFileOps::with_collisions(3);
    let res_multi = cfg.save_with_custom(&target_file, &protector, &ops_multi_coll);
    assert!(res_multi.is_ok(), "save_with_custom must succeed after 3 retries");
    assert_eq!(ops_multi_coll.collision_retries_reached.load(Ordering::SeqCst), 3);
    assert_eq!(fs::read(&sentinel_candidate).unwrap(), sentinel_content);
    let _ = fs::remove_file(&target_file);

    // 4. Exhausted collision retry limit (10 consecutive collisions abort cleanly)
    let ops_exhausted = MockAtomicFileOps::with_collisions(10);
    let res_exh = cfg.save_with_custom(&target_file, &protector, &ops_exhausted);
    assert!(res_exh.is_err(), "Exhausted collision retries must return Err");
    assert!(res_exh.unwrap_err().contains("after 10 attempts"));
    assert!(!target_file.exists(), "Target file must not be written when collisions exhaust retries");
    assert_eq!(fs::read(&sentinel_candidate).unwrap(), sentinel_content);

    // 5. Pre-open failure (e.g. permission denied) with pre-existing candidate:
    // Guard is not armed, so pre-existing sentinel file is NOT deleted.
    let ops_open_fail = MockAtomicFileOps { fail_open: true, ..Default::default() };
    let res_open = cfg.save_with_custom(&target_file, &protector, &ops_open_fail);
    assert!(res_open.is_err(), "fail_open must return Err");
    assert!(!target_file.exists(), "Target file must not be created");
    assert_eq!(fs::read(&sentinel_candidate).unwrap(), sentinel_content, "Pre-existing sentinel MUST NOT be removed when open fails");
    assert!(unrelated_tmp.exists());
    assert!(static_toml_tmp.exists());

    // 6. Failure during write (simulating partial write / disk full):
    // Verifies partial write seam is reached, guard cleans up only the newly created partial file,
    // and sentinel remains intact.
    let ops_partial = MockAtomicFileOps { fail_during_write: Some(15), ..Default::default() };
    let res_partial = cfg.save_with_custom(&target_file, &protector, &ops_partial);
    assert!(res_partial.is_err(), "Partial write must return Err");
    assert!(ops_partial.partial_write_reached.load(Ordering::SeqCst), "Filesystem seam must prove partial write was reached");
    assert!(!target_file.exists(), "Target file must not be written on partial write failure");
    // Verify no temporary files other than sentinel_candidate remain
    let temp_files_after_partial: Vec<_> = fs::read_dir(&root).unwrap()
        .filter_map(|e| e.ok())
        .filter(|e| e.file_name().to_string_lossy().starts_with(".retroachievements.toml.tmp-") && e.path() != sentinel_candidate)
        .collect();
    assert_eq!(temp_files_after_partial.len(), 0, "Partial temporary files must be cleaned up by RAII guard");
    assert_eq!(fs::read(&sentinel_candidate).unwrap(), sentinel_content);
    assert!(unrelated_tmp.exists());
    assert!(static_toml_tmp.exists());

    // 7. Failure during flush
    let ops_flush = MockAtomicFileOps { fail_flush: true, ..Default::default() };
    let res_flush = cfg.save_with_custom(&target_file, &protector, &ops_flush);
    assert!(res_flush.is_err(), "Flush failure must return Err");
    assert!(!target_file.exists());
    let temp_files_after_flush: Vec<_> = fs::read_dir(&root).unwrap()
        .filter_map(|e| e.ok())
        .filter(|e| e.file_name().to_string_lossy().starts_with(".retroachievements.toml.tmp-") && e.path() != sentinel_candidate)
        .collect();
    assert_eq!(temp_files_after_flush.len(), 0);
    assert_eq!(fs::read(&sentinel_candidate).unwrap(), sentinel_content);

    // 8. Failure during rename / replace (e.g. target path is a directory or destination locked)
    let bad_path = root.join("is_a_directory");
    fs::create_dir_all(&bad_path).unwrap();
    let res_replace = cfg.save_with(&bad_path, &protector);
    assert!(res_replace.is_err(), "Saving to a directory path must fail cleanly");
    let temp_files_after_replace: Vec<_> = fs::read_dir(&root).unwrap()
        .filter_map(|e| e.ok())
        .filter(|e| e.file_name().to_string_lossy().starts_with(".is_a_directory.tmp-"))
        .collect();
    assert_eq!(temp_files_after_replace.len(), 0, "Temporary file must be removed on replace failure");

    // 9. Failure during rename via mock seam
    let ops_rename = MockAtomicFileOps { fail_replace: true, ..Default::default() };
    let res_rename = cfg.save_with_custom(&target_file, &protector, &ops_rename);
    assert!(res_rename.is_err(), "fail_replace must return Err");
    assert!(!target_file.exists());
    assert_eq!(fs::read(&sentinel_candidate).unwrap(), sentinel_content);

    // 10. Normal successful atomic save
    assert!(cfg.save_with(&target_file, &protector).is_ok());
    assert!(target_file.exists(), "Target file must exist after successful save");

    // 11. Process interruption simulation (synthetic orphan from abrupt external termination / SIGKILL):
    // Note: In an un-mocked process kill or power outage, Rust Drop guards do not execute.
    // This synthetic orphan test verifies that if an orphan temp file was left on disk before rename,
    // it contains NO plaintext secrets, and RaConfig::load safely ignores it.
    let orphaned_crash_tmp = root.join(".retroachievements.toml.tmp-99999-1");
    let mut interrupted_disk_cfg = cfg.clone();
    interrupted_disk_cfg.token = String::new();
    interrupted_disk_cfg.token_protected = Some("fake:v1:01020304".to_string());
    let body = toml::to_string_pretty(&interrupted_disk_cfg).unwrap();
    fs::write(&orphaned_crash_tmp, body.as_bytes()).unwrap();

    // Verify orphaned file contains NO plaintext secret
    let orphaned_content = fs::read_to_string(&orphaned_crash_tmp).unwrap();
    assert!(!orphaned_content.contains("tok_write_test"), "Temporary file must NEVER contain plaintext secrets");
    assert!(orphaned_content.contains("token_protected"));

    // Verify RaConfig::load ignores the orphaned file and safely loads the real config
    let loaded = RaConfig::load_with(&target_file, &protector);
    assert_eq!(loaded.username, "WriteTester");
    assert_eq!(loaded.token, "tok_write_test");

    // Verify subsequent retry successfully updates config without failure or data loss
    let mut retry_cfg = loaded.clone();
    retry_cfg.score = 999;
    assert!(retry_cfg.save_with(&target_file, &protector).is_ok());
    let loaded_retry = RaConfig::load_with(&target_file, &protector);
    assert_eq!(loaded_retry.score, 999);
    assert_eq!(loaded_retry.token, "tok_write_test");

    // 12. Instance concurrency boundaries:
    // Unique temporary files ensure two instances saving simultaneously do not collide on temp files.
    // Note that filesystem rename semantics are last-write-wins; application-level locking is not provided.
    let cfg1 = cfg.clone();
    let cfg2 = cfg.clone();
    let target1 = root.join("inst1.toml");
    let target2 = root.join("inst2.toml");
    assert!(cfg1.save_with(&target1, &protector).is_ok());
    assert!(cfg2.save_with(&target2, &protector).is_ok());
    assert!(target1.exists());
    assert!(target2.exists());

    let _ = fs::remove_dir_all(root);
}
