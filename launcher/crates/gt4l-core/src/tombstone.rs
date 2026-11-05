//! TASK-180: an Android tombstone as text for the log bundle. When the game's process dies of a native crash, Android 12+
//! hands the app its tombstone (ApplicationExitInfo::getTraceInputStream) as a protobuf (AOSP
//! `system/core/debuggerd/proto/tombstone.proto`). The launcher keeps the raw file and writes this text next to it: the
//! signal, the abort message, the causes, the crashing thread's backtrace in the familiar `#00 pc ...` form, the other
//! threads' names and the process's last log lines. Fields not read here stay in the raw file.
//!
//! Pure: a small protobuf wire reader (varint, 64-bit, length-delimited, 32-bit) and the field numbers of tombstone.proto.

/// One field of a protobuf message.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Value<'a> {
    Varint(u64),
    Fixed64(u64),
    Bytes(&'a [u8]),
    Fixed32(u32),
}

fn varint(data: &[u8], at: &mut usize) -> Option<u64> {
    let mut value = 0u64;
    for shift in (0..64).step_by(7) {
        let byte = *data.get(*at)?;
        *at += 1;
        value |= u64::from(byte & 0x7F) << shift;
        if byte & 0x80 == 0 {
            return Some(value);
        }
    }
    None
}

/// The fields of one message, in order. `None` when the bytes are not a well-formed message.
fn fields(data: &[u8]) -> Option<Vec<(u64, Value<'_>)>> {
    let mut out = Vec::new();
    let mut at = 0usize;
    while at < data.len() {
        let key = varint(data, &mut at)?;
        let number = key >> 3;
        let value = match key & 7 {
            0 => Value::Varint(varint(data, &mut at)?),
            1 => {
                let bytes: [u8; 8] = data.get(at..at + 8)?.try_into().ok()?;
                at += 8;
                Value::Fixed64(u64::from_le_bytes(bytes))
            }
            2 => {
                let length = usize::try_from(varint(data, &mut at)?).ok()?;
                let end = at.checked_add(length)?;
                let bytes = data.get(at..end)?;
                at = end;
                Value::Bytes(bytes)
            }
            5 => {
                let bytes: [u8; 4] = data.get(at..at + 4)?.try_into().ok()?;
                at += 4;
                Value::Fixed32(u32::from_le_bytes(bytes))
            }
            _ => return None,
        };
        if number == 0 {
            return None;
        }
        out.push((number, value));
    }
    Some(out)
}

