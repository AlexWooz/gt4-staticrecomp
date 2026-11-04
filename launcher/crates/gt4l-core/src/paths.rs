//! Where the launcher keeps its files (TASK-091, release 1.2.1, owner 2026-10-22).
//!
//! Two places, and nothing found through the registry or a stored absolute path:
//!
//! - **Large data** lives in `<install folder>\data`, found relative to `GT4Recomp.exe` ([`DataDirs`]): the disc
//!   image copy, the prepared game files, the gt4-gs versions with their caches, the runtime card, sessions, logs
//!   and the custom textures folder. The whole program folder can be copied to another drive or PC and started from
//!   there (portable). When `<install>\data` exists but cannot be written (a copy into Program Files made without
//!   the installer), it is still read, and new writes go to `%LOCALAPPDATA%\GT4Recomp` (logged).
//! - **User data** lives per Windows user in `%APPDATA%\GT4Recomp` ([`UserDirs`]): settings, controls, the save
//!   library (`Saves`, `Replays`) and the card backups.
//!
//! Launchers 1.0.0 to 1.2.0 kept the large data in `%LOCALAPPDATA%\GT4Recomp` and the save library in
//! `Documents\Gran Turismo 4`. The launcher moves both at its first start ([`crate::relocate`]); until that has
//! happened (a failed copy, or the game running), the old places stay in use ([`LegacyLocations`]).
//!
//! A reinstall into another folder leaves the data of the old folder behind; the setup records it as
//! `PreviousDataDir` (HKLM), and the launcher moves its large data the same way at its first start
//! ([`LegacyLocations::previous_data`]). Until then that folder is a read layer too.

use std::ffi::OsString;
use std::path::{Component, Path, PathBuf};

use sha2::{Digest, Sha256};

use crate::games::{Game, GT4, GAMES_FOLDER};

/// Development and tests: replaces `<install>\data` (no read-only layer, no 1.2.0 folders unless given).
pub const DATA_DIR_OVERRIDE_VAR: &str = "GT4RECOMP_DATA_DIR";

/// Development and tests: replaces `%APPDATA%\GT4Recomp` (the user data folder).
pub const USER_DIR_OVERRIDE_VAR: &str = "GT4RECOMP_USER_DIR";

/// Development and tests: replaces `%LOCALAPPDATA%\GT4Recomp` (the 1.2.0 data folder and the write fallback).
pub const LOCAL_DIR_OVERRIDE_VAR: &str = "GT4RECOMP_LOCAL_DIR";

/// Development and tests: stands in for the Documents folder (where 1.2.0 kept `Gran Turismo 4`), so a test run
/// never touches the real Documents. Not for players.
pub const DOCUMENTS_DIR_OVERRIDE_VAR: &str = "GT4RECOMP_DOCUMENTS_DIR";

/// Name of the large data folder inside the install folder.
pub const DATA_FOLDER_NAME: &str = "data";

/// Name of the user data folder inside a portable folder (TASK-098). The installer build keeps that data in
/// `%APPDATA%\GT4Recomp`; the game (TASK-097) reads its settings from `<install>\user\config` when this folder exists.
pub const PORTABLE_USER_FOLDER: &str = "user";

/// Name of the per-user folders in `%APPDATA%` and `%LOCALAPPDATA%`.
pub const APP_FOLDER_NAME: &str = "GT4Recomp";

/// Name of the player's folder that launchers 1.0.0 to 1.2.0 kept inside Documents.
pub const USER_FOLDER_NAME: &str = "Gran Turismo 4";

/// What that folder is renamed to once its saves and replays are in `%APPDATA%\GT4Recomp`.
pub const MOVED_USER_FOLDER_NAME: &str = "Gran Turismo 4 (moved to AppData)";

/// Name of the game executable inside the game folder.
pub const GAME_EXECUTABLE: &str = "gt4.exe";

/// Name of the custom texture folder (TASK-088; TASK-093: gt4-gs 1.2.1 reads the folder itself).
pub const TEXTURES_FOLDER_NAME: &str = "Textures";

/// The setup receipt name (`gt4l_disc::setup::RECEIPT_FILE`), repeated here so the paths need no disc crate.
pub const PREPARE_RECEIPT: &str = "prepare.json";

/// Name of the controls file (`gt4controls::CONTROLS_FILE_NAME`).
pub const CONTROLS_FILE: &str = "controls.toml";

/// The large data folder. `root` takes every write; `read_layers` are older or read-only folders that still hold
/// game files or a renderer (the read-only `<install>\data`, or the 1.2.0 `%LOCALAPPDATA%\GT4Recomp` until its move
/// finished). Reads of the game files, the renderer and the textures use the first folder that has them.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct DataDirs {
    root: PathBuf,
    read_layers: Vec<PathBuf>,
    /// The custom textures folder when it could not be moved yet (it stays in use where it is).
    textures_in_place: Option<PathBuf>,
    /// The game whose files the per-game methods name (`prepared`, `disc_folder`, `runtime_card`, ...). Gran Turismo 4
    /// unless [`DataDirs::for_game`] says otherwise. Everything else (renderer, logs, sessions, textures) is shared.
    game: &'static Game,
    /// Whether the 1.2.x flat game folders (`<data>\disc`, `<data>\prepared`, `<data>\runtime`) are still read for
    /// Gran Turismo 4 until their move into `games\gt4` has happened (`crate::games_layout`).
    flat_gt4: bool,
}

impl DataDirs {
    pub fn new(root: impl Into<PathBuf>) -> Self {
        DataDirs { root: root.into(), read_layers: Vec::new(), textures_in_place: None, game: &GT4, flat_gt4: true }
    }

    /// The same folders, naming the files of `game`. Settings, the renderer, logs, sessions and textures are shared.
    pub fn for_game(&self, game: &'static Game) -> DataDirs {
        DataDirs { game, ..self.clone() }
    }

