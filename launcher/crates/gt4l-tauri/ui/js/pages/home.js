/*
 * Home: cover art; the logo, Play and the version switch centred in one column; the Settings gear at the top left
 * (owner, 2026-10-23). While the game runs, Play
 * becomes "Running · Stop". Polls app_state for game_running.
 * Commands: app_state, play, stop_game, select_game (js/games.js), setup_choose_disc and setup_start (Install).
 * Two games (TASK-101): under Play the version's name, Normal (Gran Turismo 4) or Spec II, between two arrows; an
 * arrow switches to the next version in an endless loop and the name slides in from that side (owner, 2026-10-23). An
 * installed game shows Play; a game that is not installed shows Install, which opens the disc picker and runs that
 * game's setup. The logo stays the Gran Turismo 4 logo for both.
 * Patch notes (TASK-099): a glass card on the right shows the release notes embedded in the launcher (release_notes:
 * every release up to this version, newest first), rendered by js/markdown.js; arrows in its header step back through
 * the older releases.
 * Support (owner, 2026-10-23): "Support the project" with the Ko-fi cup at the foot of the What's new card opens Ko-fi; the
 * version line at
 * the bottom right reads "GT4Recomp <version> - Made with passion, by Moesuito", Moesuito opening the YouTube channel
 * (open_link with a name; the addresses are fixed in the launcher).
 * Controller: A or Start = Play (or Install), Y = Settings, LB / RB = the other game. Hints show only once a pad is used.
 * Play is glass when idle (no focus): it takes the solid white state only when focused or hovered.
 */
