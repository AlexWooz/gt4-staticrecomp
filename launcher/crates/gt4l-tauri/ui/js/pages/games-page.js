/*
 * Settings > Games (TASK-101): the two games, installed or not, the disc image each reads, its size and where it is.
 * Commands (js/games.js): games_list, setup_choose_disc + setup_start (Install, Update), games_remove.
 *   - Install: opens the disc picker for that game and runs its setup (the Setup page shows the progress).
 *   - Update: the same with a new disc image; it is checked and replaces the old link only when it passes.
 *   - Remove: deletes the prepared files of that game (and the launcher's own copy of the image, in an older install), after
 *     asking. An image the player keeps elsewhere is never touched, and its saves and replays are kept.
 */
(function (root) {
  "use strict";
  var GT4 = (root.GT4 = root.GT4 || {});
  var h = GT4.ui.h;

  GT4.tabPages = GT4.tabPages || {};
  GT4.tabPages.games = function (content, host) {
    var list = h("div", { class: "games-list" });
    var firstButton = null;

    function started() {
      GT4.go("#/setup");
    }

    function confirmRemove(game) {
      var size = GT4.games.size(game.size_bytes);
      GT4.ui.openDialog({
        title: GT4.t("games.remove_title", { game: game.name }),
        text: GT4.t(game.linked ? "games.remove_text_linked" : "games.remove_text", { size: size ? " (" + size + ")" : "" }),
        buttons: [
          { label: GT4.t("common.cancel") },
          {
            label: GT4.t("common.remove"),
            primary: true,
            danger: true,
            run: function () {
              GT4.games.remove(game.id).then(function (view) {
                GT4.ui.toast(GT4.t("games.removed", { game: game.name }));
                paint(view);
              }, function (err) {
                GT4.ui.toast(GT4.errorText(err), true);
              });
            }
          }
        ]
      });
    }

    function card(game) {
      // The detail: the status, what tells this image apart (region and version, or the Spec II image) and its size.
      var status = game.installed
        ? [GT4.t("games.installed"), game.variant || "", game.image_bytes ? GT4.games.size(game.image_bytes) : ""]
          .filter(Boolean).join(" · ")
        : (game.damaged ? GT4.t("games.missing_reason", { reason: game.reason }) : GT4.t("games.not_installed"));
      var buttons = [];
      if (game.installed) {
        buttons.push(h("button", {
          class: "btn small", type: "button",
          on: { click: function () { GT4.games.installGame(game.id, started); } }
        }, GT4.t("games.update")));
        buttons.push(h("button", {
          class: "btn small danger", type: "button",
          on: { click: function () { confirmRemove(game); } }
        }, GT4.t("common.remove")));
      } else {
        buttons.push(h("button", {
          class: "btn small primary", type: "button",
          on: { click: function () { GT4.games.installGame(game.id, started); } }
        }, game.damaged ? GT4.t("games.choose_disc") : GT4.t("common.install")));
        if (game.damaged) {
          buttons.push(h("button", {
            class: "btn small danger", type: "button",
            on: { click: function () { confirmRemove(game); } }
          }, GT4.t("common.remove")));
        }
      }
      return h("div", { class: "row game-row", data: { gameId: game.id } }, [
        h("div", { class: "k" }, [
          h("span", { text: game.title || game.name }),
          game.installed ? GT4.games.testedBadge(game.tested) : null,
          h("span", { class: "val", text: status }),
          game.image_path
            ? h("span", { class: "game-image", title: game.image_path, text: GT4.t("games.image_at", { path: game.image_path }) })
            : null
        ]),
        h("div", { class: "ctl" }, buttons)
      ]);
    }

    // The entrance runs once, on the first draw (TASK-114 rc5). Every draw keeps the view in memory (GT4.cache.games).
    var entering = true;
    function paint(view) {
      GT4.ui.clear(list);
      view.games.forEach(function (game) {
        list.appendChild(card(game));
      });
      firstButton = list.querySelector("button");
      GT4.cache.games = view;
      if (entering) {
        entering = false;
        GT4.enterAnimation(content);
      }
    }

    content.appendChild(h("h1", { text: GT4.t("games.page_title") }));
    content.appendChild(h("p", {
      class: "note",
      text: GT4.t("games.page_note")
    }));
    content.appendChild(list);

    host.hints([]);
    host.entry(function () { return firstButton || content.querySelector("button"); });

    // TASK-114 rc5: the list read before draws at once and is read again behind; it is drawn again only when it changed.
    // Until a list is known, the loading line shows (after 300 ms) instead of an empty page.
    if (GT4.cache.games) {
      paint(GT4.cache.games);
    }
    var cancelLoading = GT4.loadingLater(list, function () {
      return !!GT4.cache.games;
    });
    return GT4.games.list().then(function (view) {
      cancelLoading();
      if (GT4.sameData(GT4.cache.games, view)) {
        return;
      }
      // Focus on a card stays in the list when the list is drawn again.
      var hadFocus = list.contains(document.activeElement);
      paint(view);
      if (hadFocus && firstButton) {
        firstButton.focus({ preventScroll: true });
      }
    }, function (err) {
      cancelLoading();
      GT4.ui.toast(GT4.errorText(err), true);
    });
  };
})(window);
