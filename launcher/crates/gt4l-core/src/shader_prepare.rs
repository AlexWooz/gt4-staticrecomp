//! The shader prepare of CoreGS 2.0 and its retries (TASK-170b), and the readable listing of the renderer's guard files.
//!
//! `gt4.exe --prepare-shaders` compiles the shader variants the cache lacks. On some AMD drivers one pipeline compile ends the
//! process. The renderer notes every variant in `compiling.bin` before it compiles it. The next start makes the one that was
//! in flight a suspect (`suspects.bin`, TASK-170c); the same key in flight at a second end is quarantined (`quarantine.bin`).
//! Every new run therefore gets further. The launcher runs the prepare again by itself, while each attempt makes progress
//! ([`run_attempts`]), and starts the game only when the prepare finished or can go no further. The file format is written by
//! `native/hw_gs/core2/core2_gpu.cpp` (`GuardFileHeader`); [`parse_guard_file`] reads it for the log bundle
//! (`logs/shader-guard.txt`), and the progress rule compares the three files' bytes.

use std::fmt::Write as _;

/// The most prepare attempts one Play makes. With two strikes (TASK-170c) a variant that ends the process costs two attempts
/// (suspect, then quarantine). The RX 6700 XT report needed 7 quarantined variants, so 14 crashes in the worst case, plus one
/// that first makes the prepares serial and the attempt that finishes: 16. 20 leaves a margin for a few more bad variants
/// while still ending a card that crashes without end (each attempt that crashes late can cost minutes).
pub const MAX_PREPARE_ATTEMPTS: u32 = 20;

/// The file names the renderer keeps in the shader cache folder.
pub const QUARANTINE_FILE: &str = "quarantine.bin";
pub const COMPILING_FILE: &str = "compiling.bin";
pub const SUSPECTS_FILE: &str = "suspects.bin";

/// The bytes of the magic at the start of the guard files.
const GUARD_MAGIC: &[u8; 8] = b"GT4GRD1\0";
/// `magic[8]`, `shaderSet` u64, `vendorId`, `deviceId`, `driverVersion`, `keyWords`, `flags`, `count` (u32 each).
const GUARD_HEADER_BYTES: usize = 40;
/// A key longer than this is not a key of this renderer; a damaged file must not make a huge listing.
const MAX_KEY_WORDS: u32 = 256;
/// The flag bit of `flags`: prepares compile one variant at a time.
const GUARD_SERIAL: u32 = 1;

/// A guard file read: the header fields and every key as 32-bit words.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct GuardFile {
    pub shader_set: u64,
    pub vendor_id: u32,
    pub device_id: u32,
    pub driver_version: u32,
    pub key_words: u32,
    pub flags: u32,
    pub keys: Vec<Vec<u32>>,
}

impl GuardFile {
    /// Prepares compile one variant at a time (the renderer saw a run stop with several compiles in flight).
    pub fn serial(&self) -> bool {
        self.flags & GUARD_SERIAL != 0
    }
}

fn word(bytes: &[u8], at: usize) -> u32 {
    u32::from_le_bytes([bytes[at], bytes[at + 1], bytes[at + 2], bytes[at + 3]])
}

/// Reads a guard file (`quarantine.bin`, `compiling.bin`); the error says what is wrong with a damaged one.
pub fn parse_guard_file(bytes: &[u8]) -> Result<GuardFile, String> {
    if bytes.len() < GUARD_HEADER_BYTES {
        return Err(format!("{} bytes, shorter than the {GUARD_HEADER_BYTES}-byte header", bytes.len()));
    }
    if &bytes[..8] != GUARD_MAGIC {
        return Err("the first bytes are not the guard file magic (GT4GRD1)".to_string());
    }
    let shader_set = u64::from_le_bytes(bytes[8..16].try_into().expect("eight bytes"));
    let (vendor_id, device_id, driver_version) = (word(bytes, 16), word(bytes, 20), word(bytes, 24));
    let (key_words, flags, count) = (word(bytes, 28), word(bytes, 32), word(bytes, 36));
    if key_words == 0 || key_words > MAX_KEY_WORDS {
        return Err(format!("a key of {key_words} words is not plausible"));
    }
    let key_bytes = key_words as usize * 4;
    let expected = (count as usize).checked_mul(key_bytes).and_then(|keys| keys.checked_add(GUARD_HEADER_BYTES));
    if expected != Some(bytes.len()) {
        return Err(format!("{} bytes do not hold {count} keys of {key_words} words", bytes.len()));
    }
    let keys = bytes[GUARD_HEADER_BYTES..]
        .chunks_exact(key_bytes)
        .map(|key| key.chunks_exact(4).map(|chunk| word(chunk, 0)).collect())
        .collect();
    Ok(GuardFile { shader_set, vendor_id, device_id, driver_version, key_words, flags, keys })
}