(function (root) {
  "use strict";
  var GT4 = (root.GT4 = root.GT4 || {});
  var h = GT4.ui.h;
  var POLL_MS = 1000;

  function call(command, args) {
    return GT4.backend.call(command, args);
  }

  var GEAR_PATH = "M19.4 15a1.7 1.7 0 0 0 .3 1.8l.1.1a2 2 0 1 1-2.8 2.8l-.1-.1a1.7 1.7 0 0 0-1.8-.3 1.7 1.7 0 0 0-1 1.5V21a2 2 0 1 1-4 0v-.1a1.7 1.7 0 0 0-1.1-1.5 1.7 1.7 0 0 0-1.8.3l-.1.1a2 2 0 1 1-2.8-2.8l.1-.1a1.7 1.7 0 0 0 .3-1.8 1.7 1.7 0 0 0-1.5-1H3a2 2 0 1 1 0-4h.1a1.7 1.7 0 0 0 1.5-1.1 1.7 1.7 0 0 0-.3-1.8l-.1-.1a2 2 0 1 1 2.8-2.8l.1.1a1.7 1.7 0 0 0 1.8.3H9a1.7 1.7 0 0 0 1-1.5V3a2 2 0 1 1 4 0v.1a1.7 1.7 0 0 0 1 1.5 1.7 1.7 0 0 0 1.8-.3l.1-.1a2 2 0 1 1 2.8 2.8l-.1.1a1.7 1.7 0 0 0-.3 1.8V9a1.7 1.7 0 0 0 1.5 1H21a2 2 0 1 1 0 4h-.1a1.7 1.7 0 0 0-1.5 1z";

  function gearIcon() {
    var svg = document.createElementNS("http://www.w3.org/2000/svg", "svg");
    svg.setAttribute("width", "22");
    svg.setAttribute("height", "22");
    svg.setAttribute("viewBox", "0 0 24 24");
    svg.setAttribute("fill", "none");
    svg.setAttribute("stroke", "currentColor");
    svg.setAttribute("stroke-width", "1.5");
    svg.setAttribute("aria-hidden", "true");
    var circle = document.createElementNS("http://www.w3.org/2000/svg", "circle");
    circle.setAttribute("cx", "12");
    circle.setAttribute("cy", "12");
    circle.setAttribute("r", "3");
    var path = document.createElementNS("http://www.w3.org/2000/svg", "path");
    path.setAttribute("d", GEAR_PATH);
    svg.appendChild(circle);
    svg.appendChild(path);
    return svg;
  }

  // The short names of the versions (owner, 2026-10-23); the full game names stay in Settings > Games.
  // Read when used, so the name follows a language change.
  function versionNameOf(id) {
    return id === "spec2" ? GT4.t("home.spec2") : GT4.t("home.normal");
  }

  // A small chevron pointing left (-1) or right (+1).
  function chevronIcon(direction) {
    var svg = document.createElementNS("http://www.w3.org/2000/svg", "svg");
    svg.setAttribute("width", "16");
    svg.setAttribute("height", "16");
    svg.setAttribute("viewBox", "0 0 24 24");
    svg.setAttribute("fill", "none");
    svg.setAttribute("stroke", "currentColor");
    svg.setAttribute("stroke-width", "1.8");
    svg.setAttribute("aria-hidden", "true");
    var path = document.createElementNS("http://www.w3.org/2000/svg", "path");
    path.setAttribute("d", direction < 0 ? "M15 6l-6 6 6 6" : "M9 6l6 6-6 6");
    svg.appendChild(path);
    return svg;
  }

  // Smooth wheel scrolling for a panel without a scroll bar (TASK-099, owner 2026-10-23): each wheel step moves a
  // target and the panel eases towards it every frame. Reduced motion keeps the plain, immediate scroll. The fade
  // classes show a soft edge where more text is hidden (the panel has no bar to say so).
  function smoothScroll(panel) {
    var target = panel.scrollTop;
    var frame = 0;
    var reduced = root.matchMedia && root.matchMedia("(prefers-reduced-motion: reduce)").matches;

    function updateEdges() {
      var max = panel.scrollHeight - panel.clientHeight;
      panel.classList.toggle("fade-top", panel.scrollTop > 2);
      panel.classList.toggle("fade-bottom", panel.scrollTop < max - 2);
    }

    function step() {
      var distance = target - panel.scrollTop;
      if (Math.abs(distance) < 0.5) {
        panel.scrollTop = target;
        frame = 0;
        updateEdges();
        return;
      }
      panel.scrollTop += distance * 0.18;
      updateEdges();
      frame = root.requestAnimationFrame(step);
    }

    panel.addEventListener("wheel", function (event) {
      var max = panel.scrollHeight - panel.clientHeight;
      if (max <= 0 || reduced) {
        return;
      }
      event.preventDefault();
      var lines = event.deltaMode === 1 ? 40 : event.deltaMode === 2 ? panel.clientHeight : 1;
      if (!frame) {
        target = panel.scrollTop;
      }
      target = Math.max(0, Math.min(max, target + event.deltaY * lines));
      if (!frame) {
        frame = root.requestAnimationFrame(step);
      }
    }, { passive: false });
    panel.addEventListener("scroll", function () {
      if (!frame) {
        target = panel.scrollTop;
      }
      updateEdges();
    });
    return updateEdges;
  }

  GT4.pages = GT4.pages || {};
  GT4.pages.home = {
    mount: function (root) {
      var page = h("div", { class: "stage home" });
      var playButton = h("button", { class: "play", type: "button" }, GT4.t("common.play"));
      var gameList = [];
      var selectedId = "gt4";
      var installedNow = true;
      var gameSignature = "";
      var logoImage = h("img", { class: "home-logo", src: "assets/logo-gt4-white.png", alt: "Gran Turismo 4" });
      // The version switch under Play: the version's name between two arrows (owner, 2026-10-23). On a switch the old name
      // slides out on one side while the new one slides in from the other, inside a clipped window.
      var versionWindow = h("span", { class: "version-window", "aria-live": "polite" });
      var versionName = h("span", { class: "version-name", text: versionNameOf("gt4") });
      versionWindow.appendChild(versionName);
      var versionPrev = h("button", { class: "version-arrow", type: "button", "aria-label": GT4.t("home.previous_version"), title: GT4.t("home.previous_version") },
        [chevronIcon(-1)]);
      var versionNext = h("button", { class: "version-arrow", type: "button", "aria-label": GT4.t("home.next_version"), title: GT4.t("home.next_version") },
        [chevronIcon(1)]);
      var versionPicker = h("div", { class: "version-picker", role: "group", "aria-label": GT4.t("home.version_group") },
        [versionPrev, versionWindow, versionNext]);
      var paintedId = null;
      var slideFrom = 0;
      var stopButton = h("button", { class: "btn", type: "button" }, GT4.t("home.stop"));
      var stateText = h("span", { class: "state", text: GT4.t("home.running") });
      var running = h("div", { class: "running" }, [h("span", { class: "dot" }), stateText, stopButton]);
      var timer = null;
      var isRunning = false;

      // TASK-180 test2: on Android a running game is a minimized one (its own process, paused): the Play button stays
      // and reads Resume, which brings it back. Play shows again once its process is gone.
      var resumable = GT4.platform === "android";

      function paint(nowRunning) {
        isRunning = nowRunning;
        if (resumable) {
          playButton.textContent = GT4.t(nowRunning ? "home.resume" : "common.play");
          return;
        }
        page.classList.toggle("is-running", nowRunning);
      }

      function selectedGame() {
        for (var i = 0; i < gameList.length; i++) {
          if (gameList[i].id === selectedId) {
            return gameList[i];
          }
        }
        return null;
      }

      // A switch: the shown name leaves towards the opposite side while the new name comes in from the side of the arrow
      // used (+1 = right arrow: the new name comes from the right). Without a direction the text just changes.
      function showName(text, direction) {
        if (!direction || versionName.textContent === text) {
          versionName.textContent = text;
          return;
        }
        var leaving = versionName;
        versionWindow.querySelectorAll(".version-name.is-leaving").forEach(function (old) { old.remove(); });
        versionName = h("span", { class: "version-name " + (direction > 0 ? "enter-from-right" : "enter-from-left"), text: text });
        leaving.classList.remove("enter-from-left", "enter-from-right");
        leaving.classList.add("is-leaving", direction > 0 ? "leave-to-left" : "leave-to-right");
        versionWindow.appendChild(versionName);
        var done = function () { leaving.remove(); };
        leaving.addEventListener("animationend", done, { once: true });
        setTimeout(done, 600);  // reduced motion: no animationend
      }

      // The version switch and the main button follow the game list (rebuilt only when it changed).
      function paintGames() {
        var signature = JSON.stringify([selectedId, isRunning, gameList.map(function (game) { return [game.id, game.installed]; })]);
        if (signature === gameSignature) {
          return;
        }
        gameSignature = signature;
        var game = selectedGame();
        installedNow = !game || game.installed;
        page.classList.toggle("is-spec2", selectedId === "spec2");
        var locked = isRunning || gameList.length < 2;
        versionPrev.disabled = locked;
        versionNext.disabled = locked;
        var label = selectedId === "gt4" || selectedId === "spec2" ? versionNameOf(selectedId) : (game ? game.name : "");
        showName(label, paintedId !== null && paintedId !== selectedId ? (slideFrom || 1) : 0);
        versionWindow.setAttribute("title", game ? (game.title || game.name) : "");
        paintedId = selectedId;
        slideFrom = 0;
        playButton.textContent = installedNow ? GT4.t("common.play") : GT4.t("common.install");
        playButton.classList.toggle("is-install", !installedNow);
        playButton.setAttribute("title", installedNow || !game || !game.reason ? "" : game.reason);
      }

      function applyGames(view) {
        if (view && Array.isArray(view.games)) {
          gameList = view.games;
          selectedId = view.selected || view.selected_game || selectedId;
          paintGames();
        }
      }

      function chooseGame(id) {
        if (id === selectedId || isRunning) {
          return;
        }
        GT4.games.select(id).then(applyGames, function (err) {
          GT4.ui.toast(GT4.errorText(err), true);
        });
      }

      // The arrows and LB / RB: the next version on that side, in an endless loop.
      function cycleGame(delta) {
        if (gameList.length < 2 || isRunning) {
          return;
        }
        slideFrom = delta;
        var at = 0;
        gameList.forEach(function (game, index) {
          if (game.id === selectedId) {
            at = index;
          }
        });
        chooseGame(gameList[(at + delta + gameList.length) % gameList.length].id);
      }

      // Install: the disc picker for the selected game, then its setup on the Setup page.
      function install() {
        GT4.games.installGame(selectedId, function () {
          GT4.go("#/setup");
        });
      }

      // Issues #139 and #179: GSdx is the stored renderer but is not installed (its download failed, or GitHub is blocked), and
      // this PC's card runs CoreGS 2.0. Play asks instead of stopping with an error. Resolves true when Play may go on:
      // CoreGS 2.0 is now the renderer ("Use CoreGS 2.0"), or GSdx was downloaded here ("Download GSdx", the dialog of Settings >
      // Renderer, js/pages/settings.js). Resolves false when the player closed the dialog or the switch failed (a toast says why).
      function offerCoregs20() {
        return new Promise(function (resolve) {
          GT4.gsdxDownloadDialog({
            title: GT4.t("home.gsdx_missing_title"),
            text: GT4.t("home.gsdx_missing_text"),
            downloadLabel: GT4.t("home.gsdx_missing_download"),
            alternative: {
              label: GT4.t("home.gsdx_missing_use"),
              run: function () {
                call("use_coregs20").then(function () {
                  // Settings must read the renderer again.
                  GT4.cache.table = null;
                  resolve(true);
                }, function (err) {
                  GT4.ui.toast(GT4.errorText(err), true);
                  resolve(false);
                });
              }
            },
            onInstalled: function () { resolve(true); },
            onDeclined: function () { resolve(false); }
          });
        });
      }

      var preparingShaders = false;
      // TASK-180 test2: on Android a running game is a minimized one; Play (Resume) brings it back.
      function resumeGame() {
        call("resume_game").then(null, function (err) {
          GT4.ui.toast(GT4.errorText(err), true);
          paint(false);
        });
      }

      function play() {
        if (isRunning) {
          if (resumable) {
            resumeGame();
          }
          return;
        }
        if (!installedNow) {
          install();
          return;
        }
        if (preparingShaders) {
          return;
        }
        // Held from the first question to the start, so a second press cannot open a second dialog or a second game.
        preparingShaders = true;
        call("coregs20_offer").then(function (offered) {
          return offered ? offerCoregs20() : true;
        }, function () {
          // The check itself failed: Play goes on and reports whatever it finds.
          return true;
        }).then(function (goOn) {
          if (goOn) {
            startGame();
          } else {
            preparingShaders = false;
          }
        });
      }

      // TASK-164 (issue #160): on a start that meets the game's first-start questions (no save yet), a note says which keys
      // answer them, from the current controls (first_start_hint: null, or {game, confirm, pad, right}). Resolves true to start,
      // false when the player closed the note. A failed check starts the game.
      function firstStartHint() {
        return call("first_start_hint").then(function (hint) {
          if (!hint) {
            return true;
          }
          return new Promise(function (resolve) {
            var answered = false;
            function answer(start) {
              answered = true;
              resolve(start);
            }
            var confirmKey = hint.pad ? "home.first_start_confirm_pad" : "home.first_start_confirm";
            var lines = [
              GT4.t("home.first_start_text", { game: hint.game }),
              GT4.t(confirmKey, { keys: hint.confirm, pad: hint.pad }),
              GT4.t("home.first_start_cancel_first", { right: hint.right })
            ];
            GT4.ui.openDialog({
              title: GT4.t("home.first_start_title"),
              body: h("div", { class: "first-start-hint" }, lines.map(function (line) {
                return h("p", { class: "modal-text", text: line });
              })),
              buttons: [
                { label: GT4.t("common.play"), primary: true, run: function () { answer(true); } },
                { label: GT4.t("common.cancel"), run: function () { answer(false); } }
              ],
              onClose: function () {
                if (!answered) {
                  resolve(false);
                }
              }
            });
          });
        }, function () {
          return true;
        });
      }

      function startAfterHint() {
        return firstStartHint().then(function (start) {
          preparingShaders = false;
          if (!start) {
            paint(false);
            return null;
          }
          return call("play");
        });
      }

      function startGame() {
        paint(true);
        // IN-507: asks whether the shaders are compiled; the window opens by itself when they are not (js/app.js).
        call("prepare_shaders").then(function (goOn) {
          GT4.closeShaderWindow();
          if (!goOn) {
            preparingShaders = false;
            paint(false);
            return null;
          }
          return startAfterHint();
        }, function () {
          GT4.closeShaderWindow();
          return startAfterHint();
        }).then(function () {
          // Follow the next poll for the real state.
        }, function (err) {
          paint(false);
          GT4.ui.toast(GT4.errorText(err), true);
          // Game files missing (the data folder was cleaned): the button becomes Install for that game.
          call("app_state").then(function (appState) {
            applyGames(appState);
            if (!appState.any_installed && appState.setup && appState.setup.state !== "done") {
              GT4.go("#/setup");
            }
          }, function () {});
        });
      }

      function stop() {
        call("stop_game").then(function () {
          paint(false);
        }, function (err) {
          GT4.ui.toast(GT4.errorText(err), true);
        });
      }

      playButton.addEventListener("click", play);
      stopButton.addEventListener("click", stop);
      versionPrev.addEventListener("click", function () { cycleGame(-1); });
      versionNext.addEventListener("click", function () { cycleGame(1); });

      // Settings: the plain gear at the top left (owner, 2026-10-23), no box until hovered or focused.
      var gear = h("button", {
        class: "gear",
        type: "button",
        "aria-label": GT4.t("home.settings"),
        title: GT4.t("home.settings"),
        on: { click: function () { GT4.go("#/settings/graphics"); } }
      }, [gearIcon()]);

      // Slideshow: one img per slot (bg-slide-1..6 crossfade in CSS). The slots come from GT4.config.
      // At most six slots: css/app.css defines bg-slide-1..6.
      var backgrounds = ((GT4.config && GT4.config.backgrounds) || []).slice(0, 6);
      var slides = backgrounds.map(function (src, index) {
        return h("img", { class: "bg-slide bg-slide-" + (index + 1), src: src, alt: "" });
      });
      page.appendChild(h("div", { class: "art" }, slides.concat([h("div", { class: "shade" })])));
      function openLink(name) {
        call("open_link", { name: name }).catch(function (err) { GT4.ui.toast(GT4.errorText(err), true); });
      }
      var versionText = h("span", { text: "GT4Recomp" });
      var versionLabel = h("div", { class: "home-version" }, [
        versionText,
        h("span", { class: "home-version-credit", text: " - Made with passion, by " }),
        h("button", { class: "home-author", type: "button", title: GT4.t("home.youtube_title"),
          on: { click: function () { openLink("youtube"); } } }, "Moesuito")
      ]);
      page.appendChild(versionLabel);
      var supportButton = h("button", { class: "support", type: "button", title: GT4.t("home.support_title"),
        on: { click: function () { openLink("kofi"); } } }, [
        h("img", { class: "support-cup", src: "assets/kofi-cup.avif", alt: "" }),
        h("span", { text: GT4.t("home.support") })
      ]);
      call("about_info").then(function (info) {
        versionText.textContent = "GT4Recomp " + (info.version || "");
        // The 1.3.0 intro, once per version (js/intro.js). The dev mock plays it only with ?intro=1, so the focus checks
        // are not covered by it.
        if (GT4.intro) {
          if (GT4.devIntro) {
            GT4.intro.play();
          } else if (!GT4.devMock) {
            GT4.intro.maybePlay(info.version || "");
          }
        }
      }, function () {});
      // The patch notes card (TASK-099). It scrolls on its own; wide windows only (css/app.css hides it when narrow).
      // Its header arrows step through the release history (owner, 2026-10-23): it opens on the newest notes, the left
      // arrow goes to older releases, the right arrow back to newer ones; no loop, an arrow is disabled at its end.
      var notesBody = h("div", { class: "home-notes-body", tabindex: "0", "aria-label": GT4.t("home.release_notes_label") });
      var notesVersion = h("span", { class: "home-notes-version", text: "" });
      var notesOlder = h("button", { class: "notes-arrow", type: "button", "aria-label": GT4.t("home.older_label"), title: GT4.t("home.older") },
        [chevronIcon(-1)]);
      var notesNewer = h("button", { class: "notes-arrow", type: "button", "aria-label": GT4.t("home.newer_label"), title: GT4.t("home.newer") },
        [chevronIcon(1)]);
      var notesCard = h("aside", { class: "home-notes", "aria-label": GT4.t("home.whats_new") }, [
        h("header", { class: "home-notes-head" }, [
          h("span", { class: "home-notes-title", text: GT4.t("home.whats_new") }),
          h("span", { class: "home-notes-pager" }, [notesOlder, notesVersion, notesNewer])
        ]),
        notesBody,
        supportButton
      ]);
      notesCard.hidden = true;
      page.appendChild(notesCard);
      var updateNotesEdges = smoothScroll(notesBody);
      window.addEventListener("resize", updateNotesEdges);
      var releases = [];  // newest first
      var shownRelease = 0;

      // The notes in the current language, with the en-US text as the fallback (TASK-102).
      function releaseText(release) {
        var tag = GT4.i18n.current();
        return (release.translations && release.translations[tag]) || release.markdown;
      }

      // Shows releases[index]; direction -1 = an older release (it comes in from the left), +1 = a newer one.
      function showRelease(index, direction) {
        if (index < 0 || index >= releases.length) {
          return;
        }
        shownRelease = index;
        var release = releases[index];
        notesVersion.textContent = release.version;
        notesOlder.disabled = index >= releases.length - 1;
        notesNewer.disabled = index === 0;
        var content = h("div", { class: "home-notes-page" + (direction < 0 ? " enter-from-left" : direction > 0 ? " enter-from-right" : "") });
        content.appendChild(GT4.markdown.render(releaseText(release), { skipTitle: true }));
        GT4.ui.clear(notesBody);
        notesBody.appendChild(content);
        notesBody.scrollTop = 0;
        window.requestAnimationFrame(updateNotesEdges);
      }

      notesOlder.addEventListener("click", function () { showRelease(shownRelease + 1, -1); });
      notesNewer.addEventListener("click", function () { showRelease(shownRelease - 1, 1); });
      call("release_notes").then(function (notes) {
        releases = (notes && Array.isArray(notes.releases) ? notes.releases : []).filter(function (release) {
          return release && release.markdown;
        });
        if (!releases.length) {
          return;
        }
        showRelease(0, 0);
        notesCard.hidden = false;
        window.requestAnimationFrame(updateNotesEdges);
      }, function () {});
      page.appendChild(h("div", { class: "home-bottom" }, [
        logoImage,
        h("div", { class: "home-actions" }, [playButton, running]),
        versionPicker
      ]));
      page.appendChild(gear);
      root.appendChild(page);
      document.body.classList.add("is-home");

      function poll() {
        focusPlay();
        call("app_state").then(function (appState) {
          var was = isRunning;
          paint(!!appState.game_running);
          if (was !== isRunning) {
            gameSignature = "";
            focusPlay(true);  // TASK-180: the button changed between Play and Resume
          }
          applyGames(appState);
        }, function () {});
      }
      poll();
      timer = setInterval(poll, POLL_MS);

      // Dev harness only (dev/mock-data.js sets GT4.devFocus from ?focus=play): focus Play for screenshots.
      if (GT4.devFocus === "play") {
        playButton.focus({ preventScroll: true });
      }
      // TASK-180 test2 (owner): the home page starts on Play (Resume while the game is minimized), also when the
      // launcher comes back from the game: one A press starts or resumes it. TASK-180 test3 (owner): the same on
      // Windows, so the pad and Enter reach Play first there too.
      function focusPlay(always) {
        if (!playButton.isConnected || GT4.ui.topDialog()) {
          return;
        }
        if (always === true || document.activeElement === null || document.activeElement === document.body ||
            !page.contains(document.activeElement)) {
          playButton.focus({ preventScroll: true });
        }
      }
      // After the page is in place (the router and nav.js settle the focus first).
      setTimeout(function () { focusPlay(true); }, 0);
      // TASK-180 test3: when the launcher comes back over a minimized game, the WebView restores the element it last had
      // (the footer link) a moment after the activity resumes, so the first press landed there instead of on Resume.
      // One more attempt after that settles puts Play/Resume back on top.
      function focusPlaySettled() {
        setTimeout(function () { focusPlay(true); }, 250);
      }
      // Back from the game (or another app): Play/Resume again, whatever had the focus before.
      function onVisible() {
        if (document.visibilityState === "visible") {
          movedThisVisit = false;
          focusPlay(true);
          focusPlaySettled();
        }
      }
      document.addEventListener("visibilitychange", onVisible);
      // Android gives the WebView the input focus when the launcher comes back, and the WebView then focuses the element at
      // the edge it was entered from (the footer link): Play/Resume takes it back right after.
      function onWindowFocus() {
        setTimeout(function () { focusPlay(true); }, 0);
        focusPlaySettled();
      }
      window.addEventListener("focus", onWindowFocus);
      // The first key the WebView gets after the launcher comes back may also be the one that gives it the input focus,
      // landing on the element at the edge it was entered from. Until the player moves on this visit, A / Enter is Play
      // (or Resume) on Android, whatever holds the focus.
      var movedThisVisit = false;
      function onKeyFirst(event) {
        if (GT4.platform !== "android" || GT4.ui.topDialog()) {
          return;
        }
        if (/^Arrow/.test(event.key) || event.key === "Tab") {
          movedThisVisit = true;
          return;
        }
        if (!movedThisVisit && (event.key === "Enter" || event.key === " ") && document.activeElement !== playButton) {
          event.preventDefault();
          event.stopPropagation();
          playButton.focus({ preventScroll: true });
          play();
        }
      }
      document.addEventListener("keydown", onKeyFirst, true);

      GT4.hooks.start = play;
      GT4.hooks.y = function () { GT4.go("#/settings/graphics"); };
      GT4.hooks.tabs = cycleGame;
      GT4.ui.setHints([
        { key: "A", label: GT4.t("hint.select"), run: play },
        { key: "LB / RB", label: GT4.t("home.hint_version"), run: function () { cycleGame(1); } },
        { key: "Y", label: GT4.t("home.settings"), run: function () { GT4.go("#/settings/graphics"); } }
      ]);

      return {
        destroy: function () {
          document.removeEventListener("visibilitychange", onVisible);
          window.removeEventListener("focus", onWindowFocus);
          document.removeEventListener("keydown", onKeyFirst, true);
          clearInterval(timer);
          window.removeEventListener("resize", updateNotesEdges);
          document.body.classList.remove("is-home");
        }
      };
    }
  };
})(window);