    /// The game the per-game methods name.
    pub fn game(&self) -> &'static Game {
        self.game
    }

    /// `<data>\games\<id>` in `layer`: the files of one game.
    fn game_folder_in(layer: &Path, game: &Game) -> PathBuf {
        layer.join(GAMES_FOLDER).join(game.id)
    }

    /// Where every write of this game goes: `<data>\games\<id>`.
    pub fn game_root(&self) -> PathBuf {
        DataDirs::game_folder_in(&self.root, self.game)
    }

    /// The place of this game's files in `layer`, and the older flat place of Gran Turismo 4 (1.2.x) while it is read.
    fn game_candidates<'a>(&'a self, layer: &'a PathBuf) -> impl Iterator<Item = PathBuf> + 'a {
        let flat = (self.game.id == GT4.id && self.flat_gt4).then(|| layer.clone());
        std::iter::once(DataDirs::game_folder_in(layer, self.game)).chain(flat)
    }

    /// Every place of this game's files in the write folder: `games\<id>`, and for Gran Turismo 4 the 1.2.x flat
    /// folder too. "Remove game" clears all of them.
    pub fn write_places(&self) -> Vec<PathBuf> {
        self.game_candidates(&self.root).collect()
    }

    /// Stops reading the 1.2.x flat folders for Gran Turismo 4 (their move into `games\gt4` finished).
    pub fn set_flat_gt4(&mut self, flat: bool) {
        self.flat_gt4 = flat;
    }

    /// The 1.2.x flat folder of Gran Turismo 4's files: `<data>` itself (`disc`, `prepared`, `runtime` inside).
    pub fn flat_gt4_root(&self) -> &Path {
        &self.root
    }

    /// A data folder with read layers after it. Layers equal to the root (or to each other) are dropped.
    pub fn with_read_layers(root: impl Into<PathBuf>, layers: impl IntoIterator<Item = PathBuf>) -> Self {
        let mut dirs = DataDirs::new(root);
        for layer in layers {
            dirs.add_read_layer(layer);
        }
        dirs
    }

    pub fn add_read_layer(&mut self, layer: PathBuf) {
        if !same_path(&layer, &self.root) && !self.read_layers.iter().any(|known| same_path(known, &layer)) {
            self.read_layers.push(layer);
        }
    }

    /// Uses `folder` for the custom textures (a folder whose move has not happened yet).
    pub fn set_textures_in_place(&mut self, folder: Option<PathBuf>) {
        self.textures_in_place = folder;
    }

    /// The folder every write goes to.
    pub fn root(&self) -> &Path {
        &self.root
    }

    pub fn read_layers(&self) -> &[PathBuf] {
        &self.read_layers
    }

    /// The root first, then the read layers.
    fn layers(&self) -> impl Iterator<Item = &PathBuf> {
        std::iter::once(&self.root).chain(self.read_layers.iter())
    }

    /// The folder whose game files are used: the first with a setup receipt, else the first with any game file
    /// (so a damaged setup is reported), else the root.
    pub fn game_files_root(&self) -> PathBuf {
        let places: Vec<PathBuf> = self.layers().flat_map(|layer| self.game_candidates(layer)).collect();
        if let Some(place) = places.iter().find(|place| place.join("prepared").join(PREPARE_RECEIPT).is_file()) {
            return place.clone();
        }
        if let Some(place) = places.iter().find(|place| place.join("prepared").exists() || place.join("disc").exists()) {
            return place.clone();
        }
        self.game_root()
    }

    /// Prepared game files in use: `CORE.GT4`, `SCUS_973.28` (the rebuilt ELF) and `prepare.json`.
    pub fn prepared(&self) -> PathBuf {
        self.game_files_root().join("prepared")
    }

    /// The copy of the player's disc image in use (`disc\SCUS-97328.iso`). Protected user data (see `guard`).
    pub fn disc_folder(&self) -> PathBuf {
        self.game_files_root().join("disc")
    }

    /// Where setup writes the prepared files (always the root's `games\<id>`).
    /// RetroAchievements runtime cache directory under `data/games/gt4/runtime/ra_cache` (TASK-133).
    pub fn retroachievements_cache(&self) -> PathBuf {
        DataDirs::game_folder_in(&self.root, &crate::games::GT4).join("runtime").join("ra_cache")
    }

    pub fn setup_prepared(&self) -> PathBuf {
        self.game_root().join("prepared")
    }

    /// Where setup copies the disc image (always the root's `games\<id>`).
    pub fn setup_disc_folder(&self) -> PathBuf {
        self.game_root().join("disc")
    }

    /// Setup writes here first and renames the folder into place at the end.
    pub fn prepared_staging(&self) -> PathBuf {
        self.game_root().join("prepared.tmp")
    }

    /// The installed gt4-gs versions in use: the first folder with a `gs\current.txt`, else the root's `gs`.
    pub fn renderer_root(&self) -> PathBuf {
        self.layers()
            .map(|layer| layer.join("gs"))
            .find(|gs| gs.join("current.txt").is_file())
            .unwrap_or_else(|| self.root.join("gs"))
    }

    /// Where a gt4-gs install or update writes (always the root's `gs`), and where its caches go.
    pub fn renderer_install_root(&self) -> PathBuf {
        self.root.join("gs")
    }

    /// The memory card image that the game reads. A missing file is a blank card (see `sio2.cpp`).
    pub fn runtime_card(&self) -> PathBuf {
        self.game_root().join("runtime").join("card.ps2")
    }

    pub fn sessions(&self) -> PathBuf {
        self.root.join("sessions")
    }

    pub fn session(&self, name: &str) -> PathBuf {
        self.sessions().join(name)
    }

    pub fn logs(&self) -> PathBuf {
        self.root.join("logs")
    }

    /// The custom texture folder (TASK-088): one not moved yet, else the first folder that has `Textures`, else the
    /// root's. The texture pack goes straight in this folder (gt4-gs 1.2.1, TASK-093).
    pub fn textures(&self) -> PathBuf {
        if let Some(folder) = &self.textures_in_place {
            return folder.clone();
        }
        self.layers()
            .map(|layer| layer.join(TEXTURES_FOLDER_NAME))
            .find(|folder| folder.is_dir())
            .unwrap_or_else(|| self.root.join(TEXTURES_FOLDER_NAME))
    }

    /// Where the save library goes when the user data folder cannot be used: `<data>\Gran Turismo 4`.
    pub fn fallback_user_root(&self) -> PathBuf {
        self.root.join(USER_FOLDER_NAME)
    }

    /// Marker of the move of the 1.2.0 large data into this folder (`crate::relocate`).
    pub fn relocation_marker(&self) -> PathBuf {
        self.root.join("migration-1.2.1.json")
    }

    /// Marker of the move of the 1.2.x game folders into `games\gt4` (`crate::games_layout`, TASK-101).
    pub fn games_layout_marker(&self) -> PathBuf {
        self.root.join("migration-multi-game.json")
    }

    /// Marker of the move of the large data of an earlier install's data folder (`previous`). One marker per source
    /// folder, named from a hash of its path, so the move runs once per folder and a second reinstall elsewhere
    /// gets its own.
    pub fn previous_data_marker(&self, previous: &Path) -> PathBuf {
        self.root.join(format!("migration-from-{}.json", folder_key(previous)))
    }
}

