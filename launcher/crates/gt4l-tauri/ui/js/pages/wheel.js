/*
 * Wheel tab (TASK-181): the racing wheel the game drives as a virtual Logitech Driving Force Pro (DFP). GT4 sees a DFP
 * on its USB port, runs its own wheel driver (900 degrees) and sends its own force feedback; this page sets up the
 * mapping in wheel.toml and shows the wheel live.
 *
 * Credits: the page grew from the 1.2.5 launcher's wheel configurator (commit cbfd8b8, PR #101, js/pages/wheel.js) by
 * JulioHeroZ, with luanantony: the wheel-and-pedal stage beside a side panel, the selected control card with its
 * calibration, live meters, capture and the force test. TASK-181 (owner, 2026-11-05: the DFP path only) drops the
 * DualShock 2 bridge settings and adds the real-angle wheel, the identity card, the set-up wizard and presets.
 *
 * Layout: header (title, note, the wheel's status), the "racing wheel in the game" switch and Set up; the drawing
 * (js/wheel-svg.js: the wheel turns at its real angle, the pedals show the raw travel and the value the game gets, one
 * chip per control) and the side panel (the wheel card, the selected control with its calibration, force feedback,
 * presets). Every change writes wheel.toml at once (no Save).
 *
 * Commands (ui/dev/README.md): get_wheel_config, set_wheel_setting, apply_wheel_preset, assign_wheel_binding,
 * reset_wheel_binding, reset_wheel_config, capture_wheel_binding, cancel_wheel_capture, test_wheel_ffb, wheel_watch,
 * get_wheel_session_check, list_wheel_presets, save_wheel_preset, delete_wheel_preset.
 * Events: wheel-status (on change), wheel-live (about 60 Hz while this page asks with wheel_watch, renewed every 2 s).
 *
 * Navigation (two levels, js/focus-levels.js): A on the tab list enters on the control selected last (the steering at
 * first). The D-pad moves between the chips; A binds the control (a capture), Y clears it, X starts the set-up wizard,
 * B returns to the tab list. Mouse and touch: a click selects the nearest control; a second click binds it.
 */
