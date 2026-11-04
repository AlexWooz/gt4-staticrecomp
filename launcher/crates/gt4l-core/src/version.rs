//! Semantic versions of the two release streams (GT4Recomp and gt4-gs), SemVer `MAJOR.MINOR.PATCH`.
//!
//! [`Version`] is the plain `MAJOR.MINOR.PATCH` (pre-release tags and build metadata are refused). Comparison is
//! numeric, so `1.10.0` is newer than `1.9.0`. [`ReleaseVersion`] (TASK-109) adds the optional release candidate part
//! `-rcN` of the GT4Recomp test builds: `1.3.0-rc4 < 1.3.0-rc5 < 1.3.0 < 1.3.1-rc1 < 1.3.1` (see `docs/RELEASES.md`).

use std::fmt;

#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct Version(pub u32, pub u32, pub u32);

impl Version {
    /// Parses `1.2.3` or `v1.2.3`. Pre-release tags, build metadata, signs and missing parts are refused.
    pub fn parse(text: &str) -> Option<Version> {
        let body = text.strip_prefix('v').unwrap_or(text);
        let mut parts = body.split('.');
        let major = number(parts.next()?)?;
        let minor = number(parts.next()?)?;
        let patch = number(parts.next()?)?;
        if parts.next().is_some() {
            return None;
        }
        Some(Version(major, minor, patch))
    }

    /// `1.2.3`, without a `v`.
    pub fn text(&self) -> String {
        format!("{}.{}.{}", self.0, self.1, self.2)
    }
}

impl fmt::Display for Version {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(formatter, "{}.{}.{}", self.0, self.1, self.2)
    }
}

/// A GT4Recomp release: a [`Version`] with an optional release candidate number (`1.3.0-rc5`). A candidate is older than
/// the final release of the same numbers; candidates of one version compare by their number, numerically.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct ReleaseVersion {
    pub base: Version,
    /// `Some(5)` for `-rc5`; `None` for a final (stable) release.
    pub candidate: Option<u32>,
}

impl ReleaseVersion {
    pub const fn stable(base: Version) -> ReleaseVersion {
        ReleaseVersion { base, candidate: None }
    }

    pub const fn candidate(base: Version, number: u32) -> ReleaseVersion {
        ReleaseVersion { base, candidate: Some(number) }
    }

    /// Parses `1.3.0`, `v1.3.0`, `1.3.0-rc5` or `v1.3.0-rc5`. The candidate number is `1` or more, digits only.
    pub fn parse(text: &str) -> Option<ReleaseVersion> {
        let body = text.strip_prefix('v').unwrap_or(text);
        match body.split_once('-') {
            None => Version::parse(body).map(ReleaseVersion::stable),
            Some((numbers, suffix)) => {
                let digits = suffix.strip_prefix("rc")?;
                let number = number(digits)?;
                if number == 0 || (digits.len() > 1 && digits.starts_with('0')) {
                    return None;
                }
                Some(ReleaseVersion::candidate(Version::parse(numbers)?, number))
            }
        }
    }

    pub fn is_candidate(&self) -> bool {
        self.candidate.is_some()
    }

    /// `1.3.0` or `1.3.0-rc5`, without a `v`. This is also the label of a build and the part of asset names.
    pub fn text(&self) -> String {
        match self.candidate {
            Some(number) => format!("{}-rc{number}", self.base.text()),
            None => self.base.text(),
        }
    }
}

impl From<Version> for ReleaseVersion {
    fn from(base: Version) -> ReleaseVersion {
        ReleaseVersion::stable(base)
    }
}

impl Ord for ReleaseVersion {
    fn cmp(&self, other: &Self) -> std::cmp::Ordering {
        // A final release (no candidate) is newer than any candidate of the same numbers.
        self.base.cmp(&other.base).then_with(|| match (self.candidate, other.candidate) {
            (None, None) => std::cmp::Ordering::Equal,
            (None, Some(_)) => std::cmp::Ordering::Greater,
            (Some(_), None) => std::cmp::Ordering::Less,
            (Some(a), Some(b)) => a.cmp(&b),
        })
    }
}

impl PartialOrd for ReleaseVersion {
    fn partial_cmp(&self, other: &Self) -> Option<std::cmp::Ordering> {
        Some(self.cmp(other))
    }
}

impl fmt::Display for ReleaseVersion {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(&self.text())
    }
}

/// Digits only: `u32::from_str` alone would also accept a leading `+`.
fn number(part: &str) -> Option<u32> {
    if part.is_empty() || !part.bytes().all(|byte| byte.is_ascii_digit()) {
        return None;
    }
    part.parse().ok()
}

#[cfg(test)]
mod tests {
    use super::{ReleaseVersion, Version};

    fn rv(text: &str) -> ReleaseVersion {
        ReleaseVersion::parse(text).unwrap_or_else(|| panic!("{text} parses"))
    }

    #[test]
    fn release_versions_parse_tags_with_an_optional_candidate_part() {
        assert_eq!(rv("v1.3.0"), ReleaseVersion::stable(Version(1, 3, 0)));
        assert_eq!(rv("v1.3.0-rc5"), ReleaseVersion::candidate(Version(1, 3, 0), 5));
        assert_eq!(rv("1.3.0-rc12").text(), "1.3.0-rc12");
        for text in ["1.3.0-rc", "1.3.0-rc0", "1.3.0-rc05", "1.3.0-beta1", "1.3.0-rc1-x", "1.3-rc1", "1.3.0-RC1", "-rc1", "1.3.0+b"] {
            assert_eq!(ReleaseVersion::parse(text), None, "{text} must be refused");
        }
    }

    #[test]
    fn candidates_sort_before_their_final_release_and_by_number() {
        let order = ["1.3.0-rc4", "1.3.0-rc5", "1.3.0-rc10", "1.3.0", "1.3.1-rc1", "1.3.1", "1.10.0-rc1"];
        for pair in order.windows(2) {
            assert!(rv(pair[0]) < rv(pair[1]), "{} < {}", pair[0], pair[1]);
        }
        assert!(rv("1.3.0-rc9") < rv("1.3.0-rc10"), "rc numbers compare numerically");
        assert_eq!(rv("1.3.0").cmp(&rv("1.3.0")), std::cmp::Ordering::Equal);
    }

    #[test]
    fn parses_plain_and_v_prefixed_stable_versions() {
        assert_eq!(Version::parse("1.0.0"), Some(Version(1, 0, 0)));
        assert_eq!(Version::parse("v0.3.2"), Some(Version(0, 3, 2)));
        assert_eq!(Version::parse("10.20.30"), Some(Version(10, 20, 30)));
    }

    #[test]
    fn refuses_prereleases_metadata_signs_and_missing_parts() {
        for text in ["1.0.0-rc1", "1.0.0+build7", "1.0", "1.0.0.0", "+1.0.0", "1.-0.0", "", "v", "a.b.c", "1..0"] {
            assert_eq!(Version::parse(text), None, "{text} must be refused");
        }
    }

    #[test]
    fn comparison_is_numeric_not_textual() {
        assert!(Version(1, 10, 0) > Version(1, 9, 0));
        assert!(Version(1, 0, 1) > Version(1, 0, 0));
        assert!(Version(2, 0, 0) > Version(1, 99, 99));
        assert_eq!(Version(1, 0, 1).to_string(), "1.0.1");
    }
}
