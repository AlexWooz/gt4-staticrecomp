# Translating GT4Recomp

The launcher's text lives in plain files. A new language is data, not code: no Rust or JavaScript change is needed.

## What to translate

| File | What it holds |
|---|---|
| `launcher/crates/gt4l-tauri/ui/locales/<tag>.json` | every text of the launcher page, and the messages that Rust builds (keys starting with `rust.`) |
| `launcher/crates/gt4l-tauri/ui/locales/index.json` | the list of languages, in the order of the Settings list: `[{"tag": "en-US", "name": "English"}, ...]` |
| `docs/release-notes/launcher/<tag>/<version>.md` | the short release notes ("What's new") in that language, optional |

`en-US` is the source of truth and the fallback for any missing key. Read `en-US.json` first.

## Steps

1. Copy `launcher/crates/gt4l-tauri/ui/locales/en-US.json` to `<tag>.json` (for example `de-DE.json`).
2. Translate the **values**. Never change the **keys**.
3. Keep every `{placeholder}` exactly as it is (`{version}`, `{game}`, `{size}`...). Translate the text around it, and
   put the placeholders where the sentence needs them.
4. Add an entry to `index.json`: `{"tag": "de-DE", "name": "Deutsch"}`. The name is written in that language.
5. Optional: add the short release notes. Copy `docs/release-notes/launcher/pt-BR/1.2.3.md` as a model. A version
   without a file in your folder shows the en-US notes.
6. Run the checks (below).

No Rust or JavaScript change is needed. A language is picked up when it is in `index.json` and its file exists.

## Style

- Short, plain, natural. Use the form of address the language uses for the player (informal in pt-BR: "você").
- Sentence case, like the English UI.
- Never translate: GT4Recomp, GSdx, CoreGS 2.0, Spec II, Gran Turismo 4, Windows, WebView2, Vulkan, PCSX2, Ko-fi,
  the controller and key names that the game uses as they are (L2, R2, START, SELECT).
- "Made with passion, by Moesuito" is not a locale key: it stays English in every language.
- Keep the text about the same length as the English, so buttons, tabs and the 64 px version switch still fit. The
  focus checks and the screenshots show overflow.

## Checks

From the `launcher/crates/gt4l-tauri/ui` folder, serve it over HTTP (the page loads its locale files from its own
origin) and run these in headless Edge:

```
python -m http.server 8765
msedge --headless=new --disable-gpu --virtual-time-budget=120000 --dump-dom "http://127.0.0.1:8765/dev/focus-check.html?lang=<tag>"
msedge --headless=new --disable-gpu --virtual-time-budget=30000 --dump-dom "http://127.0.0.1:8765/dev/nav-test.html"
```

- `focus-check.html?lang=<tag>` prints `N of N passed`. The locale block fails when a file lacks a key that en-US
  has, has keys that en-US lacks, changes a `{placeholder}`, has an empty text, or when `index.json` lists a tag with
  no file.
- Rust: `cargo test -p gt4l-core` checks that every `rust.` key is in every language and keeps the placeholders.

Look at the Home, Settings > General and Settings > Graphics screenshots in your language for clipped text.
