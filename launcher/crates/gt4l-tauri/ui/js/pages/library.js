/*
 * Saves and Replays tabs. One list component, two commands sets:
 *   saves:   saves_list, saves_import, saves_activate({id}), saves_delete({id}),
 *            saves_new, saves_export({id}), saves_rename({id, name})
 *   replays: replays_list, replays_import, replays_delete({id})
 * Item = {id (library name, sent back to activate/delete/export/rename), name (shown: the display name when renamed),
 * modified (ISO 8601), active (saves only), renamed (saves only)}. The list result also has `folder`: the
 * user data folder that holds the files (%APPDATA%\GT4Recomp\Saves or \Replays), shown with
 * "Open folder" (open_saves_folder / open_replays_folder).
 * saves_import / replays_import / saves_export return the new list, or null when cancelled.
 *
 * Keys on a focused row: A = select (saves: make active), Y = make active
 * (saves) or delete (replays). X = import. F2 = rename (saves). Delete also has a trash button.
 * Saves also have New save (a confirmed command), and per row Rename and Export buttons.
 */
(function (root) {
  "use strict";
  var GT4 = (root.GT4 = root.GT4 || {});
  var h = GT4.ui.h;

  // The texts of each list are read when the list is drawn, so a language change applies.
  function kindInfo(kindName) {
    if (kindName === "saves") {
      return {
        title: GT4.t("library.saves_title"),
        note: GT4.t("library.saves_note"),
        importLabel: GT4.t("library.import_saves"),
        itemWord: GT4.t("library.item_save"),
        empty: { title: GT4.t("library.empty_saves_title"), text: GT4.t("library.empty_saves_text") },
        list: "saves_list",
        importCmd: "saves_import",
        activate: "saves_activate",
        remove: "saves_delete",
        removeTitle: GT4.t("library.delete_save_title"),
        removeText: GT4.t("library.delete_save_text")
      };
    }
    return {
      title: GT4.t("library.replays_title"),
      note: GT4.t("library.replays_note"),
      importLabel: GT4.t("library.import_replays"),
      itemWord: GT4.t("library.item_replay"),
      empty: { title: GT4.t("library.empty_replays_title"), text: GT4.t("library.empty_replays_text") },
      list: "replays_list",
      importCmd: "replays_import",
      activate: null,
      remove: "replays_delete",
      removeTitle: GT4.t("library.delete_replay_title"),
      removeText: GT4.t("library.delete_replay_text")
    };
  }

  function mountLibrary(content, host, kindName) {
    var kind = kindInfo(kindName);
    var items = [];
    var list = h("div", { class: "lib-list" });
    var headNote = h("p", { class: "note", text: kind.note });
    // The game these files belong to (TASK-101): each game has its own saves and replays.
    var gameTag = h("span", { class: "game-tag", text: "" });
    var title = h("h1", { text: kind.title }, [gameTag]);
    var currentGame = "";
    var folderText = h("span", { class: "val lib-folder-path", text: "" });

    // The game tag of a save row: the game's name as the home version switch shows it (TASK-101 item 2).
    function gameTagText(gameId) {
      return gameId === "spec2" ? GT4.t("home.spec2") : GT4.t("home.normal");
    }

    function openFolder() {
      var request = kindName === "saves" ? GT4.backend.call("open_saves_folder", {}) : GT4.backend.call("open_replays_folder", {});
      request.catch(function (err) {
        GT4.ui.toast(GT4.errorText(err), true);
      });
    }

    function selectedRow() {
      var active = document.activeElement;
      return active && active.dataset && active.dataset.itemId ? active : null;
    }

    // The slot the launcher parks the current save in: the next number after the highest one of that game ("Save 02").
    function nextSlotLabel(gameId) {
      var highest = 0;
      items.forEach(function (item) {
        if (item.game !== gameId) {
          return;
        }
        var match = /^Save (\d+)$/.exec(item.slot || "");
        if (match) {
          highest = Math.max(highest, parseInt(match[1], 10));
        }
      });
      return "Save " + String(highest + 1).padStart(2, "0");
    }

    // New save: the current save of the game is kept in a new slot and that game's next Play starts a new profile. When
    // two games are installed the player says which one first; otherwise it is the game the page shows.
    function newSave() {
      GT4.backend.call("games_list", {}).then(function (view) {
        var games = (view.games || []).filter(function (game) { return game.installed; });
        if (games.length > 1) {
          GT4.games.chooseGame({
            title: GT4.t("games.which_title"),
            text: GT4.t("games.which_text"),
            current: currentGame
          }, function (gameId) {
            askNewSave(gameId, games);
          });
        } else {
          askNewSave(currentGame, games);
        }
      }, function (err) {
        GT4.ui.toast(GT4.errorText(err), true);
      });
    }

    function askNewSave(gameId, games) {
      var game = games.filter(function (entry) { return entry.id === gameId; })[0];
      var gameName = game ? game.name : "";
      var hasActive = items.some(function (item) { return item.active && item.game === gameId; });
      GT4.ui.openDialog({
        title: GT4.t("library.new_save_title"),
        text: (gameName ? gameName + ". " : "") + (hasActive
          ? GT4.t("library.new_save_text_kept", { slot: nextSlotLabel(gameId) })
          : GT4.t("library.new_save_text_none")),
        buttons: [
          { label: GT4.t("common.cancel") },
          {
            label: GT4.t("library.new_save_confirm"),
            primary: true,
            run: function () {
              GT4.backend.call("saves_new", { game: gameId }).then(function (result) {
                apply(result, result && result.status ? result.status : "");
              }, function (err) {
                GT4.ui.toast(GT4.errorText(err), true);
              });
            }
          }
        ]
      });
    }

    // Rename: a text box (up to 40 characters). Enter confirms; the folder name of the save does not change.
    function renameItem(item) {
      var input = h("input", {
        class: "modal-input",
        type: "text",
        maxlength: "40",
        value: item.renamed ? item.name : "",
        "aria-label": GT4.t("library.rename_label"),
        placeholder: item.slot || item.id
      });
      var dialog = null;
      function submit() {
        GT4.backend.call("saves_rename", { id: item.id, name: input.value }).then(function (result) {
          apply(result, result && result.status ? result.status : "");
        }, function (err) {
          GT4.ui.toast(GT4.errorText(err), true);
        });
      }
      dialog = GT4.ui.openDialog({
        title: GT4.t("library.rename_title"),
        body: input,
        buttons: [
          { label: GT4.t("common.cancel") },
          { label: GT4.t("library.rename_confirm"), primary: true, run: submit }
        ]
      });
      input.addEventListener("keydown", function (event) {
        if (event.key === "Enter") {
          event.preventDefault();
          submit();
          dialog.close();
        }
      });
      input.focus({ preventScroll: true });
      input.select();
    }

    // Export: the player picks where the .psu file goes (the save dialog is in the launcher).
    function exportItem(item) {
      GT4.backend.call("saves_export", { id: item.id }).then(function (result) {
        if (result) {
          apply(result, result.status || "");
        }
      }, function (err) {
        GT4.ui.toast(GT4.errorText(err), true);
      });
    }

    function removeItem(id) {
      var target = items.filter(function (item) { return item.id === id; })[0];
      // The active save goes to the Recycle Bin too, and the next Play starts a new profile: say so.
      var text = target && target.active && kindName === "saves" ? GT4.t("library.delete_active_save_text") : kind.removeText;
      GT4.ui.openDialog({
        title: kind.removeTitle,
        text: text,
        buttons: [
          { label: GT4.t("common.cancel") },
          {
            label: GT4.t("common.delete"),
            primary: true,
            danger: true,
            run: function () {
              GT4.backend.call(kind.remove, { id: id }).then(function (result) {
                apply(result, GT4.t("library.deleted"));
              }, function (err) {
                GT4.ui.toast(GT4.errorText(err), true);
              });
            }
          }
        ]
      });
    }

    function makeActive(id) {
      if (!kind.activate) {
        return;
      }
      GT4.backend.call(kind.activate, { id: id }).then(function (result) {
        apply(result, GT4.t("library.next_play_used"));
      }, function (err) {
        GT4.ui.toast(GT4.errorText(err), true);
      });
    }

    function row(item, index) {
      var children = [];
      if (kindName === "saves") {
        children.push(h("span", { class: "radio", "aria-hidden": "true" }));
      } else {
        children.push(h("span"));
      }
      // Row number: the position in the list (the id is the library name, e.g. "Save 02").
      children.push(h("span", { class: "no", text: String(index + 1).padStart(2, "0") }));
      var nameNode = h("span", { class: "nm", text: item.name || kind.itemWord + " " + (item.slot || item.id) });
      if (item.game) {
        nameNode.appendChild(h("span", { class: "game-tag", text: gameTagText(item.game) }));
      }
      if (item.active) {
        nameNode.appendChild(h("small", { text: GT4.t("library.next_play") }));
      }
      if (item.renamed) {
        nameNode.appendChild(h("span", { class: "slot-id", text: item.slot || item.id }));
      }
      children.push(nameNode);
      children.push(h("span", { class: "date", text: GT4.ui.formatDate(item.modified) }));
      var actions = [];
      if (kindName === "saves") {
        actions.push(h("button", {
          class: "iconbtn",
          type: "button",
          "aria-label": GT4.t("library.rename_aria", { name: item.name || item.id }),
          title: GT4.t("library.rename"),
          on: {
            click: function (event) {
              event.stopPropagation();
              renameItem(item);
            }
          }
        }, pencilIcon()));
        actions.push(h("button", {
          class: "iconbtn",
          type: "button",
          "aria-label": GT4.t("library.export_aria", { name: item.name || item.id }),
          title: GT4.t("library.export"),
          on: {
            click: function (event) {
              event.stopPropagation();
              exportItem(item);
            }
          }
        }, exportIcon()));
      }
      actions.push(h("button", {
        class: "iconbtn del",
        type: "button",
        "aria-label": GT4.t("library.aria_delete", { name: item.name || item.id }),
        title: GT4.t("common.delete"),
        on: {
          click: function (event) {
            event.stopPropagation();
            removeItem(item.id);
          }
        }
      }, trashIcon()));
      children.push(h("span", { class: "acts" }, actions));

      var node = h("div", {
        class: "slot-row" + (item.active ? " active" : ""),
        role: kindName === "saves" ? "radio" : "listitem",
        "aria-checked": kindName === "saves" ? String(!!item.active) : null,
        tabindex: "0",
        data: { focusable: "1", itemId: item.id },
        on: {
          click: function () {
            if (kind.activate) {
              makeActive(item.id);
            }
          }
        }
      }, children);
      return node;
    }

    // A line icon from one path (the same style as the trash icon).
    function lineIcon(pathData) {
      var svg = document.createElementNS("http://www.w3.org/2000/svg", "svg");
      svg.setAttribute("width", "18");
      svg.setAttribute("height", "18");
      svg.setAttribute("viewBox", "0 0 24 24");
      svg.setAttribute("fill", "none");
      svg.setAttribute("stroke", "currentColor");
      svg.setAttribute("stroke-width", "1.6");
      svg.setAttribute("stroke-linecap", "round");
      svg.setAttribute("stroke-linejoin", "round");
      svg.setAttribute("aria-hidden", "true");
      var path = document.createElementNS("http://www.w3.org/2000/svg", "path");
      path.setAttribute("d", pathData);
      svg.appendChild(path);
      return svg;
    }

    function pencilIcon() {
      return lineIcon("M4 20h4L19 9l-4-4L4 16z");
    }

    function exportIcon() {
      return lineIcon("M12 3v12M7 10l5 5 5-5M5 21h14");
    }

    function trashIcon() {
      var svg = document.createElementNS("http://www.w3.org/2000/svg", "svg");
      svg.setAttribute("width", "18");
      svg.setAttribute("height", "18");
      svg.setAttribute("viewBox", "0 0 24 24");
      svg.setAttribute("fill", "none");
      svg.setAttribute("stroke", "currentColor");
      svg.setAttribute("stroke-width", "1.6");
      svg.setAttribute("stroke-linecap", "round");
      svg.setAttribute("aria-hidden", "true");
      var path = document.createElementNS("http://www.w3.org/2000/svg", "path");
      path.setAttribute("d", "M4 7h16M9 7V4h6v3M6 7l1 13h10l1-13");
      svg.appendChild(path);
      return svg;
    }

    // The entrance of the page's items runs once, on the first draw (TASK-114 rc5).
    var entering = true;
    function paint(focusId) {
      GT4.ui.clear(list);
      if (entering) {
        entering = false;
        GT4.enterAnimation(content);
      }
      if (!items.length) {
        list.classList.remove("lib-list");
        list.appendChild(h("div", { class: "emptystate" }, [
          h("h3", { text: kind.empty.title }),
          h("p", { text: kind.empty.text })
        ]));
      } else {
        list.classList.add("lib-list");
        items.forEach(function (item, index) {
          list.appendChild(row(item, index));
        });
      }
      if (focusId !== undefined) {
        var target = list.querySelector("[data-item-id=\"" + focusId + "\"]");
        if (target) {
          target.focus({ preventScroll: true });
        }
      }
    }

    // The answer of every list and change command (list, import, new, rename, delete, activate) is the list: it is
    // drawn and kept in memory for the next open (TASK-114 rc5). `focusId` keeps the focus on that row when drawn again.
    function apply(result, message, focusId) {
      if (result && typeof result.game === "string") {
        currentGame = result.game || currentGame;
      }
      // Replays belong to the game the page shows, so their title names it; the Saves list holds both games.
      if (result && typeof result.gameName === "string" && kindName === "replays") {
        gameTag.textContent = result.gameName;
      }
      if (result && Array.isArray(result.items)) {
        items = result.items;
        GT4.cache.library[kindName] = result;
      }
      if (result && typeof result.folder === "string" && result.folder) {
        folderText.textContent = result.folder;
      }
      if (message) {
        GT4.ui.toast(message);
      }
      paint(focusId);
    }

    // An import needs no question: each save or replay names its game, and the launcher files it there.
    function importItems() {
      GT4.backend.call(kind.importCmd, {}).then(function (result) {
        if (!result) {
          return;
        }
        apply(result, result.status || GT4.t("library.import_finished"));
      }, function (err) {
        GT4.ui.toast(GT4.errorText(err), true);
      });
    }

    var importButton = h("button", {
      class: "btn primary",
      type: "button",
      on: { click: importItems }
    }, kind.importLabel);

    var newButton = kindName === "saves"
      ? h("button", { class: "btn", type: "button", on: { click: newSave } }, GT4.t("library.new_save"))
      : null;

    content.appendChild(h("div", { class: "lib-head" }, [
      h("div", null, [title, headNote]),
      h("div", { class: "lib-actions" }, [newButton, importButton].filter(Boolean))
    ]));
    content.appendChild(list);
    // Where the files are: the user data folder (%APPDATA%\GT4Recomp), easy to open and back up.
    content.appendChild(h("div", { class: "row lib-folder" }, [
      h("div", { class: "k" }, [h("span", { text: GT4.t("library.folder") }), folderText]),
      h("div", { class: "ctl" }, [
        h("button", { class: "btn small", type: "button", on: { click: openFolder } }, GT4.t("common.open_folder"))
      ])
    ]));

    host.setHooks({
      x: importItems,
      y: function () {
        var row = selectedRow();
        if (!row) {
          GT4.ui.toast(GT4.t("hint.select_first"), true);
          return;
        }
        var id = row.dataset.itemId;
        if (kind.activate) {
          makeActive(id);
        } else {
          removeItem(id);
        }
      }
    });
    // A enters the list on its first row; Import is the entry only when the list is empty.
    host.entry(function () {
      var rows = list.querySelectorAll("[data-item-id]");
      return GT4.settingsModel.libraryEntry(rows.length) === "row" ? rows[0] : importButton;
    });
    host.hints([
      { key: "X", label: GT4.t("hint.import"), run: importItems },
      { key: "Y", label: kind.activate ? GT4.t("hint.make_active") : GT4.t("hint.delete"), run: function () { if (GT4.hooks.y) { GT4.hooks.y(); } } }
    ]);

    content.addEventListener("keydown", function (event) {
      if (event.key === "Delete") {
        var row = selectedRow();
        if (row) {
          removeItem(row.dataset.itemId);
        }
      }
      if (event.key === "F2" && kindName === "saves") {
        var renamed = selectedRow();
        var target = renamed ? items.filter(function (item) { return item.id === renamed.dataset.itemId; })[0] : null;
        if (target) {
          renameItem(target);
        }
      }
    });

    // Drawing the list never moves focus: the tab list keeps it until the player presses A (host.entry).
    // TASK-114 rc5: the list read before draws at once and is read again behind; it is drawn again only when it changed.
    // Until a list is known, the loading line shows (after 300 ms) instead of the empty state.
    if (GT4.cache.library[kindName]) {
      apply(GT4.cache.library[kindName], "");
    }
    var cancelLoading = GT4.loadingLater(list, function () {
      return !!GT4.cache.library[kindName];
    });
    return GT4.backend.call(kind.list, {}).then(function (result) {
      cancelLoading();
      if (GT4.sameData(GT4.cache.library[kindName], result)) {
        return;
      }
      var focused = list.contains(document.activeElement) ? document.activeElement.getAttribute("data-item-id") : null;
      apply(result, "", focused || undefined);
    }, function (err) {
      cancelLoading();
      GT4.ui.toast(GT4.errorText(err), true);
    });
  }

  GT4.tabPages.saves = function (content, host) {
    return mountLibrary(content, host, "saves");
  };
  GT4.tabPages.replays = function (content, host) {
    return mountLibrary(content, host, "replays");
  };
})(window);
