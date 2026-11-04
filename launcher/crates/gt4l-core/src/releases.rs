//! Release lists of the two streams the launcher follows (see `docs/RELEASES.md`):
//!
//! - [`Stream::Launcher`]: GT4Recomp (launcher, game executable and installer), repository
//!   `moesuito/gt4-staticrecomp`, tags `vX.Y.Z`, asset `GT4Recomp-Setup-X.Y.Z.exe` and `SHA256SUMS`.
//! - [`Stream::Renderer`]: gt4-gs (the PCSX2 GS renderer), repository `moesuito/gt4recomp-gs`, tags
//!   `gt4gs-vX.Y.Z`, asset `gt4-gs-X.Y.Z-windows-x64.zip` and `SHA256SUMS`.
//!
//! Drafts are never offered. Release candidates (`vX.Y.Z-rcN`, GitHub pre-releases) are offered only on the Beta channel
//! (TASK-109, `update::UpdateChannel`); the Stable channel never sees them. The release body is the release notes. Tests and the offline
//! folder source read the same JSON as the GitHub REST API, so no test touches the network.

use std::fs;
use std::io::Read;
use std::path::PathBuf;
use std::time::Duration;

use serde::Deserialize;

pub use crate::version::{ReleaseVersion, Version};

/// GT4Recomp repository (launcher, game and installer).
pub const LAUNCHER_REPOSITORY: &str = "moesuito/gt4-staticrecomp";
/// gt4-gs repository (the PCSX2 GS renderer).
pub const RENDERER_REPOSITORY: &str = "moesuito/gt4recomp-gs";
/// Checksum list attached to each release of both streams.
pub const CHECKSUM_ASSET: &str = "SHA256SUMS";
/// Release notes longer than this are cut (characters). The popup shows a summary, not a page.
pub const MAX_NOTES_CHARS: usize = 6000;
/// Lowest stable GT4Recomp version the launcher offers.
pub const LAUNCHER_MINIMUM: Version = Version(1, 0, 0);
/// Lowest stable gt4-gs version the launcher installs. 1.2.1 (TASK-093): gt4-gs reads the `Textures` folder itself, so
/// the launcher stops making `replacements` and flattens an old one (`textures::flatten_replacements`). 1.2.0 (TASK-088)
/// added the custom texture loading and the 2-row bottom crop. An older install is replaced at the next launch
/// (`gs_release::needs_install`).
pub const RENDERER_MINIMUM: Version = Version(1, 2, 1);
/// Lowest GT4Recomp version the version picker lists (TASK-109): `1.3.0-rc5` is the first build that reads the
/// multi-game data layout, settings schema 8 and the per-game save names, so an older one cannot read this data.
pub const PICKER_MINIMUM: ReleaseVersion = ReleaseVersion::candidate(Version(1, 3, 0), 5);
/// Longest time a download may take. The gt4-gs package is far below the size limit of the install code.
pub const DOWNLOAD_TIMEOUT: Duration = Duration::from_secs(600);
/// Longest time the release list may take at launch. The check never blocks Play.
pub const LIST_TIMEOUT: Duration = Duration::from_secs(6);

/// The two release streams.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Stream {
    Launcher,
    Renderer,
}

