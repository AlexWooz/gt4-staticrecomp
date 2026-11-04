//! The build kind of this launcher (TASK-098, release 1.3.0): the installer build or the portable build.
//!
//! One source, two builds. The Cargo feature `portable` (forwarded by `gt4l-tauri`) makes [`BUILD`] `Portable`. The
//! portable build keeps everything in its own folder (`<exe dir>\user`, `<exe dir>\data`), touches no registry key and
//! no system folder, and updates itself by swapping program files from a zip (`crate::portable_update`). Code that must
//! behave differently takes a [`BuildKind`] value (or a `portable` flag), so both behaviours are tested in either build.

/// Which launcher build this is.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum BuildKind {
    /// Installed per machine by `GT4Recomp-Setup-<version>.exe`.
    Installer,
    /// A folder (from `GT4Recomp-<version>-portable.zip`) that can be carried to another PC.
    Portable,
}

/// The kind this binary was compiled as.
pub const BUILD: BuildKind = if cfg!(feature = "portable") { BuildKind::Portable } else { BuildKind::Installer };

impl BuildKind {
    pub fn is_portable(self) -> bool {
        self == BuildKind::Portable
    }

    /// The word Settings > About shows.
    pub fn label(self) -> &'static str {
        match self {
            BuildKind::Installer => "Installer",
            BuildKind::Portable => "Portable",
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_compiled_kind_follows_the_cargo_feature() {
        assert_eq!(BUILD.is_portable(), cfg!(feature = "portable"));
        assert_eq!(BUILD.label(), if cfg!(feature = "portable") { "Portable" } else { "Installer" });
    }

    #[test]
    fn the_labels_are_the_words_of_the_about_page() {
        assert_eq!(BuildKind::Portable.label(), "Portable");
        assert_eq!(BuildKind::Installer.label(), "Installer");
    }
}
