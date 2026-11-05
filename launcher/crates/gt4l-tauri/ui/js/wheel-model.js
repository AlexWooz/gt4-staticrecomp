/*
 * Pure rules of the Wheel page (TASK-181), tested in dev/nav-test.js. No DOM, no text: the page translates.
 *
 * The game's contract (native/wheel.cpp, gt4l_core::wheel): the host wheel feeds a virtual Logitech Driving Force Pro
 * (DFP). The [buttons] slots keep the DualShock 2 names of the 1.2.x files; what each presses on the DFP is in CONTROLS.
 * The live values follow the game's own formulas: the steering angle is the position (-1..1) times half the
 * lock-to-lock range (a 900-degree wheel reads -450..450), a pedal's byte comes from its mode, dead zone, saturation
 * and curve (read_pedal), a button source is pressed with the game's thresholds (evaluate_digital_source).
 *
 * Based on cbfd8b8's GT4.wheelModel (JulioHeroZ, with luanantony): readSourceAxis, sourcePressed and the pedal reading.
 */
(function (root) {
  "use strict";
  var GT4 = (root.GT4 = root.GT4 || {});

  // The DFP controls the page shows, in the set-up wizard's order: `slot` is the wheel.toml key it writes, `dfp` the
  // control (the locale keys are wheel.control.<dfp>), `glyph` the DualShock 2 glyph the panel draws.
  var CONTROLS = [
    { dfp: "cross", slot: "cross", glyph: "cross" },
    { dfp: "circle", slot: "circle", glyph: "circle" },
    { dfp: "square", slot: "square", glyph: "square" },
    { dfp: "triangle", slot: "triangle", glyph: "triangle" },
    { dfp: "gear_up", slot: "r2", glyph: "r1" },
    { dfp: "gear_down", slot: "l2", glyph: "l1" },
    { dfp: "r2", slot: "r1", glyph: "r2" },
    { dfp: "l2", slot: "l1", glyph: "l2" },
    { dfp: "r3", slot: "r3", glyph: "r3" },
    { dfp: "l3", slot: "l3", glyph: "l3" },
    { dfp: "start", slot: "start", glyph: "start" },
    { dfp: "select", slot: "select", glyph: "select" },
    { dfp: "dpad_up", slot: "dpad_up", glyph: "dpad_up" },
    { dfp: "dpad_right", slot: "dpad_right", glyph: "dpad_right" },
    { dfp: "dpad_down", slot: "dpad_down", glyph: "dpad_down" },
    { dfp: "dpad_left", slot: "dpad_left", glyph: "dpad_left" }
  ];
  var PEDALS = ["throttle", "brake", "clutch", "handbrake"];

  // The wizard: steering (fully left, then fully right), the three pedals, then every DFP button. Each step can be
  // skipped. `target` is what capture_wheel_binding and assign_wheel_binding take.
  var WIZARD = [
    { id: "steer_left", target: "steering", kind: "steering", phase: "left" },
    { id: "steer_right", target: "steering", kind: "steering", phase: "right" },
    { id: "throttle", target: "throttle", kind: "pedal" },
    { id: "brake", target: "brake", kind: "pedal" },
    { id: "clutch", target: "clutch", kind: "pedal" }
  ].concat(CONTROLS.map(function (control) {
    return { id: control.dfp, target: control.slot, kind: "button", dfp: control.dfp };
  }));

  function controlOfSlot(slot) {
    return CONTROLS.filter(function (control) { return control.slot === slot; })[0] || null;
  }
  function controlOfDfp(dfp) {
    return CONTROLS.filter(function (control) { return control.dfp === dfp; })[0] || null;
  }

  // "dev:<guid>:axis:3+" -> {guid, kind: "axis"|"axis+"|"axis-"|"btn"|"hat", index, dir}; null for empty or invalid.
  function parseSource(text) {
    var rest = String(text || "").trim();
    if (!rest || rest === "none") {
      return null;
    }
    var guid = "";
    if (rest.indexOf("dev:") === 0) {
      var colon = rest.indexOf(":", 4);
      if (colon < 0) {
        return null;
      }
      guid = rest.slice(4, colon).toLowerCase();
      if (!/^[0-9a-f]{32}$/.test(guid)) {
        return null;
      }
      rest = rest.slice(colon + 1);
    }
    var match = /^axis:(\d+)([+-]?)$/.exec(rest);
    if (match) {
      var axis = Number(match[1]);
      return axis > 31 ? null : { guid: guid, kind: match[2] ? "axis" + match[2] : "axis", index: axis, dir: "" };
    }
    match = /^btn:(\d+)$/.exec(rest);
    if (match) {
      return Number(match[1]) > 127 ? null : { guid: guid, kind: "btn", index: Number(match[1]), dir: "" };
    }
    match = /^hat:(\d+):(up|right|down|left)$/.exec(rest);
    if (match) {
      return Number(match[1]) > 7 ? null : { guid: guid, kind: "hat", index: Number(match[1]), dir: match[2] };
    }
    return null;
  }

  // What a chip says about a source: {kind, number (1-based, as players count buttons), dir, sign, aux}. The page
  // turns it into "Button 5", "Axis 3", "Hat up"; `aux` marks a source on another device than the wheel.
  function describe(text) {
    var source = parseSource(text);
    if (!source) {
      return null;
    }
    var kind = source.kind === "btn" ? "button" : source.kind === "hat" ? "hat" : "axis";
    return {
      kind: kind,
      number: source.index + 1,
      dir: source.dir,
      sign: source.kind === "axis+" ? "+" : source.kind === "axis-" ? "-" : "",
      aux: !!source.guid
    };
  }

  // The live reading of the device a source names: the one with its GUID (only when exactly one has it), else the
  // device the game drives (`primary`, an SDL id). `live` is the wheel-live payload's devices.
  function deviceOf(source, live, primary) {
    var devices = live || [];
    if (source.guid) {
      var same = devices.filter(function (device) { return String(device.guid || "").toLowerCase() === source.guid; });
      return same.length === 1 ? same[0] : null;
    }
    if (primary === null || primary === undefined) {
      return null;
    }
    return devices.filter(function (device) { return device.id === primary; })[0] || null;
  }

  // The raw value (-32768..32767) of an axis source, or null.
  function axisValue(text, live, primary) {
    var source = parseSource(text);
    if (!source || source.kind.indexOf("axis") !== 0) {
      return null;
    }
    var device = deviceOf(source, live, primary);
    if (!device || !device.axes || source.index >= device.axes.length) {
      return null;
    }
    return device.axes[source.index];
  }

  var HAT_BITS = { up: 1, right: 2, down: 4, left: 8 };

  // A source pressed with the game's rules: a button, a hat direction, axis+ above +16384, axis- below -16384, a plain
  // axis above 0 or past 20000 either way.
  function sourcePressed(text, live, primary) {
    var source = parseSource(text);
    if (!source) {
      return false;
    }
    var device = deviceOf(source, live, primary);
    if (!device) {
      return false;
    }
    if (source.kind === "btn") {
      return (device.pressed || []).indexOf(source.index) >= 0;
    }
    if (source.kind === "hat") {
      var bits = (device.hats || [])[source.index] || 0;
      return (bits & HAT_BITS[source.dir]) !== 0;
    }
    var value = (device.axes || [])[source.index];
    if (typeof value !== "number") {
      return false;
    }
    if (source.kind === "axis+") {
      return value > 16384;
    }
    if (source.kind === "axis-") {
      return value < -16384;
    }
    return value > 0 || Math.abs(value) > 20000;
  }

  // A pedal axis's travel 0..1 before calibration (normalize_pedal_axis).
  function pedalTravel(mode, raw) {
    switch (mode) {
      case "inverted": return Math.min(1, Math.max(0, (32767 - raw) / 65535));
      case "normal": return Math.min(1, Math.max(0, (raw + 32768) / 65535));
      case "positive":
      case "combined_positive": return raw <= 0 ? 0 : Math.min(1, raw / 32767);
      case "combined_negative": return raw >= 0 ? 0 : Math.min(1, -raw / 32768);
      default: return 0;
    }
  }

  // The byte the game sends for a pedal axis (read_pedal): 0 inside the dead zone, else 1..255.
  function pedalByte(pedal, raw) {
    var travel = pedalTravel(pedal.mode, raw);
    var deadzone = Math.min(0.45, Math.max(0, pedal.deadzone));
    var saturation = Math.min(1, Math.max(Math.min(1, deadzone + 0.05), pedal.saturation));
    if (travel <= deadzone) {
      return 0;
    }
    var normalized = Math.min(1, Math.max(0, (travel - deadzone) / (saturation - deadzone)));
    var curved = pedal.curve === "quadratic" ? normalized * normalized : pedal.curve === "cubic" ? normalized * normalized * normalized : normalized;
    return Math.min(255, Math.max(1, Math.round(Math.min(1, Math.max(0, curved)) * 255)));
  }

  // A pedal now: {bound, connected, travel (0..1 raw), output (0..1 the game's value), digital}.
  function pedalLive(pedal, live, primary) {
    var empty = { bound: false, connected: false, travel: 0, output: 0, digital: false };
    if (!pedal || !pedal.source) {
      return empty;
    }
    var source = parseSource(pedal.source);
    if (!source) {
      return empty;
    }
    if (source.kind !== "axis") {
      var down = sourcePressed(pedal.source, live, primary);
      return { bound: true, connected: !!deviceOf(source, live, primary), travel: down ? 1 : 0, output: down ? 1 : 0, digital: true };
    }
    var raw = axisValue(pedal.source, live, primary);
    if (raw === null) {
      return { bound: true, connected: false, travel: 0, output: 0, digital: false };
    }
    return { bound: true, connected: true, travel: pedalTravel(pedal.mode, raw), output: pedalByte(pedal, raw) / 255, digital: false };
  }

  // The wheel's angle in degrees: the position times half the lock-to-lock range (the angle the game's DFP gets).
  function steeringDegrees(steering, raw) {
    var position = raw < 0 ? raw / 32768 : raw / 32767;
    if (steering.invert) {
      position = -position;
    }
    var range = Math.min(1080, Math.max(180, steering.rotation_degrees || 900));
    return position * range / 2;
  }

  // The steering now: {bound, connected, degrees, position (-1..1)}.
  function steeringLive(steering, live, primary) {
    if (!steering || !steering.source) {
      return { bound: false, connected: false, degrees: 0, position: 0 };
    }
    var raw = axisValue(steering.source, live, primary);
    if (raw === null) {
      return { bound: true, connected: false, degrees: 0, position: 0 };
    }
    var degrees = steeringDegrees(steering, raw);
    var half = Math.min(1080, Math.max(180, steering.rotation_degrees || 900)) / 2;
    return { bound: true, connected: true, degrees: degrees, position: degrees / half };
  }

  // "+90°", "−45°", "0°": the readout of an angle (whole degrees, a real minus sign).
  function angleText(degrees) {
    var whole = Math.round(degrees);
    if (whole === 0) {
      return "0°";
    }
    return (whole > 0 ? "+" : "−") + Math.abs(whole) + "°";
  }

  // The sources of a control: steering and pedals have one, a button slot a list.
  function sourcesOf(config, target) {
    if (!config) {
      return [];
    }
    if (target === "steering") {
      return config.steering && config.steering.source ? [config.steering.source] : [];
    }
    if (PEDALS.indexOf(target) >= 0) {
      return config[target] && config[target].source ? [config[target].source] : [];
    }
    return ((config.buttons || {})[target] || []).filter(Boolean);
  }

  // Which DFP controls are down now (slot -> true), the handbrake pressing Circle past half travel as the game does.
  function pressedSlots(config, live, primary) {
    var down = {};
    if (!config) {
      return down;
    }
    CONTROLS.forEach(function (control) {
      down[control.slot] = sourcesOf(config, control.slot).some(function (source) {
        return sourcePressed(source, live, primary);
      });
    });
    return down;
  }

  // How many of the wizard's controls have a binding (the summary at the end).
  function boundCount(config) {
    var targets = ["steering", "throttle", "brake", "clutch"].concat(CONTROLS.map(function (control) { return control.slot; }));
    return {
      bound: targets.filter(function (target) { return sourcesOf(config, target).length > 0; }).length,
      total: targets.length
    };
  }

  // The steering direction from the wizard's two steps: turning left gave `left`, turning right gave `right` (raw values
  // of the same axis). The game expects left negative; a wheel that reads the other way needs `invert`.
  function steeringInvert(left, right) {
    return left > right;
  }

  // The 900-degree rule: GT4 turns its wheel 900 degrees; a different range turns the car more or less than the wheel.
  function rotationWarning(degrees) {
    return Number(degrees) !== 900;
  }

  // The keys of a wheel-status note (gt4l_core::wheel_identity::Note).
  function noteKey(note) {
    return "wheel.note." + note;
  }

  // The "force feedback received" line from get_wheel_session_check: {value: yes|no|unknown, reason}.
  function received(check) {
    if (!check || !check.found) {
      return { value: "unknown", reason: "no_session" };
    }
    var value = check.received === "yes" || check.received === "no" ? check.received : "unknown";
    return { value: value, reason: check.reason || "no_wheel_lines" };
  }

  // The device the page talks about: the one the game drives, else the first wheel, else the first device.
  function mainDevice(status) {
    var devices = (status && status.devices) || [];
    var primary = status ? status.primary : null;
    return devices.filter(function (device) { return device.id === primary; })[0] ||
      devices.filter(function (device) { return device.role !== "auxiliary"; })[0] || devices[0] || null;
  }

  GT4.wheelModel = {
    CONTROLS: CONTROLS,
    PEDALS: PEDALS,
    WIZARD: WIZARD,
    controlOfSlot: controlOfSlot,
    controlOfDfp: controlOfDfp,
    parseSource: parseSource,
    describe: describe,
    axisValue: axisValue,
    sourcePressed: sourcePressed,
    pedalTravel: pedalTravel,
    pedalByte: pedalByte,
    pedalLive: pedalLive,
    steeringDegrees: steeringDegrees,
    steeringLive: steeringLive,
    angleText: angleText,
    sourcesOf: sourcesOf,
    pressedSlots: pressedSlots,
    boundCount: boundCount,
    steeringInvert: steeringInvert,
    rotationWarning: rotationWarning,
    noteKey: noteKey,
    received: received,
    mainDevice: mainDevice
  };
})(window);