impl Stream {
    /// Stable key used in the UI, the settings file and the log.
    pub fn key(self) -> &'static str {
        match self {
            Stream::Launcher => "launcher",
            Stream::Renderer => "renderer",
        }
    }

    pub fn repository(self) -> &'static str {
        match self {
            Stream::Launcher => LAUNCHER_REPOSITORY,
            Stream::Renderer => RENDERER_REPOSITORY,
        }
    }

    pub fn minimum(self) -> Version {
        match self {
            Stream::Launcher => LAUNCHER_MINIMUM,
            Stream::Renderer => RENDERER_MINIMUM,
        }
    }

    /// The tag prefix: `v` for GT4Recomp, `gt4gs-v` for gt4-gs.
    pub fn tag_prefix(self) -> &'static str {
        match self {
            Stream::Launcher => "v",
            Stream::Renderer => "gt4gs-v",
        }
    }

    /// The version of a tag of this stream, or `None` for any other tag.
    pub fn parse_tag(self, tag: &str) -> Option<Version> {
        tag.strip_prefix(self.tag_prefix()).and_then(Version::parse)
    }

    /// The version of a tag including a release candidate part (`v1.3.0-rc5`), or `None` for any other tag.
    pub fn parse_release_tag(self, tag: &str) -> Option<ReleaseVersion> {
        tag.strip_prefix(self.tag_prefix()).and_then(ReleaseVersion::parse)
    }

    /// The tag of a version of this stream.
    pub fn tag(self, version: impl Into<ReleaseVersion>) -> String {
        format!("{}{}", self.tag_prefix(), version.into().text())
    }

    /// The download asset of a version: the setup program for GT4Recomp, the zip for gt4-gs. A release candidate
    /// carries its label: `GT4Recomp-Setup-1.3.0-rc5.exe`.
    pub fn package_name(self, version: impl Into<ReleaseVersion>) -> String {
        let text = version.into().text();
        match self {
            Stream::Launcher => format!("GT4Recomp-Setup-{text}.exe"),
            Stream::Renderer => format!("gt4-gs-{text}-windows-x64.zip"),
        }
    }

    /// The download asset for a build kind (TASK-098): the portable build updates from `GT4Recomp-<version>-portable.zip`,
    /// the installer build from the setup program. gt4-gs has one package for both.
    pub fn package_name_for(self, version: impl Into<ReleaseVersion>, portable: bool) -> String {
        let version = version.into();
        match (self, portable) {
            (Stream::Launcher, true) => format!("GT4Recomp-{}-portable.zip", version.text()),
            _ => self.package_name(version),
        }
    }
}

/// One downloadable file of a release.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Asset {
    pub name: String,
    pub url: String,
}

/// One release as the launcher reads it.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Release {
    pub tag: String,
    /// `None` when the tag does not match the stream's tag format.
    pub version: Option<Version>,
    /// The version with its release candidate part: `1.3.0-rc5` has `version` None (not stable) and this `Some`.
    pub release_version: Option<ReleaseVersion>,
    /// GitHub's `published_at` (ISO 8601 UTC, sorts as text); `None` for a draft.
    pub published_at: Option<String>,
    pub prerelease: bool,
    pub draft: bool,
    /// The release body (the release notes), cut to [`MAX_NOTES_CHARS`]. Plain text as GitHub stores it.
    pub notes: String,
    pub assets: Vec<Asset>,
}

impl Release {
    pub fn asset(&self, name: &str) -> Option<&Asset> {
        self.assets.iter().find(|asset| asset.name == name)
    }

    /// A release candidate (`vX.Y.Z-rcN`): not a draft, with a candidate tag.
    pub fn is_candidate(&self) -> bool {
        !self.draft && self.release_version.is_some_and(|version| version.is_candidate())
    }

    /// A stable release: not a draft, not a prerelease, with a valid version of the stream.
    pub fn is_stable(&self) -> bool {
        !self.draft && !self.prerelease && self.version.is_some()
    }
}

#[derive(Deserialize)]
struct ApiRelease {
    tag_name: String,
    #[serde(default)]
    prerelease: bool,
    #[serde(default)]
    draft: bool,
    #[serde(default)]
    published_at: Option<String>,
    #[serde(default)]
    body: Option<String>,
    #[serde(default)]
    assets: Vec<ApiAsset>,
}

#[derive(Deserialize)]
struct ApiAsset {
    name: String,
    browser_download_url: String,
}

/// Reads the release list (GitHub REST `GET /repos/{repo}/releases` format) of one stream.
pub fn parse_releases(json: &str, stream: Stream) -> Result<Vec<Release>, String> {
    let raw: Vec<ApiRelease> = serde_json::from_str(json).map_err(|error| format!("release list: {error}"))?;
    Ok(raw
        .into_iter()
        .map(|release| Release {
            version: stream.parse_tag(&release.tag_name),
            release_version: stream.parse_release_tag(&release.tag_name),
            published_at: release.published_at,
            tag: release.tag_name,
            prerelease: release.prerelease,
            draft: release.draft,
            notes: truncate_chars(&release.body.unwrap_or_default(), MAX_NOTES_CHARS),
            assets: release
                .assets
                .into_iter()
                .map(|asset| Asset { name: asset.name, url: asset.browser_download_url })
                .collect(),
        })
        .collect())
}

