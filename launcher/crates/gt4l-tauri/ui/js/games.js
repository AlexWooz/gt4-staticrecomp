/*
 * Games (TASK-101, release 1.3.0): the launcher runs Gran Turismo 4 and Gran Turismo 4 Spec II side by side.
 * Shared page helpers over the game commands:
 *   games_list           -> {selected, any_installed, games: [{id, name, installed, tested, damaged, reason, selected,
 *                            disc_name, folder, size_bytes, image_path, image_bytes}]}
 *   select_game({id})    -> the same list; the page's choice, not stored until a game starts
 *   games_remove({id})   -> the same list; deletes the disc copy and prepared files only (the page asks first)
 *   setup_choose_disc({game}) -> {path, serial, version, name, game, gameName, mismatch, requested} | null
 *   setup_start
 * The disc decides which game is installed. When the page asked for one game and the disc is the other, `mismatch` is set
 * and the page offers to install the disc as the game it is, or to pick another file. Nothing is installed before that.
 */
(function (root) {
  "use strict";
  var GT4 = (root.GT4 = root.GT4 || {});

  function call(command, args) {
    return GT4.backend.call(command, args);
  }

  function list() {
    return call("games_list", {});
  }

  function byId(view, id) {
    var games = (view && view.games) || [];
    for (var i = 0; i < games.length; i++) {
      if (games[i].id === id) {
        return games[i];
      }
    }
    return null;
  }

  // The game that is not `id` (there are two games today).
  function otherOf(view, id) {
    var games = (view && view.games) || [];
    for (var i = 0; i < games.length; i++) {
      if (games[i].id !== id) {
        return games[i];
      }
    }
    return null;
  }

  // Asks which game a file belongs to (an import: a save file does not say, and the games never share saves).
  // options = {title, text, current: id of the game to offer first}; run(id) gets the answer.
  function chooseGame(options, run) {
    list().then(function (view) {
      var games = view.games.slice().sort(function (a, b) {
        return (a.id === options.current ? -1 : 0) - (b.id === options.current ? -1 : 0);
      });
      GT4.ui.openDialog({
        title: options.title,
        text: options.text,
        buttons: [{ label: GT4.t("common.cancel") }].concat(games.map(function (game, index) {
          return { label: game.name, primary: index === 0, run: function () { run(game.id); } };
        }))
      });
    }, function (err) {
      GT4.ui.toast(GT4.errorText(err), true);
    });
  }

  // The disc is not the game that was asked for: install it as the game it is, pick another file, or stop.
  function offerOtherGame(info, install, pickAgain) {
    GT4.ui.openDialog({
      title: GT4.t("games.other_title", { game: info.gameName }),
      text: GT4.t("games.other_text", { game: info.gameName }),
      buttons: [
        { label: GT4.t("common.cancel") },
        { label: GT4.t("common.choose_another_file"), run: pickAgain },
        { label: GT4.t("games.install_as", { game: info.gameName }), primary: true, run: install }
      ]
    });
  }

  // Picks a disc image for `gameId` (or for the game the disc turns out to be when gameId is null), checks it, and
  // starts the setup. `started()` runs once setup_start was accepted (the callers open the setup page).
  function installGame(gameId, started) {
    var args = gameId ? { game: gameId } : {};
    return call("setup_choose_disc", args).then(function (info) {
      if (!info) {
        return null;
      }
      function go() {
        return call("setup_start", {}).then(function () {
          if (started) {
            started(info);
          }
        }, function (err) {
          GT4.ui.toast(GT4.errorText(err), true);
        });
      }
      if (info.mismatch) {
        offerOtherGame(info, go, function () { installGame(gameId, started); });
        return info;
      }
      return go().then(function () { return info; });
    }, function (err) {
      // A refused file: the reason is the error; app_state says wrong_disc too.
      GT4.ui.toast(GT4.errorText(err), true);
      return null;
    });
  }

  function size(bytes) {
    if (!bytes) {
      return "";
    }
    // What removing a linked game frees is a few megabytes: it must not read as 0.0 GB.
    if (bytes < 1e9) {
      return GT4.t("common.mb", { size: Math.max(1, Math.round(bytes / 1e6)).toLocaleString(GT4.i18n.intlLocale()) });
    }
    return GT4.t("common.gb", { size: (bytes / 1e9).toLocaleString(GT4.i18n.intlLocale(), { minimumFractionDigits: 1, maximumFractionDigits: 1 }) });
  }

  // TASK-173: the badge of an installed game's disc image: "Tested" for a known image (its hash is pinned), "Not tested" for
  // an image that only has the supported game code (a Spec II 1.10 image we have not seen). The help text is the tooltip.
  function testedBadge(tested) {
    return GT4.ui.h("span", {
      class: "tested-badge " + (tested ? "is-tested" : "is-untested"),
      title: GT4.t(tested ? "games.tested_help" : "games.not_tested_help"),
      text: GT4.t(tested ? "games.tested" : "games.not_tested")
    });
  }

  GT4.games = {
    list: list,
    testedBadge: testedBadge,
    byId: byId,
    otherOf: otherOf,
    chooseGame: chooseGame,
    offerOtherGame: offerOtherGame,
    installGame: installGame,
    size: size,
    select: function (id) { return call("select_game", { id: id }); },
    remove: function (id) { return call("games_remove", { id: id }); }
  };
})(window);
