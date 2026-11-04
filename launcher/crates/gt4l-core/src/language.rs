//! The launcher's language: one of the languages of `ui/locales/index.json` (TASK-102). The list, and the texts, come
//! from the locale files (see `build.rs`); nothing in this module names a language.
//!
//! The Settings row `language` (Auto, then one entry per language) is stored as `auto` or a tag. `auto` follows the
//! Windows user language. The rule is shared with the UI (`ui/js/i18n.js`, `GT4.i18n.resolve`): an exact tag match
//! first, then the base language (`pt`, `pt-PT` -> `pt-BR`), then the fallback (en-US). Change both together and keep
//! the cases of both test suites.

include!(concat!(env!("OUT_DIR"), "/locales.rs"));

/// A language of the launcher, by its tag (`en-US`, `pt-BR`, ...) as listed in `index.json`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Language(&'static str);

/// The tag as the UI and the settings file write it: case and `_` / `-` do not matter for the lookup.
fn normalized(tag: &str) -> String {
    tag.trim().replace('_', "-").to_ascii_lowercase()
}

impl Language {
    /// The fallback language (the first entry of `index.json`, en-US).
    pub const fn fallback() -> Language {
        Language(FALLBACK_TAG)
    }

    /// The tag of this language, as listed in `index.json`.
    pub fn tag(self) -> &'static str {
        self.0
    }

    /// The language with this tag, when `index.json` lists it.
    pub fn from_tag(tag: &str) -> Option<Language> {
        let wanted = normalized(tag);
        LANGUAGE_TAGS.iter().find(|(known, _)| normalized(known) == wanted).map(|(known, _)| Language(*known))
    }

    /// The name of this language, as `index.json` gives it (for the Settings list and "Auto (name)").
    pub fn name(self) -> &'static str {
        LANGUAGE_TAGS.iter().find(|(known, _)| *known == self.0).map(|(_, name)| *name).unwrap_or(self.0)
    }
}

/// Maps one OS locale name to a language: an exact tag first, then the base language, then the fallback. An empty
/// locale gives the fallback.
pub fn from_os_locale(locale: &str) -> Language {
    let wanted = normalized(locale);
    if wanted.is_empty() {
        return Language::fallback();
    }
    if let Some(exact) = Language::from_tag(&wanted) {
        return exact;
    }
    let base = wanted.split('-').next().unwrap_or("");
    LANGUAGE_TAGS
        .iter()
        .find(|(known, _)| normalized(known).split('-').next() == Some(base))
        .map(|(known, _)| Language(*known))
        .unwrap_or_else(Language::fallback)
}

/// The language the launcher uses: the stored setting when it names a language, else the OS locale (`auto`, or any
/// other stored text, which the settings table never holds).
pub fn resolve(setting: &str, os_locale: &str) -> Language {
    if setting != "auto" {
        if let Some(language) = Language::from_tag(setting) {
            return language;
        }
    }
    from_os_locale(os_locale)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn pt() -> Language {
        Language::from_tag("pt-BR").expect("pt-BR is listed in index.json")
    }

    fn en() -> Language {
        Language::from_tag("en-US").expect("en-US is listed in index.json")
    }

    #[test]
    fn the_fallback_is_the_first_listed_language_and_english() {
        assert_eq!(Language::fallback(), en());
        assert_eq!(Language::fallback().tag(), "en-US");
    }

    #[test]
    fn every_listed_language_has_a_name() {
        assert_eq!(pt().name(), "Português (Brasil)");
        assert_eq!(en().name(), "English");
    }

    #[test]
    fn portuguese_locales_resolve_to_brazilian_portuguese() {
        for locale in ["pt-BR", "pt_BR", "pt-br", "pt", "PT", "pt-PT"] {
            assert_eq!(from_os_locale(locale), pt(), "{locale}");
        }
    }

    #[test]
    fn other_locales_and_empty_text_resolve_to_english() {
        for locale in ["en-US", "en-GB", "es-ES", "fr", "", "  "] {
            assert_eq!(from_os_locale(locale), en(), "{locale:?}");
        }
    }

    #[test]
    fn a_stored_language_wins_over_the_os_locale() {
        assert_eq!(resolve("en-US", "pt-BR"), en());
        assert_eq!(resolve("pt-BR", "en-US"), pt());
    }

    #[test]
    fn auto_follows_the_os_locale() {
        assert_eq!(resolve("auto", "pt-BR"), pt());
        assert_eq!(resolve("auto", "fr-FR"), en());
        assert_eq!(resolve("auto", ""), en());
    }

    #[test]
    fn an_unknown_stored_tag_follows_the_os_locale() {
        assert_eq!(resolve("xx-YY", "pt-BR"), pt());
    }

    #[test]
    fn tags_are_the_ones_of_the_index() {
        assert_eq!(en().tag(), "en-US");
        assert_eq!(pt().tag(), "pt-BR");
        assert!(Language::from_tag("de-DE").is_none());
    }
}