/// A short, stable key of a folder: the first 16 hex digits of the SHA-256 of its normalised path (case, `.`/`..` and
/// separators do not count). Names the migration markers, and the single-instance lock of a portable folder.
pub fn folder_key(folder: &Path) -> String {
    let digest = Sha256::digest(normalized(folder).join("\\").as_bytes());
    digest.iter().take(8).map(|byte| format!("{byte:02x}")).collect()
}

/// How the user data folder was found.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum UserRootSource {
    /// `%APPDATA%\GT4Recomp` (the normal case).
    AppData,
    /// `GT4RECOMP_USER_DIR`, or `<GT4RECOMP_DATA_DIR>\user` (development and tests).
    Override,
    /// The 1.2.0 places, because the move to AppData has not happened yet. The text says why (for the log).
    Legacy(String),
    /// The save library could not be used where it should be; it is in the data folder. The text says why.
    Fallback(String),
    /// The portable build: `<exe dir>\user`, beside the program (TASK-098).
    Portable,
}

/// The user data: settings, controls, the save library and the card backups. Normally all under
/// `%APPDATA%\GT4Recomp`; before the move from 1.2.0, the old places ([`UserDirs::legacy`]).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct UserDirs {
    root: PathBuf,
    saves: PathBuf,
    replays: PathBuf,
    config_dir: PathBuf,
    controls_file: PathBuf,
    card_backups: PathBuf,
    source: UserRootSource,
    /// The game whose library `saves`, `replays` and `card_backups` name. Gran Turismo 4 unless [`UserDirs::for_game`].
    game: &'static Game,
    /// Gran Turismo 4's library is still in the flat 1.2.x place (`saves`, `replays`, `card_backups` as given) until
    /// its move into `games\gt4` has happened. Any other game always has its own folder under `games`.
    flat_gt4: bool,
}

impl UserDirs {
    /// The standard layout under one folder: `config\settings.toml`, `controls.toml`, and one library per game under
    /// `games\<id>` (`Saves`, `Replays`, `backups`). Gran Turismo 4's 1.2.x library, flat in this folder, is used only
    /// after [`UserDirs::set_flat_gt4`]`(true)` (its move into `games\gt4` has not happened).
    pub fn new(root: impl Into<PathBuf>, source: UserRootSource) -> Self {
        let root = root.into();
        UserDirs {
            saves: root.join("Saves"),
            replays: root.join("Replays"),
            config_dir: root.join("config"),
            controls_file: root.join(CONTROLS_FILE),
            card_backups: root.join("backups"),
            root,
            source,
            game: &GT4,
            flat_gt4: false,
        }
    }

    /// The same folders, naming the library of `game` (TASK-101): `games\<id>\Saves`, `Replays`, `backups` under the
    /// folder that holds the library. Gran Turismo 4's flat 1.2.x folders are used until
    /// [`UserDirs::set_flat_gt4`] says their move happened.
    pub fn for_game(&self, game: &'static Game) -> UserDirs {
        UserDirs { game, ..self.clone() }
    }

