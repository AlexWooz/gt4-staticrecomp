//! The update decision (pure, no network). The launcher only checks and offers; it never installs by itself, except
//! the first gt4-gs install during setup, which has no installed version to replace (see `gs_release`).
//!
//! An offer exists when the stream has a release on the player's channel newer than the installed version, at or
//! above the stream's minimum, and the player has not chosen "Skip this version" for exactly that version.
//!
//! Channels (TASK-109) apply to the GT4Recomp stream only: Stable offers stable releases; Beta offers the newest of the
//! release candidates and the stable releases. Moving from Beta back to Stable never downgrades: the offer exists only
//! when a stable release is newer than the installed build, which is an ordinary version comparison.

use crate::releases::{self, Release, Stream};
use crate::settings::Settings;
use crate::version::ReleaseVersion;

/// The setting id of the channel in `settings.toml`.
pub const CHANNEL_SETTING: &str = "update_channel";

/// Which releases the launcher offers.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum UpdateChannel {
    Stable,
    Beta,
}

impl UpdateChannel {
    pub fn key(self) -> &'static str {
        match self {
            UpdateChannel::Stable => "stable",
            UpdateChannel::Beta => "beta",
        }
    }

    pub fn parse(text: &str) -> Option<UpdateChannel> {
        match text {
            "stable" => Some(UpdateChannel::Stable),
            "beta" => Some(UpdateChannel::Beta),
            _ => None,
        }
    }

    /// The channel a build starts on when the player never chose one: a release candidate build stays on Beta, so a
    /// tester keeps getting candidates; any other build is on Stable.
    pub fn default_for(installed: Option<ReleaseVersion>) -> UpdateChannel {
        if installed.is_some_and(|version| version.is_candidate()) {
            UpdateChannel::Beta
        } else {
            UpdateChannel::Stable
        }
    }

    /// The channel in the settings.
    pub fn of(settings: &Settings) -> UpdateChannel {
        UpdateChannel::parse(settings.get(CHANNEL_SETTING)).unwrap_or(UpdateChannel::Stable)
    }
}

/// Applies the build's default channel when the player never chose one (`Settings::was_chosen`). Called when the
/// settings are loaded; a stored value that was not chosen by the player is replaced, so a stable build does not keep the
/// Beta channel an earlier candidate build wrote.
pub fn apply_channel_default(settings: &mut Settings, installed: Option<ReleaseVersion>) {
    if !settings.was_chosen(CHANNEL_SETTING) {
        let _ = settings.set(CHANNEL_SETTING, UpdateChannel::default_for(installed).key());
    }
}

/// One update the launcher offers (or, with `installed` = None, one first install it performs silently).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Offer {
    pub stream: Stream,
    /// The installed version, `None` when the stream is not installed yet.
    pub installed: Option<ReleaseVersion>,
    pub latest: ReleaseVersion,
    /// The release notes, as GitHub stores them (plain text; the UI renders a safe subset).
    pub notes: String,
    /// The download asset of the latest version.
    pub package: String,
}

impl Offer {
    /// True when there is something to replace, i.e. the popup applies. A missing install is not an update.
    pub fn is_update(&self) -> bool {
        self.installed.is_some()
    }
}

