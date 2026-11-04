//! The games the launcher installs and runs (TASK-101, release 1.3.0, owner 2026-10-23;
//! `docs/MULTI-GAME-LAUNCHER-PLAN.md`).
//!
//! Two games side by side: Gran Turismo 4 v2.00 (retail, SCUS-97328) and Gran Turismo 4 Spec II 1.10 (SCUS-97436
//! lineage). The ids (`gt4`, `spec2`) are used in folder names, settings and logs; the display names are only for the
//! player. Each game has its own executable (their generated code shares symbol names, decision 0072), disc copy,
//! prepared files, runtime card, save library and replays. Settings, controls, GSdx and the textures are shared.

/// One game of the table.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Game {
    /// `gt4` or `spec2`: folder name, settings value, log text.
    pub id: &'static str,
    /// "Gran Turismo 4" / "Gran Turismo 4 Spec II".
    pub display_name: &'static str,
    /// The serial of the disc (`SYSTEM.CNF`).
    pub serial: &'static str,
    /// The game program inside `<program folder>\game`.
    pub executable: &'static str,
    /// The prepared ELF, in `games\<id>\prepared`. Its file name is the argv0 rule of the game (the donor derives the
    /// boot name from it).
    pub elf_name: &'static str,
    /// The home-screen logo asset (a file under `ui/assets`).
    pub logo_asset: &'static str,
    /// The game data folder on the memory card: `BASCUS-97328GAMEDATA` (GT4), `BASCUS-97436GAMEDATA` (Spec II).
    pub save_folder: &'static str,
    /// The prefix of the game's replays (the serial): `BASCUS-97328` (GT4); `BASCUS-97436` for Spec II, confirmed with two
    /// Spec II replays on a PCSX2 card (2026-11-03), whose tags are seven characters (`gt4saves::names`).
    pub replay_prefix: &'static str,
}

/// Gran Turismo 4 v2.00 (USA, SCUS-97328).
pub const GT4: Game = Game {
    id: "gt4",
    display_name: "Gran Turismo 4",
    serial: "SCUS-97328",
    executable: "gt4.exe",
    elf_name: "SCUS_973.28",
    logo_asset: "assets/logo-gt4.png",
    save_folder: "BASCUS-97328GAMEDATA",
    replay_prefix: "BASCUS-97328",
};

/// Gran Turismo 4 Spec II 1.10 (SCUS-97436 lineage). The authors' logo file is not in the launcher yet: the page shows
/// the GT4 logo with a "Spec II" line under it (placeholder, to be replaced with the authors' logo and credit).
pub const SPEC2: Game = Game {
    id: "spec2",
    display_name: "Gran Turismo 4 Spec II",
    serial: "SCUS-97436",
    executable: "gt4-spec2.exe",
    elf_name: "SCUS_974.36",
    logo_asset: "assets/logo-gt4.png",
    save_folder: "BASCUS-97436GAMEDATA",
    replay_prefix: "BASCUS-97436",
};

/// Every game, in display order.
pub const GAMES: &[&Game] = &[&GT4, &SPEC2];

/// The game of an id (`gt4`, `spec2`).
pub fn by_id(id: &str) -> Option<&'static Game> {
    GAMES.iter().copied().find(|game| game.id == id)
}

/// The game of an id, or Gran Turismo 4 for an unknown one (a settings file from a newer launcher).
pub fn by_id_or_default(id: &str) -> &'static Game {
    by_id(id).unwrap_or(&GT4)
}

/// The game the home screen shows at start (owner, 2026-10-23): the game played last (the stored choice) when it is
/// installed; otherwise the first installed game in table order; with nothing installed, the stored choice or Gran
/// Turismo 4 (the page then offers "Install").
pub fn initial_selection(stored: Option<&str>, installed: &[&'static Game]) -> &'static Game {
    let stored = stored.and_then(by_id);
    match stored {
        Some(game) if installed.contains(&game) => game,
        _ => installed.first().copied().or(stored).unwrap_or(&GT4),
    }
}

/// The `[game] selected` value after `game` was set up: the first game installed becomes the stored choice; once a game
/// is stored, installing another changes nothing (only playing does, see [`after_play`]).
pub fn after_install(stored: Option<&str>, installed_before: &[&'static Game], game: &'static Game) -> Option<String> {
    match stored.and_then(by_id) {
        Some(known) if !installed_before.is_empty() => Some(known.id.to_string()),
        _ => Some(game.id.to_string()),
    }
}

/// The `[game] selected` value once `game` was started: the game played last.
pub fn after_play(game: &'static Game) -> Option<String> {
    Some(game.id.to_string())
}

/// The folder (under `games`) of a game's files, in the data folder and in the user data folder.
pub const GAMES_FOLDER: &str = "games";

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_table_names_two_games_with_their_own_programs() {
        assert_eq!(GAMES.len(), 2);
        assert_eq!(by_id("gt4"), Some(&GT4));
        assert_eq!(by_id("spec2"), Some(&SPEC2));
        assert_eq!(by_id("gt5"), None);
        assert_eq!(by_id_or_default("gt5"), &GT4);
        assert_eq!(GT4.executable, "gt4.exe");
        assert_eq!(SPEC2.executable, "gt4-spec2.exe");
        assert_eq!((GT4.elf_name, SPEC2.elf_name), ("SCUS_973.28", "SCUS_974.36"));
        assert_eq!((GT4.display_name, SPEC2.display_name), ("Gran Turismo 4", "Gran Turismo 4 Spec II"));
        assert_ne!(GT4.id, SPEC2.id);
        assert_ne!(GT4.executable, SPEC2.executable, "the two games cannot share one executable (decision 0072)");
        assert_eq!((GT4.save_folder, SPEC2.save_folder), ("BASCUS-97328GAMEDATA", "BASCUS-97436GAMEDATA"));
        assert_eq!((GT4.replay_prefix, SPEC2.replay_prefix), ("BASCUS-97328", "BASCUS-97436"));
    }

    #[test]
    fn the_first_installed_game_is_selected_then_the_one_played_last() {
        let both: &[&'static Game] = &[&GT4, &SPEC2];
        // Nothing stored yet: the first installed game.
        assert_eq!(initial_selection(None, &[&SPEC2]), &SPEC2);
        assert_eq!(initial_selection(None, both), &GT4);
        // Played last wins while it is installed.
        assert_eq!(initial_selection(Some("spec2"), both), &SPEC2);
        assert_eq!(initial_selection(Some("gt4"), both), &GT4);
        // A stored game that is gone falls back to an installed one; with nothing installed the stored one is shown.
        assert_eq!(initial_selection(Some("spec2"), &[&GT4]), &GT4);
        assert_eq!(initial_selection(Some("spec2"), &[]), &SPEC2);
        assert_eq!(initial_selection(Some("unknown"), &[]), &GT4);
        // Installing: the first game becomes the choice, a second install keeps it, playing changes it.
        assert_eq!(after_install(None, &[], &SPEC2).as_deref(), Some("spec2"));
        assert_eq!(after_install(Some("spec2"), &[&SPEC2], &GT4).as_deref(), Some("spec2"));
        assert_eq!(after_install(Some("unknown"), &[], &GT4).as_deref(), Some("gt4"));
        assert_eq!(after_play(&GT4).as_deref(), Some("gt4"));
    }

    #[test]
    fn the_game_executable_constant_is_the_gt4_one() {
        assert_eq!(crate::paths::GAME_EXECUTABLE, GT4.executable);
    }
}