    /// The game whose library the methods below name.
    pub fn game(&self) -> &'static Game {
        self.game
    }

    /// Gran Turismo 4's library is still in the flat 1.2.x place.
    fn in_flat_place(&self) -> bool {
        self.game.id == GT4.id && self.flat_gt4
    }

    /// `<library folder>\games\<id>`: where a game's library is once it is in the `games` layout.
    pub fn game_library(&self) -> PathBuf {
        self.root.join(GAMES_FOLDER).join(self.game.id)
    }

    /// Whether Gran Turismo 4's flat library (1.2.x) is in use. The move into `games\gt4` sets this to false.
    pub fn set_flat_gt4(&mut self, flat: bool) {
        self.flat_gt4 = flat;
    }

    /// The flat 1.2.x library folders of Gran Turismo 4: `Saves`, `Replays` and the card backups.
    pub fn flat_gt4_places(&self) -> [(&'static str, PathBuf); 3] {
        [("Saves", self.saves.clone()), ("Replays", self.replays.clone()), ("backups", self.card_backups.clone())]
    }

    /// The 1.2.0 places: settings, controls and card backups in `local` (`%LOCALAPPDATA%\GT4Recomp`), the save
    /// library in `player` (`Documents\Gran Turismo 4`).
    pub fn legacy(local: &Path, player: &Path, reason: String) -> Self {
        UserDirs {
            root: player.to_path_buf(),
            saves: player.join("Saves"),
            replays: player.join("Replays"),
            config_dir: local.join("config"),
            controls_file: local.join(CONTROLS_FILE),
            card_backups: local.join("library").join("backups"),
            source: UserRootSource::Legacy(reason),
            game: &GT4,
            flat_gt4: true,
        }
    }

    /// The same settings and controls, with the save library (saves, replays, card backups) at `root` (the
    /// data-folder fallback).
    pub fn with_library_at(&self, root: &Path, source: UserRootSource) -> Self {
        UserDirs {
            root: root.to_path_buf(),
            saves: root.join("Saves"),
            replays: root.join("Replays"),
            card_backups: root.join("backups"),
            source,
            // A library folder that already holds a flat Saves folder keeps it; a new one starts in the `games` layout.
            flat_gt4: root.join("Saves").is_dir(),
            ..self.clone()
        }
    }

    /// The folder shown to the player and opened by "Open folder".
    pub fn root(&self) -> &Path {
        &self.root
    }

    pub fn source(&self) -> &UserRootSource {
        &self.source
    }

    /// `Saves`: the active save (`Active`), inactive slots (`Save NN`) and harvest copies (`.backup`).
    pub fn saves(&self) -> PathBuf {
        if self.in_flat_place() { self.saves.clone() } else { self.game_library().join("Saves") }
    }

    /// `Replays`: one folder per replay.
    pub fn replays(&self) -> PathBuf {
        if self.in_flat_place() { self.replays.clone() } else { self.game_library().join("Replays") }
    }

    pub fn config_dir(&self) -> &Path {
        &self.config_dir
    }

    /// RetroAchievements user config file under `config/retroachievements.toml` (TASK-133).
    pub fn retroachievements_file(&self) -> PathBuf {
        self.config_dir.join("retroachievements.toml")
    }

    pub fn config_file(&self) -> PathBuf {
        self.config_dir.join("settings.toml")
    }

    pub fn controls_file(&self) -> PathBuf {
        self.controls_file.clone()
    }

    /// Card images kept before `runtime\card.ps2` is replaced.
    pub fn card_backups(&self) -> PathBuf {
        if self.in_flat_place() { self.card_backups.clone() } else { self.game_library().join("backups") }
    }

    /// Marker of the one-time move of the save library from launchers before 1.0.0 (`gt4saves::migration`).
    pub fn library_migration_marker(&self) -> PathBuf {
        self.config_dir.join("library-migration.json")
    }

    /// Marker of the move of the 1.2.0 user data into this folder (`crate::relocate`).
    pub fn relocation_marker(&self) -> PathBuf {
        self.config_dir.join("migration-1.2.1.json")
    }
}

/// The places launchers 1.0.0 to 1.2.0 used, as far as they can be named on this PC.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct LegacyLocations {
    /// `%LOCALAPPDATA%\GT4Recomp`: settings, controls, disc copy, prepared files, gt4-gs, logs, sessions, card backups.
    pub local: Option<PathBuf>,
    /// `Documents\Gran Turismo 4`: Saves, Replays and Textures.
    pub documents_player: Option<PathBuf>,
    /// The data folder of an earlier 1.2.1 install in another folder (`PreviousDataDir`, TASK-091). Its large data is
    /// moved like the 1.2.0 folder's, with a marker of its own per folder.
    pub previous_data: Option<PathBuf>,
}

impl LegacyLocations {
    /// `Documents\Gran Turismo 4 (moved to AppData)`: the renamed folder after the move.
    pub fn documents_moved(&self) -> Option<PathBuf> {
        self.documents_player.as_ref().map(|player| player.with_file_name(MOVED_USER_FOLDER_NAME))
    }

    /// `%LOCALAPPDATA%\GT4Recomp\Gran Turismo 4`: the 1.2.0 save library when Documents was not available.
    pub fn local_player(&self) -> Option<PathBuf> {
        self.local.as_ref().map(|local| local.join(USER_FOLDER_NAME))
    }
}

/// Everything the path resolution reads from the process environment, so tests can give their own.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct LayoutInputs {
    /// The folder of `GT4Recomp.exe`.
    pub exe_dir: PathBuf,
    pub data_override: Option<OsString>,
    pub user_override: Option<OsString>,
    pub local_override: Option<OsString>,
    pub documents_override: Option<OsString>,
    /// `%APPDATA%` (Roaming).
    pub app_data: Option<PathBuf>,
    /// `%LOCALAPPDATA%`.
    pub local_app_data: Option<PathBuf>,
    /// The Documents known folder as Windows reports it (`None` when the call failed).
    pub known_documents: Option<PathBuf>,
    /// The `PreviousDataDir` the setup recorded (HKLM), read by the caller; ignored under the test overrides.
    pub previous_data: Option<PathBuf>,
    /// The portable build (TASK-098): `<exe dir>\user` and `<exe dir>\data` only, no read layers, no system folders.
    pub portable: bool,
}

impl LayoutInputs {
    /// Reads the variables of this process. `known_documents` comes from the Windows known-folder API. The caller
    /// sets `previous_data` from the registry (`gt4l_platform::install_record`).
    pub fn from_environment(exe_dir: &Path, known_documents: Option<PathBuf>) -> LayoutInputs {
        let non_empty = |name: &str| std::env::var_os(name).filter(|value| !value.is_empty());
        LayoutInputs {
            exe_dir: exe_dir.to_path_buf(),
            data_override: non_empty(DATA_DIR_OVERRIDE_VAR),
            user_override: non_empty(USER_DIR_OVERRIDE_VAR),
            local_override: non_empty(LOCAL_DIR_OVERRIDE_VAR),
            documents_override: non_empty(DOCUMENTS_DIR_OVERRIDE_VAR),
            app_data: non_empty("APPDATA").map(PathBuf::from),
            local_app_data: non_empty("LOCALAPPDATA").map(PathBuf::from),
            known_documents: if crate::build_kind::BUILD.is_portable() { None } else { known_documents },
            previous_data: None,
            portable: crate::build_kind::BUILD.is_portable(),
        }
    }
}

/// The resolved folders of this launcher run.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Layout {
    pub data: DataDirs,
    /// The user data folder (`%APPDATA%\GT4Recomp`) and how it was found.
    pub user_root: PathBuf,
    pub user_source: UserRootSource,
    pub legacy: LegacyLocations,
    /// Lines for the launcher log (the write fallback, for example).
    pub notices: Vec<String>,
}