/// The offer for one stream, given its release list and the installed version. `skipped` is the version the
/// player chose to skip. Returns `None` when there is nothing new.
pub fn decide(
    stream: Stream,
    channel: UpdateChannel,
    installed: Option<ReleaseVersion>,
    releases: &[Release],
    skipped: Option<ReleaseVersion>,
) -> Option<Offer> {
    // The renderer has no channels: it always follows its stable releases.
    let channel = if stream == Stream::Renderer { UpdateChannel::Stable } else { channel };
    let latest_release: &Release = releases::select_offer(releases, channel, stream.minimum().into())?;
    let latest = latest_release.release_version?;
    if let Some(current) = installed {
        if latest <= current {
            return None;
        }
    }
    if skipped == Some(latest) {
        return None;
    }
    Some(Offer {
        stream,
        installed,
        latest,
        notes: latest_release.notes.clone(),
        package: stream.package_name(latest),
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::releases::parse_releases;
    use crate::version::Version;

    fn rv(major: u32, minor: u32, patch: u32) -> ReleaseVersion {
        ReleaseVersion::stable(Version(major, minor, patch))
    }

    fn rc(major: u32, minor: u32, patch: u32, number: u32) -> ReleaseVersion {
        ReleaseVersion::candidate(Version(major, minor, patch), number)
    }

    const BETA_LIST: &str = r#"[
      {"tag_name":"v1.3.0-rc5","prerelease":true,"draft":false,"published_at":"2026-10-23T10:00:00Z","body":"Five.","assets":[]},
      {"tag_name":"v1.3.0-rc6","prerelease":true,"draft":true,"body":"Draft.","assets":[]},
      {"tag_name":"v1.3.0-rc4","prerelease":true,"draft":false,"published_at":"2026-10-22T10:00:00Z","assets":[]},
      {"tag_name":"v1.2.3","prerelease":false,"draft":false,"published_at":"2026-10-20T10:00:00Z","assets":[]}
    ]"#;

    fn beta_list() -> Vec<Release> {
        parse_releases(BETA_LIST, Stream::Launcher).unwrap()
    }

    const LAUNCHER_LIST: &str = r#"[
      {"tag_name":"v1.0.1","prerelease":false,"draft":false,"body":"Fixes.","assets":[]},
      {"tag_name":"v1.1.0-rc1","prerelease":true,"draft":false,"body":"","assets":[]},
      {"tag_name":"v1.0.0","prerelease":false,"draft":false,"body":"First.","assets":[]}
    ]"#;

    const RENDERER_LIST: &str = r#"[
      {"tag_name":"gt4gs-v1.10.0","prerelease":false,"draft":false,"body":"Faster.","assets":[]},
      {"tag_name":"gt4gs-v1.9.0","prerelease":false,"draft":false,"body":"","assets":[]}
    ]"#;

    fn launcher() -> Vec<Release> {
        parse_releases(LAUNCHER_LIST, Stream::Launcher).unwrap()
    }

    fn renderer() -> Vec<Release> {
        parse_releases(RENDERER_LIST, Stream::Renderer).unwrap()
    }

    #[test]
    fn a_newer_stable_release_is_offered_with_its_notes_and_package() {
        let offer = decide(Stream::Launcher, UpdateChannel::Stable, Some(rv(1, 0, 0)), &launcher(), None).unwrap();
        assert_eq!(offer.latest, rv(1, 0, 1));
        assert_eq!(offer.notes, "Fixes.");
        assert_eq!(offer.package, "GT4Recomp-Setup-1.0.1.exe");
        assert!(offer.is_update());
    }

    #[test]
    fn the_same_or_an_older_version_gives_no_offer() {
        assert!(decide(Stream::Launcher, UpdateChannel::Stable, Some(rv(1, 0, 1)), &launcher(), None).is_none());
        assert!(decide(Stream::Launcher, UpdateChannel::Stable, Some(rv(1, 0, 2)), &launcher(), None).is_none());
    }

    #[test]
    fn prereleases_are_never_offered() {
        let offer = decide(Stream::Launcher, UpdateChannel::Stable, Some(rv(1, 0, 1)), &launcher(), None);
        assert!(offer.is_none(), "1.1.0-rc1 is a prerelease and must not be offered");
    }

    #[test]
    fn numeric_comparison_offers_1_10_over_1_9() {
        let offer = decide(Stream::Renderer, UpdateChannel::Stable, Some(rv(1, 9, 0)), &renderer(), None).unwrap();
        assert_eq!(offer.latest, rv(1, 10, 0));
        assert_eq!(offer.package, "gt4-gs-1.10.0-windows-x64.zip");
    }

    #[test]
    fn a_skipped_version_is_not_offered_again_but_a_later_one_is() {
        let skipped = Some(rv(1, 10, 0));
        assert!(decide(Stream::Renderer, UpdateChannel::Stable, Some(rv(1, 9, 0)), &renderer(), skipped).is_none());
        let newer = parse_releases(
            r#"[{"tag_name":"gt4gs-v1.11.0","prerelease":false,"draft":false,"assets":[]}]"#,
            Stream::Renderer,
        )
        .unwrap();
        let mut all = renderer();
        all.extend(newer);
        assert_eq!(decide(Stream::Renderer, UpdateChannel::Stable, Some(rv(1, 9, 0)), &all, skipped).unwrap().latest, rv(1, 11, 0));
    }

    #[test]
    fn a_missing_install_is_a_first_install_not_an_update() {
        let offer = decide(Stream::Renderer, UpdateChannel::Stable, None, &renderer(), None).unwrap();
        assert!(!offer.is_update());
        assert_eq!(offer.latest, rv(1, 10, 0));
    }

    #[test]
    fn below_the_minimum_nothing_is_offered() {
        let old = parse_releases(r#"[{"tag_name":"gt4gs-v1.0.0","assets":[]}]"#, Stream::Renderer).unwrap();
        assert!(decide(Stream::Renderer, UpdateChannel::Stable, None, &old, None).is_none());
    }

    #[test]
    fn the_channel_defaults_to_beta_only_for_a_candidate_build() {
        assert_eq!(UpdateChannel::default_for(Some(rc(1, 3, 0, 5))), UpdateChannel::Beta);
        assert_eq!(UpdateChannel::default_for(Some(rv(1, 3, 0))), UpdateChannel::Stable);
        assert_eq!(UpdateChannel::default_for(None), UpdateChannel::Stable);
        let mut settings = Settings::default();
        assert_eq!(UpdateChannel::of(&settings), UpdateChannel::Stable, "the table default is Stable");
        apply_channel_default(&mut settings, Some(rc(1, 3, 0, 5)));
        assert_eq!(UpdateChannel::of(&settings), UpdateChannel::Beta, "no stored choice on a candidate build");
        // A stable build replaces a Beta value the player never chose.
        apply_channel_default(&mut settings, Some(rv(1, 3, 0)));
        assert_eq!(UpdateChannel::of(&settings), UpdateChannel::Stable);
        // The player's own choice always stays.
        settings.set_by_user(CHANNEL_SETTING, "stable").unwrap();
        apply_channel_default(&mut settings, Some(rc(1, 3, 0, 5)));
        assert_eq!(UpdateChannel::of(&settings), UpdateChannel::Stable);
    }

    #[test]
    fn stable_never_offers_a_candidate_and_never_downgrades_from_one() {
        let list = beta_list();
        // On rc4: Stable sees only 1.2.3, which is older than the installed build.
        assert!(decide(Stream::Launcher, UpdateChannel::Stable, Some(rc(1, 3, 0, 4)), &list, None).is_none());
        assert!(decide(Stream::Launcher, UpdateChannel::Stable, Some(rc(1, 3, 0, 5)), &list, None).is_none());
        // The final 1.3.0 ships: a player who went back to Stable gets it.
        let mut with_final = list.clone();
        with_final.extend(
            parse_releases(r#"[{"tag_name":"v1.3.0","prerelease":false,"draft":false,"assets":[]}]"#, Stream::Launcher).unwrap(),
        );
        let offer = decide(Stream::Launcher, UpdateChannel::Stable, Some(rc(1, 3, 0, 5)), &with_final, None).unwrap();
        assert_eq!(offer.latest, rv(1, 3, 0));
        assert_eq!(offer.package, "GT4Recomp-Setup-1.3.0.exe");
    }

    #[test]
    fn beta_offers_the_newest_candidate_or_stable_and_never_a_draft() {
        let list = beta_list();
        let offer = decide(Stream::Launcher, UpdateChannel::Beta, Some(rc(1, 3, 0, 4)), &list, None).unwrap();
        assert_eq!(offer.latest, rc(1, 3, 0, 5), "rc6 is a draft");
        assert_eq!(offer.package, "GT4Recomp-Setup-1.3.0-rc5.exe");
        assert_eq!(offer.notes, "Five.");
        assert!(decide(Stream::Launcher, UpdateChannel::Beta, Some(rc(1, 3, 0, 5)), &list, None).is_none());
        // From a stable build the Beta channel offers the candidate above it.
        let from_stable = decide(Stream::Launcher, UpdateChannel::Beta, Some(rv(1, 2, 3)), &list, None).unwrap();
        assert_eq!(from_stable.latest, rc(1, 3, 0, 5));
        // The skipped version is not offered again.
        assert!(decide(Stream::Launcher, UpdateChannel::Beta, Some(rc(1, 3, 0, 4)), &list, Some(rc(1, 3, 0, 5))).is_none());
    }

    #[test]
    fn beta_gets_the_final_release_when_it_ships() {
        let mut list = beta_list();
        list.extend(
            parse_releases(r#"[{"tag_name":"v1.3.0","prerelease":false,"draft":false,"assets":[]}]"#, Stream::Launcher).unwrap(),
        );
        let offer = decide(Stream::Launcher, UpdateChannel::Beta, Some(rc(1, 3, 0, 5)), &list, None).unwrap();
        assert_eq!(offer.latest, rv(1, 3, 0));
    }

    #[test]
    fn the_renderer_ignores_the_channel() {
        let list = parse_releases(
            r#"[{"tag_name":"gt4gs-v1.2.2-rc1","prerelease":true,"draft":false,"assets":[]},
                {"tag_name":"gt4gs-v1.2.1","prerelease":false,"draft":false,"assets":[]}]"#,
            Stream::Renderer,
        )
        .unwrap();
        let offer = decide(Stream::Renderer, UpdateChannel::Beta, None, &list, None).unwrap();
        assert_eq!(offer.latest, rv(1, 2, 1));
    }
}