fn text_of(fields: &[(u64, Value<'_>)], number: u64) -> String {
    fields
        .iter()
        .find_map(|(field, value)| match value {
            Value::Bytes(bytes) if *field == number => Some(String::from_utf8_lossy(bytes).into_owned()),
            _ => None,
        })
        .unwrap_or_default()
}

fn texts_of(fields: &[(u64, Value<'_>)], number: u64) -> Vec<String> {
    fields
        .iter()
        .filter_map(|(field, value)| match value {
            Value::Bytes(bytes) if *field == number => Some(String::from_utf8_lossy(bytes).into_owned()),
            _ => None,
        })
        .collect()
}

fn number_of(fields: &[(u64, Value<'_>)], number: u64) -> u64 {
    fields
        .iter()
        .find_map(|(field, value)| match value {
            Value::Varint(value) | Value::Fixed64(value) if *field == number => Some(*value),
            Value::Fixed32(value) if *field == number => Some(u64::from(*value)),
            _ => None,
        })
        .unwrap_or(0)
}

fn messages_of<'a>(fields: &[(u64, Value<'a>)], number: u64) -> Vec<Vec<(u64, Value<'a>)>> {
    fields
        .iter()
        .filter_map(|(field, value)| match value {
            Value::Bytes(bytes) if *field == number => self::fields(bytes),
            _ => None,
        })
        .collect()
}

/// The backtrace lines of a Thread message (`current_backtrace`, field 4), as debuggerd prints them.
fn backtrace(thread: &[(u64, Value<'_>)]) -> Vec<String> {
    messages_of(thread, 4)
        .iter()
        .enumerate()
        .map(|(index, frame)| {
            // BacktraceFrame: rel_pc 1, pc 2, sp 3, function_name 4, function_offset 5, file_name 6, build_id 8.
            let mut line = format!("  #{index:02} pc {:016x}  {}", number_of(frame, 1), text_of(frame, 6));
            let function = text_of(frame, 4);
            if !function.is_empty() {
                line.push_str(&format!(" ({function}+{})", number_of(frame, 5)));
            }
            let build_id = text_of(frame, 8);
            if !build_id.is_empty() {
                line.push_str(&format!(" (BuildId: {build_id})"));
            }
            line
        })
        .collect()
}

/// The text of a tombstone, or `None` when `data` is not one. `log_lines` caps the process's own log lines kept.
pub fn to_text(data: &[u8], log_lines: usize) -> Option<String> {
    let top = fields(data)?;
    let mut out = Vec::new();
    out.push(format!("Build fingerprint: '{}'", text_of(&top, 2)));
    out.push(format!("Timestamp: {}", text_of(&top, 4)));
    let pid = number_of(&top, 5);
    let tid = number_of(&top, 6);
    out.push(format!("pid: {pid}, tid: {tid}, uid: {}, process uptime: {}s", number_of(&top, 7), number_of(&top, 20)));
    out.push(format!("Cmdline: {}", texts_of(&top, 9).join(" ")));
    // Signal: number 1, name 2, code 3, code_name 4, has_fault_address 8, fault_address 9.
    if let Some(signal) = messages_of(&top, 10).first() {
        let mut line = format!(
            "signal {} ({}), code {} ({})",
            number_of(signal, 1),
            text_of(signal, 2),
            number_of(signal, 3),
            text_of(signal, 4)
        );
        if number_of(signal, 8) != 0 {
            line.push_str(&format!(", fault addr 0x{:x}", number_of(signal, 9)));
        }
        out.push(line);
    }
    let abort = text_of(&top, 14);
    if !abort.is_empty() {
        out.push(format!("Abort message: '{abort}'"));
    }
    for cause in messages_of(&top, 15) {
        out.push(format!("Cause: {}", text_of(&cause, 1)));
    }
    // threads: map<uint32, Thread> (field 16): entries of key 1 and value 2. Thread: id 1, name 2.
    let mut others = Vec::new();
    for entry in messages_of(&top, 16) {
        let key = number_of(&entry, 1);
        let Some(thread) = messages_of(&entry, 2).into_iter().next() else {
            continue;
        };
        if key == tid {
            out.push(String::new());
            out.push(format!("Crashing thread {key} \"{}\" backtrace:", text_of(&thread, 2)));
            out.extend(backtrace(&thread));
            for note in texts_of(&thread, 7) {
                out.push(format!("  note: {note}"));
            }
        } else {
            others.push((key, thread));
        }
    }
    if !others.is_empty() {
        out.push(String::new());
        out.push("Other threads:".to_string());
        for (key, thread) in &others {
            out.push(format!("thread {key} \"{}\"", text_of(thread, 2)));
            out.extend(backtrace(thread).into_iter().take(12));
        }
    }
    // log_buffers (18): LogBuffer name 1, logs 2 (LogMessage: timestamp 1, pid 2, tid 3, priority 4, tag 5, message 6).
    let mut log = Vec::new();
    for buffer in messages_of(&top, 18) {
        let name = text_of(&buffer, 1);
        for message in messages_of(&buffer, 2) {
            log.push(format!(
                "{} {} {:5} {:5} {} {}: {}",
                name,
                text_of(&message, 1),
                number_of(&message, 2),
                number_of(&message, 3),
                number_of(&message, 4),
                text_of(&message, 5),
                text_of(&message, 6).trim_end()
            ));
        }
    }
    if !log.is_empty() {
        out.push(String::new());
        out.push(format!("Last log lines of the process ({} of {}):", log.len().min(log_lines), log.len()));
        let skip = log.len().saturating_sub(log_lines);
        out.extend(log.into_iter().skip(skip));
    }
    out.push(String::new());
    Some(out.join("\n"))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn put_varint(out: &mut Vec<u8>, mut value: u64) {
        loop {
            let byte = (value & 0x7F) as u8;
            value >>= 7;
            if value == 0 {
                out.push(byte);
                return;
            }
            out.push(byte | 0x80);
        }
    }

    fn number(out: &mut Vec<u8>, field: u64, value: u64) {
        put_varint(out, field << 3);
        put_varint(out, value);
    }

    fn bytes(out: &mut Vec<u8>, field: u64, value: &[u8]) {
        put_varint(out, (field << 3) | 2);
        put_varint(out, value.len() as u64);
        out.extend_from_slice(value);
    }

    /// A synthetic tombstone with the fields the text reads (no real crash data).
    fn sample() -> Vec<u8> {
        let mut frame = Vec::new();
        number(&mut frame, 1, 0x1234);
        bytes(&mut frame, 4, b"gt4_step");
        number(&mut frame, 5, 16);
        bytes(&mut frame, 6, b"/data/app/lib/arm64/libgt4.so");
        bytes(&mut frame, 8, b"abcdef");
        let mut thread = Vec::new();
        number(&mut thread, 1, 42);
        bytes(&mut thread, 2, b"SDLThread");
        bytes(&mut thread, 4, &frame);
        let mut entry = Vec::new();
        number(&mut entry, 1, 42);
        bytes(&mut entry, 2, &thread);
        let mut signal = Vec::new();
        number(&mut signal, 1, 11);
        bytes(&mut signal, 2, b"SIGSEGV");
        number(&mut signal, 3, 1);
        bytes(&mut signal, 4, b"SEGV_MAPERR");
        number(&mut signal, 8, 1);
        number(&mut signal, 9, 0x10);
        let mut message = Vec::new();
        bytes(&mut message, 5, b"GT4Recomp-game");
        bytes(&mut message, 6, b"[core2] frame 120");
        let mut buffer = Vec::new();
        bytes(&mut buffer, 1, b"main");
        bytes(&mut buffer, 2, &message);
        let mut top = Vec::new();
        bytes(&mut top, 2, b"samsung/test/fingerprint");
        number(&mut top, 5, 40);
        number(&mut top, 6, 42);
        bytes(&mut top, 9, b"io.github.moesuito.gt4recomp:game");
        bytes(&mut top, 10, &signal);
        bytes(&mut top, 14, b"assert failed");
        bytes(&mut top, 16, &entry);
        bytes(&mut top, 18, &buffer);
        top
    }

    #[test]
    fn a_tombstone_reads_as_debuggerd_text() {
        let text = to_text(&sample(), 100).unwrap();
        assert!(text.contains("Build fingerprint: 'samsung/test/fingerprint'"));
        assert!(text.contains("pid: 40, tid: 42"));
        assert!(text.contains("Cmdline: io.github.moesuito.gt4recomp:game"));
        assert!(text.contains("signal 11 (SIGSEGV), code 1 (SEGV_MAPERR), fault addr 0x10"));
        assert!(text.contains("Abort message: 'assert failed'"));
        assert!(text.contains("Crashing thread 42 \"SDLThread\" backtrace:"));
        assert!(text.contains("#00 pc 0000000000001234  /data/app/lib/arm64/libgt4.so (gt4_step+16) (BuildId: abcdef)"));
        assert!(text.contains("GT4Recomp-game: [core2] frame 120"));
    }

    #[test]
    fn other_bytes_are_not_a_tombstone() {
        assert!(to_text(b"\xff\xff\xff", 10).is_none());
        assert!(to_text(&[0x0A, 0x05, b'a'], 10).is_none(), "a field longer than the data");
    }
}