impl Layout {
    /// The user data in its 1.2.1 place.
    pub fn user_dirs(&self) -> UserDirs {
        UserDirs::new(&self.user_root, self.user_source.clone())
    }
}

/// Resolves the folders. `writable` tells whether a folder can be written (it may create the folder; see
/// [`probe_writable`]). Nothing else is created here. Errors only when no data or user folder can be named at all.
pub fn resolve_layout(inputs: &LayoutInputs, writable: &dyn Fn(&Path) -> bool) -> Result<Layout, String> {
    if inputs.portable && inputs.data_override.is_none() {
        return resolve_portable_layout(inputs, writable);
    }
    let mut notices = Vec::new();
    let testing = inputs.data_override.is_some();
    // The 1.2.0 places. Under GT4RECOMP_DATA_DIR only the given overrides count, so a test never sees real folders.
    let local = match (&inputs.local_override, testing) {
        (Some(value), _) => Some(PathBuf::from(value)),
        (None, false) => inputs.local_app_data.as_ref().map(|folder| folder.join(APP_FOLDER_NAME)),
        (None, true) => None,
    };
    let documents_player = match (&inputs.documents_override, testing) {
        (Some(value), _) => Some(PathBuf::from(value).join(USER_FOLDER_NAME)),
        (None, false) => inputs
            .known_documents
            .as_ref()
            .filter(|documents| documents.is_absolute() && documents.is_dir())
            .map(|documents| documents.join(USER_FOLDER_NAME)),
        (None, true) => None,
    };
    // The previous install's data is taken over only outside the test overrides (they name every folder themselves).
    let previous_data = if testing { None } else { inputs.previous_data.clone() };
    let legacy = LegacyLocations { local: local.clone(), documents_player, previous_data: previous_data.clone() };

    let mut data = match &inputs.data_override {
        Some(value) => DataDirs::new(PathBuf::from(value)),
        None => {
            let installed = inputs.exe_dir.join(DATA_FOLDER_NAME);
            if writable(&installed) {
                DataDirs::new(installed)
            } else {
                let Some(fallback) = local.clone() else {
                    return Err(format!("{} cannot be written and LOCALAPPDATA is not set", installed.display()));
                };
                notices.push(format!(
                    "data folder {} cannot be written; reading it, and writing to {} instead",
                    installed.display(),
                    fallback.display()
                ));
                let mut dirs = DataDirs::new(fallback);
                if installed.is_dir() {
                    dirs.add_read_layer(installed);
                }
                dirs
            }
        }
    };
    // Until its move has finished, the 1.2.0 folder still serves the game files and the renderer it holds.
    if let Some(local) = &local {
        data.add_read_layer(local.clone());
    }
    // The same for the data folder of an earlier install in another folder, until its large data has moved.
    if let Some(previous) = &previous_data {
        data.add_read_layer(previous.clone());
    }

    let (user_root, user_source) = match (&inputs.user_override, &inputs.data_override) {
        (Some(value), _) => (PathBuf::from(value), UserRootSource::Override),
        (None, Some(data_value)) => (PathBuf::from(data_value).join("user"), UserRootSource::Override),
        (None, None) => match &inputs.app_data {
            Some(app_data) => (app_data.join(APP_FOLDER_NAME), UserRootSource::AppData),
            None => return Err("APPDATA is not set and GT4RECOMP_USER_DIR is not given".to_string()),
        },
    };
    Ok(Layout { data, user_root, user_source, legacy, notices })
}

/// The message of a portable folder that cannot be written (a copy inside Program Files, a read-only drive).
pub fn portable_not_writable_message(folder: &Path) -> String {
    format!(
        "GT4Recomp cannot write to {}. Move the GT4Recomp folder to a place you can write to (another drive, or a folder in your \
         Documents) and start it again.",
        folder.display()
    )
}

/// The portable build (TASK-098): `UserDirs` in `<exe dir>\user`, `DataDirs` in `<exe dir>\data`, the standard per-game
/// layout inside both. No read layers, no `%LOCALAPPDATA%` write fallback, no 1.2.0 places, no previous install: a folder
/// that cannot be written is an error that tells the player to move the folder. `GT4RECOMP_USER_DIR` still replaces the
/// user folder (tests).
fn resolve_portable_layout(inputs: &LayoutInputs, writable: &dyn Fn(&Path) -> bool) -> Result<Layout, String> {
    let data_root = inputs.exe_dir.join(DATA_FOLDER_NAME);
    if !writable(&data_root) {
        return Err(portable_not_writable_message(&data_root));
    }
    let (user_root, user_source) = match &inputs.user_override {
        Some(value) => (PathBuf::from(value), UserRootSource::Override),
        None => (inputs.exe_dir.join(PORTABLE_USER_FOLDER), UserRootSource::Portable),
    };
    if user_source == UserRootSource::Portable && !writable(&user_root) {
        return Err(portable_not_writable_message(&user_root));
    }
    let mut data = DataDirs::new(data_root);
    // A portable folder is new in 1.3.0: it never had the flat 1.2.x game folders.
    data.set_flat_gt4(false);
    Ok(Layout { data, user_root, user_source, legacy: LegacyLocations::default(), notices: Vec::new() })
}

/// Whether `folder` can be written: it is created if missing, then a probe file is written and removed.
pub fn probe_writable(folder: &Path) -> bool {
    if std::fs::create_dir_all(folder).is_err() {
        return false;
    }
    let probe = folder.join(format!(".write-probe-{}", std::process::id()));
    let written = std::fs::write(&probe, b"probe").is_ok();
    let _ = std::fs::remove_file(&probe);
    written
}

/// Creates the custom texture folder on demand: `Textures` (empty when new). The launcher never writes the player's
/// texture files; it only makes this folder so a pack can be dropped in (gt4-gs 1.2.1 reads it directly, TASK-093).
pub fn ensure_texture_folders(textures: &Path) -> std::io::Result<()> {
    std::fs::create_dir_all(textures)
}

