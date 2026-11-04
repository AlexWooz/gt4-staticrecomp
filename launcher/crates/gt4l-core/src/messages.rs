//! The text the launcher shows that Rust builds: setup states and problems, command errors, notices after a game
//! session, the migration and renderer notices, the About text and the native dialogs. The texts are in the locale
//! files of the UI (`ui/locales/<tag>.json`, keys `rust.<id>`); `build.rs` puts them into a table for every language
//! (`TEXTS`, see `language.rs`). A text that a language lacks falls back to the fallback language, so a new language
//! can be partial. Dynamic parts (paths, versions, numbers, names) are `{name}` placeholders filled by [`render`]; a
//! translation keeps the placeholders of the fallback text (a test checks it). Developer and log-only text stays
//! English and is not in the locale files.

use crate::language::{Language, FALLBACK_TAG, TEXTS};

/// The text of message `id` in `language`, or in the fallback language when `language` has none. An unknown id is
/// returned as it is (a test checks that every id is translated into the fallback language).
pub fn text(id: &'static str, language: Language) -> &'static str {
    let lookup = |tag: &str| {
        TEXTS
            .iter()
            .find(|(text_tag, text_id, _)| *text_tag == tag && *text_id == id)
            .map(|(_, _, text)| *text)
    };
    lookup(language.tag()).or_else(|| lookup(FALLBACK_TAG)).unwrap_or(id)
}

/// The message `id` in `language` with its `{name}` placeholders filled from `params`. A placeholder without a value
/// is left as it is.
pub fn render(id: &'static str, language: Language, params: &[(&str, &str)]) -> String {
    let mut out = text(id, language).to_string();
    for (name, value) in params {
        out = out.replace(&format!("{{{name}}}"), value);
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::language::LANGUAGE_TAGS;

    /// The `{name}` placeholders of a text, sorted, without duplicates.
    fn placeholders(text: &str) -> Vec<String> {
        let mut names = Vec::new();
        let mut rest = text;
        while let Some(start) = rest.find('{') {
            let Some(end) = rest[start..].find('}') else { break };
            names.push(rest[start + 1..start + end].to_string());
            rest = &rest[start + end + 1..];
        }
        names.sort();
        names.dedup();
        names
    }

    /// Every message id, from the fallback language.
    fn ids() -> Vec<&'static str> {
        TEXTS.iter().filter(|(tag, _, _)| *tag == FALLBACK_TAG).map(|(_, id, _)| *id).collect()
    }

    #[test]
    fn ids_are_unique_within_a_language() {
        let mut seen: Vec<(&str, &str)> = Vec::new();
        for (tag, id, _) in TEXTS {
            assert!(!seen.contains(&(*tag, *id)), "duplicate {tag} {id}");
            seen.push((*tag, *id));
        }
    }

    #[test]
    fn every_language_has_every_message_and_no_empty_text() {
        let fallback_ids = ids();
        assert!(fallback_ids.len() > 50, "the fallback language has the messages");
        for (tag, _) in LANGUAGE_TAGS {
            for id in &fallback_ids {
                let found = TEXTS.iter().find(|(text_tag, text_id, _)| text_tag == tag && text_id == id);
                let (_, _, text) = found.unwrap_or_else(|| panic!("{tag} has no text for {id}"));
                assert!(!text.trim().is_empty(), "{tag} {id} is empty");
            }
        }
    }

    #[test]
    fn every_language_uses_the_placeholders_of_the_fallback() {
        for (tag, id, text) in TEXTS {
            let fallback = TEXTS
                .iter()
                .find(|(text_tag, text_id, _)| *text_tag == FALLBACK_TAG && text_id == id)
                .map(|(_, _, text)| *text)
                .expect("every id has the fallback text");
            assert_eq!(placeholders(text), placeholders(fallback), "{tag} {id}");
        }
    }

    #[test]
    fn rendering_fills_the_placeholders_in_the_chosen_language() {
        let pt = Language::from_tag("pt-BR").expect("pt-BR is listed");
        assert_eq!(
            render("setup.picker_title_game", pt, &[("game", "Gran Turismo 4")]),
            "Escolha a imagem do disco de Gran Turismo 4"
        );
        assert_eq!(
            render("setup.picker_title_game", Language::fallback(), &[("game", "Gran Turismo 4")]),
            "Choose your Gran Turismo 4 disc image"
        );
    }

    #[test]
    fn an_unknown_id_is_returned_as_it_is() {
        let pt = Language::from_tag("pt-BR").expect("pt-BR is listed");
        assert_eq!(text("no.such.message", pt), "no.such.message");
    }
}
