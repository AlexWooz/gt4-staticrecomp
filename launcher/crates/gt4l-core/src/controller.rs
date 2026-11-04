//! TASK-148: the controller the player chose on the Controls page (the `controller` setting). The launcher's pad service
//! and the game (`GT4_PAD`, `native/pad_mapping.cpp`) read the same text and pick the same controller.

/// TASK-148: the `controller` setting when no controller is chosen: the game reads controller 1 (`active_pad`).
pub const AUTO_CONTROLLER: &str = "auto";

/// TASK-148: the controller the player chose on the Controls page. Stored as `<guid>` or `<guid>/<serial>`: the GUID SDL3
/// gives the model (32 hex digits) and, when the controller reports one, its serial number, so one of two identical pads
/// can be told apart. The game gets the same text as `GT4_PAD` and picks the same controller (`native/pad_mapping.cpp`).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct PadChoice {
    pub guid: String,
    pub serial: String,
}

impl PadChoice {
    /// `None` for `auto` and for any text that is not a choice.
    pub fn parse(value: &str) -> Option<PadChoice> {
        let (guid, serial) = match value.split_once('/') {
            Some((guid, serial)) => (guid, serial),
            None => (value, ""),
        };
        let guid_ok = guid.len() == 32 && guid.bytes().all(|byte| byte.is_ascii_hexdigit());
        let serial_ok = serial.len() <= 64 && serial.bytes().all(|byte| (0x21..0x7F).contains(&byte));
        (guid_ok && serial_ok).then(|| PadChoice { guid: guid.to_ascii_lowercase(), serial: serial.to_string() })
    }

    /// The stored text of a controller with this GUID and serial (an empty serial is left out).
    pub fn key(guid: &str, serial: &str) -> String {
        let serial: String = serial.chars().filter(|character| (0x21..0x7F).contains(&(*character as u32))).take(64).collect();
        if serial.is_empty() {
            guid.to_ascii_lowercase()
        } else {
            format!("{}/{serial}", guid.to_ascii_lowercase())
        }
    }
}

/// `auto` or a well-formed choice.
pub fn is_controller_setting(value: &str) -> bool {
    value == AUTO_CONTROLLER || PadChoice::parse(value).is_some()
}

#[cfg(test)]
mod tests {
    use super::*;

    const XBOX: &str = "030000005e0400008e02000014010000";
    const DUALSENSE: &str = "050000004c050000e60c000000810000";

    #[test]
    fn a_choice_is_a_guid_and_an_optional_serial() {
        assert_eq!(PadChoice::parse("auto"), None);
        assert_eq!(PadChoice::parse("not a guid"), None);
        assert_eq!(PadChoice::parse(&XBOX[..31]), None, "a GUID is 32 hex digits");
        assert_eq!(PadChoice::parse(XBOX), Some(PadChoice { guid: XBOX.into(), serial: String::new() }));
        let with_serial = PadChoice::parse(&format!("{DUALSENSE}/a0:ab:51:12:34:56")).unwrap();
        assert_eq!(with_serial.serial, "a0:ab:51:12:34:56");
        assert_eq!(PadChoice::key(&DUALSENSE.to_ascii_uppercase(), "a0:ab:51:12:34:56"), format!("{DUALSENSE}/a0:ab:51:12:34:56"));
        assert_eq!(PadChoice::key(XBOX, ""), XBOX);
        assert!(is_controller_setting("auto") && is_controller_setting(XBOX) && !is_controller_setting(""));
    }

}