/// Folder that holds `gt4.exe`: `<program folder>\game` for an installed copy, or the
/// `[dev] game_dir` override from the config file.
pub fn game_directory(program_directory: &Path, dev_override: Option<&Path>) -> PathBuf {
    match dev_override {
        Some(path) => path.to_path_buf(),
        None => program_directory.join("game"),
    }
}

/// The components of a path, with `.` dropped, `..` resolved and names lower-cased (Windows compares names
/// without case).
fn normalized(path: &Path) -> Vec<String> {
    let mut parts: Vec<String> = Vec::new();
    for component in path.components() {
        match component {
            Component::CurDir => {}
            Component::ParentDir => {
                parts.pop();
            }
            other => parts.push(other.as_os_str().to_string_lossy().replace('/', "\\").to_lowercase()),
        }
    }
    parts
}

/// Whether two paths name the same folder (lexically, ignoring case and separators).
pub fn same_path(left: &Path, right: &Path) -> bool {
    normalized(left) == normalized(right)
}

/// Whether `path` is `base` or inside it (lexically, ignoring case).
pub fn is_within(path: &Path, base: &Path) -> bool {
    let base = normalized(base);
    !base.is_empty() && normalized(path).starts_with(&base)
}

/// The form of a path stored in the settings file: relative to the install folder when it lies inside it (so a
/// copied folder still finds it), otherwise unchanged (a file the player picked elsewhere, such as the disc image).
pub fn portable_path(path: &Path, install_dir: &Path) -> PathBuf {
    if path.is_absolute() && is_within(path, install_dir) {
        let skip = normalized(install_dir).len();
        let relative: PathBuf = path.components().filter(|component| !matches!(component, Component::CurDir)).skip(skip).collect();
        if !relative.as_os_str().is_empty() {
            return relative;
        }
    }
    path.to_path_buf()
}