/// The newest stable release at or above `minimum`. Drafts and prereleases are never chosen.
pub fn select_latest(releases: &[Release], minimum: Version) -> Option<&Release> {
    releases
        .iter()
        .filter(|release| release.is_stable())
        .filter_map(|release| release.version.map(|version| (version, release)))
        .filter(|(version, _)| *version >= minimum)
        .max_by_key(|(version, _)| *version)
        .map(|(_, release)| release)
}

/// The release the launcher offers on a channel, newest by version, at or above `minimum`, never a draft. Stable: the
/// stable releases only. Beta: the stable releases and the release candidates together, so a Beta player gets the final
/// release when it ships.
pub fn select_offer(releases: &[Release], channel: crate::update::UpdateChannel, minimum: ReleaseVersion) -> Option<&Release> {
    releases
        .iter()
        .filter(|release| release.is_stable() || (channel == crate::update::UpdateChannel::Beta && release.is_candidate()))
        .filter_map(|release| release.release_version.map(|version| (version, release)))
        .filter(|(version, _)| *version >= minimum)
        .max_by_key(|(version, _)| *version)
        .map(|(_, release)| release)
}

/// Checks a downloaded package against the `SHA256SUMS` text of its release. The package must have a line in the
/// list and its SHA-256 must match. Used before the GT4Recomp setup program is started.
pub fn verify_package(name: &str, bytes: &[u8], sums_text: &str) -> Result<(), String> {
    check_sum(name, &crate::gs_release::sha256_hex(bytes), sums_text)
}

/// The same check for a SHA-256 that was computed elsewhere (a file hashed while it was downloaded).
pub fn check_sum(name: &str, actual_hex: &str, sums_text: &str) -> Result<(), String> {
    let sums = crate::gs_release::parse_checksums(sums_text);
    let expected = sums.get(name).ok_or_else(|| format!("{CHECKSUM_ASSET} has no line for {name}"))?;
    if actual_hex != expected.as_str() {
        return Err(format!("{name} does not match its SHA-256 (corrupt or changed download)"));
    }
    Ok(())
}

/// Cuts text to at most `limit` characters (never inside a character).
fn truncate_chars(text: &str, limit: usize) -> String {
    text.chars().take(limit).collect()
}

/// Where release data comes from. GitHub in the release build; a folder in tests.
pub trait ReleaseSource {
    /// The JSON array of the release list of the source's repository.
    fn releases_json(&self) -> Result<String, String>;
    /// The bytes behind a download URL taken from a release asset.
    fn download(&self, url: &str) -> Result<Vec<u8>, String>;

    /// [`ReleaseSource::download`] that reports `(bytes done, total bytes if known)` while it runs. The default
    /// reports once at the end; the GitHub source reports every chunk.
    fn download_reporting(&self, url: &str, report: &mut dyn FnMut(u64, Option<u64>)) -> Result<Vec<u8>, String> {
        let bytes = self.download(url)?;
        report(bytes.len() as u64, Some(bytes.len() as u64));
        Ok(bytes)
    }
}

/// A release source that reads a folder: `releases.json` plus the asset files. Download URLs of the form
/// `https://github.com/<repository>/releases/download/<tag>/<name>` map to `<root>/<tag>/<name>`. Used by tests
/// and for an offline copy of a release.
pub struct FolderSource {
    pub root: PathBuf,
    pub repository: String,
}

impl ReleaseSource for FolderSource {
    fn releases_json(&self) -> Result<String, String> {
        fs::read_to_string(self.root.join("releases.json")).map_err(|error| error.to_string())
    }

    fn download(&self, url: &str) -> Result<Vec<u8>, String> {
        let prefix = format!("https://github.com/{}/releases/download/", self.repository);
        let relative = url.strip_prefix(&prefix).ok_or_else(|| format!("not a release URL: {url}"))?;
        if relative.contains("..") {
            return Err(format!("refused URL: {url}"));
        }
        fs::read(self.root.join(relative)).map_err(|error| format!("download failed: {error}"))
    }
}

/// The release list and downloads of one repository from GitHub.
pub struct GithubSource {
    pub repository: &'static str,
    /// Timeout of the release list request.
    pub list_timeout: Duration,
}