(function (root) {
  "use strict";
  var GT4 = (root.GT4 = root.GT4 || {});
  var h = GT4.ui.h;

  var BIND_SECONDS = 8;
  var KEEPALIVE_MS = 2000;
  var remembered = { selected: "steering" }; // kept while the app runs

  function call(command, args) {
    return GT4.backend.call(command, args);
  }

  function t(key, params) {
    return GT4.t(key, params);
  }

  GT4.tabPages.wheel = function (content, host) {
    var M = GT4.wheelModel;
    content.classList.add("is-controls", "is-wheel");
    var dev = GT4.devWheel || {};
    var state = {
      view: null,
      status: null,
      live: [],
      selected: dev.select || remembered.selected,
      session: null,
      test: null,
      testing: false,
      capture: null
    };

    // ---- names -----------------------------------------------------------------------------------------------------
    function controlName(id) {
      if (id === "steering") {
        return t("wheel.steering");
      }
      if (M.PEDALS.indexOf(id) >= 0) {
        return t("wheel.pedal." + id);
      }
      var control = M.controlOfSlot(id);
      return control ? t("wheel.control." + control.dfp) : id;
    }
    function controlSub(id) {
      var key = id === "steering" ? "wheel.sub.steering" : M.PEDALS.indexOf(id) >= 0 ? "wheel.sub." + id : "wheel.sub." + (M.controlOfSlot(id) || {}).dfp;
      return GT4.i18n.has(key) ? t(key) : "";
    }
    function sourceText(source) {
      var info = M.describe(source);
      if (!info) {
        return t("wheel.unbound");
      }
      if (info.kind === "button") {
        return t("wheel.source.button", { n: info.number });
      }
      if (info.kind === "hat") {
        return t("wheel.source.hat", { dir: t("wheel.dir." + info.dir) });
      }
      return info.sign ? t("wheel.source.axis_dir", { n: info.number, sign: info.sign === "+" ? "+" : "−" }) : t("wheel.source.axis", { n: info.number });
    }
    function deviceNameOf(source) {
      var parsed = M.parseSource(source);
      if (!parsed || !parsed.guid || !state.status) {
        return "";
      }
      var device = (state.status.devices || []).filter(function (one) { return String(one.guid).toLowerCase() === parsed.guid; })[0];
      return device ? device.name : t("wheel.other_device_unknown");
    }
    function config() {
      return state.view ? state.view.config : null;
    }
    function primary() {
      return state.status ? state.status.primary : null;
    }
    function locked() {
      return !!(state.view && state.view.file && state.view.file.locked);
    }
    function gameRunning() {
      return !!((state.view && state.view.game_running) || (state.status && state.status.game_running));
    }

    // ---- header -------------------------------------------------------------------------------------------------------
    var statusName = h("span", { class: "cx-status-name", text: t("wheel.no_wheel_title") });
    var statusSub = h("span", { class: "cx-status-sub", text: "" });
    var status = h("div", { class: "cx-status", role: "status" }, [
      h("span", { class: "dot", "aria-hidden": "true" }),
      h("span", { class: "cx-status-text" }, [statusName, statusSub])
    ]);
    var info = h("button", {
      class: "info",
      type: "button",
      "aria-label": t("wheel.about"),
      "aria-expanded": "false",
      on: { click: function () { GT4.ui.toggleTip(info); } }
    }, ["i", h("span", { class: "tip", role: "tooltip" }, [h("b", { text: t("wheel.title") }), t("wheel.tip")])]);
    content.appendChild(h("div", { class: "cx-head" }, [
      h("div", { class: "cx-title" }, [
        h("h1", { text: t("wheel.title") }),
        h("p", { class: "note" }, [h("span", { text: t("wheel.saves_at_once") }), info])
      ]),
      h("div", { class: "cx-head-right" }, [status])
    ]));

    // The switch (the virtual DFP plugged in or not) and Set up.
    var enabledBox = h("div", { class: "wh-enabled" });
    var setupButton = h("button", { class: "btn primary wh-setup", type: "button", on: { click: function () { startWizard(); } } }, t("wheel.setup"));
    content.appendChild(h("div", { class: "wh-top" }, [enabledBox, setupButton]));
    var notice = h("p", { class: "cx-locked", hidden: true, text: "" });
    content.appendChild(notice);

    // ---- the drawing ------------------------------------------------------------------------------------------------
    var drawing = GT4.wheelSvg({ onPick: onPick });
    drawing.setPedalNames({
      throttle: t("wheel.pedal_short.throttle"),
      brake: t("wheel.pedal_short.brake"),
      clutch: t("wheel.pedal_short.clutch"),
      handbrake: t("wheel.pedal_short.handbrake")
    });
    var legend = h("div", { class: "cx-legend" }, [
      h("span", { class: "cx-legend-item" }, [h("i", { class: "wh-key raw" }), h("span", { class: "cx-legend-t", text: t("wheel.legend_raw") })]),
      h("span", { class: "cx-legend-item" }, [h("i", { class: "wh-key out" }), h("span", { class: "cx-legend-t", text: t("wheel.legend_out") })]),
      h("span", { class: "cx-legend-item" }, [h("i", { class: "wh-key aux" }), h("span", { class: "cx-legend-t", text: t("wheel.legend_aux") })])
    ]);
    var stage = h("div", { class: "cx-stage" }, [
      h("div", { class: "cx-pad wh-pad" }, [drawing.svg]),
      h("div", { class: "cx-under" }, [legend])
    ]);

    // ---- side panel ---------------------------------------------------------------------------------------------------
    var panel = h("aside", { class: "cx-panel wh-panel", "aria-label": t("wheel.panel_label") });
    // The wheel card.
    var deviceName = h("div", { class: "cx-name wh-device-name" });
    var deviceLines = h("div", { class: "wh-device-lines" });
    var deviceNotes = h("div", { class: "wh-notes" });
    var deviceRow = h("div", { class: "wh-device-row" });
    var deviceCard = h("section", { class: "cx-card wh-device" }, [
      h("div", { class: "cx-state", text: t("wheel.device_title") }),
      deviceName,
      deviceLines,
      deviceRow,
      deviceNotes
    ]);
    // The selected control.
    var cardGlyph = h("span", { class: "cx-glyph" });
    var cardName = h("div", { class: "cx-name" });
    var cardSub = h("div", { class: "cx-state" });
    var cardSources = h("div", { class: "cx-src-v wh-sources" });
    var bindButton = h("button", { class: "btn primary small", type: "button", on: { click: function () { bind(state.selected); } } }, t("wheel.bind"));
    var clearButton = h("button", { class: "btn small", type: "button", on: { click: function () { clearOne(state.selected); } } }, t("wheel.clear"));
    var cardHint = h("p", { class: "cx-hint", text: "" });
    var meterRaw = h("i", { class: "raw" });
    var meterOut = h("i", { class: "out" });
    var meterDz = h("b", { class: "dz" });
    var meterSat = h("b", { class: "sat" });
    var meterValue = h("span", { class: "cx-meter-v", text: "0%" });
    var meter = h("div", { class: "cx-block-head wh-meter-row", hidden: true }, [
      h("span", { class: "cx-meter wh-meter", title: t("wheel.meter_help") }, [meterDz, meterSat, meterRaw, meterOut]),
      meterValue
    ]);
    var cardRows = h("div", { class: "cx-rows" });
    var card = h("section", { class: "cx-card wh-control" }, [
      h("div", { class: "cx-card-head" }, [cardGlyph, h("div", { class: "cx-card-title" }, [cardName, cardSub])]),
      h("div", { class: "cx-src" }, [h("span", { class: "cx-src-k", text: t("wheel.bound_to") }), cardSources]),
      meter,
      cardRows,
      h("div", { class: "cx-actions" }, [bindButton, clearButton]),
      cardHint
    ]);
    // Force feedback.
    var ffbRows = h("div", { class: "cx-rows" });
    var testButton = h("button", { class: "btn small wh-test", type: "button", on: { click: function () { testForce(); } } }, t("wheel.ffb.test"));
    var testResult = h("p", { class: "wh-test-result", role: "status", text: t("wheel.ffb.test_help") });
    var receivedValue = h("span", { class: "wh-received-v" });
    var receivedReason = h("p", { class: "wh-received-why" });
    var ffbCard = h("section", { class: "cx-card wh-ffb", data: { section: "ffb" } }, [
      h("h2", { class: "wh-card-title", text: t("wheel.ffb.title") }),
      ffbRows,
      h("div", { class: "wh-test-row" }, [testButton, testResult]),
      h("div", { class: "wh-received" }, [h("span", { class: "wh-received-k", text: t("wheel.ffb.received") }), receivedValue]),
      receivedReason
    ]);
    // Presets.
    var presetsCard = h("section", { class: "cx-card wh-presets" }, [
      h("h2", { class: "wh-card-title", text: t("wheel.presets.title") }),
      h("p", { class: "wh-card-note", text: t("wheel.presets.note") }),
      h("div", { class: "cx-actions wh-preset-actions" }, [
        h("button", { class: "btn small", type: "button", on: { click: function () { savePreset(); } } }, t("wheel.presets.save")),
        h("button", { class: "btn small", type: "button", on: { click: function () { loadPreset(); } } }, t("wheel.presets.load"))
      ]),
      h("button", { class: "btn danger small cx-reset-all", type: "button", on: { click: function () { confirmReset(); } } }, t("wheel.presets.reset"))
    ]);
    panel.appendChild(deviceCard);
    panel.appendChild(card);
    panel.appendChild(ffbCard);
    panel.appendChild(presetsCard);
    content.appendChild(h("div", { class: "cx-body" }, [stage, panel]));

    // ---- rows (the settings row components, written with set_wheel_setting) -------------------------------------------
    // Each row id maps to a wheel.toml key and to/from the value the row shows (percent for the 0..1 values).
    var ROWS = {};
    function percentRow(id, key, label, help, min, max, step, get) {
      ROWS[id] = { key: key, toFile: function (value) { return Math.round(Number(value)) / 100; } };
      return { id: id, control: "slider", label: label, help: help, values: { min: min, max: max, step: step, unit: "%" }, current: Math.round(get() * 100) };
    }
    function toggleRow(id, key, label, help, value) {
      ROWS[id] = { key: key, toFile: function (next) { return !!next; } };
      return { id: id, control: "toggle", label: label, help: help, values: null, current: !!value };
    }
    function choiceRow(id, key, label, help, values, current) {
      ROWS[id] = { key: key, toFile: function (next) { return next; } };
      return { id: id, control: "dropdown", label: label, help: help, values: values, current: current };
    }
    var rowContext = {
      table: { rows: [] },
      content: panel,
      selfDrawn: {},
      refresh: function (focusId) { paint(focusId); return Promise.resolve(); },
      commit: function (id, value) {
        var row = ROWS[id];
        if (!row) {
          return Promise.resolve();
        }
        return setting(row.key, row.toFile(value), id);
      }
    };
    function rowNodes(rows, box) {
      GT4.ui.clear(box);
      rows.forEach(function (row) {
        var node = GT4.settingsRows.rowNode(rowContext, Object.assign({ disabled: locked() }, row));
        if (node) {
          box.appendChild(node);
        }
      });
    }
    function setting(key, value, focusId) {
      return call("set_wheel_setting", { key: key, value: value }).then(function (view) {
        apply(view, focusId);
      }, function (err) {
        GT4.ui.toast(GT4.errorText(err), true);
        load(focusId);
      });
    }

    // ---- painting -----------------------------------------------------------------------------------------------------
    function paintEnabled(focusId) {
      var cfg = config();
      var row = toggleRow("wheel-enabled", "wheel.enabled", t("wheel.enabled"), t("wheel.enabled_help"), cfg && cfg.enabled);
      rowNodes([row], enabledBox);
      if (focusId === "wheel-enabled") {
        var target = enabledBox.querySelector("[data-setting=\"wheel-enabled\"]");
        if (target) {
          target.focus({ preventScroll: true });
        }
      }
      setupButton.disabled = locked();
    }

    function paintStatus() {
      var device = M.mainDevice(state.status);
      var connected = !!device && state.status && state.status.primary === device.id;
      status.classList.toggle("is-on", connected);
      if (state.status && state.status.available === false) {
        statusName.textContent = t("wheel.input_unavailable");
        statusSub.textContent = state.status.error || "";
        return;
      }
      statusName.textContent = device ? device.name : t("wheel.no_wheel_title");
      if (!device) {
        statusSub.textContent = t("wheel.connect_hint");
      } else if (gameRunning()) {
        statusSub.textContent = t("wheel.status_game");
      } else {
        statusSub.textContent = connected ? t("wheel.status_driven", { id: device.vid_pid || "?" }) : t("wheel.status_other");
      }
    }

    function line(text, cls) {
      return h("p", { class: "wh-line" + (cls ? " " + cls : ""), text: text });
    }

    function ffbLine(device) {
      if (device.features) {
        var names = ["constant", "spring", "damper", "friction", "gain", "autocenter"].filter(function (key) { return device.features[key]; })
          .map(function (key) { return t("wheel.effect." + key); });
        return t("wheel.ffb_line.features", { effects: names.length ? names.join(", ") : t("wheel.effect.none") });
      }
      if (device.haptic === true) {
        return t("wheel.ffb_line.yes");
      }
      if (device.haptic === false) {
        return t("wheel.ffb_line.no");
      }
      return t("wheel.ffb_line.unknown");
    }

    var lastDeviceKey = "";
    function paintDevice() {
      var cfg = config();
      // Only what the card shows: the watch flag and the event's timing never draw it again (that would drop the focus of
      // its dropdown).
      var shown = state.status ? {
        available: state.status.available,
        primary: state.status.primary,
        configured_connected: state.status.configured_connected,
        devices: (state.status.devices || []).map(function (one) {
          return [one.guid, one.name, one.role, one.family_name, one.model, one.vid_pid, one.sdl_type, one.haptic, one.features, one.notes, one.software];
        })
      } : null;
      var key = JSON.stringify([shown, cfg && cfg.device_guid, cfg && cfg.device_name, locked()]);
      if (key === lastDeviceKey) {
        return;
      }
      lastDeviceKey = key;
      var hadFocus = deviceCard.contains(document.activeElement);
      var devices = (state.status && state.status.devices) || [];
      var device = M.mainDevice(state.status);
      GT4.ui.clear(deviceLines);
      GT4.ui.clear(deviceNotes);
      GT4.ui.clear(deviceRow);
      if (!device) {
        deviceName.textContent = t("wheel.no_wheel_title");
        deviceLines.appendChild(line(t("wheel.no_wheel_text")));
        if (cfg && cfg.device_guid && cfg.device_name) {
          deviceNotes.appendChild(line(t("wheel.configured_missing", { name: cfg.device_name }), "is-warn"));
        }
        return;
      }
      deviceName.textContent = device.name;
      var family = device.family_name ? (device.model ? device.family_name + " · " + device.model : device.family_name) : t("wheel.family_unknown");
      deviceLines.appendChild(line(family, "is-strong"));
      deviceLines.appendChild(line(t("wheel.ids", { id: device.vid_pid || "?", type: t("wheel.sdl_type." + (device.sdl_type || "other")) })));
      deviceLines.appendChild(line(ffbLine(device)));
      if (device.role !== "wheel" && state.status.primary === null && cfg && cfg.device_guid) {
        deviceNotes.appendChild(line(t("wheel.configured_missing", { name: cfg.device_name || cfg.device_guid }), "is-warn"));
      }
      (device.notes || []).forEach(function (note) {
        deviceNotes.appendChild(line(t(M.noteKey(note)), "is-warn"));
      });
      if (device.force_feedback !== false) {
        deviceNotes.appendChild(line(device.software ? t("wheel.set_900", { software: device.software }) : t("wheel.set_900_generic"), "is-tip"));
      }
      // Several devices, or the configured wheel is not the one shown: choose the wheel the game drives.
      var choices = devices.filter(function (one) { return one.role !== "auxiliary" || (cfg && cfg.device_guid === one.guid); });
      if (choices.length > 1 || (cfg && cfg.device_guid && !state.status.configured_connected)) {
        var values = choices.map(function (one) { return { value: one.guid, label: one.name }; });
        if (cfg && cfg.device_guid && !state.status.configured_connected) {
          values.unshift({ value: cfg.device_guid, label: t("wheel.missing_choice", { name: cfg.device_name || "?" }), disabled: true });
        }
        var current = cfg && cfg.device_guid ? cfg.device_guid : device.guid;
        ROWS["wheel-device"] = { key: "wheel.device_guid", toFile: function (value) { return value; } };
        var row = { id: "wheel-device", control: "dropdown", label: t("wheel.drive_device"), help: t("wheel.drive_device_help"), values: values, current: current };
        var context = Object.assign({}, rowContext, {
          commit: function (id, value) {
            var chosen = devices.filter(function (one) { return one.guid === value; })[0];
            return setting("wheel.device_guid", value, id).then(function () {
              return setting("wheel.device_name", chosen ? chosen.name : "", id);
            });
          }
        });
        var node = GT4.settingsRows.rowNode(context, Object.assign({ disabled: locked() }, row));
        if (node) {
          deviceRow.appendChild(node);
        }
        if (hadFocus) {
          var again = deviceRow.querySelector("[data-setting=\"wheel-device\"]");
          if (again) {
            again.focus({ preventScroll: true });
          }
        }
      }
      var aux = devices.filter(function (one) { return one.role === "auxiliary"; });
      if (aux.length) {
        deviceNotes.appendChild(line(t("wheel.also_connected", { names: aux.map(function (one) { return one.name; }).join(", ") })));
      }
    }

    function paintBindings() {
      var cfg = config();
      drawing.setBindings(function (id) {
        var sources = M.sourcesOf(cfg, id);
        var first = sources[0] || "";
        var label = first ? sourceText(first) + (sources.length > 1 ? " +" + (sources.length - 1) : "") : t("wheel.unbound");
        if (id === "steering" && first && cfg) {
          label = sourceText(first) + " · " + cfg.steering.rotation_degrees + "°";
        }
        return {
          text: label,
          unbound: !first,
          aux: sources.some(function (source) { return (M.parseSource(source) || {}).guid; }),
          title: controlName(id) + ": " + label
        };
      });
      drawing.setLockText(cfg && cfg.steering.source ? t("wheel.lock", { half: Math.round(cfg.steering.rotation_degrees / 2) }) : "");
    }

    function paintCard(focusId) {
      var id = state.selected;
      var cfg = config();
      GT4.ui.clear(cardGlyph);
      cardGlyph.appendChild(GT4.wheelSvg.glyph(id));
      cardName.textContent = controlName(id);
      cardSub.textContent = controlSub(id);
      GT4.ui.clear(cardSources);
      var sources = M.sourcesOf(cfg, id);
      if (!sources.length) {
        cardSources.appendChild(h("span", { class: "xchip kind-none", text: "?" }));
        cardSources.appendChild(h("span", { class: "cx-src-t", text: t("wheel.unbound") }));
      }
      sources.forEach(function (source) {
        var device = deviceNameOf(source);
        cardSources.appendChild(h("span", { class: "xchip wh-chip" + (device ? " is-aux" : ""), text: sourceText(source), title: device ? t("wheel.on_device", { name: device }) : "" }));
      });
      var isPedal = M.PEDALS.indexOf(id) >= 0;
      meter.hidden = !isPedal;
      var rows = [];
      if (cfg && id === "steering") {
        ROWS["wheel-rotation"] = { key: "steering.rotation_degrees", toFile: function (value) { return Math.round(Number(value)); } };
        rows.push({ id: "wheel-rotation", control: "slider", label: t("wheel.rotation"), help: t("wheel.rotation_help"),
          values: { min: 180, max: 1080, step: 10, unit: "°" }, current: cfg.steering.rotation_degrees,
          note: M.rotationWarning(cfg.steering.rotation_degrees) ? t("wheel.rotation_warning") : null });
        rows.push(toggleRow("wheel-steer-invert", "steering.invert", t("wheel.steer_invert"), t("wheel.steer_invert_help"), cfg.steering.invert));
      }
      if (cfg && isPedal) {
        var pedal = cfg[id];
        rows.push(choiceRow("wheel-" + id + "-mode", id + ".mode", t("wheel.pedal_mode"), t("wheel.pedal_mode_help"),
          ((state.view && state.view.pedal_modes) || []).map(function (mode) { return { value: mode, label: t("wheel.mode." + mode) }; }), pedal.mode));
        rows.push(percentRow("wheel-" + id + "-deadzone", id + ".deadzone", t("wheel.deadzone"), t("wheel.deadzone_help"), 0, 45, 1, function () { return pedal.deadzone; }));
        rows.push(percentRow("wheel-" + id + "-saturation", id + ".saturation", t("wheel.saturation"), t("wheel.saturation_help"), 20, 100, 1, function () { return pedal.saturation; }));
        rows.push(choiceRow("wheel-" + id + "-curve", id + ".curve", t("wheel.curve"), t("wheel.curve_help"),
          ((state.view && state.view.curves) || []).map(function (curve) { return { value: curve, label: t("choice.curve." + curve) }; }), pedal.curve));
        meterDz.style.setProperty("--w", (pedal.deadzone * 100).toFixed(1) + "%");
        meterSat.style.setProperty("--l", (pedal.saturation * 100).toFixed(1) + "%");
      }
      rowNodes(rows, cardRows);
      paintBindState();
      if (focusId && panel.contains(document.activeElement) === false) {
        var target = panel.querySelector("[data-setting=\"" + focusId + "\"]");
        if (target) {
          target.focus({ preventScroll: true });
        }
      } else if (focusId) {
        var again = panel.querySelector("[data-setting=\"" + focusId + "\"]");
        if (again && again !== document.activeElement) {
          again.focus({ preventScroll: true });
        }
      }
    }

    // The parts that follow wheel-status (the game running, a device coming or going), without drawing any row again:
    // a row drawn again would drop the pad's focus.
    function paintBindState() {
      var sources = M.sourcesOf(config(), state.selected);
      bindButton.disabled = locked() || gameRunning();
      clearButton.disabled = locked() || !sources.length;
      var pointer = GT4.inputMode && GT4.inputMode() === "mouse";
      cardHint.textContent = gameRunning() ? t("wheel.game_running") : locked() ? "" : pointer ? t("wheel.click_again") : t("wheel.press_a");
    }

    function paintTestState() {
      // Never disabled while it runs: a focused button that is disabled loses the pad's focus. A second press waits.
      testButton.disabled = !state.testing && (gameRunning() || !M.mainDevice(state.status));
      testButton.textContent = state.testing ? t("wheel.ffb.testing") : t("wheel.ffb.test");
      if (gameRunning() && !state.testing) {
        testResult.textContent = t("wheel.ffb.test_game");
      }
    }

    function paintFfb(focusId) {
      var cfg = config();
      if (!cfg) {
        return;
      }
      rowNodes([
        toggleRow("wheel-ffb-enabled", "ffb.enabled", t("wheel.ffb.enabled"), t("wheel.ffb.enabled_help"), cfg.ffb.enabled),
        percentRow("wheel-ffb-strength", "ffb.strength", t("wheel.ffb.strength"), t("wheel.ffb.strength_help"), 0, 200, 5, function () { return cfg.ffb.strength; }),
        percentRow("wheel-ffb-damper", "ffb.damper", t("wheel.ffb.damper"), t("wheel.ffb.damper_help"), 0, 100, 5, function () { return cfg.ffb.damper; }),
        toggleRow("wheel-ffb-invert", "ffb.invert", t("wheel.ffb.invert"), t("wheel.ffb.invert_help"), cfg.ffb.invert)
      ], ffbRows);
      if (focusId && focusId.indexOf("wheel-ffb-") === 0) {
        var target = ffbRows.querySelector("[data-setting=\"" + focusId + "\"]");
        if (target) {
          target.focus({ preventScroll: true });
        }
      }
      paintTestState();
    }

    function paintReceived() {
      var found = M.received(state.session);
      receivedValue.textContent = t("wheel.received." + found.value);
      receivedValue.className = "wh-received-v is-" + found.value;
      var check = state.session || {};
      var summary = check.summary || {};
      receivedReason.textContent = t("wheel.received_reason." + found.reason, {
        played: summary.game_forces_played || 0,
        commands: summary.game_force_commands || 0,
        error: check.refused || check.no_ffb || ""
      });
    }

    function paintNotice() {
      var file = state.view && state.view.file;
      var text = "";
      if (file && file.state === "migrated") {
        text = t("wheel.file.migrated");
      } else if (file && file.state === "refused") {
        text = t("wheel.file.refused", { detail: file.detail });
      } else if (file && file.state === "newer") {
        text = t("wheel.file.newer");
      }
      notice.hidden = !text;
      notice.textContent = text;
    }

    function paint(focusId) {
      paintEnabled(focusId);
      paintStatus();
      paintDevice();
      paintBindings();
      paintCard(focusId);
      paintFfb(focusId);
      paintReceived();
      paintNotice();
    }

    function apply(view, focusId) {
      state.view = view;
      if (view && view.status && view.status.devices && (!state.status || !state.status.devices)) {
        state.status = view.status;
      }
      paint(focusId);
    }

    var reads = GT4.settingsModel.readGate();
    function load(focusId) {
      var ticket = reads.ticket();
      return call("get_wheel_config").then(function (view) {
        if (reads.isLatest(ticket)) {
          apply(view, focusId);
        }
      }, function (err) {
        GT4.ui.toast(GT4.errorText(err), true);
      });
    }

    function select(id) {
      if (!drawing.items[id]) {
        return;
      }
      state.selected = id;
      remembered.selected = id;
      drawing.setSelected(id);
      paintCard();
      paintHints();
    }

    // ---- live ---------------------------------------------------------------------------------------------------------
    GT4.backend.listen("wheel-status", function (payload) {
      if (drawing.svg.isConnected && payload) {
        state.status = payload;
        if (!payload.devices || !payload.devices.length) {
          state.live = [];
        }
        paintStatus();
        paintDevice();
        paintBindState();
        paintTestState();
      }
    });
    GT4.backend.listen("wheel-live", function (payload) {
      if (drawing.svg.isConnected && payload) {
        state.live = payload.devices || [];
      }
    });

    function liveState() {
      var cfg = config();
      if (!cfg) {
        return { degrees: 0, steerPosition: 0, steerBound: false, pedals: {}, down: {} };
      }
      var steering = M.steeringLive(cfg.steering, state.live, primary());
      var pedals = {};
      M.PEDALS.forEach(function (id) {
        pedals[id] = M.pedalLive(cfg[id], state.live, primary());
      });
      return {
        degrees: steering.degrees,
        steerPosition: steering.position,
        steerBound: steering.bound && steering.connected,
        pedals: pedals,
        down: M.pressedSlots(cfg, state.live, primary())
      };
    }

    function tick() {
      var live = liveState();
      drawing.setLive(live);
      var reading = live.pedals[state.selected];
      if (reading) {
        meterRaw.style.setProperty("--w", (reading.travel * 100).toFixed(1) + "%");
        meterOut.style.setProperty("--w", (reading.output * 100).toFixed(1) + "%");
        meterValue.textContent = reading.bound ? Math.round(reading.output * 100) + "%" : "—";
      }
    }

    var lastTick = 0;
    var watchdog = null;
    var keepalive = null;
    function alive() {
      if (drawing.svg.isConnected) {
        return true;
      }
      root.clearInterval(watchdog);
      root.clearInterval(keepalive);
      call("wheel_watch", { on: false }).catch(function () {});
      return false;
    }
    function frame() {
      if (alive()) {
        lastTick = Date.now();
        tick();
        root.requestAnimationFrame(frame);
      }
    }
    root.requestAnimationFrame(frame);
    watchdog = root.setInterval(function () {
      if (alive() && Date.now() - lastTick > 90) {
        lastTick = Date.now();
        tick();
      }
    }, 100);
    function watch() {
      if (!document.hidden) {
        call("wheel_watch", { on: true }).then(function (payload) {
          if (payload && drawing.svg.isConnected && (!state.status || !state.status.devices)) {
            state.status = payload;
            paintStatus();
            paintDevice();
          }
        }, function () {});
      }
    }
    keepalive = root.setInterval(function () {
      if (alive()) {
        watch();
      }
    }, KEEPALIVE_MS);
    watch();

    // ---- footer -------------------------------------------------------------------------------------------------------
    function onDrawing() {
      var active = document.activeElement;
      return !!(active && active.closest && active.closest(".wh-item"));
    }
    function paintHints() {
      if (state.capture) {
        return; // a capture or the wizard owns the footer
      }
      var tabs = { key: "LB RB", label: t("common.tabs"), run: function () { if (GT4.hooks.tabs) { GT4.hooks.tabs(1); } } };
      var back = { key: "B", label: t("common.back"), run: function () { GT4.back(); } };
      var setup = { key: "X", label: t("wheel.hint.setup"), run: function () { startWizard(); } };
      if (onDrawing() || !content.contains(document.activeElement)) {
        host.hints([
          { key: "A", label: t("wheel.hint.bind"), run: function () { bind(state.selected); } },
          { key: "Y", label: t("wheel.hint.clear"), run: function () { clearOne(state.selected); } },
          setup, back, tabs
        ], { replace: true });
      } else {
        host.hints([
          { key: "A", label: t("controls.select_hint"), run: function () { GT4.nav.activate(); } },
          setup, back, tabs
        ], { replace: true });
      }
    }
    content.addEventListener("focusin", function (event) {
      var item = event.target && event.target.closest ? event.target.closest(".wh-item") : null;
      if (item) {
        select(item.getAttribute("data-control"));
      }
      paintHints();
    });
    Object.keys(drawing.items).forEach(function (id) {
      drawing.items[id].addEventListener("keydown", function (event) {
        if (event.key === "Enter" || event.key === " ") {
          event.preventDefault();
          onPick(id, "activate");
        }
      });
    });

    // ---- picking and binding ----------------------------------------------------------------------------------------
    function onPick(id, how) {
      if (state.capture) {
        return;
      }
      var item = drawing.items[id];
      if (how === "activate") {
        select(id);
        bind(id);
        return;
      }
      var again = id === state.selected && document.activeElement === item;
      select(id);
      if (item && document.activeElement !== item) {
        item.focus({ preventScroll: true });
      }
      if (again) {
        bind(id);
      }
    }

    function svgNode(tag, attrs, parent) {
      var node = document.createElementNS("http://www.w3.org/2000/svg", tag);
      Object.keys(attrs).forEach(function (key) { node.setAttribute(key, String(attrs[key])); });
      if (parent) {
        parent.appendChild(node);
      }
      return node;
    }

    // The capture overlay of one control (and of the wizard's steps): a ring, the control's glyph, the prompt.
    function captureBody(id, prompt, counting) {
      var ring = svgNode("svg", { viewBox: "0 0 100 100", class: "cx-ring" + (counting ? "" : " is-waiting"), "aria-hidden": "true" });
      svgNode("circle", { class: "track", cx: 50, cy: 50, r: 46 }, ring);
      var arc = svgNode("circle", { class: "arc", cx: 50, cy: 50, r: 46, pathLength: 100, "stroke-dasharray": counting ? "100 100" : "18 82",
        "stroke-dashoffset": 0, transform: "rotate(-90 50 50)" }, ring);
      var seconds = h("span", { class: "cx-count", text: counting ? String(BIND_SECONDS) : "" });
      var glyph = h("span", { class: "cx-capture-glyph" }, [GT4.wheelSvg.glyph(id)]);
      var text = h("p", { class: "modal-text wh-prompt", text: prompt });
      var sub = h("p", { class: "cx-capture-sub wh-capture-sub", text: "" });
      var message = h("p", { class: "cx-capture-msg", role: "status", text: "" });
      var body = h("div", { class: "cx-capture" }, [
        h("div", { class: "cx-capture-art" }, [ring, glyph, seconds]),
        h("div", { class: "cx-capture-text" }, [text, sub, message])
      ]);
      return { body: body, arc: arc, seconds: seconds, glyph: glyph, text: text, sub: sub, message: message };
    }

    function promptFor(id, phase) {
      if (id === "steering") {
        return phase === "right" ? t("wheel.prompt.steer_right") : t("wheel.prompt.steer_left");
      }
      if (M.PEDALS.indexOf(id) >= 0) {
        return t("wheel.prompt." + id);
      }
      return t("wheel.prompt.button", { control: controlName(id) });
    }

    // Asks Rust for the next control the player moves. Resolves with the capture, or null when cancelled.
    function capture(target, phase, axis) {
      var args = { target: target };
      if (phase) {
        args.phase = phase;
      }
      if (axis) {
        args.axis = axis;
      }
      return call("capture_wheel_binding", args);
    }

    // After a capture: bind it (a steering axis also sets the direction: turning left must read negative).
    function assignCaptured(target, found, invert) {
      var args = { target: target, source: found.source };
      if (found.mode) {
        args.mode = found.mode;
      }
      return call("assign_wheel_binding", args).then(function (view) {
        apply(view);
        if (target === "steering" && typeof invert === "boolean" && config() && config().steering.invert !== invert) {
          return call("set_wheel_setting", { key: "steering.invert", value: invert }).then(function (next) {
            apply(next);
            return view;
          });
        }
        return view;
      });
    }

    function bind(id) {
      if (state.capture || !drawing.items[id]) {
        return;
      }
      if (locked()) {
        GT4.ui.toast(t("wheel.file.newer"), true);
        return;
      }
      if (gameRunning()) {
        GT4.ui.toast(t("wheel.game_running"), true);
        return;
      }
      select(id);
      var parts = captureBody(id, promptFor(id, "left"), true);
      parts.sub.textContent = t("wheel.capture_cancels");
      var job = { done: false, started: Date.now(), timer: null, dialog: null };
      state.capture = job;
      function stop(reason, detail) {
        if (job.done) {
          return;
        }
        job.done = true;
        root.clearInterval(job.timer);
        GT4.capturing = false;
        GT4.cancelCapture = null;
        GT4.captureKey = null;
        state.capture = null;
        drawing.setCapturing(null);
        call("cancel_wheel_capture").catch(function () {});
        job.dialog.close();
        paintHints();
        if (reason === "timeout") {
          GT4.ui.toast(t("wheel.no_press"));
        } else if (reason === "error") {
          GT4.ui.toast(GT4.errorText(detail), true);
        } else if (reason === "found") {
          var invert = id === "steering" && typeof detail.value === "number" && typeof detail.rest === "number"
            ? M.steeringInvert(detail.value, detail.rest) : undefined;
          assignCaptured(id, detail, invert).then(function (view) {
            var taken = (view && view.taken_from) || [];
            drawing.flash([id].concat(taken));
            GT4.ui.toast(taken.length
              ? t("wheel.moved", { control: controlName(id), source: sourceText(detail.source), other: taken.map(controlName).join(", ") })
              : t("wheel.bound", { control: controlName(id), source: sourceText(detail.source) }));
          }, function (err) {
            GT4.ui.toast(GT4.errorText(err), true);
            load();
          });
        }
      }
      GT4.capturing = true;
      GT4.cancelCapture = function () { stop("cancel"); };
      GT4.captureKey = function (event) {
        if (event.key === "Escape") {
          stop("cancel");
        }
        return true;
      };
      drawing.setCapturing(id);
      job.dialog = GT4.ui.openDialog({
        title: t("wheel.bind_title", { control: controlName(id) }),
        body: parts.body,
        wide: true,
        dismissable: false,
        buttons: [{ label: t("common.cancel"), closes: false, run: function () { stop("cancel"); } }]
      });
      job.dialog.el.classList.add("is-capture");
      GT4.ui.setHints([{ key: "B", label: t("common.cancel"), run: function () { stop("cancel"); } }]);
      job.timer = root.setInterval(function () {
        if (dev.capture === "hold") {
          parts.seconds.textContent = "6";
          parts.arc.setAttribute("stroke-dashoffset", "25");
          return;
        }
        var left = Math.max(0, BIND_SECONDS - (Date.now() - job.started) / 1000);
        parts.seconds.textContent = String(Math.ceil(left));
        parts.arc.setAttribute("stroke-dashoffset", String(Math.round((1 - left / BIND_SECONDS) * 1000) / 10));
        if (left <= 0) {
          stop("timeout");
        }
      }, 100);
      capture(id, id === "steering" ? "left" : null).then(function (found) {
        if (found && found.source) {
          stop("found", found);
        }
      }, function (err) {
        if (!job.done && String(GT4.errorText(err)).indexOf("cancel") < 0) {
          stop("error", err);
        }
      });
    }

    function clearOne(id) {
      if (state.capture || locked() || !drawing.items[id]) {
        return;
      }
      if (!M.sourcesOf(config(), id).length) {
        return;
      }
      call("reset_wheel_binding", { target: id, clear: true }).then(function (view) {
        apply(view);
        drawing.flash([id]);
        GT4.ui.toast(t("wheel.cleared", { control: controlName(id) }));
      }, function (err) {
        GT4.ui.toast(GT4.errorText(err), true);
      });
    }

    // ---- the set-up wizard --------------------------------------------------------------------------------------------
    function startWizard() {
      if (state.capture) {
        return;
      }
      if (locked()) {
        GT4.ui.toast(t("wheel.file.newer"), true);
        return;
      }
      if (gameRunning()) {
        GT4.ui.toast(t("wheel.game_running"), true);
        return;
      }
      var device = M.mainDevice(state.status);
      if (!device) {
        GT4.ui.toast(t("wheel.connect_first"), true);
        return;
      }
      var family = device.family || "generic";
      var familyName = device.family_name || t("wheel.family_generic");
      GT4.ui.openDialog({
        title: t("wheel.wizard.title", { name: device.name }),
        text: t("wheel.wizard.intro", { family: familyName }),
        buttons: [
          { label: t("common.cancel") },
          {
            label: t("wheel.wizard.start"),
            primary: true,
            run: function () {
              call("apply_wheel_preset", { name: family, guid: device.guid }).then(function (view) {
                apply(view);
                runWizard(device);
              }, function (err) {
                GT4.ui.toast(GT4.errorText(err), true);
              });
            }
          }
        ],
        onClose: function () { root.setTimeout(paintHints, 0); }
      });
      GT4.ui.setHints([
        { key: "A", label: t("controls.choose"), run: function () { GT4.nav.activate(); } },
        { key: "B", label: t("common.cancel"), run: function () { GT4.back(); } }
      ]);
    }

    function runWizard(device) {
      var steps = M.WIZARD;
      var job = { index: 0, done: false, dialog: null, left: null, waiting: null, generation: 0 };
      state.capture = job;
      var parts = captureBody(steps[0].target, "", false);
      var progress = h("div", { class: "wh-progress" }, [h("i", { class: "wh-progress-fill" })]);
      var stepText = h("p", { class: "wh-step", text: "" });
      var now = h("p", { class: "wh-now", text: "" });
      parts.body.querySelector(".cx-capture-text").appendChild(now);
      var body = h("div", { class: "wh-wizard" }, [stepText, progress, parts.body]);
      var padDriven = !device.is_gamepad;
      function finish(completed) {
        if (job.done) {
          return;
        }
        job.done = true;
        GT4.capturing = false;
        GT4.cancelCapture = null;
        GT4.captureKey = null;
        GT4.capturePad = null;
        state.capture = null;
        drawing.setCapturing(null);
        call("cancel_wheel_capture").catch(function () {});
        job.dialog.close();
        if (completed) {
          summary();
        } else {
          paintHints();
          GT4.ui.toast(t("wheel.wizard.stopped"));
        }
      }
      function show(index) {
        job.index = index;
        job.generation += 1;
        var generation = job.generation;
        if (index >= steps.length) {
          finish(true);
          return;
        }
        var step = steps[index];
        stepText.textContent = t("wheel.wizard.step", { n: index + 1, total: steps.length });
        progress.firstChild.style.setProperty("--w", ((index / steps.length) * 100).toFixed(1) + "%");
        GT4.ui.clear(parts.glyph);
        parts.glyph.appendChild(GT4.wheelSvg.glyph(step.target));
        parts.text.textContent = step.kind === "button" ? t("wheel.prompt.button", { control: controlName(step.target) }) : t("wheel.prompt." + (step.kind === "steering" ? "steer_" + step.phase : step.target));
        var sub = step.kind === "button" ? controlSub(step.target) : step.kind === "steering" ? t("wheel.wizard.feet") : (step.target === "clutch" ? t("wheel.sub.clutch") : "");
        parts.sub.textContent = sub;
        parts.message.textContent = "";
        var current = M.sourcesOf(config(), step.target);
        now.textContent = current.length && step.phase !== "right" ? t("wheel.wizard.now", { source: sourceText(current[0]) }) : "";
        drawing.setCapturing(step.target);
        select(step.target);
        var axis = step.phase === "right" && job.left ? job.left.source : null;
        if (step.phase === "right" && !job.left) {
          show(index + 1); // the left turn was skipped: the right turn has nothing to confirm
          return;
        }
        capture(step.target, step.phase || null, axis).then(function (found) {
          if (job.done || generation !== job.generation || !found || !found.source) {
            return;
          }
          parts.message.textContent = "";
          now.textContent = t("wheel.wizard.got", { source: sourceText(found.source) });
          if (step.phase === "left") {
            job.left = found;
            root.setTimeout(function () { if (generation === job.generation) { show(index + 1); } }, 350);
            return;
          }
          var invert;
          var source = found;
          if (step.phase === "right") {
            invert = M.steeringInvert(job.left.value, found.value);
            source = job.left;
          }
          assignCaptured(step.target, source, invert).then(function (view) {
            var taken = (view && view.taken_from) || [];
            drawing.flash([step.target].concat(taken));
            root.setTimeout(function () { if (generation === job.generation) { show(index + 1); } }, 350);
          }, function (err) {
            parts.message.textContent = GT4.errorText(err);
          });
        }, function (err) {
          if (!job.done && generation === job.generation && String(GT4.errorText(err)).indexOf("cancel") < 0) {
            parts.message.textContent = GT4.errorText(err);
          }
        });
      }
      function skip() {
        call("cancel_wheel_capture").catch(function () {});
        show(job.index + 1);
      }
      function back() {
        call("cancel_wheel_capture").catch(function () {});
        var previous = Math.max(0, job.index - 1);
        if (steps[previous].phase === "right") {
          previous = Math.max(0, previous - 1);
        }
        show(previous);
      }
      GT4.capturing = true;
      GT4.cancelCapture = skip; // B skips the step
      GT4.captureKey = function (event) {
        if (event.key === "Escape") {
          skip();
        } else if (event.key === "Backspace") {
          back();
        }
        return true;
      };
      // A pad that is not the wheel: X goes back a step, Y stops the wizard (B skips).
      GT4.capturePad = padDriven ? function (name) {
        if (name === "x") {
          back();
        } else if (name === "y") {
          finish(false);
        }
      } : null;
      job.dialog = GT4.ui.openDialog({
        title: t("wheel.wizard.title", { name: device.name }),
        body: body,
        wide: true,
        dismissable: false,
        buttons: [
          { label: t("wheel.wizard.stop"), closes: false, run: function () { finish(false); } },
          { label: t("wheel.wizard.back"), closes: false, run: function () { back(); } },
          { label: t("wheel.wizard.skip"), closes: false, primary: true, run: function () { skip(); } }
        ]
      });
      job.dialog.el.classList.add("is-capture", "is-wizard");
      GT4.ui.setHints([
        { key: "B", label: t("wheel.wizard.skip"), run: skip },
        padDriven ? { key: "X", label: t("wheel.wizard.back"), run: back } : null,
        padDriven ? { key: "Y", label: t("wheel.wizard.stop"), run: function () { finish(false); } } : null
      ].filter(Boolean));
      show(dev.wizardStep || 0);
    }

    function summary() {
      var count = M.boundCount(config());
      var turnOn = config() && !config().enabled
        ? call("set_wheel_setting", { key: "wheel.enabled", value: true }).then(apply, function () {})
        : Promise.resolve();
      turnOn.then(function () {
        GT4.ui.openDialog({
          title: t("wheel.wizard.done_title"),
          text: t("wheel.wizard.done_text", { bound: count.bound, total: count.total }),
          buttons: [
            { label: t("wheel.presets.save"), run: function () { root.setTimeout(savePreset, 0); } },
            { label: t("wheel.wizard.done"), primary: true }
          ],
          onClose: function () { root.setTimeout(paintHints, 0); }
        });
      });
    }

    // ---- force feedback ---------------------------------------------------------------------------------------------
    function testForce() {
      if (state.testing || gameRunning()) {
        return;
      }
      state.testing = true;
      paintTestState();
      testResult.textContent = t("wheel.ffb.test_help");
      call("test_wheel_ffb").then(function (result) {
        state.testing = false;
        state.test = result;
        var effects = ["constant", "spring", "damper", "friction", "gain", "autocenter"].filter(function (key) {
          return result.features && result.features[key];
        }).map(function (key) { return t("wheel.effect." + key); }).join(", ") || t("wheel.effect.none");
        if (!result.has_ffb) {
          testResult.textContent = t("wheel.ffb.test_no_ffb", { name: result.device_name, error: result.error || "" });
        } else if (!result.played) {
          testResult.textContent = t("wheel.ffb.test_no_constant", { name: result.device_name, effects: effects });
        } else {
          testResult.textContent = t("wheel.ffb.test_played", { name: result.device_name, effects: effects });
        }
        paintTestState();
      }, function (err) {
        state.testing = false;
        testResult.textContent = GT4.errorText(err);
        paintTestState();
      });
    }

    // ---- presets ------------------------------------------------------------------------------------------------------
    function defaultPresetName() {
      var device = M.mainDevice(state.status);
      var name = (device && (device.model ? (device.family_name || "").split(" ")[0] + " " + device.model : device.name)) || (config() && config().device_name) || "Wheel";
      return name.replace(/[^A-Za-z0-9 _().+\-]/g, " ").replace(/\s+/g, " ").trim().slice(0, 40) || "Wheel";
    }

    function savePreset() {
      var input = h("input", { class: "modal-input", type: "text", maxlength: "48", value: defaultPresetName(), "aria-label": t("wheel.presets.name") });
      GT4.ui.openDialog({
        title: t("wheel.presets.save_title"),
        text: t("wheel.presets.save_text"),
        body: input,
        buttons: [
          { label: t("common.cancel") },
          {
            label: t("wheel.presets.save_button"),
            primary: true,
            run: function () {
              call("save_wheel_preset", { name: input.value }).then(function () {
                GT4.ui.toast(t("wheel.presets.saved", { name: input.value.trim() }));
                load();
              }, function (err) {
                GT4.ui.toast(GT4.errorText(err), true);
              });
            }
          }
        ],
        onClose: function () { root.setTimeout(paintHints, 0); }
      });
    }

    function loadPreset() {
      call("list_wheel_presets").then(function (list) {
        var presets = list || [];
        var dialog = null;
        var rows = presets.map(function (preset) {
          return h("div", { class: "wh-preset-row" }, [
            h("div", { class: "wh-preset-name" }, [
              h("span", { text: preset.name }),
              h("small", { text: preset.problem ? t("wheel.presets.broken") : (preset.device_name || "") })
            ]),
            h("button", {
              class: "btn small primary",
              type: "button",
              disabled: !!preset.problem,
              on: { click: function () {
                dialog.close();
                call("apply_wheel_preset", { name: preset.name, saved: true }).then(function (view) {
                  apply(view);
                  GT4.ui.toast(t("wheel.presets.loaded", { name: preset.name }));
                }, function (err) { GT4.ui.toast(GT4.errorText(err), true); });
              } }
            }, t("wheel.presets.load_button")),
            h("button", {
              class: "btn small danger",
              type: "button",
              on: { click: function () {
                dialog.close();
                confirmDeletePreset(preset.name);
              } }
            }, t("common.delete"))
          ]);
        });
        dialog = GT4.ui.openDialog({
          title: t("wheel.presets.load_title"),
          text: presets.length ? "" : t("wheel.presets.none"),
          body: h("div", { class: "wh-preset-list" }, rows),
          buttons: [{ label: t("common.cancel") }],
          onClose: function () { root.setTimeout(paintHints, 0); }
        });
      }, function (err) {
        GT4.ui.toast(GT4.errorText(err), true);
      });
    }

    function confirmDeletePreset(name) {
      GT4.ui.openDialog({
        title: t("wheel.presets.delete_title", { name: name }),
        buttons: [
          { label: t("common.cancel") },
          {
            label: t("common.delete"),
            primary: true,
            danger: true,
            run: function () {
              call("delete_wheel_preset", { name: name }).then(function () {
                GT4.ui.toast(t("wheel.presets.deleted", { name: name }));
              }, function (err) { GT4.ui.toast(GT4.errorText(err), true); });
            }
          }
        ],
        onClose: function () { root.setTimeout(paintHints, 0); }
      });
    }

    function confirmReset() {
      if (state.capture || locked()) {
        return;
      }
      GT4.ui.openDialog({
        title: t("wheel.presets.reset_title"),
        text: t("wheel.presets.reset_text"),
        buttons: [
          { label: t("common.cancel") },
          {
            label: t("wheel.presets.reset"),
            primary: true,
            danger: true,
            run: function () {
              call("reset_wheel_config").then(function (view) {
                apply(view);
                GT4.ui.toast(t("wheel.presets.reset_done"));
              }, function (err) { GT4.ui.toast(GT4.errorText(err), true); });
            }
          }
        ],
        onClose: function () { root.setTimeout(paintHints, 0); }
      });
    }

    // ---- hooks and start ------------------------------------------------------------------------------------------------
    host.setHooks({
      x: function () { startWizard(); },
      y: function () { clearOne(state.selected); }
    });
    // A enters on the control selected last (the steering at first). Drawing the page does not move focus.
    host.entry(function () { return drawing.items[state.selected] || drawing.items.steering; });

    drawing.setSelected(state.selected);
    paintHints();
    call("get_wheel_session_check").then(function (check) {
      state.session = check;
      paintReceived();
    }, function () {});
    return load().then(function () {
      if (dev.panel === "ffb") {
        panel.scrollTop = ffbCard.offsetTop - panel.offsetTop - 4;
      }
      if (dev.wizard) {
        root.setTimeout(function () {
          var device = M.mainDevice(state.status);
          if (device) {
            runWizard(device);
          }
        }, 300);
      }
    });
  };
})(window);