/// The path a stored value names: a relative value is resolved against the install folder.
pub fn resolve_portable(stored: &Path, install_dir: &Path) -> PathBuf {
    if stored.is_absolute() {
        stored.to_path_buf()
    } else {
        install_dir.join(stored)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn scratch(name: &str) -> PathBuf {
        let path = std::env::temp_dir().join(format!("gt4l-paths-{}-{name}", std::process::id()));
        let _ = std::fs::remove_dir_all(&path);
        std::fs::create_dir_all(&path).unwrap();
        path
    }

    fn inputs(exe_dir: &Path, base: &Path) -> LayoutInputs {
        LayoutInputs {
            exe_dir: exe_dir.to_path_buf(),
            app_data: Some(base.join("Roaming")),
            local_app_data: Some(base.join("Local")),
            known_documents: None,
            ..LayoutInputs::default()
        }
    }

    #[test]
    fn a_previous_install_data_folder_is_a_read_layer_with_its_own_marker() {
        let base = scratch("previous");
        let exe_dir = base.join("Games").join("GT4Recomp");
        let previous = base.join("Program Files").join("GT4Recomp").join("data");
        let mut with_previous = inputs(&exe_dir, &base);
        with_previous.previous_data = Some(previous.clone());
        let layout = resolve_layout(&with_previous, &probe_writable).unwrap();
        assert_eq!(layout.legacy.previous_data, Some(previous.clone()));
        assert!(layout.data.read_layers().contains(&previous), "read until its large data has moved");
        let other = base.join("Old Place").join("data");
        assert_ne!(layout.data.previous_data_marker(&previous), layout.data.previous_data_marker(&other), "one marker per folder");
        let upper = PathBuf::from(previous.to_string_lossy().to_uppercase());
        assert_eq!(layout.data.previous_data_marker(&upper), layout.data.previous_data_marker(&previous), "case does not count");
        assert_eq!(layout.data.previous_data_marker(&previous).parent(), Some(layout.data.root()));
        with_previous.data_override = Some(base.join("test-data").into_os_string());
        assert_eq!(resolve_layout(&with_previous, &probe_writable).unwrap().legacy.previous_data, None, "the test overrides keep it out");
        let _ = std::fs::remove_dir_all(&base);
    }

    fn receipt(folder: &Path) {
        std::fs::create_dir_all(folder.join("prepared")).unwrap();
        std::fs::write(folder.join("prepared").join(PREPARE_RECEIPT), b"{}").unwrap();
    }

    #[test]
    fn the_data_folder_is_next_to_the_executable_and_user_data_is_in_roaming_appdata() {
        let base = scratch("installed");
        let exe_dir = base.join("Program Files").join("GT4Recomp");
        let layout = resolve_layout(&inputs(&exe_dir, &base), &probe_writable).unwrap();
        assert_eq!(layout.data.root(), exe_dir.join("data"));
        assert_eq!(layout.user_root, base.join("Roaming").join("GT4Recomp"));
        assert_eq!(layout.user_source, UserRootSource::AppData);
        assert_eq!(layout.legacy.local, Some(base.join("Local").join("GT4Recomp")));
        assert!(layout.notices.is_empty());
        let user = layout.user_dirs();
        assert_eq!(user.config_file(), base.join("Roaming").join("GT4Recomp").join("config").join("settings.toml"));
        assert_eq!(user.controls_file(), base.join("Roaming").join("GT4Recomp").join("controls.toml"));
        // TASK-101: each game has its own library under `games`; the flat 1.2.x place is read only until it moves.
        assert_eq!(user.saves(), base.join("Roaming").join("GT4Recomp").join("games").join("gt4").join("Saves"));
        assert_eq!(
            user.for_game(&crate::games::SPEC2).saves(),
            base.join("Roaming").join("GT4Recomp").join("games").join("spec2").join("Saves")
        );
    }

    #[test]
    fn a_portable_copy_finds_its_data_relative_to_the_new_executable() {
        let base = scratch("portable");
        let first = base.join("C").join("GT4Recomp");
        let copy = base.join("E").join("Games").join("GT4Recomp");
        for folder in [&first, &copy] {
            receipt(&folder.join("data"));
            std::fs::create_dir_all(folder.join("data").join("gs")).unwrap();
            std::fs::write(folder.join("data").join("gs").join("current.txt"), b"1.2.0").unwrap();
        }
        let layout = resolve_layout(&inputs(&copy, &base.join("new-pc")), &probe_writable).unwrap();
        assert_eq!(layout.data.root(), copy.join("data"));
        assert_eq!(layout.data.prepared(), copy.join("data").join("prepared"));
        assert_eq!(layout.data.disc_folder(), copy.join("data").join("disc"));
        assert_eq!(layout.data.renderer_root(), copy.join("data").join("gs"));
        assert!(!is_within(&layout.data.prepared(), &first), "nothing points at the original folder");
    }

    #[test]
    fn an_empty_appdata_on_a_new_pc_still_sees_the_game_files_in_the_data_folder() {
        let base = scratch("empty-appdata");
        let exe_dir = base.join("GT4Recomp");
        receipt(&exe_dir.join("data"));
        let layout = resolve_layout(&inputs(&exe_dir, &base.join("fresh-user")), &probe_writable).unwrap();
        assert_eq!(layout.data.prepared(), exe_dir.join("data").join("prepared"));
        assert!(!layout.user_root.exists(), "resolving creates no user folder");
        assert!(!base.join("fresh-user").join("Local").join("GT4Recomp").exists());
    }

    #[test]
    fn a_data_folder_that_cannot_be_written_is_read_and_writes_go_to_local_appdata() {
        let base = scratch("read-only");
        let exe_dir = base.join("Program Files").join("GT4Recomp");
        let installed = exe_dir.join("data");
        receipt(&installed);
        std::fs::create_dir_all(installed.join("gs")).unwrap();
        std::fs::write(installed.join("gs").join("current.txt"), b"1.2.0").unwrap();
        std::fs::create_dir_all(installed.join("Textures")).unwrap();
        let read_only = installed.clone();
        let layout = resolve_layout(&inputs(&exe_dir, &base), &move |folder: &Path| !same_path(folder, &read_only)).unwrap();
        let local = base.join("Local").join("GT4Recomp");
        assert_eq!(layout.data.root(), local);
        assert_eq!(layout.data.read_layers(), &[installed.clone()]);
        assert_eq!(layout.notices.len(), 1, "the fallback is logged");
        assert!(layout.notices[0].contains("cannot be written"));
        // Reads use the installed folder; writes go to the local fallback.
        assert_eq!(layout.data.prepared(), installed.join("prepared"));
        assert_eq!(layout.data.renderer_root(), installed.join("gs"));
        assert_eq!(layout.data.textures(), installed.join("Textures"));
        assert_eq!(layout.data.logs(), local.join("logs"));
        assert_eq!(layout.data.runtime_card(), local.join("games").join("gt4").join("runtime").join("card.ps2"));
        assert_eq!(layout.data.setup_prepared(), local.join("games").join("gt4").join("prepared"));
        assert_eq!(layout.data.renderer_install_root(), local.join("gs"));
        // A renderer installed later into the fallback wins over the read-only one.
        std::fs::create_dir_all(local.join("gs")).unwrap();
        std::fs::write(local.join("gs").join("current.txt"), b"1.3.0").unwrap();
        assert_eq!(layout.data.renderer_root(), local.join("gs"));
    }

    #[test]
    fn the_test_overrides_keep_every_real_folder_out() {
        let base = scratch("overrides");
        let mut given = inputs(&base.join("exe"), &base);
        given.data_override = Some(base.join("data").into_os_string());
        given.known_documents = Some(base.clone());
        let layout = resolve_layout(&given, &|_: &Path| panic!("no probe under the override")).unwrap();
        assert_eq!(layout.data.root(), base.join("data"));
        assert!(layout.data.read_layers().is_empty());
        assert_eq!(layout.user_root, base.join("data").join("user"));
        assert_eq!(layout.legacy, LegacyLocations::default());
        given.user_override = Some(base.join("user").into_os_string());
        given.local_override = Some(base.join("local").into_os_string());
        given.documents_override = Some(base.join("docs").into_os_string());
        let layout = resolve_layout(&given, &|_: &Path| true).unwrap();
        assert_eq!(layout.user_root, base.join("user"));
        assert_eq!(layout.legacy.local, Some(base.join("local")));
        assert_eq!(layout.legacy.documents_player, Some(base.join("docs").join(USER_FOLDER_NAME)));
        assert_eq!(layout.data.read_layers(), &[base.join("local")]);
    }

    #[test]
    fn the_1_2_0_folder_serves_game_files_until_they_have_moved() {
        let base = scratch("layers");
        let root = base.join("data");
        let local = base.join("local");
        receipt(&local);
        let dirs = DataDirs::with_read_layers(&root, [local.clone(), root.clone(), local.clone()]);
        assert_eq!(dirs.read_layers(), &[local.clone()], "duplicates and the root are dropped");
        assert_eq!(dirs.prepared(), local.join("prepared"));
        assert_eq!(dirs.disc_folder(), local.join("disc"), "the image and the receipt come from one folder");
        receipt(&root);
        assert_eq!(dirs.prepared(), root.join("prepared"));
    }

    #[test]
    fn stored_paths_inside_the_install_folder_become_relative() {
        let install = Path::new("C:/Program Files/GT4Recomp");
        let inside = Path::new("C:/Program Files/GT4Recomp/data/disc/SCUS-97328.iso");
        let stored = portable_path(inside, install);
        assert_eq!(stored, Path::new("data").join("disc").join("SCUS-97328.iso"));
        assert_eq!(resolve_portable(&stored, Path::new("E:/Games/GT4Recomp")), Path::new("E:/Games/GT4Recomp").join(&stored));
        let elsewhere = Path::new("D:/Games/Gran Turismo 4 (USA).iso");
        assert_eq!(portable_path(elsewhere, install), elsewhere);
        assert_eq!(resolve_portable(elsewhere, install), elsewhere);
        // Case differences still count as inside.
        assert!(portable_path(Path::new("c:/program files/gt4recomp/game"), install).is_relative());
    }

    fn portable_inputs(exe_dir: &Path, base: &Path) -> LayoutInputs {
        LayoutInputs {
            portable: true,
            // Everything an installed build would read is present, to prove the portable build ignores it.
            previous_data: Some(base.join("Program Files").join("GT4Recomp").join("data")),
            known_documents: Some(base.to_path_buf()),
            ..inputs(exe_dir, base)
        }
    }

    #[test]
    fn the_portable_build_keeps_user_and_data_beside_the_executable() {
        let base = scratch("portable-layout");
        let exe_dir = base.join("E").join("GT4Recomp");
        let layout = resolve_layout(&portable_inputs(&exe_dir, &base), &probe_writable).unwrap();
        assert_eq!(layout.data.root(), exe_dir.join("data"));
        assert_eq!(layout.user_root, exe_dir.join("user"));
        assert_eq!(layout.user_source, UserRootSource::Portable);
        assert!(layout.data.read_layers().is_empty(), "no read layers");
        assert_eq!(layout.legacy, LegacyLocations::default(), "no 1.2.0 places, no previous install");
        assert!(layout.notices.is_empty());
        // The same per-game layout as the installer build.
        let user = layout.user_dirs();
        assert_eq!(user.config_file(), exe_dir.join("user").join("config").join("settings.toml"));
        assert_eq!(user.controls_file(), exe_dir.join("user").join("controls.toml"));
        assert_eq!(user.saves(), exe_dir.join("user").join("games").join("gt4").join("Saves"));
        assert_eq!(
            user.for_game(&crate::games::SPEC2).card_backups(),
            exe_dir.join("user").join("games").join("spec2").join("backups")
        );
        assert_eq!(layout.data.runtime_card(), exe_dir.join("data").join("games").join("gt4").join("runtime").join("card.ps2"));
        assert_eq!(layout.data.setup_prepared(), exe_dir.join("data").join("games").join("gt4").join("prepared"));
        assert_eq!(layout.data.renderer_install_root(), exe_dir.join("data").join("gs"));
        assert_eq!(layout.data.textures(), exe_dir.join("data").join("Textures"));
        assert_eq!(layout.data.logs(), exe_dir.join("data").join("logs"));
        assert!(!layout.data.write_places().contains(&exe_dir.join("data")), "no flat 1.2.x folder in a portable copy");
        assert!(!base.join("Local").exists() && !base.join("Roaming").exists(), "no system folder is created");
        let _ = std::fs::remove_dir_all(&base);
    }

    #[test]
    fn the_portable_build_never_falls_back_to_a_system_folder() {
        let base = scratch("portable-readonly");
        let exe_dir = base.join("Program Files").join("GT4Recomp");
        let data = exe_dir.join("data");
        let user = exe_dir.join("user");
        let given = portable_inputs(&exe_dir, &base);
        let data_locked = data.clone();
        let error = resolve_layout(&given, &move |folder: &Path| !same_path(folder, &data_locked)).unwrap_err();
        assert!(error.contains("Move the GT4Recomp folder"), "{error}");
        assert!(error.contains(&data.display().to_string()), "the message names the folder: {error}");
        let user_locked = user.clone();
        let error = resolve_layout(&given, &move |folder: &Path| !same_path(folder, &user_locked)).unwrap_err();
        assert!(error.contains(&user.display().to_string()), "{error}");
        assert!(!base.join("Local").exists(), "%LOCALAPPDATA% is not used");
        // The installer build, in the same situation, falls back to %LOCALAPPDATA% (unchanged).
        let installer = LayoutInputs { portable: false, ..given };
        let layout = resolve_layout(&installer, &move |folder: &Path| !same_path(folder, &data)).unwrap();
        assert_eq!(layout.data.root(), base.join("Local").join("GT4Recomp"));
        let _ = std::fs::remove_dir_all(&base);
    }

    #[test]
    fn the_portable_build_still_honours_the_test_overrides() {
        let base = scratch("portable-overrides");
        let mut given = portable_inputs(&base.join("exe"), &base);
        given.data_override = Some(base.join("test-data").into_os_string());
        let layout = resolve_layout(&given, &|_: &Path| panic!("no probe under the override")).unwrap();
        assert_eq!(layout.data.root(), base.join("test-data"));
        assert_eq!(layout.user_root, base.join("test-data").join("user"));
        assert_eq!(layout.user_source, UserRootSource::Override);
        given.data_override = None;
        given.user_override = Some(base.join("test-user").into_os_string());
        let layout = resolve_layout(&given, &|_: &Path| true).unwrap();
        assert_eq!(layout.data.root(), base.join("exe").join("data"));
        assert_eq!(layout.user_root, base.join("test-user"));
        let _ = std::fs::remove_dir_all(&base);
    }

    #[test]
    fn the_environment_inputs_follow_the_compiled_build_kind() {
        let from_environment = LayoutInputs::from_environment(Path::new("X:/GT4Recomp"), Some(PathBuf::from("X:/Docs")));
        assert_eq!(from_environment.portable, crate::build_kind::BUILD.is_portable());
        assert_eq!(from_environment.known_documents.is_none(), crate::build_kind::BUILD.is_portable());
    }

    #[test]
    fn the_legacy_user_dirs_point_at_the_1_2_0_places() {
        let user = UserDirs::legacy(Path::new("L:/GT4Recomp"), Path::new("D:/Docs/Gran Turismo 4"), "why".into());
        assert_eq!(user.config_file(), Path::new("L:/GT4Recomp").join("config").join("settings.toml"));
        assert_eq!(user.controls_file(), Path::new("L:/GT4Recomp").join("controls.toml"));
        assert_eq!(user.card_backups(), Path::new("L:/GT4Recomp").join("library").join("backups"));
        assert_eq!(user.saves(), Path::new("D:/Docs/Gran Turismo 4").join("Saves"));
        assert_eq!(user.library_migration_marker(), Path::new("L:/GT4Recomp").join("config").join("library-migration.json"));
    }
}