impl GithubSource {
    /// The source of a stream with the short timeout used at launch.
    pub fn for_stream(stream: Stream) -> GithubSource {
        GithubSource { repository: stream.repository(), list_timeout: LIST_TIMEOUT }
    }

    fn agent(timeout: Duration) -> ureq::Agent {
        ureq::Agent::new_with_config(ureq::config::Config::builder().timeout_global(Some(timeout)).build())
    }
}

impl ReleaseSource for GithubSource {
    fn releases_json(&self) -> Result<String, String> {
        let url = format!("https://api.github.com/repos/{}/releases", self.repository);
        let mut response = GithubSource::agent(self.list_timeout)
            .get(&url)
            .header("User-Agent", "GT4Recomp-launcher")
            .header("Accept", "application/vnd.github+json")
            .call()
            .map_err(|error| format!("release list: {error}"))?;
        response.body_mut().read_to_string().map_err(|error| format!("release list: {error}"))
    }

    fn download(&self, url: &str) -> Result<Vec<u8>, String> {
        self.download_reporting(url, &mut |_, _| {})
    }

    fn download_reporting(&self, url: &str, report: &mut dyn FnMut(u64, Option<u64>)) -> Result<Vec<u8>, String> {
        let mut response = GithubSource::agent(DOWNLOAD_TIMEOUT)
            .get(url)
            .header("User-Agent", "GT4Recomp-launcher")
            .call()
            .map_err(|error| format!("download: {error}"))?;
        let total = response
            .headers()
            .get("content-length")
            .and_then(|value| value.to_str().ok())
            .and_then(|value| value.trim().parse::<u64>().ok());
        let mut reader = response.body_mut().with_config().limit(crate::gs_release::MAX_PACKAGE_BYTES).reader();
        let mut bytes = Vec::new();
        let mut buffer = vec![0u8; 256 * 1024];
        loop {
            let read = reader.read(&mut buffer).map_err(|error| format!("download: {error}"))?;
            if read == 0 {
                break;
            }
            bytes.extend_from_slice(&buffer[..read]);
            report(bytes.len() as u64, total);
        }
        Ok(bytes)
    }
}

/// Largest GT4Recomp setup program the launcher downloads (bytes). The game files make it large; this bound only
/// protects the disk against a bad response.
pub const MAX_SETUP_BYTES: u64 = 4 * 1024 * 1024 * 1024;