/// What `logs/shader-guard.txt` holds, or `None` when no file exists: the header fields and each key as hex words.
/// A damaged file is named as such instead of being left out, so a report shows it.
pub fn guard_listing(quarantine: Option<&[u8]>, compiling: Option<&[u8]>, suspects: Option<&[u8]>) -> Option<String> {
    if quarantine.is_none() && compiling.is_none() && suspects.is_none() {
        return None;
    }
    let mut text = String::from("Shader guard of the CoreGS 2.0 shader cache (TASK-170). Keys are 32-bit words in hex.\n");
    for (name, bytes, meaning) in [
        (QUARANTINE_FILE, quarantine, "variants never compiled again on this card and driver"),
        (SUSPECTS_FILE, suspects, "variants that ended one run; compiled again, quarantined if they end a second"),
        (COMPILING_FILE, compiling, "variants in flight when the last run stopped"),
    ] {
        let Some(bytes) = bytes else { continue };
        let _ = writeln!(text, "\n{name} ({meaning}):");
        match parse_guard_file(bytes) {
            Err(problem) => {
                let _ = writeln!(text, "  damaged: {problem}");
            }
            Ok(file) => {
                let _ = writeln!(
                    text,
                    "  shader set {:016x}, vendor {:04x}, device {:04x}, driver {:08x}, key words {}, flags {:#x} ({}), {} keys",
                    file.shader_set,
                    file.vendor_id,
                    file.device_id,
                    file.driver_version,
                    file.key_words,
                    file.flags,
                    if file.serial() { "one variant at a time" } else { "parallel" },
                    file.keys.len()
                );
                for (index, key) in file.keys.iter().enumerate() {
                    let words: Vec<String> = key.iter().map(|value| format!("{value:08x}")).collect();
                    let _ = writeln!(text, "  {index:>3}: {}", words.join(" "));
                }
            }
        }
    }
    Some(text)
}

/// What the guard files say now: compared before and after an attempt, it shows whether the attempt got the renderer further.
/// The raw bytes of the three files, so that every change counts: a first crash that makes a suspect, a second that
/// quarantines it, a file rewritten with the same key count (a driver update, a new shader set). An absent file is empty.
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct GuardSnapshot {
    /// `quarantine.bin`: variants never compiled again, and the serial flag.
    pub quarantine: Vec<u8>,
    /// `compiling.bin`: keys left by a run that stopped inside those compiles.
    pub compiling: Vec<u8>,
    /// `suspects.bin`: keys that ended one run.
    pub suspects: Vec<u8>,
}

/// The snapshot of the three files' bytes.
pub fn snapshot_of(quarantine: Option<&[u8]>, compiling: Option<&[u8]>, suspects: Option<&[u8]>) -> GuardSnapshot {
    GuardSnapshot {
        quarantine: quarantine.unwrap_or_default().to_vec(),
        compiling: compiling.unwrap_or_default().to_vec(),
        suspects: suspects.unwrap_or_default().to_vec(),
    }
}

/// What one prepare attempt reports.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct AttemptReport {
    /// The program ended with the cache ready.
    pub finished: bool,
    /// The most variants this attempt reported as done (`shaders progress D T`).
    pub best_done: u32,
}

/// Why the attempts stopped without a finished prepare.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum GaveUp {
    /// The last attempt ended without getting further (no more variants done, the guard files unchanged).
    NoProgress,
    /// [`MAX_PREPARE_ATTEMPTS`] attempts were made.
    LimitReached,
}

/// How the attempts ended; `attempts` is the number made.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum PrepareOutcome {
    Finished { attempts: u32 },
    GaveUp { attempts: u32, why: GaveUp },
    Cancelled { attempts: u32 },
    Failed { attempts: u32, message: String },
}

