/*
 * Translations (TASK-102). The languages are data, not code:
 *   ui/locales/index.json      the list of languages: [{tag, name}], the first entry names the language
 *   ui/locales/<tag>.json      one flat object per language: {"home.play": "Play", "home.version": "{version}"}
 * en-US is the source of truth and the fallback for any missing key. A new language is a new <tag>.json plus an entry
 * in index.json (docs/TRANSLATING.md); nothing here names a language.
 *
 * GT4.t(key, params)           the text of `key` in the current language, with {name} placeholders filled from params
 * GT4.i18n.setSetting(value)   the Settings value ("auto" or a tag): picks the language and tells the listeners
 * GT4.i18n.onChange(fn)        fn() runs when the language changes (the app re-draws the current page)
 * GT4.i18n.ready               a promise: the locale files are loaded (app.js starts after it)
 *
 * Auto (the same rule as gt4l_core::language in Rust): the first OS language, an exact tag match first, then the
 * base language (pt-PT or pt -> pt-BR), then en-US. Locales are fetched from the page's own origin (CSP 'self').
 */
(function (root) {
  "use strict";
  var GT4 = (root.GT4 = root.GT4 || {});
  var FALLBACK = "en-US";
  var LOCALE_DIR = "locales/";

  var index = [];          // [{tag, name}] from index.json, in its order
  var dictionaries = {};   // tag -> {key: text}
  var setting = "auto";    // the stored Settings value
  var current = FALLBACK;  // the tag in use now
  var listeners = [];
  var warned = {};

  function normalizeTag(tag) {
    return String(tag || "").trim().replace(/_/g, "-").toLowerCase();
  }

  function baseOf(tag) {
    return normalizeTag(tag).split("-")[0];
  }

  // The index entry whose tag is `tag` (case and _ / - ignored), or null.
  function entryFor(tag) {
    var wanted = normalizeTag(tag);
    for (var i = 0; i < index.length; i++) {
      if (normalizeTag(index[i].tag) === wanted) {
        return index[i];
      }
    }
    return null;
  }

  // The OS language, as the browser reports it: the first entry of navigator.languages (the Windows UI language).
  // GT4.devOsLanguage overrides it in the dev mock (?os=...).
  function osLanguage() {
    if (GT4.devOsLanguage) {
      return GT4.devOsLanguage;
    }
    var list = root.navigator && root.navigator.languages;
    if (list && list.length) {
      return list[0];
    }
    return (root.navigator && root.navigator.language) || "";
  }

  // The language for an OS tag: exact, then the base language, then the fallback.
  function resolveOs(osTag) {
    var exact = entryFor(osTag);
    if (exact) {
      return exact.tag;
    }
    var base = baseOf(osTag);
    for (var i = 0; i < index.length; i++) {
      if (baseOf(index[i].tag) === base && base) {
        return index[i].tag;
      }
    }
    return FALLBACK;
  }

  // Pure rule (tested): the tag a Settings value gives. A tag in the index is used as it is; anything else follows the OS.
  function resolve(settingValue, osTag) {
    var chosen = entryFor(settingValue);
    if (settingValue !== "auto" && chosen) {
      return chosen.tag;
    }
    return resolveOs(osTag);
  }

  function lookup(tag, key) {
    var dictionary = dictionaries[tag];
    return dictionary && Object.prototype.hasOwnProperty.call(dictionary, key) ? dictionary[key] : undefined;
  }

  function fill(text, params) {
    if (!params) {
      return text;
    }
    return String(text).replace(/\{([A-Za-z0-9_]+)\}/g, function (whole, name) {
      return Object.prototype.hasOwnProperty.call(params, name) ? String(params[name]) : whole;
    });
  }

  function warnOnce(message) {
    if (!GT4.devMock || warned[message]) {
      return;
    }
    warned[message] = true;
    if (root.console) {
      root.console.warn("[i18n] " + message);
    }
  }

  function t(key, params) {
    var text = lookup(current, key);
    if (text === undefined) {
      warnOnce("missing key in " + current + ": " + key);
      text = lookup(FALLBACK, key);
    }
    if (text === undefined) {
      warnOnce("missing key in " + FALLBACK + ": " + key);
      return key;
    }
    return fill(text, params);
  }

  var listenersCallbacks = listeners;

  function apply(next) {
    if (next === current) {
      return false;
    }
    current = next;
    if (root.document && root.document.documentElement) {
      root.document.documentElement.setAttribute("lang", current);
    }
    listenersCallbacks.slice().forEach(function (fn) {
      fn(current);
    });
    return true;
  }

  // The Settings value ("auto" or a tag). Returns true when the language changed.
  function setSetting(value) {
    setting = value || "auto";
    return apply(resolve(setting, osLanguage()));
  }

  function fetchJson(name) {
    return root.fetch(LOCALE_DIR + name, { cache: "no-cache" }).then(function (response) {
      if (!response.ok) {
        throw new Error(name + " is " + response.status);
      }
      return response.json();
    });
  }

  // Loads index.json and every locale it lists. A listed tag without its file is dropped (with a warning), so the
  // Settings list and Auto never offer a language that has no text.
  function load() {
    return fetchJson("index.json").then(function (list) {
      index = (Array.isArray(list) ? list : []).filter(function (entry) {
        return entry && typeof entry.tag === "string" && typeof entry.name === "string";
      });
      return Promise.all(index.map(function (entry) {
        return fetchJson(entry.tag + ".json").then(function (dictionary) {
          dictionaries[entry.tag] = dictionary || {};
          return true;
        }, function (error) {
          warnOnce("locale file missing for " + entry.tag + ": " + error.message);
          return false;
        });
      }));
    }).then(function (loaded) {
      index = index.filter(function (entry, position) {
        return loaded[position];
      });
      if (!entryFor(FALLBACK)) {
        throw new Error("the fallback language " + FALLBACK + " is not in index.json");
      }
      current = FALLBACK;
      return true;
    });
  }

  GT4.t = t;
  GT4.i18n = {
    t: t,
    ready: load(),
    load: load,
    setSetting: setSetting,
    setting: function () { return setting; },
    current: function () { return current; },
    // The tag Auto would pick now (the Settings row shows it as "Auto (<name>)").
    detected: function () { return resolve("auto", osLanguage()); },
    name: function (tag) {
      var entry = entryFor(tag);
      return entry ? entry.name : tag;
    },
    // True when the current language has `key` (a missing one lets the backend text show).
    has: function (key) { return lookup(current, key) !== undefined; },
    languages: function () { return index.slice(); },
    onChange: function (fn) { listeners.push(fn); },
    // The Intl locale for dates and numbers in the current language.
    intlLocale: function () { return current === "en-US" ? "en-GB" : current; },
    // Pure rules and the dictionaries, for the tests (ui/dev/focus-check.js, ui/dev/nav-test.js).
    resolve: resolve,
    dictionaries: function () { return dictionaries; },
    index: function () { return index; },
    fill: fill
  };
})(window);