/// Streams a download to `path` and returns its SHA-256 (lowercase hex). The file is never held in memory, so a
/// large setup program does not need the same amount of RAM. Refuses a response larger than `limit`.
pub fn download_to_file(url: &str, path: &std::path::Path, limit: u64) -> Result<String, String> {
    use sha2::{Digest, Sha256};
    use std::io::Write;
    let mut response = GithubSource::agent(DOWNLOAD_TIMEOUT)
        .get(url)
        .header("User-Agent", "GT4Recomp-launcher")
        .call()
        .map_err(|error| format!("download: {error}"))?;
    let mut reader = response.body_mut().with_config().limit(limit).reader();
    let mut file = fs::File::create(path).map_err(|error| format!("{}: {error}", path.display()))?;
    let mut hasher = Sha256::new();
    let mut buffer = vec![0u8; 64 * 1024];
    loop {
        let read = reader.read(&mut buffer).map_err(|error| format!("download: {error}"))?;
        if read == 0 {
            break;
        }
        hasher.update(&buffer[..read]);
        file.write_all(&buffer[..read]).map_err(|error| format!("{}: {error}", path.display()))?;
    }
    file.sync_all().map_err(|error| format!("{}: {error}", path.display()))?;
    Ok(hasher.finalize().iter().map(|byte| format!("{byte:02x}")).collect())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn tags_and_package_names_follow_the_two_streams() {
        assert_eq!(Stream::Launcher.parse_tag("v1.0.0"), Some(Version(1, 0, 0)));
        assert_eq!(Stream::Launcher.parse_tag("gt4gs-v1.0.1"), None, "a renderer tag is not a launcher tag");
        assert_eq!(Stream::Renderer.parse_tag("gt4gs-v1.0.1"), Some(Version(1, 0, 1)));
        assert_eq!(Stream::Renderer.parse_tag("v1.0.1"), None);
        assert_eq!(Stream::Renderer.tag(Version(1, 0, 1)), "gt4gs-v1.0.1");
        assert_eq!(Stream::Launcher.package_name(Version(1, 0, 0)), "GT4Recomp-Setup-1.0.0.exe");
        assert_eq!(Stream::Renderer.package_name(Version(1, 0, 1)), "gt4-gs-1.0.1-windows-x64.zip");
    }

    #[test]
    fn a_candidate_build_carries_its_label_in_the_asset_names_and_the_tag() {
        let rc5 = ReleaseVersion::parse("1.3.0-rc5").unwrap();
        assert_eq!(Stream::Launcher.package_name(rc5), "GT4Recomp-Setup-1.3.0-rc5.exe");
        assert_eq!(Stream::Launcher.package_name_for(rc5, false), "GT4Recomp-Setup-1.3.0-rc5.exe");
        assert_eq!(Stream::Launcher.package_name_for(rc5, true), "GT4Recomp-1.3.0-rc5-portable.zip");
        assert_eq!(Stream::Launcher.tag(rc5), "v1.3.0-rc5");
        assert_eq!(Stream::Launcher.parse_release_tag("v1.3.0-rc5"), Some(rc5));
        assert_eq!(Stream::Launcher.parse_tag("v1.3.0-rc5"), None, "a candidate is not a stable version");
        assert_eq!(Stream::Launcher.package_name(Version(1, 3, 0)), "GT4Recomp-Setup-1.3.0.exe");
    }

    #[test]
    fn selection_takes_the_highest_stable_at_or_above_the_minimum() {
        let json = r#"[
          {"tag_name":"gt4gs-v1.2.0","prerelease":false,"draft":true,"assets":[]},
          {"tag_name":"gt4gs-v1.1.1","prerelease":true,"draft":false,"assets":[]},
          {"tag_name":"gt4gs-v1.2.0","prerelease":false,"draft":false,"assets":[]},
          {"tag_name":"gt4gs-v1.0.1","prerelease":false,"draft":false,"assets":[]},
          {"tag_name":"gt4gs-v1.10.0","prerelease":false,"draft":false,"assets":[]},
          {"tag_name":"gt4gs-v1.9.0","prerelease":false,"draft":false,"assets":[]}
        ]"#;
        let releases = parse_releases(json, Stream::Renderer).unwrap();
        let chosen = select_latest(&releases, RENDERER_MINIMUM).unwrap();
        assert_eq!(chosen.tag, "gt4gs-v1.10.0", "numeric comparison: 1.10.0 beats 1.9.0");

        let only_old = r#"[
          {"tag_name":"gt4gs-v1.0.1","prerelease":false,"draft":false,"assets":[]},
          {"tag_name":"gt4gs-v1.1.1","prerelease":true,"draft":false,"assets":[]}
        ]"#;
        let releases = parse_releases(only_old, Stream::Renderer).unwrap();
        assert!(select_latest(&releases, RENDERER_MINIMUM).is_none(), "1.0.1 is below the minimum, 1.1.1 a prerelease");
    }

    #[test]
    fn release_notes_are_cut_on_a_character_boundary() {
        let long = "\u{e9}".repeat(MAX_NOTES_CHARS + 10);
        let json = format!(r#"[{{"tag_name":"v1.0.0","body":"{long}","assets":[]}}]"#);
        let releases = parse_releases(&json, Stream::Launcher).unwrap();
        assert_eq!(releases[0].notes.chars().count(), MAX_NOTES_CHARS);
    }

    #[test]
    fn packages_are_verified_against_their_sums_line() {
        let bytes = b"setup program";
        let sums = format!("{}  GT4Recomp-Setup-1.0.0.exe
", crate::gs_release::sha256_hex(bytes));
        assert!(verify_package("GT4Recomp-Setup-1.0.0.exe", bytes, &sums).is_ok());
        assert!(verify_package("GT4Recomp-Setup-1.0.0.exe", b"changed", &sums).is_err());
        assert!(verify_package("GT4Recomp-Setup-1.0.1.exe", bytes, &sums).is_err(), "no line for that name");
    }

    #[test]
    fn a_missing_body_gives_empty_notes() {
        let releases = parse_releases(r#"[{"tag_name":"v1.0.0","assets":[]}]"#, Stream::Launcher).unwrap();
        assert_eq!(releases[0].notes, "");
        assert!(releases[0].is_stable());
    }
}
