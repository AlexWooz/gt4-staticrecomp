//! The names of one game's items on a memory card (TASK-101 item 1, owner 2026-10-23).
//!
//! Gran Turismo 4 writes its game data as `BASCUS-97328GAMEDATA`; Spec II writes `BASCUS-97436GAMEDATA`. Each game's
//! replays start with that game's serial. The names come from the game table of the launcher (`gt4l_core::games`), so
//! no library rule depends on one global name.

/// The names a game's items carry on a card.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct SaveNames {
    /// The game data folder, fixed by the game (`BASCUS-97328GAMEDATA`).
    pub game_save: &'static str,
    /// The serial that starts every replay of the game. A replay is this prefix, a letter (`R` full replay, `B` best
    /// lap) and the game's own tag: four digits on Gran Turismo 4 (`BASCUS-97328R0001`), seven letters and digits on
    /// Spec II (`BASCUS-97436R5MWiYYY`, seen on a PCSX2 card on 2026-11-03; issue #185).
    pub replay_prefix: &'static str,
}

/// The shortest and longest tag after the replay letter (four digits on GT4, seven characters on Spec II).
const REPLAY_TAG_LENGTHS: std::ops::RangeInclusive<usize> = 4..=7;

impl SaveNames {
    pub const fn new(game_save: &'static str, replay_prefix: &'static str) -> SaveNames {
        SaveNames { game_save, replay_prefix }
    }

    /// True for this game's game data folder.
    pub fn is_game_save(&self, name: &str) -> bool {
        name == self.game_save
    }

    /// True for this game's replay names: the prefix, a letter `R` or `B`, and a tag of four to seven ASCII letters and
    /// digits. Before 1.3.0-rc10 only four digits were accepted, so every Spec II replay was left off the library and lost
    /// when the next card was built (issue #185).
    pub fn is_replay(&self, name: &str) -> bool {
        let bytes = name.as_bytes();
        let letter = self.replay_prefix.len();
        name.len() > letter
            && name.starts_with(self.replay_prefix)
            && matches!(bytes[letter], b'R' | b'B')
            && REPLAY_TAG_LENGTHS.contains(&(name.len() - letter - 1))
            && bytes[letter + 1..].iter().all(u8::is_ascii_alphanumeric)
    }

    /// True when this game owns the item: its game data or one of its replays.
    pub fn owns(&self, name: &str) -> bool {
        self.is_game_save(name) || self.is_replay(name)
    }

    /// The name of full replay `number` (`BASCUS-97328R0001` for 1).
    pub fn full_replay_name(&self, number: u32) -> String {
        format!("{}R{number:04}", self.replay_prefix)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const GT4: SaveNames = SaveNames::new("BASCUS-97328GAMEDATA", "BASCUS-97328");
    const SPEC2: SaveNames = SaveNames::new("BASCUS-97436GAMEDATA", "BASCUS-97436");

    #[test]
    fn each_game_owns_only_its_own_names() {
        assert!(GT4.owns("BASCUS-97328GAMEDATA"));
        assert!(GT4.owns("BASCUS-97328R0001"));
        assert!(GT4.owns("BASCUS-97328B0042"));
        assert!(!GT4.owns("BASCUS-97436GAMEDATA"));
        assert!(!GT4.owns("BASCUS-97436R0001"));
        assert!(SPEC2.owns("BASCUS-97436GAMEDATA"));
        assert!(SPEC2.owns("BASCUS-97436R0001"));
        assert!(!SPEC2.owns("BASCUS-97328GAMEDATA"));
        assert!(!SPEC2.owns("BASCUS-97328R0001"));
    }

    #[test]
    fn replay_names_need_a_letter_and_a_tag_of_four_to_seven_characters() {
        assert!(!GT4.is_replay("BASCUS-97328X0001"));
        assert!(!GT4.is_replay("BASCUS-97328R001"));
        assert!(!GT4.is_replay("BASCUS-97328R00000001"));
        assert!(!GT4.is_replay("BASCUS-97328R00-1"));
        assert!(!GT4.is_replay("BASCUS-97328R"));
        assert!(!GT4.is_replay("BASCUS-9732"));
    }

    // Issue #185: the names Spec II gives its replays (two replays saved on a PCSX2 card, 2026-11-03).
    #[test]
    fn spec2_replay_names_are_seven_letters_and_digits() {
        assert!(SPEC2.is_replay("BASCUS-97436R5MWiYYY"));
        assert!(SPEC2.is_replay("BASCUS-97436R8MWiY4Y"));
        assert!(SPEC2.owns("BASCUS-97436R8MWiY4Y"));
        assert!(!GT4.owns("BASCUS-97436R8MWiY4Y"));
        assert!(!SPEC2.owns("BASCUS-97328R8MWiY4Y"));
    }

    #[test]
    fn a_new_replay_name_keeps_the_games_prefix() {
        assert_eq!(GT4.full_replay_name(1), "BASCUS-97328R0001");
        assert_eq!(SPEC2.full_replay_name(12), "BASCUS-97436R0012");
    }
}
