//! TASK-211: the command line of a direct launch, the couch path from Steam.
//!
//! `--skip-launcher` starts the launcher without its window and ends it with the game, so the front end that started it
//! (Steam, Big Picture, a shortcut) gets the process back when the game closes. `--game=retail|spec2` (`--game retail`
//! works too, and `gt4` is the alias of `retail`) chooses the game, because the page where a player picks it is skipped.
//!
//! Everything in this module is a pure function over the arguments. `main` reads them before any window exists; the
//! launch itself is [`crate::direct_launch`]. A normal start (no `--skip-launcher`) ignores `--game`: it opens the
//! launcher exactly as before.

use gt4l_core::games::{self, Game, GT4, SPEC2};
use gt4l_core::language::Language;
use gt4l_core::messages;

/// `--skip-launcher`: the launcher stays hidden and ends with the game (TASK-211).
pub const SKIP_LAUNCHER: &str = "--skip-launcher";
/// `--game=<id>`: the game a direct launch starts; `--game <id>` is accepted as well.
pub const GAME: &str = "--game";

/// The game a `--game` value names: `retail` (alias `gt4`) is Gran Turismo 4 v2.00 and `spec2` is Gran Turismo 4
/// Spec II. Case and surrounding spaces do not matter.
pub fn game_of(value: &str) -> Option<&'static Game> {
    match value.trim().to_ascii_lowercase().as_str() {
        "retail" | "gt4" => Some(&GT4),
        "spec2" => Some(&SPEC2),
        _ => None,
    }
}

/// A `--game` value that cannot be used. The text is built in the player's language when the launcher shows it.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum GameProblem {
    /// `--game` with no value after it, or `--game=` with nothing.
    NoValue,
    /// `--game=<value>` names no game of this launcher.
    Unknown(String),
}

impl GameProblem {
    /// The sentence for the player: what to type instead.
    pub fn message(&self, language: Language) -> String {
        match self {
            GameProblem::NoValue => messages::text("direct_launch.game_needs_value", language).to_string(),
            GameProblem::Unknown(value) => messages::render("direct_launch.unknown_game", language, &[("value", value)]),
        }
    }
}

/// Where the game of a direct launch came from (the log names it).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum GameSource {
    /// `--game=...`
    CommandLine,
    /// The game played last (the stored choice, `config.selected_game`).
    LastPlayed,
    /// No game was played yet (or the stored one is unknown): the game the launcher shows at start.
    StartSelection,
}

/// What the command line asks for. `game` is `None` without `--game`: a direct launch then uses the game played last.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct LaunchArguments {
    /// `--skip-launcher` was given: a hidden launch that ends with the game.
    pub skip_launcher: bool,
    /// The game the last `--game` named.
    pub game: Option<&'static Game>,
    /// The last `--game` that could not be used. Only an error for a direct launch; a normal start ignores it.
    pub game_problem: Option<GameProblem>,
}

impl LaunchArguments {
    /// The last `--game` wins, as on any command line: a later valid value clears an earlier error.
    fn set_game(&mut self, value: &str) {
        match game_of(value) {
            Some(game) => {
                self.game = Some(game);
                self.game_problem = None;
            }
            None => {
                self.game = None;
                self.game_problem = Some(GameProblem::Unknown(value.trim().to_string()));
            }
        }
    }

    fn set_game_missing(&mut self) {
        self.game = None;
        self.game_problem = Some(GameProblem::NoValue);
    }

    /// What a `--skip-launcher` start runs: the game of `--game`, or `None` for the game played last. `None` outside
    /// tells that this is a normal start, where `--game` changes nothing.
    pub fn direct_game(&self) -> Option<Result<Option<&'static Game>, &GameProblem>> {
        if !self.skip_launcher {
            return None;
        }
        Some(match &self.game_problem {
            Some(problem) => Err(problem),
            None => Ok(self.game),
        })
    }

    /// The arguments a safe-mode restart repeats (TASK-091 with TASK-211): a page that has to draw again with WebView2
    /// GPU acceleration off must not lose the direct launch. A normal start repeats nothing.
    pub fn restart_arguments(&self) -> Vec<String> {
        if !self.skip_launcher {
            return Vec::new();
        }
        let mut arguments = vec![SKIP_LAUNCHER.to_string()];
        match (&self.game, &self.game_problem) {
            (Some(game), _) => arguments.push(format!("{GAME}={}", game.id)),
            (None, Some(GameProblem::Unknown(value))) => arguments.push(format!("{GAME}={value}")),
            (None, Some(GameProblem::NoValue)) => arguments.push(GAME.to_string()),
            (None, None) => {}
        }
        arguments
    }
}

