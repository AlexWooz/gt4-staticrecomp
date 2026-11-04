/*
 * The 1.3.0 intro (owner, 2026-10-23): about 8 s in three scenes, the first time the launcher opens on this version, to
 * say that GT4Recomp now runs Gran Turismo 4 Spec II: "You asked for it." (light streaks), "We made it." (an impact),
 * then the GT4 logo with "spec-II" (lower case in a wide geometric face, as on the community Spec II cover: Michroma,
 * SIL OFL, assets/Michroma-Regular.ttf), "Natively recompiled" and the version, then the launcher. Any key, click or pad button skips it. Sound and music are made live with the
 * Web Audio API (no audio files, nothing third-party). Reduced motion keeps a plain fade and no sound.
 * Shown once per version: the version is remembered in localStorage (`gt4recomp.intro.seen`). The animation is pure
 * CSS on classes (no inline styles, CSP); the timing is in css/app.css ("intro" section).
 * GT4.intro.maybePlay(version) on the home page; GT4.intro.play() replays it (dev mock: ?intro=1).
 */
(function (root) {
  "use strict";
  var GT4 = (root.GT4 = root.GT4 || {});
  var h = GT4.ui.h;
  var SEEN_KEY = "gt4recomp.intro.seen";
  // The versions that have an intro. Later releases add theirs here.
  var INTRO_VERSIONS = { "1.3.0": true };
  var DURATION_MS = 8200;
  // Whatever happens (a throttled timer in a hidden web view, a missed event), the overlay is removed by this bound.
  var HARD_LIMIT_MS = 12000;

  // A line in the launcher log (js -> Rust `ui_log`); never an error for the page.
  function logLine(message) {
    try {
      if (GT4.backend && GT4.backend.call) {
        GT4.backend.call("ui_log", { message: "intro " + message }).catch(function () {});
      }
    } catch (error) {
      // the log is a help only
    }
  }

  // "1.3.0-rc5" -> "1.3.0": a release candidate plays the intro of the version it leads to.
  function baseVersion(version) {
    return String(version || "").replace(/[-+].*$/, "");
  }

  function reducedMotion() {
    return !!(root.matchMedia && root.matchMedia("(prefers-reduced-motion: reduce)").matches);
  }

  // ---------- the overlay ----------
  var active = null;

  function finish() {
    if (!active) {
      return;
    }
    var state = active;
    active = null;
    clearTimeout(state.timer);
    clearTimeout(state.hardTimer);
    document.removeEventListener("keydown", state.onKey, true);
    state.overlay.classList.add("is-leaving");
    logLine("ended after " + (Date.now() - state.started) + " ms");
    setTimeout(function () { state.overlay.remove(); }, 650);
    // If that timer is throttled, the overlay still stops covering the page at once.
    state.overlay.style.pointerEvents = "none";
  }

  function play() {
    if (active || !document.body) {
      return;
    }
    var reduced = reducedMotion();
    var streaks = [];
    for (var i = 1; i <= 7; i++) {
      streaks.push(h("span", { class: "intro-streak intro-streak-" + i }));
    }
    var letters = "spec-II".split("").map(function (letter, index) {
      return h("span", { class: "intro-letter intro-letter-" + (index + 1), text: letter });
    });
    var overlay = h("div", { class: "intro" + (reduced ? " is-reduced" : ""), role: "dialog", "aria-label": GT4.t("intro.label") }, [
      h("div", { class: "intro-streaks", "aria-hidden": "true" }, streaks),
      h("div", { class: "intro-scene intro-scene-1", text: GT4.t("intro.asked") }),
      h("div", { class: "intro-scene intro-scene-2", text: GT4.t("intro.made") }),
      h("div", { class: "intro-stage" }, [
        h("img", { class: "intro-logo", src: "assets/logo-gt4-white.png", alt: "Gran Turismo 4" }),
        h("div", { class: "intro-spec" }, letters),
        h("div", { class: "intro-bar", "aria-hidden": "true" }),
        h("div", { class: "intro-line", text: GT4.t("intro.natively") }),
        h("div", { class: "intro-sub", text: GT4.t("intro.now", { version: "1.3.0" }) })
      ]),
      h("div", { class: "intro-skip", text: GT4.t("intro.skip") })
    ]);
    overlay.addEventListener("click", finish);
    var onKey = function (event) {
      event.preventDefault();
      event.stopImmediatePropagation();
      finish();
    };
    document.addEventListener("keydown", onKey, true);
    document.body.appendChild(overlay);
    active = { overlay: overlay, onKey: onKey, timer: 0, hardTimer: 0, started: Date.now() };
    active.timer = setTimeout(finish, reduced ? 4200 : DURATION_MS);
    active.hardTimer = setTimeout(function () {
      if (active && active.overlay === overlay) {
        logLine("hard limit reached; removing the overlay");
        finish();
        overlay.remove();
      }
    }, HARD_LIMIT_MS);
    logLine("started" + (reduced ? " (reduced motion)" : ""));
  }

  function seenVersion() {
    try {
      return root.localStorage.getItem(SEEN_KEY) || "";
    } catch (error) {
      return "";
    }
  }

  function markSeen(version) {
    try {
      root.localStorage.setItem(SEEN_KEY, version);
    } catch (error) {
      // private storage: the intro may show again, which is harmless
    }
  }

  // Plays the intro once for a version that has one. The release candidates of a version (1.3.0-rc5) play that version's
  // intro, and the seen marker is per base version, so 1.3.0 plays it once whatever rc the player was on.
  // It never starts while the page is hidden (timers and animations are throttled there): it waits for visibilitychange.
  // It is not shown on the first-run setup page: the caller passes `onHome` only from Home.
  function maybePlay(version) {
    var base = baseVersion(version);
    if (!INTRO_VERSIONS[base]) {
      return false;
    }
    if (seenVersion() === base) {
      logLine("skipped for " + version + " (already seen)");
      return false;
    }
    if (document.hidden) {
      logLine("waiting: the page is hidden");
      var onVisible = function () {
        if (document.hidden) {
          return;
        }
        document.removeEventListener("visibilitychange", onVisible);
        if (/^#\/home/.test(root.location.hash || "#/home") && seenVersion() !== base) {
          markSeen(base);
          play();
        }
      };
      document.addEventListener("visibilitychange", onVisible);
      return false;
    }
    markSeen(base);
    play();
    return true;
  }

  // A pad button skips the intro too (pad-nav comes from the SDL3 service in Rust).
  if (GT4.backend && GT4.backend.listen) {
    GT4.backend.listen("pad-nav", function () {
      if (active) {
        finish();
      }
    });
  }

  GT4.intro = { maybePlay: maybePlay, baseVersion: baseVersion, play: play, skip: finish, isPlaying: function () { return !!active; } };
})(window);
