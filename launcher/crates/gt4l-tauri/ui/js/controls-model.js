/*
 * Pure rules of the Controls page (no DOM; tested in dev/nav-test.html):
 *
 *   GT4.controlsModel.BUTTONS               the sixteen DS2 buttons (ids of the controls file), names, symbols
 *   GT4.controlsModel.padSources(pad)       a Gamepad-like {buttons, axes} -> {"xinput:A": 0..1, ...}
 *   GT4.controlsModel.liveState(live, controls, keysDown)
 *                                           what each DS2 button, stick and trigger shows right now; live is the
 *                                           pad-live event {sources, sticks} from the Rust pad service (SDL3)
 *   GT4.controlsModel.padLive(pad)          dev mock only: a Gamepad-like object -> the pad-live shape
 *   GT4.controlsModel.KEYBOARD_LAYOUT       the fixed modern keyboard (TASK-076), several keys per DS2 button
 *   GT4.controlsModel.KEYBOARD_ACTIONS      race actions of those keys (nitro, gears, look back, camera, pause)
 *   GT4.controlsModel.triggerOutput(raw, deadzone, curve)
 *                                           the pressure the game computes from a trigger (native/controls.cpp)
 *   GT4.controlsModel.keyFromEvent(event)   a key event -> {name} | {reserved} | {unsupported} | {cancel}
 *   GT4.controlsModel.conflict(controls, id, source)
 *                                           null, {same: true}, or {holder, previous} when another button has it
 *   GT4.controlsModel.label(source)         {family, kind, text, long} for chips and the panel
 *   GT4.controlsModel.padSummary(pads)      dev mock only: the first connected Gamepad-like object and how many there are
 *
 * The launcher reads pads through SDL3 in Rust (launcher/crates/gt4l-platform/src/pad.rs): the page gets `pad-live`
 * events whose sources are the controls-file names (xinput:A ... xinput:RT, pressure 0..1). The Web Gamepad API
 * (navigator.getGamepads) is not used here; the dev mock turns Gamepad-like objects into the same shape.
 */