/// Reads the launcher's own arguments (the command line without the program name). Arguments this module does not know
/// (`--webview-restart`, an update restart, Steam's own quoting leftovers) are left alone.
pub fn parse<I, S>(arguments: I) -> LaunchArguments
where
    I: IntoIterator<Item = S>,
    S: AsRef<str>,
{
    let arguments: Vec<String> = arguments.into_iter().map(|argument| argument.as_ref().to_string()).collect();
    let mut parsed = LaunchArguments::default();
    let mut index = 0;
    while index < arguments.len() {
        let argument = arguments[index].as_str();
        if argument == SKIP_LAUNCHER {
            parsed.skip_launcher = true;
        } else if let Some(value) = argument.strip_prefix(GAME).and_then(|rest| rest.strip_prefix('=')) {
            if value.trim().is_empty() {
                parsed.set_game_missing();
            } else {
                parsed.set_game(value);
            }
        } else if argument == GAME {
            // The value is the next argument; another flag is never a game name.
            match arguments.get(index + 1) {
                Some(next) if !next.starts_with("--") => {
                    parsed.set_game(next);
                    index += 1;
                }
                _ => parsed.set_game_missing(),
            }
        }
        index += 1;
    }
    parsed
}

/// The game a direct launch starts and where it came from. `stored` is the game played last
/// (`config.selected_game`); `fallback` is the game the launcher shows at start. Only a direct launch calls this, so a
/// broken `--game` is an error here; a normal start never asks.
pub fn resolve_game(
    arguments: &LaunchArguments,
    stored: Option<&str>,
    fallback: &'static Game,
) -> Result<(&'static Game, GameSource), GameProblem> {
    if let Some(problem) = &arguments.game_problem {
        return Err(problem.clone());
    }
    if let Some(game) = arguments.game {
        return Ok((game, GameSource::CommandLine));
    }
    match stored.and_then(games::by_id) {
        Some(game) => Ok((game, GameSource::LastPlayed)),
        None => Ok((fallback, GameSource::StartSelection)),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn arguments(values: &[&str]) -> LaunchArguments {
        parse(values.iter().copied())
    }

    #[test]
    fn no_argument_asks_for_a_normal_start() {
        let parsed = arguments(&[]);
        assert!(!parsed.skip_launcher);
        assert_eq!(parsed.game, None);
        assert_eq!(parsed.game_problem, None);
        assert_eq!(parsed.direct_game(), None, "a start without --skip-launcher is the launcher page, as always");
        assert!(parsed.restart_arguments().is_empty());
    }

    #[test]
    fn the_skip_flag_runs_a_hidden_launch_with_the_game_played_last() {
        let parsed = arguments(&["--skip-launcher"]);
        assert!(parsed.skip_launcher);
        assert_eq!(parsed.direct_game(), Some(Ok(None)));
        assert_eq!(parsed.restart_arguments(), vec!["--skip-launcher"]);
    }

    #[test]
    fn the_skip_flag_is_only_the_exact_argument() {
        assert!(arguments(&["--skip-launcher"]).skip_launcher);
        assert!(!arguments(&["--skip-launcher=1"]).skip_launcher);
        assert!(!arguments(&["--Skip-Launcher"]).skip_launcher, "the flag is exact, as the other launcher flags");
        assert!(!arguments(&["--webview-restart"]).skip_launcher);
    }

    #[test]
    fn every_game_name_and_alias_maps_to_its_game() {
        assert_eq!(game_of("retail"), Some(&GT4));
        assert_eq!(game_of("gt4"), Some(&GT4));
        assert_eq!(game_of("spec2"), Some(&SPEC2));
        assert_eq!(game_of(" Retail "), Some(&GT4), "case and spaces do not matter");
        assert_eq!(game_of("SPEC2"), Some(&SPEC2));
        assert_eq!(game_of("gt5"), None);
        assert_eq!(game_of("spec-2"), None);
        assert_eq!(game_of(""), None);
    }

    #[test]
    fn the_game_flag_takes_an_equals_form_and_a_separate_value() {
        let equals = arguments(&["--skip-launcher", "--game=retail"]);
        assert_eq!(equals.game, Some(&GT4));
        let separate = arguments(&["--skip-launcher", "--game", "spec2"]);
        assert_eq!(separate.game, Some(&SPEC2));
        assert_eq!(separate.direct_game(), Some(Ok(Some(&SPEC2))));
        assert_eq!(separate.restart_arguments(), vec!["--skip-launcher", "--game=spec2"]);
    }

    #[test]
    fn a_missing_game_value_is_reported_and_never_eats_the_next_flag() {
        let last = arguments(&["--skip-launcher", "--game"]);
        assert_eq!(last.game_problem, Some(GameProblem::NoValue));
        assert!(last.skip_launcher);
        assert_eq!(last.restart_arguments(), vec!["--skip-launcher", "--game"]);
        let empty = arguments(&["--skip-launcher", "--game="]);
        assert_eq!(empty.game_problem, Some(GameProblem::NoValue));
        let followed_by_a_flag = arguments(&["--skip-launcher", "--game", "--webview-restart"]);
        assert_eq!(followed_by_a_flag.game_problem, Some(GameProblem::NoValue));
        assert!(followed_by_a_flag.skip_launcher);
        assert_eq!(followed_by_a_flag.game, None);
    }

    #[test]
    fn an_unknown_game_is_reported_with_the_value() {
        let parsed = arguments(&["--skip-launcher", "--game=gt5"]);
        assert_eq!(parsed.game, None);
        assert_eq!(parsed.game_problem, Some(GameProblem::Unknown("gt5".into())));
        assert_eq!(parsed.direct_game(), Some(Err(&GameProblem::Unknown("gt5".into()))));
        assert_eq!(parsed.restart_arguments(), vec!["--skip-launcher", "--game=gt5"]);
    }

    #[test]
    fn the_last_game_flag_wins() {
        let repaired = arguments(&["--skip-launcher", "--game=gt5", "--game=spec2"]);
        assert_eq!((repaired.game, repaired.game_problem), (Some(&SPEC2), None));
        let broken = arguments(&["--skip-launcher", "--game=spec2", "--game=gt5"]);
        assert_eq!((broken.game, broken.game_problem), (None, Some(GameProblem::Unknown("gt5".into()))));
    }

    #[test]
    fn a_normal_start_ignores_the_game_flag() {
        let parsed = arguments(&["--game=spec2"]);
        assert!(!parsed.skip_launcher);
        assert_eq!(parsed.direct_game(), None, "the launcher window and its own selection, exactly as before");
        assert!(parsed.restart_arguments().is_empty());
        let broken = arguments(&["--game=gt5"]);
        assert_eq!(broken.direct_game(), None, "a bad --game alone never fails a normal start");
    }

    #[test]
    fn a_direct_launch_uses_the_game_played_last_or_the_start_selection() {
        let none = arguments(&["--skip-launcher"]);
        assert_eq!(resolve_game(&none, Some("spec2"), &GT4), Ok((&SPEC2, GameSource::LastPlayed)));
        assert_eq!(resolve_game(&none, None, &GT4), Ok((&GT4, GameSource::StartSelection)));
        assert_eq!(resolve_game(&none, Some("gt5"), &SPEC2), Ok((&SPEC2, GameSource::StartSelection)));
        let asked = arguments(&["--skip-launcher", "--game=retail"]);
        assert_eq!(resolve_game(&asked, Some("spec2"), &SPEC2), Ok((&GT4, GameSource::CommandLine)), "--game wins");
        let broken = arguments(&["--skip-launcher", "--game=gt5"]);
        assert_eq!(resolve_game(&broken, Some("spec2"), &SPEC2), Err(GameProblem::Unknown("gt5".into())));
    }

    #[test]
    fn the_game_problem_text_names_the_two_games() {
        let language = Language::fallback();
        let unknown = GameProblem::Unknown("gt5".into()).message(language);
        assert!(unknown.contains("gt5"), "{unknown}");
        assert!(unknown.contains("--game=retail") && unknown.contains("--game=spec2"), "{unknown}");
        let missing = GameProblem::NoValue.message(language);
        assert!(missing.contains("--game=retail") && missing.contains("--game=spec2"), "{missing}");
    }
}
