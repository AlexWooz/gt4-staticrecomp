//! The version picker's list (TASK-109): which GT4Recomp versions Settings > General offers to install. Pure; the
//! release list comes from GitHub every time the page opens (never cached for long).
//!
//! - Stable channel: stable releases, newest first by version.
//! - Beta channel: release candidates, newest first by publication date; a stable release newer (by version) than every
//!   listed candidate is shown above them.
//! - Never a draft, and never a version below [`releases::PICKER_MINIMUM`]: an older build cannot read this version's data
//!   (multi-game layout, settings schema 8, per-game save names).
//!
//! Downgrade rule (owner): SemVer only, whatever the channel. A target older than the installed version, with the
//! pre-release ordering, is a downgrade and needs the strong warning; a newer one is always the plain confirmation.

use crate::releases::{self, Release};
use crate::update::UpdateChannel;
use crate::version::ReleaseVersion;

/// One line of the list.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct VersionEntry {
    pub tag: String,
    pub version: ReleaseVersion,
    /// Publication date, `YYYY-MM-DD`; empty when GitHub gave none.
    pub date: String,
    pub candidate: bool,
    /// The running build.
    pub installed: bool,
    /// Older than the running build: choosing it is a downgrade.
    pub downgrade: bool,
}

fn entry(release: &Release, version: ReleaseVersion, installed: Option<ReleaseVersion>) -> VersionEntry {
    VersionEntry {
        tag: release.tag.clone(),
        version,
        date: release.published_at.as_deref().map(|stamp| stamp.chars().take(10).collect()).unwrap_or_default(),
        candidate: version.is_candidate(),
        installed: installed == Some(version),
        downgrade: is_downgrade(version, installed),
    }
}

/// True when installing `target` over `installed` goes back to an older version (SemVer with the release candidate
/// ordering, any channel). Same version or a newer one is not a downgrade.
pub fn is_downgrade(target: ReleaseVersion, installed: Option<ReleaseVersion>) -> bool {
    installed.is_some_and(|installed| target < installed)
}

/// Which confirmation dialog the page shows before an install of a chosen version.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ConfirmKind {
    /// "Install version X?" with Install / Cancel.
    Plain,
    /// The strong warning (back up saves first) with an explicit "Install older version" button, never the default focus.
    Downgrade,
}

impl ConfirmKind {
    pub fn key(self) -> &'static str {
        match self {
            ConfirmKind::Plain => "plain",
            ConfirmKind::Downgrade => "downgrade",
        }
    }
}

pub fn confirm_kind(target: ReleaseVersion, installed: Option<ReleaseVersion>) -> ConfirmKind {
    if is_downgrade(target, installed) {
        ConfirmKind::Downgrade
    } else {
        ConfirmKind::Plain
    }
}

/// True when the list holds nothing at or above the running build, for example a Stable list while a newer candidate
/// runs. Not an error: the page says that updates resume when a newer release ships.
pub fn nothing_at_or_above(list: &[VersionEntry], installed: Option<ReleaseVersion>) -> bool {
    installed.is_some_and(|installed| list.iter().all(|entry| entry.version < installed))
}

/// Builds the list for a channel. `installed` is the running build, marked in the list when present.
pub fn build_list(releases: &[Release], channel: UpdateChannel, installed: Option<ReleaseVersion>) -> Vec<VersionEntry> {
    let usable =
        |release: &&Release| !release.draft && release.release_version.is_some_and(|version| version >= releases::PICKER_MINIMUM);
    let mut stable_entries: Vec<VersionEntry> = releases
        .iter()
        .filter(usable)
        .filter(|release| release.is_stable())
        .filter_map(|release| release.release_version.map(|version| entry(release, version, installed)))
        .collect();
    stable_entries.sort_by(|a, b| b.version.cmp(&a.version));
    match channel {
        UpdateChannel::Stable => stable_entries,
        UpdateChannel::Beta => {
            let mut candidates: Vec<(&Release, VersionEntry)> = releases
                .iter()
                .filter(usable)
                .filter(|release| release.is_candidate())
                .filter_map(|release| release.release_version.map(|version| (release, entry(release, version, installed))))
                .collect();
            // Newest publication first (the stamps are UTC ISO 8601, so text order is time order); no date = last.
            candidates.sort_by(|a, b| b.0.published_at.cmp(&a.0.published_at).then_with(|| b.1.version.cmp(&a.1.version)));
            let newest_candidate = candidates.iter().map(|(_, entry)| entry.version).max();
            let mut list: Vec<VersionEntry> = stable_entries
                .into_iter()
                .filter(|stable| newest_candidate.is_none_or(|newest| stable.version > newest))
                .collect();
            list.extend(candidates.into_iter().map(|(_, entry)| entry));
            list
        }
    }
}