(function (root) {
  "use strict";
  var GT4 = (root.GT4 = root.GT4 || {});

  var BUTTONS = [
    { id: "l2", name: "L2", symbol: "L2" },
    { id: "l1", name: "L1", symbol: "L1" },
    { id: "r2", name: "R2", symbol: "R2" },
    { id: "r1", name: "R1", symbol: "R1" },
    { id: "dpad_up", name: "D-pad up", symbol: "↑" },
    { id: "dpad_left", name: "D-pad left", symbol: "←" },
    { id: "dpad_right", name: "D-pad right", symbol: "→" },
    { id: "dpad_down", name: "D-pad down", symbol: "↓" },
    { id: "select", name: "Select", symbol: "SELECT" },
    { id: "start", name: "Start", symbol: "START" },
    { id: "triangle", name: "Triangle", symbol: "△" },
    { id: "square", name: "Square", symbol: "□" },
    { id: "circle", name: "Circle", symbol: "○" },
    { id: "cross", name: "Cross", symbol: "✕" },
    { id: "l3", name: "L3", symbol: "L3" },
    { id: "r3", name: "R3", symbol: "R3" }
  ];

  var STANDARD_TO_XINPUT = ["A", "B", "X", "Y", "LB", "RB", "LT", "RT", "Back", "Start", "LThumb", "RThumb",
    "DpadUp", "DpadDown", "DpadLeft", "DpadRight"];
  var TRIGGER_INDEX = { 6: true, 7: true };

  // The game's defaults (docs/CONTROLS.md): trigger 30 of 255, both sticks 3277 of 32767 (10%, TASK-147).
  var TRIGGER_DEFAULT_DEADZONE = 30 / 255;

  function find(id) {
    for (var i = 0; i < BUTTONS.length; i++) {
      if (BUTTONS[i].id === id) {
        return BUTTONS[i];
      }
    }
    return null;
  }

  // "Cross" -> "✕ Cross"; shoulders and the rest keep their plain name.
  function titleOf(id) {
    var button = find(id);
    if (!button) {
      return id;
    }
    var face = ["triangle", "square", "circle", "cross"].indexOf(id) >= 0;
    return face ? button.symbol + " " + button.name : button.name;
  }

  function sourceBase(source) {
    return String(source || "").split("|")[0].trim();
  }

  function family(source) {
    var base = sourceBase(source);
    var colon = base.indexOf(":");
    return colon < 0 ? base : base.slice(0, colon);
  }

  function sourceName(source) {
    var base = sourceBase(source);
    var colon = base.indexOf(":");
    return colon < 0 ? base : base.slice(colon + 1);
  }

  // Options after "|": {deadzone: 0.05, curve: "cubic"}.
  function sourceOptions(source) {
    var options = {};
    var parts = String(source || "").split("|");
    if (parts.length < 2) {
      return options;
    }
    parts[1].trim().split(/\s+/).forEach(function (pair) {
      var eq = pair.indexOf("=");
      if (eq > 0) {
        options[pair.slice(0, eq)] = pair.slice(eq + 1);
      }
    });
    return options;
  }

  // TASK-161: the pressure byte that is the game's 100% at the end of a trigger's travel (native/controls.cpp
  // pressure_full_for): with the full trigger range on, the binding's `range_full` or the whole byte (255); off, its `range`
  // or the game's own 100%, 45 on R2 (throttle) and 84 on L2 (brake), 255 on any other button.
  function triggerFull(id, sources, fullRange) {
    var source = (sources || []).filter(function (candidate) {
      var name = sourceName(candidate);
      return family(candidate) === "xinput" && (name === "LT" || name === "RT");
    })[0];
    var options = source ? sourceOptions(source) : {};
    if (fullRange) {
      return options.range_full ? Number(options.range_full) : 255;
    }
    if (options.range) {
      return Number(options.range);
    }
    return id === "r2" ? 45 : id === "l2" ? 84 : 255;
  }

  // The pressure the game computes from a trigger reading (native/controls.cpp): raw and deadzone are fractions of
  // full travel (255). Linear: (v - d) * 255 / (255 - d), integer division; otherwise curve(n) with n = (v - d) /
  // (255 - d). Returns 0..1.
  function triggerOutput(raw, deadzone, curve) {
    var v = Math.round(Math.min(1, Math.max(0, Number(raw) || 0)) * 255);
    var d = Math.round(Math.min(0.99, Math.max(0, deadzone === undefined ? TRIGGER_DEFAULT_DEADZONE : Number(deadzone))) * 255);
    if (v <= d) {
      return 0;
    }
    if (!curve || curve === "linear") {
      return Math.floor(((v - d) * 255) / (255 - d)) / 255;
    }
    var n = (v - d) / (255 - d);
    var shaped = n;
    if (curve === "quadratic") {
      shaped = n * n;
    } else if (curve === "cubic") {
      shaped = n * n * n;
    } else if (curve === "s_curve" || curve === "scurve" || curve === "s-curve") {
      shaped = n * n * (3 - 2 * n);
    } else if (curve === "smooth_power" || curve === "smooth") {
      shaped = n * Math.sqrt(n);
    } else if (curve === "sublinear" || curve === "root") {
      shaped = Math.sqrt(n);
    } else if (curve === "logarithmic" || curve === "log") {
      shaped = Math.log(1 + 9 * n) / Math.log(10);
    }
    return Math.floor(Math.min(1, Math.max(0, shaped)) * 255) / 255;
  }

  // Dev mock only: {"xinput:A": 1, "xinput:LT": 0.6, ...} for every standard button of a Gamepad-like object.
  function padSources(pad) {
    var out = {};
    if (!pad || !pad.buttons) {
      return out;
    }
    for (var i = 0; i < STANDARD_TO_XINPUT.length; i++) {
      var button = pad.buttons[i];
      if (!button) {
        continue;
      }
      var value = TRIGGER_INDEX[i] ? Number(button.value) || 0 : (button.pressed ? 1 : 0);
      if (TRIGGER_INDEX[i] && button.pressed && value === 0) {
        value = 1;
      }
      out["xinput:" + STANDARD_TO_XINPUT[i]] = Math.min(1, Math.max(0, value));
    }
    return out;
  }

  function axis(pad, index) {
    var value = pad && pad.axes ? Number(pad.axes[index]) || 0 : 0;
    return Math.min(1, Math.max(-1, value));
  }

   // Dev mock only: a Gamepad-like object -> {sources, sticks}, the shape of the pad-live event.
  function padLive(pad) {
    var sources = padSources(pad);
    return {
      sources: sources,
      sticks: {
        left: { x: axis(pad, 0), y: axis(pad, 1) },
        right: { x: axis(pad, 2), y: axis(pad, 3) }
      }
    };
  }

  // One stick of a pad-live shape, each axis clamped to -1..1 (y positive down).
  function stickOf(live, side) {
    var part = live && live.sticks && live.sticks[side] ? live.sticks[side] : {};
    var clamp = function (value) { return Math.min(1, Math.max(-1, Number(value) || 0)); };
    return { x: clamp(part.x), y: clamp(part.y) };
  }

  // What the drawing shows this frame. live = {sources, sticks} (pad-live); controls = get_controls().controls;
  // keysDown = {"W": true}. A button also lights when a key of the fixed keyboard layout is down.
  // buttons[id] = {value (raw 0..1, the fill of L2/R2), output (what the game reads, 0..1), pressed}
  // sticks = {left: {x, y}, right: {x, y}} raw, y positive down; sources = the pad readings by source.
  function liveState(live, controls, keysDown) {
    var sources = Object.assign({}, (live && live.sources) || {});
    Object.keys(keysDown || {}).forEach(function (name) {
      if (keysDown[name]) {
        sources["key:" + name] = 1;
      }
    });
    var buttons = {};
    (controls || []).forEach(function (control) {
      var value = 0;
      var output = 0;
      var fixedKeys = KEYBOARD_LAYOUT[control.id] || [];
      var fixedDown = fixedKeys.some(function (name) { return !!(keysDown && keysDown[name]); });
      (control.sources || []).forEach(function (source) {
        var raw = sources[sourceBase(source)] || 0;
        if (!raw) {
          return;
        }
        var name = sourceName(source);
        var shaped = raw;
        if (family(source) === "xinput" && (name === "LT" || name === "RT")) {
          var options = sourceOptions(source);
          shaped = triggerOutput(raw, options.deadzone === undefined ? undefined : Number(options.deadzone), options.curve);
        }
        value = Math.max(value, raw);
        output = Math.max(output, shaped);
      });
      if (fixedDown) {
        value = 1;
        output = 1;
      }
      buttons[control.id] = { value: value, output: output, pressed: output > 0 };
    });
    return {
      buttons: buttons,
      sticks: {
        left: stickOf(live, "left"),
        right: stickOf(live, "right")
      },
      sources: sources
    };
  }

  // The fixed keyboard layout (TASK-076, read-only on the Controls page): several keys per DS2 button. Names use the
  // controls-file key names. Same table as the game (docs/CONTROLS.md, slice 210); P is Start (owner, 2026-10-21).
  var KEYBOARD_LAYOUT = {
    dpad_up: ["Up", "E"],
    dpad_down: ["Down"],
    dpad_left: ["A", "Left"],
    dpad_right: ["D", "Right"],
    cross: ["W", "Enter"],
    square: ["S"],
    circle: ["Space"],
    triangle: ["Backspace", "Esc"],
    r1: ["Shift"],
    r2: ["R"],
    l2: ["F"],
    l1: ["Q"],
    select: ["C", "Tab"],
    start: ["P"]
  };
  // What those buttons do in a race (GT4's own controls; to confirm in game by the owner).
  var KEYBOARD_ACTIONS = [
    { name: "Nitro", keys: ["Shift"], button: "R1" },
    { name: "Gear up", keys: ["R"], button: "R2" },
    { name: "Gear down", keys: ["F"], button: "L2" },
    { name: "Look back", keys: ["Q"], button: "L1" },
    { name: "Camera", keys: ["C", "Tab"], button: "Select" },
    { name: "Pause", keys: ["P"], button: "Start" }
  ];

  var RESERVED_KEYS = { F7: true, F8: true, F9: true, F11: true };
  var CODE_NAMES = {
    ArrowUp: "Up", ArrowDown: "Down", ArrowLeft: "Left", ArrowRight: "Right",
    Enter: "Enter", NumpadEnter: "Enter", Backspace: "Backspace", Tab: "Tab", Space: "Space",
    ShiftLeft: "Shift", ShiftRight: "Shift", ControlLeft: "Ctrl", ControlRight: "Ctrl", AltLeft: "Alt", AltRight: "Alt",
    Insert: "Insert", Delete: "Delete", Home: "Home", End: "End", PageUp: "PageUp", PageDown: "PageDown"
  };

  // A keydown -> the key name of the controls file. Esc cancels a capture; F7, F8, F9 and F11 belong to the game
  // window; keys the file has no name for are refused.
  function keyFromEvent(event) {
    var code = (event && event.code) || "";
    var key = (event && event.key) || "";
    if (code === "Escape" || key === "Escape") {
      return { cancel: true, name: "Esc" };
    }
    var match = /^Key([A-Z])$/.exec(code) || /^Digit([0-9])$/.exec(code);
    if (match) {
      return { name: match[1] };
    }
    match = /^Numpad([0-9])$/.exec(code);
    if (match) {
      return { name: "Num" + match[1] };
    }
    match = /^F([0-9]{1,2})$/.exec(code);
    if (match && Number(match[1]) >= 1 && Number(match[1]) <= 12) {
      return RESERVED_KEYS[code] ? { reserved: code } : { name: code };
    }
    if (CODE_NAMES[code]) {
      return { name: CODE_NAMES[code] };
    }
    if (!code && key.length === 1 && /[a-z0-9]/i.test(key)) {
      return { name: key.toUpperCase() };
    }
    return { unsupported: key || code || "?" };
  }

  // Before binding `source` to button `id`: null when it is free, {same: true} when the button already has it, else
  // {holder, previous}: the other button that has it, and the source of the same family `id` would give it in a swap
  // (null: the other button would lose it).
  function conflict(controls, id, source) {
    var base = sourceBase(source);
    var list = controls || [];
    function has(control) {
      return (control.sources || []).some(function (existing) { return sourceBase(existing) === base; });
    }
    var own = list.filter(function (control) { return control.id === id; })[0] || { sources: [] };
    if (has(own)) {
      return { same: true };
    }
    var holder = list.filter(function (control) { return control.id !== id && has(control); })[0];
    if (!holder) {
      return null;
    }
    var previous = (own.sources || []).filter(function (existing) { return family(existing) === family(base); })[0];
    return { holder: holder.id, previous: previous ? sourceBase(previous) : null };
  }

  var XINPUT_LABELS = {
    A: ["face", "A", "A button"], B: ["face", "B", "B button"], X: ["face", "X", "X button"], Y: ["face", "Y", "Y button"],
    LB: ["bumper", "LB", "Left bumper"], RB: ["bumper", "RB", "Right bumper"],
    LT: ["trigger", "LT", "Left trigger"], RT: ["trigger", "RT", "Right trigger"],
    Back: ["menu", "Back", "Back button"], Start: ["menu", "Start", "Start button"],
    LThumb: ["thumb", "LS", "Left stick click"], RThumb: ["thumb", "RS", "Right stick click"],
    DpadUp: ["dpad", "↑", "D-pad up"], DpadDown: ["dpad", "↓", "D-pad down"],
    DpadLeft: ["dpad", "←", "D-pad left"], DpadRight: ["dpad", "→", "D-pad right"]
  };
  var KEY_TEXT = { Backspace: "Bksp", Up: "↑", Down: "↓", Left: "←", Right: "→", PageUp: "PgUp",
    PageDown: "PgDn", Delete: "Del", Insert: "Ins" };

  // The player-facing long name in the interface language (locale key controls.source.*); the English text above
  // is the fallback when the key or GT4.t is missing (the pure tests run without locales).
  function longText(key, english, params) {
    if (GT4.t && GT4.i18n && GT4.i18n.has && GT4.i18n.has(key)) {
      return GT4.t(key, params);
    }
    return english;
  }

  // Chip text for a source: {family: "xinput"|"key"|"", kind, text, long, dir?}. Unknown and empty sources read "-".
  function label(source) {
    if (!source) {
      return { family: "", kind: "none", text: "–", long: longText("controls.source.none", "Not bound") };
    }
    var name = sourceName(source);
    if (family(source) === "xinput" && XINPUT_LABELS[name]) {
      var entry = XINPUT_LABELS[name];
      var out = { family: "xinput", kind: entry[0], text: entry[1], long: longText("controls.source." + name, entry[2]) };
      if (entry[0] === "dpad") {
        out.dir = name.slice(4).toLowerCase();
      }
      return out;
    }
    if (family(source) === "key") {
      return { family: "key", kind: "key", text: KEY_TEXT[name] || name,
        long: longText("controls.source.key", name + " key", { key: name }) };
    }
    return { family: family(source), kind: "none", text: name || "?", long: sourceBase(source) };
  }

  // "Xbox 360 Controller (XInput STANDARD GAMEPAD)" -> "Xbox 360 Controller".
  function cleanPadName(id) {
    var text = String(id || "").replace(/\s*\([^)]*\)\s*/g, " ").replace(/\s+/g, " ").trim();
    return text || longText("controls.view_controller", "Controller");
  }

  // Dev mock only: pads = a list of Gamepad-like objects. The first connected one is the one the game reads.
  function padSummary(pads) {
    var connected = [];
    Array.prototype.forEach.call(pads || [], function (pad) {
      if (pad && pad.connected !== false) {
        connected.push(pad);
      }
    });
    if (!connected.length) {
      return { connected: false, count: 0, name: "", index: -1, pad: null };
    }
    return { connected: true, count: connected.length, name: cleanPadName(connected[0].id), index: connected[0].index,
      pad: connected[0] };
  }

  GT4.controlsModel = {
    BUTTONS: BUTTONS,
    STANDARD_TO_XINPUT: STANDARD_TO_XINPUT,
    TRIGGER_DEFAULT_DEADZONE: TRIGGER_DEFAULT_DEADZONE,
    find: find,
    titleOf: titleOf,
    sourceBase: sourceBase,
    family: family,
    sourceName: sourceName,
    sourceOptions: sourceOptions,
    triggerOutput: triggerOutput,
    triggerFull: triggerFull,
    padSources: padSources,
    padLive: padLive,
    liveState: liveState,
    KEYBOARD_LAYOUT: KEYBOARD_LAYOUT,
    KEYBOARD_ACTIONS: KEYBOARD_ACTIONS,
    keyFromEvent: keyFromEvent,
    conflict: conflict,
    label: label,
    cleanPadName: cleanPadName,
    padSummary: padSummary
  };
})(window);
