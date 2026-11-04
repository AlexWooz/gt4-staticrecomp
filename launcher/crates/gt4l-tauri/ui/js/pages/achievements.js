/*
 * Achievements tab (RetroAchievements account login, session options, and Gran Turismo 4 achievement progress).
 * Commands:
 *   ra_status() -> {enabled, hardcore, encore, signedIn, username, displayName, score, scoreSoftcore, game, selectedGame, supported}
 *   ra_login({username, password}) -> status
 *   ra_logout() -> status
 *   ra_set_options({enabled, hardcore, encore}) -> status
 */
(function (root) {
  "use strict";
  var GT4 = (root.GT4 = root.GT4 || {});
  var h = GT4.ui.h;

  function t(key, params) {
    if (GT4.i18n && typeof GT4.i18n.t === "function") {
      return GT4.i18n.t(key, params);
    }
    return key;
  }

  function infoTip(title, text) {
    var tipId = "ra-tip-" + title.toLowerCase().replace(/[^a-z0-9]+/g, "-");
    var btn = h("button", {
      class: "info",
      type: "button",
      "aria-label": "About " + title,
      "aria-expanded": "false",
      "aria-describedby": tipId,
      on: {
        click: function (event) {
          event.stopPropagation();
          GT4.ui.toggleTip(btn);
        }
      }
    }, [
      "i",
      h("span", { class: "tip", id: tipId, role: "tooltip" }, [
        h("b", { text: title }),
        h("span", { text: text })
      ])
    ]);
    return btn;
  }

  function optionToggleRow(label, helpText, checked, disabled, onToggle) {
    // The option control of the settings pages (settings.js): the value between two arrows, in an endless loop.
    var toggleBtn = GT4.optionSelector({
      id: "achievements-" + label,
      label: label,
      options: [
        { value: false, text: GT4.t("settings.off") },
        { value: true, text: GT4.t("settings.on") }
      ],
      current: !!checked,
      disabled: !!disabled,
      onChange: function (value) {
        onToggle(value);
      }
    });
    return {
      button: toggleBtn,
      row: h("div", { class: "row" }, [
        h("div", { class: "k" }, [
          h("span", { text: label }),
          infoTip(label, helpText)
        ]),
        h("div", { class: "ctl" }, [toggleBtn])
      ])
    };
  }

  GT4.tabPages = GT4.tabPages || {};
  GT4.tabPages.achievements = function (content, host) {
    var state = {
      enabled: true,
      hardcore: false,
      encore: false,
      signedIn: false,
      username: "",
      displayName: "",
      score: 0,
      scoreSoftcore: 0,
      game: null,
      selectedGame: "gt4",
      supported: true,
      reloginRequired: false,
      tokenErrorKind: null
    };

    var container = h("div", { class: "ra-page" });
    content.appendChild(container);

    // A later account/options/target action invalidates a slower status/login response.
    var requestSerial = 0;

    function applyState(next, toastMsg, requestId) {
      if (requestId !== requestSerial || !container.isConnected) {
        return;
      }
      if (next && typeof next === "object") {
        state.enabled = !!next.enabled;
        state.hardcore = !!next.hardcore;
        state.encore = !!next.encore;
        state.signedIn = !!next.signedIn;
        state.username = next.username || "";
        state.displayName = next.displayName || next.username || "";
        state.score = Number(next.score || 0);
        state.scoreSoftcore = Number(next.scoreSoftcore || 0);
        state.selectedGame = next.selectedGame || "gt4";
        state.supported = next.supported !== false && state.selectedGame === "gt4";
        state.reloginRequired = !!next.reloginRequired;
        state.tokenErrorKind = next.tokenErrorKind || null;
        if (state.supported && next.game !== undefined && next.game !== null) {
          state.game = next.game;
        } else {
          state.game = null;
        }
      }
      if (toastMsg) {
        GT4.ui.toast(toastMsg);
      }
      paint();
    }

    function saveOptions(enabled, hardcore, encore) {
      var requestId = ++requestSerial;
      GT4.backend.call("ra_set_options", {
        enabled: !!enabled,
        hardcore: !!hardcore,
        encore: !!encore
      }).then(function (res) {
        if (res && state.game && !res.game) {
          res.game = state.game;
        }
        applyState(res, "", requestId);
      }, function (err) {
        var errStr = GT4.errorText(err) || "";
        var msg = errStr.indexOf("legacy credentials could not be protected") >= 0
          ? t("ra.err_save_migration")
          : t("ra.err_save_failed");
        GT4.ui.toast(msg, true);
        refreshStatus();
      });
    }

    function showError(err, requestId) {
      if (requestId === requestSerial && container.isConnected) {
        GT4.ui.toast(GT4.errorText(err), true);
      }
    }

    function openSignInModal() {
      var userInput = h("input", {
        class: "ra-input",
        type: "text",
        placeholder: t("ra.username"),
        autocomplete: "username",
        spellcheck: "false",
        value: state.username || ""
      });
      var passInput = h("input", {
        class: "ra-input",
        type: "password",
        placeholder: t("ra.password"),
        autocomplete: "current-password"
      });
      var statusLine = h("div", { class: "ra-login-error", text: "" });
      var busy = false;

      function submit(dialogApi) {
        if (busy) {
          return;
        }
        var u = (userInput.value || "").trim();
        var p = passInput.value || "";
        if (!u) {
          statusLine.textContent = t("ra.err_username");
          userInput.focus();
          return;
        }
        if (!p) {
          statusLine.textContent = t("ra.err_password");
          passInput.focus();
          return;
        }
        busy = true;
        var requestId = ++requestSerial;
        statusLine.textContent = t("ra.signing_in");
        GT4.backend.call("ra_login", { username: u, password: p }).then(function (res) {
          busy = false;
          if (requestId !== requestSerial || !container.isConnected) {
            return;
          }
          dialogApi.close();
          applyState(res, t("ra.toast_signed_in", { name: res.displayName || u }), requestId);
        }, function (err) {
          busy = false;
          if (requestId === requestSerial && container.isConnected) {
            statusLine.textContent = GT4.errorText(err);
          }
        });
      }

      var dialog = GT4.ui.openDialog({
        title: t("ra.modal_title"),
        text: t("ra.modal_text"),
        body: h("div", { class: "ra-form" }, [
          h("label", { class: "ra-field" }, [
            h("span", { class: "ra-field-label", text: t("ra.username") }),
            userInput
          ]),
          h("label", { class: "ra-field" }, [
            h("span", { class: "ra-field-label", text: t("ra.password") }),
            passInput
          ]),
          statusLine
        ]),
        buttons: [
          { label: t("ra.cancel") },
          {
            label: t("ra.sign_in"),
            primary: true,
            closes: false,
            run: function (api) {
              submit(api);
            }
          }
        ]
      });

      function onEnterKey(event) {
        if (event.key === "Enter") {
          event.preventDefault();
          submit(dialog);
        }
      }
      userInput.addEventListener("keydown", onEnterKey);
      passInput.addEventListener("keydown", onEnterKey);
      setTimeout(function () {
        userInput.focus();
      }, 20);
    }

    function confirmSignOut() {
      GT4.ui.openDialog({
        title: t("ra.sign_out_title"),
        text: t("ra.sign_out_text"),
        buttons: [
          { label: t("ra.cancel") },
          {
            label: t("ra.sign_out"),
            primary: true,
            danger: true,
            run: function () {
              var requestId = ++requestSerial;
              GT4.backend.call("ra_logout", {}).then(function (res) {
                applyState(res, t("ra.toast_signed_out"), requestId);
              }, function (err) {
                showError(err, requestId);
              });
            }
          }
        ]
      });
    }

    function refreshStatus() {
      var requestId = ++requestSerial;
      GT4.backend.call("ra_status", {}).then(function (res) {
        applyState(res, "", requestId);
      }, function (err) {
        showError(err, requestId);
      });
    }

    function paint() {
      GT4.ui.clear(container);

      var actionBtn = state.signedIn
        ? h("button", { class: "btn danger", type: "button", on: { click: confirmSignOut } }, t("ra.sign_out"))
        : h("button", { class: "btn primary", type: "button", on: { click: openSignInModal } }, t("ra.sign_in"));

      container.appendChild(h("div", { class: "lib-head" }, [
        h("div", null, [
          h("h1", { text: t("ra.title") }),
          h("p", {
            class: "note",
            text: t("ra.subtitle")
          })
        ]),
        actionBtn
      ]));

      // Account summary section
      var accountSection = h("section", { class: "settings-section" }, [
        h("h2", { class: "section-head", text: t("ra.section_account") })
      ]);

      if (state.signedIn) {
        accountSection.appendChild(h("div", { class: "row" }, [
          h("div", { class: "k" }, [h("span", { text: t("ra.signed_in_as") })]),
          h("div", { class: "ctl wide" }, [
            h("span", { class: "val ra-user-badge", text: state.displayName || state.username })
          ])
        ]));
        accountSection.appendChild(h("div", { class: "row" }, [
          h("div", { class: "k" }, [h("span", { text: t("ra.score") })]),
          h("div", { class: "ctl wide" }, [
            h("span", {
              class: "val",
              text: t("ra.score_val", { score: state.score, scoreSoftcore: state.scoreSoftcore })
            })
          ])
        ]));
      } else {
        accountSection.appendChild(h("div", { class: "row" }, [
          h("div", { class: "k" }, [h("span", { text: t("ra.status") })]),
          h("div", { class: "ctl wide" }, [
            h("span", {
              class: "val" + (state.reloginRequired ? " is-warn" : ""),
              text: state.reloginRequired ? t("ra.relogin_required") : t("ra.not_signed_in")
            })
          ])
        ]));
        if (state.reloginRequired) {
          var descKey = state.tokenErrorKind === "migrationFailed"
            ? "ra.migration_failed_desc"
            : (state.tokenErrorKind === "saveFailed"
              ? "ra.save_failed_desc"
              : "ra.relogin_required_desc");
          accountSection.appendChild(h("div", { class: "row" }, [
            h("div", { class: "k" }, [h("span", { text: t("ra.relogin_notice") })]),
            h("div", { class: "ctl wide" }, [
              h("p", { class: "ra-unsupported-desc", text: t(descKey) })
            ])
          ]));
        }
      }
      container.appendChild(accountSection);

      // Session options section
      var optionsSection = h("section", { class: "settings-section" }, [
        h("h2", { class: "section-head", text: t("ra.section_options") })
      ]);

      var enableRow = optionToggleRow(
        t("ra.opt_enabled"),
        t("ra.opt_enabled_help"),
        state.enabled,
        !state.supported,
        function (val) {
          saveOptions(val, state.hardcore, state.encore);
        }
      );
      var hardcoreRow = optionToggleRow(
        t("ra.opt_hardcore"),
        t("ra.opt_hardcore_help"),
        state.hardcore,
        !state.supported,
        function (val) {
          saveOptions(state.enabled, val, state.encore);
        }
      );
      var encoreRow = optionToggleRow(
        t("ra.opt_encore"),
        t("ra.opt_encore_help"),
        state.encore,
        !state.supported,
        function (val) {
          saveOptions(state.enabled, state.hardcore, val);
        }
      );

      optionsSection.appendChild(enableRow.row);
      optionsSection.appendChild(hardcoreRow.row);
      optionsSection.appendChild(encoreRow.row);
      container.appendChild(optionsSection);

      // Spec II unsupported banner
      if (!state.supported) {
        var unsupportedSection = h("section", { class: "settings-section" }, [
          h("div", { class: "ra-unsupported-banner" }, [
            h("div", { class: "ra-unsupported-title", text: t("ra.unsupported_title") }),
            h("div", { class: "ra-unsupported-desc", text: t("ra.unsupported_desc") })
          ])
        ]);
        container.appendChild(unsupportedSection);
      } else if (state.signedIn && state.game) {
        // Gran Turismo 4 achievement progress section
        var game = state.game;
        var summaryText = t("ra.summary_progress", {
          unlocked: game.unlocked_achievements,
          total: game.total_achievements,
          points: game.unlocked_points,
          totalPoints: game.total_points
        });

        var gameSection = h("section", { class: "settings-section" }, [
          h("h2", { class: "section-head", text: game.title || "Gran Turismo 4" }),
          h("div", { class: "row" }, [
            h("div", { class: "k" }, [h("span", { text: t("ra.progress") })]),
            h("div", { class: "ctl wide" }, [h("span", { class: "val", text: summaryText })])
          ])
        ]);

        var listBox = h("div", { class: "ra-ach-list" });
        (game.achievements || []).forEach(function (ach) {
          var statusLabel = ach.unlocked_hardcore
            ? t("ra.pill_hardcore")
            : ach.unlocked
            ? t("ra.pill_unlocked")
            : t("ra.pill_locked");
          var statusClass = ach.unlocked_hardcore
            ? "ra-pill is-hc"
            : ach.unlocked
            ? "ra-pill is-unlocked"
            : "ra-pill is-locked";

          var headKids = [
            h("span", { class: "ra-ach-title", text: ach.title || "Achievement" })
          ];
          if (ach.type_label) {
            headKids.push(h("span", { class: "ra-ach-type", text: ach.type_label }));
          }

          var row = h("div", {
            class: "ra-ach-row" + (ach.unlocked ? " is-earned" : ""),
            tabindex: "0"
          }, [
            h("div", { class: "ra-ach-main" }, [
              h("div", { class: "ra-ach-head" }, headKids),
              h("div", { class: "ra-ach-desc", text: ach.description || "" })
            ]),
            h("div", { class: "ra-ach-meta" }, [
              h("span", { class: "ra-ach-pts", text: t("ra.pts", { pts: ach.points }) }),
              h("span", { class: statusClass, text: statusLabel })
            ])
          ]);
          listBox.appendChild(row);
        });

        gameSection.appendChild(listBox);
        container.appendChild(gameSection);
      }
    }

    paint();
    refreshStatus();
  };
})(window);