/// Runs the prepare until it finishes. `run(n)` makes attempt `n` (1-based); `snapshot` reads the guard files; `cancelled` is
/// the player's Cancel. After an attempt that did not finish, the next one runs only if the attempt made progress: the guard
/// files changed (any byte of quarantine.bin, compiling.bin or suspects.bin: a first crash that makes a suspect counts) or more variants were done than in
/// any earlier attempt. At most `max_attempts` run.
pub fn run_attempts(
    max_attempts: u32,
    cancelled: &dyn Fn() -> bool,
    snapshot: &dyn Fn() -> GuardSnapshot,
    run: &mut dyn FnMut(u32) -> Result<AttemptReport, String>,
) -> PrepareOutcome {
    let mut best_done = 0;
    let mut attempts = 0;
    while attempts < max_attempts {
        if cancelled() {
            return PrepareOutcome::Cancelled { attempts };
        }
        attempts += 1;
        let before = snapshot();
        let report = match run(attempts) {
            Ok(report) => report,
            Err(message) => return PrepareOutcome::Failed { attempts, message },
        };
        if cancelled() {
            return PrepareOutcome::Cancelled { attempts };
        }
        if report.finished {
            return PrepareOutcome::Finished { attempts };
        }
        let progressed = snapshot() != before || report.best_done > best_done;
        best_done = best_done.max(report.best_done);
        if !progressed {
            return PrepareOutcome::GaveUp { attempts, why: GaveUp::NoProgress };
        }
    }
    PrepareOutcome::GaveUp { attempts, why: GaveUp::LimitReached }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::cell::{Cell, RefCell};

    /// A guard file as the renderer writes it: 3-word keys keep the fixtures short.
    fn guard_bytes(flags: u32, keys: &[[u32; 3]]) -> Vec<u8> {
        let mut bytes = GUARD_MAGIC.to_vec();
        bytes.extend_from_slice(&0x1122_3344_5566_7788u64.to_le_bytes());
        for value in [0x1002u32, 0x73bf, 0x0002_0001, 3, flags, keys.len() as u32] {
            bytes.extend_from_slice(&value.to_le_bytes());
        }
        for key in keys {
            for value in key {
                bytes.extend_from_slice(&value.to_le_bytes());
            }
        }
        bytes
    }

    #[test]
    fn a_guard_file_reads_back_with_its_header_and_keys() {
        let file = parse_guard_file(&guard_bytes(1, &[[1, 2, 3], [0xdead_beef, 0, 7]])).unwrap();
        assert_eq!((file.shader_set, file.vendor_id, file.device_id, file.driver_version), (0x1122_3344_5566_7788, 0x1002, 0x73bf, 0x0002_0001));
        assert_eq!(file.key_words, 3);
        assert!(file.serial());
        assert_eq!(file.keys, vec![vec![1, 2, 3], vec![0xdead_beef, 0, 7]]);
        assert!(parse_guard_file(&guard_bytes(0, &[])).unwrap().keys.is_empty());
    }

    #[test]
    fn a_damaged_guard_file_is_refused_with_a_reason() {
        let good = guard_bytes(0, &[[1, 2, 3]]);
        assert!(parse_guard_file(&good[..20]).unwrap_err().contains("shorter"));
        assert!(parse_guard_file(&good[..good.len() - 1]).unwrap_err().contains("do not hold"));
        let mut wrong_magic = good.clone();
        wrong_magic[0] = b'X';
        assert!(parse_guard_file(&wrong_magic).unwrap_err().contains("magic"));
        let mut huge_count = good.clone();
        huge_count[36..40].copy_from_slice(&u32::MAX.to_le_bytes());
        assert!(parse_guard_file(&huge_count).is_err());
        let mut no_words = good;
        no_words[28..32].copy_from_slice(&0u32.to_le_bytes());
        assert!(parse_guard_file(&no_words).unwrap_err().contains("plausible"));
    }

    #[test]
    fn the_listing_shows_header_fields_and_hex_keys() {
        let quarantine = guard_bytes(1, &[[0x10, 0xabcdef, 0xffff_ffff]]);
        let text = guard_listing(Some(&quarantine), None, None).unwrap();
        assert!(text.contains("quarantine.bin"));
        assert!(!text.contains("compiling.bin"));
        assert!(text.contains("shader set 1122334455667788, vendor 1002, device 73bf, driver 00020001"));
        assert!(text.contains("one variant at a time"));
        assert!(text.contains("1 keys"));
        assert!(text.contains("00000010 00abcdef ffffffff"));
    }

    #[test]
    fn the_listing_names_a_damaged_file_and_is_absent_without_files() {
        assert_eq!(guard_listing(None, None, None), None);
        let good = guard_bytes(0, &[[1, 2, 3]]);
        let text = guard_listing(Some(&good), Some(&good[..10]), None).unwrap();
        assert!(text.contains("compiling.bin") && text.contains("damaged"));
        assert!(text.contains("00000001 00000002 00000003"));
    }

    #[test]
    fn the_listing_shows_the_suspects_between_the_quarantine_and_the_keys_in_flight() {
        let suspects = guard_bytes(0, &[[0xaa, 0xbb, 0xcc], [0x01, 0x02, 0x03]]);
        let text = guard_listing(None, None, Some(&suspects)).unwrap();
        assert!(text.contains("suspects.bin") && !text.contains("quarantine.bin") && text.contains("2 keys"));
        assert!(text.contains("000000aa 000000bb 000000cc") && text.contains("00000001 00000002 00000003"));
        let all = guard_listing(Some(&suspects), Some(&suspects), Some(&suspects)).unwrap();
        let order: Vec<usize> = ["quarantine.bin", "suspects.bin", "compiling.bin"].iter().map(|name| all.find(name).unwrap()).collect();
        assert!(order.windows(2).all(|pair| pair[0] < pair[1]), "{all}");
    }

    #[test]
    fn a_snapshot_holds_the_bytes_of_the_three_files_and_an_absent_file_is_empty() {
        let quarantine = guard_bytes(1, &[[1, 1, 1], [2, 2, 2]]);
        let compiling = guard_bytes(0, &[[3, 3, 3]]);
        let suspects = guard_bytes(0, &[[4, 4, 4]]);
        let snapshot = snapshot_of(Some(&quarantine), Some(&compiling), Some(&suspects));
        assert_eq!((snapshot.quarantine, snapshot.compiling, snapshot.suspects), (quarantine, compiling, suspects));
        assert_eq!(snapshot_of(None, None, None), GuardSnapshot::default());
    }

    #[test]
    fn a_new_suspect_changes_the_snapshot() {
        let none = snapshot_of(Some(&guard_bytes(1, &[])), None, None);
        let suspect = snapshot_of(Some(&guard_bytes(1, &[])), None, Some(&guard_bytes(0, &[[7, 7, 7]])));
        assert_ne!(none, suspect);
        // Then the same key ends a second run: quarantined, the suspect gone.
        let quarantined = snapshot_of(Some(&guard_bytes(1, &[[7, 7, 7]])), None, None);
        assert_ne!(suspect, quarantined);
    }

    #[test]
    fn the_same_counts_with_other_bytes_is_a_change() {
        // A driver update rewrites a file with the same number of keys; the counts cannot tell, the bytes can.
        let old = snapshot_of(Some(&guard_bytes(1, &[[1, 2, 3]])), None, None);
        let mut rewritten = guard_bytes(1, &[[1, 2, 3]]);
        rewritten[24] ^= 1; // the driver version of the header
        assert_ne!(old, snapshot_of(Some(&rewritten), None, None));
        assert_ne!(old, snapshot_of(Some(&guard_bytes(1, &[[1, 2, 4]])), None, None));
        assert_eq!(old, snapshot_of(Some(&guard_bytes(1, &[[1, 2, 3]])), None, None));
    }

    /// Runs `run_attempts` over scripted attempts: each is (report, snapshot after it).
    fn play(script: &[(AttemptReport, GuardSnapshot)], max: u32) -> (PrepareOutcome, u32) {
        let ran = Cell::new(0u32);
        let current = RefCell::new(GuardSnapshot::default());
        let outcome = run_attempts(max, &|| false, &|| current.borrow().clone(), &mut |attempt| {
            ran.set(attempt);
            let (report, after) = script[attempt as usize - 1].clone();
            *current.borrow_mut() = after;
            Ok(report)
        });
        (outcome, ran.get())
    }

    /// A guard state with `quarantined` keys quarantined, `suspects` suspected and `in_flight` left in compiling.bin.
    fn state(quarantined: u32, suspects: u32, in_flight: u32) -> GuardSnapshot {
        let keys = |count: u32, tag: u32| -> Vec<[u32; 3]> { (0..count).map(|n| [tag, n, 0]).collect() };
        snapshot_of(
            Some(&guard_bytes(1, &keys(quarantined, 1))),
            if in_flight == 0 { None } else { Some(guard_bytes(0, &keys(in_flight, 3))) }.as_deref(),
            if suspects == 0 { None } else { Some(guard_bytes(0, &keys(suspects, 2))) }.as_deref(),
        )
    }

    fn crash(best_done: u32, quarantined: u32, in_flight: u32) -> (AttemptReport, GuardSnapshot) {
        (AttemptReport { finished: false, best_done }, state(quarantined, 0, in_flight))
    }

    fn finish() -> (AttemptReport, GuardSnapshot) {
        (AttemptReport { finished: true, best_done: 10 }, state(3, 0, 0))
    }

    #[test]
    fn the_first_attempt_that_finishes_ends_it() {
        assert_eq!(play(&[finish()], 12), (PrepareOutcome::Finished { attempts: 1 }, 1));
    }

    #[test]
    fn it_retries_while_the_guard_gets_further_and_stops_on_success() {
        let script = [crash(5, 0, 1), crash(0, 1, 1), crash(0, 2, 1), finish()];
        assert_eq!(play(&script, 12), (PrepareOutcome::Finished { attempts: 4 }, 4));
    }

    #[test]
    fn a_first_crash_that_only_makes_a_suspect_counts_as_progress() {
        // Attempt 1 ends inside a compile (a suspect appears), attempt 2 ends inside the same one (quarantined), attempt 3 finishes.
        let first = (AttemptReport::default(), state(0, 0, 1));
        let second = (AttemptReport::default(), state(0, 1, 1));
        let third = (AttemptReport::default(), state(1, 0, 1));
        assert_eq!(play(&[first, second, third, finish()], 20), (PrepareOutcome::Finished { attempts: 4 }, 4));
    }

    #[test]
    fn the_same_counts_in_other_bytes_count_as_progress() {
        let mut moved = state(1, 0, 0);
        moved.quarantine[24] ^= 1;
        let script = [(AttemptReport::default(), state(1, 0, 0)), (AttemptReport::default(), moved), finish()];
        // Attempt 1 changed the empty start; attempt 2 changed the bytes with the same key count; attempt 3 finishes.
        assert_eq!(play(&script, 20), (PrepareOutcome::Finished { attempts: 3 }, 3));
    }

    #[test]
    fn the_bound_covers_seven_quarantines_of_two_crashes_each() {
        // The worst case of the RX 6700 XT report: 7 bad variants, each a suspect then quarantined, then the attempt that finishes.
        let mut script = Vec::new();
        for bad in 0..7u32 {
            script.push((AttemptReport::default(), state(bad, 1, 1)));
            script.push((AttemptReport::default(), state(bad + 1, 0, 1)));
        }
        script.push(finish());
        assert_eq!(script.len(), 15);
        assert!(MAX_PREPARE_ATTEMPTS >= 16, "the bound leaves room for the serial switch");
        assert_eq!(play(&script, MAX_PREPARE_ATTEMPTS), (PrepareOutcome::Finished { attempts: 15 }, 15));
        assert_eq!(MAX_PREPARE_ATTEMPTS, 20);
    }

    #[test]
    fn more_variants_done_counts_as_progress_even_if_the_guard_is_unchanged() {
        let script = [crash(5, 0, 1), crash(9, 0, 1), crash(9, 0, 1)];
        // The second attempt did more than the first; the third did neither more nor changed the guard files.
        assert_eq!(play(&script, 12), (PrepareOutcome::GaveUp { attempts: 3, why: GaveUp::NoProgress }, 3));
    }

    #[test]
    fn an_attempt_that_changes_nothing_stops_the_retries() {
        let silent = (AttemptReport::default(), GuardSnapshot::default());
        assert_eq!(play(&[silent], 12), (PrepareOutcome::GaveUp { attempts: 1, why: GaveUp::NoProgress }, 1));
    }

    #[test]
    fn the_limit_ends_attempts_that_keep_progressing() {
        let script: Vec<_> = (0..40).map(|n| crash(0, n + 1, 1)).collect();
        assert_eq!(
            play(&script, MAX_PREPARE_ATTEMPTS),
            (PrepareOutcome::GaveUp { attempts: MAX_PREPARE_ATTEMPTS, why: GaveUp::LimitReached }, MAX_PREPARE_ATTEMPTS)
        );
        assert_eq!(play(&script, 3), (PrepareOutcome::GaveUp { attempts: 3, why: GaveUp::LimitReached }, 3));
    }

    #[test]
    fn a_program_that_cannot_start_fails_without_retrying() {
        let outcome = run_attempts(12, &|| false, &GuardSnapshot::default, &mut |_| Err("no gt4.exe".to_string()));
        assert_eq!(outcome, PrepareOutcome::Failed { attempts: 1, message: "no gt4.exe".to_string() });
    }

    #[test]
    fn cancel_stops_before_and_after_an_attempt() {
        let outcome = run_attempts(12, &|| true, &GuardSnapshot::default, &mut |_| panic!("no attempt after Cancel"));
        assert_eq!(outcome, PrepareOutcome::Cancelled { attempts: 0 });
        let asked = Cell::new(0);
        let outcome = run_attempts(
            12,
            &|| {
                asked.set(asked.get() + 1);
                asked.get() > 1
            },
            &GuardSnapshot::default,
            &mut |_| Ok(AttemptReport { finished: false, best_done: 1 }),
        );
        assert_eq!(outcome, PrepareOutcome::Cancelled { attempts: 1 });
    }
}