/// The release to install for a tag. A draft, an unknown tag and a version below the picker minimum are refused, so a
/// hand-made command cannot install something the list would not show.
pub fn find_installable<'a>(releases: &'a [Release], tag: &str) -> Result<(&'a Release, ReleaseVersion), String> {
    let release = releases
        .iter()
        .find(|release| release.tag == tag && !release.draft)
        .ok_or_else(|| format!("no release {tag}"))?;
    let version = release.release_version.ok_or_else(|| format!("{tag} is not a GT4Recomp version"))?;
    if version < releases::PICKER_MINIMUM {
        return Err(format!("{tag} is older than {} and cannot read this version's data", releases::PICKER_MINIMUM));
    }
    if !(release.is_stable() || release.is_candidate()) {
        return Err(format!("{tag} is not an installable release"));
    }
    Ok((release, version))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::releases::{parse_releases, Stream};
    use crate::version::Version;

    const LIST: &str = r#"[
      {"tag_name":"v1.3.0-rc5","prerelease":true,"draft":false,"published_at":"2026-10-23T10:00:00Z","assets":[]},
      {"tag_name":"v1.3.0-rc7","prerelease":true,"draft":true,"published_at":null,"assets":[]},
      {"tag_name":"v1.3.0-rc6","prerelease":true,"draft":false,"published_at":"2026-10-25T09:00:00Z","assets":[]},
      {"tag_name":"v1.3.0-rc4","prerelease":true,"draft":false,"published_at":"2026-10-22T10:00:00Z","assets":[]},
      {"tag_name":"v1.3.1-rc1","prerelease":true,"draft":false,"published_at":"2026-10-24T10:00:00Z","assets":[]},
      {"tag_name":"v1.3.0","prerelease":false,"draft":false,"published_at":"2026-10-26T10:00:00Z","assets":[]},
      {"tag_name":"v1.4.0","prerelease":false,"draft":true,"published_at":null,"assets":[]},
      {"tag_name":"v1.2.3","prerelease":false,"draft":false,"published_at":"2026-10-20T10:00:00Z","assets":[]},
      {"tag_name":"v1.3.2","prerelease":false,"draft":false,"published_at":"2026-10-30T10:00:00Z","assets":[]},
      {"tag_name":"gt4gs-v1.2.1","prerelease":false,"draft":false,"assets":[]}
    ]"#;

    fn releases() -> Vec<Release> {
        parse_releases(LIST, Stream::Launcher).unwrap()
    }

    fn tags(list: &[VersionEntry]) -> Vec<&str> {
        list.iter().map(|entry| entry.tag.as_str()).collect()
    }

    fn v(text: &str) -> ReleaseVersion {
        ReleaseVersion::parse(text).unwrap()
    }

    #[test]
    fn stable_lists_stable_releases_by_version_without_drafts_or_old_versions() {
        let list = build_list(&releases(), UpdateChannel::Stable, None);
        assert_eq!(tags(&list), ["v1.3.2", "v1.3.0"], "1.2.3 is below the minimum, 1.4.0 is a draft");
    }

    #[test]
    fn beta_lists_candidates_by_date_with_a_newer_stable_on_top() {
        let list = build_list(&releases(), UpdateChannel::Beta, None);
        // 1.3.2 is newer than every candidate; 1.3.0 is not newer than 1.3.1-rc1. rc4 is below the minimum, rc7 a draft.
        assert_eq!(tags(&list), ["v1.3.2", "v1.3.0-rc6", "v1.3.1-rc1", "v1.3.0-rc5"]);
        assert_eq!(list[0].date, "2026-10-30");
        assert!(!list[0].candidate && list[1].candidate);
    }

    #[test]
    fn beta_without_a_newer_stable_is_only_candidates_in_date_order() {
        let only: Vec<Release> = releases().into_iter().filter(|release| release.tag != "v1.3.2").collect();
        let list = build_list(&only, UpdateChannel::Beta, None);
        assert_eq!(tags(&list), ["v1.3.0-rc6", "v1.3.1-rc1", "v1.3.0-rc5"]);
    }

    #[test]
    fn the_running_build_is_marked_installed_and_older_entries_are_downgrades() {
        let list = build_list(&releases(), UpdateChannel::Beta, Some(v("1.3.0-rc6")));
        let marked: Vec<&str> = list.iter().filter(|entry| entry.installed).map(|entry| entry.tag.as_str()).collect();
        assert_eq!(marked, ["v1.3.0-rc6"]);
        let downgrades: Vec<&str> = list.iter().filter(|entry| entry.downgrade).map(|entry| entry.tag.as_str()).collect();
        assert_eq!(downgrades, ["v1.3.0-rc5"]);
    }

    #[test]
    fn the_minimum_is_rc5() {
        assert_eq!(releases::PICKER_MINIMUM.text(), "1.3.0-rc5");
        assert!(ReleaseVersion::candidate(Version(1, 3, 0), 4) < releases::PICKER_MINIMUM);
    }

    #[test]
    fn only_listed_kinds_of_releases_are_installable() {
        let all = releases();
        assert_eq!(find_installable(&all, "v1.3.0-rc5").unwrap().1.text(), "1.3.0-rc5");
        assert_eq!(find_installable(&all, "v1.3.0").unwrap().1.text(), "1.3.0");
        for refused in ["v1.3.0-rc4", "v1.2.3", "v1.4.0", "v1.3.0-rc7", "gt4gs-v1.2.1", "v9.9.9"] {
            assert!(find_installable(&all, refused).is_err(), "{refused}");
        }
    }

    #[test]
    fn a_downgrade_is_semver_only_across_channels() {
        // A candidate going to an older stable release is a downgrade.
        assert!(is_downgrade(v("1.3.0"), Some(v("1.3.1-rc2"))));
        // A candidate going to its own final release is an upgrade, as is a stable going to a newer candidate.
        assert!(!is_downgrade(v("1.3.1"), Some(v("1.3.1-rc2"))));
        assert!(!is_downgrade(v("1.3.1-rc1"), Some(v("1.3.0"))));
        // An older candidate of the same version, and any older number, are downgrades.
        assert!(is_downgrade(v("1.3.0-rc5"), Some(v("1.3.0-rc6"))));
        assert!(is_downgrade(v("1.3.0-rc5"), Some(v("1.3.0"))));
        // The same version and an unknown installed version are not.
        assert!(!is_downgrade(v("1.3.0"), Some(v("1.3.0"))));
        assert!(!is_downgrade(v("1.3.0"), None));
    }

    #[test]
    fn the_dialog_variant_follows_the_downgrade_rule() {
        assert_eq!(confirm_kind(v("1.3.0"), Some(v("1.3.1-rc2"))), ConfirmKind::Downgrade);
        assert_eq!(confirm_kind(v("1.3.1"), Some(v("1.3.1-rc2"))), ConfirmKind::Plain);
        assert_eq!(confirm_kind(v("1.3.2-rc1"), Some(v("1.3.1"))), ConfirmKind::Plain);
        assert_eq!(ConfirmKind::Downgrade.key(), "downgrade");
    }

    #[test]
    fn a_list_with_nothing_at_or_above_the_running_build_is_reported() {
        let only_old: Vec<Release> = parse_releases(
            r#"[{"tag_name":"v1.3.0","prerelease":false,"draft":false,"assets":[]}]"#,
            Stream::Launcher,
        )
        .unwrap();
        let list = build_list(&only_old, UpdateChannel::Stable, Some(v("1.3.1-rc2")));
        assert!(nothing_at_or_above(&list, Some(v("1.3.1-rc2"))));
        assert!(!nothing_at_or_above(&list, Some(v("1.3.0"))));
        assert!(nothing_at_or_above(&[], Some(v("1.3.0"))), "an empty list has nothing newer either");
        assert!(!nothing_at_or_above(&list, None));
    }
}
