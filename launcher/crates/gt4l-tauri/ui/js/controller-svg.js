/*
 * GT4Recomp's own vector drawing of a DualShock 2 style pad (no Sony artwork, photos, logos or marks): the
 * silhouette is two cubic-curve halves mirrored about x = 500, the controls are plain geometry, the face symbols are
 * strokes. Everything is built with createElementNS and styled by classes and SVG attributes (CSP: no inline styles).
 *
 * GT4.controllerSvg({onPick(id, how)}) -> {
 *   svg, groups, badges,
 *   setSelected(id), setCapturing(id|null),
 *   setBadges(controls, mode)      mode "xinput": the chip beside each button (get_controls().controls, its xinput
 *                                  source); mode "key": every key of control.keys (the fixed keyboard layout), one
 *                                  chip each, side by side
 *   setLive(state)                 GT4.controlsModel.liveState(...): pressed buttons, L2/R2 fill, stick positions
 *   flash(ids)                     a short highlight on changed chips
 * }
 * onPick(id, "pointer") comes from a click or tap anywhere on the drawing (the nearest button or chip wins, so every
 * button has a large target); onPick(id, "activate") from A, Enter or Space on a focused button.
 *
 * GT4.controllerSvg.glyph(id) -> a small standalone <svg> of one DS2 button (panel and capture overlay).
 *
 * Coordinates are viewBox units, about 0.6 CSS px each at 1280 x 720 and 1.2 at 1920 x 1080.
 */
(function (root) {
  "use strict";
  var GT4 = (root.GT4 = root.GT4 || {});
  var NS = "http://www.w3.org/2000/svg";

  var VIEW = { x: 24, y: 30, w: 952, h: 632 };

  // Left half of the body outline, from the top centre (500, 174) down the left side to the bottom centre (500, 498).
  // Each entry is one cubic segment: control 1, control 2, end point.
  var BODY_START = [500, 174];
  var BODY_LEFT = [
    [450, 174, 392, 164, 352, 150],
    [312, 136, 232, 128, 176, 134],
    [134, 139, 104, 160, 90, 192],
    [70, 240, 54, 318, 47, 394],
    [40, 472, 40, 546, 64, 594],
    [87, 638, 154, 652, 198, 621],
    [230, 598, 246, 556, 270, 524],
    [298, 498, 342, 490, 400, 492],
    [440, 493, 474, 498, 500, 498]
  ];

  var DPAD = { x: 220, y: 290 };
  var FACE = { x: 780, y: 290, offset: 62, r: 28 };
  var STICK = { y: 412, left: 360, right: 640, well: 64, cap: 38, travel: 24 };

  // Every control: its centre (for the nearest-button pick), the chip position and the point its leader starts from.
  var LAYOUT = {
    l2: { x: 248, y: 82, badge: [116, 82], lead: [180, 82] },
    l1: { x: 248, y: 124, badge: [106, 128], lead: [170, 128] },
    r2: { x: 752, y: 82, badge: [884, 82], lead: [820, 82] },
    r1: { x: 752, y: 124, badge: [894, 128], lead: [830, 128] },
    dpad_up: { x: DPAD.x, y: DPAD.y - 42, badge: [DPAD.x, 186], lead: [DPAD.x, DPAD.y - 66] },
    dpad_down: { x: DPAD.x, y: DPAD.y + 42, badge: [DPAD.x, 394], lead: [DPAD.x, DPAD.y + 66] },
    dpad_left: { x: DPAD.x - 42, y: DPAD.y, badge: [114, DPAD.y], lead: [DPAD.x - 66, DPAD.y] },
    dpad_right: { x: DPAD.x + 42, y: DPAD.y, badge: [326, DPAD.y], lead: [DPAD.x + 66, DPAD.y] },
    triangle: { x: FACE.x, y: FACE.y - FACE.offset, badge: [FACE.x, 164], lead: [FACE.x, FACE.y - FACE.offset - FACE.r - 2] },
    cross: { x: FACE.x, y: FACE.y + FACE.offset, badge: [FACE.x, 416], lead: [FACE.x, FACE.y + FACE.offset + FACE.r + 2] },
    square: { x: FACE.x - FACE.offset, y: FACE.y, badge: [654, FACE.y], lead: [FACE.x - FACE.offset - FACE.r - 2, FACE.y] },
    circle: { x: FACE.x + FACE.offset, y: FACE.y, badge: [906, FACE.y], lead: [FACE.x + FACE.offset + FACE.r + 2, FACE.y] },
    select: { x: 412, y: 292, badge: [412, 248], lead: [412, 280] },
    start: { x: 588, y: 292, badge: [588, 248], lead: [588, 278] },
    l3: { x: STICK.left, y: STICK.y, badge: [STICK.left, 544], lead: [STICK.left, STICK.y + STICK.well + 8] },
    r3: { x: STICK.right, y: STICK.y, badge: [STICK.right, 544], lead: [STICK.right, STICK.y + STICK.well + 8] }
  };
  var PICK_RANGE = 120; // a click further than this from every button and chip selects nothing

  var GLYPH_CLASS = { triangle: "g-triangle", circle: "g-circle", cross: "g-cross", square: "g-square" };

  function el(tag, attrs, parent) {
    var node = document.createElementNS(NS, tag);
    Object.keys(attrs || {}).forEach(function (key) {
      node.setAttribute(key, String(attrs[key]));
    });
    if (parent) {
      parent.appendChild(node);
    }
    return node;
  }

  function text(parent, x, y, value, cls) {
    var node = el("text", { x: x, y: y, class: cls }, parent);
    node.textContent = value;
    return node;
  }

  function bodyPath() {
    var d = "M" + BODY_START[0] + " " + BODY_START[1];
    BODY_LEFT.forEach(function (s) {
      d += " C" + s.join(" ");
    });
    // The right half: the same segments mirrored (x -> 1000 - x) and walked backwards, bottom centre to top centre.
    for (var k = BODY_LEFT.length - 1; k >= 0; k--) {
      var s = BODY_LEFT[k];
      var from = k === 0 ? BODY_START : BODY_LEFT[k - 1].slice(4, 6);
      d += " C" + [1000 - s[2], s[3], 1000 - s[0], s[1], 1000 - from[0], from[1]].join(" ");
    }
    return d + " Z";
  }

  function gradient(parent, tag, id, attrs, list) {
    var node = el(tag, Object.assign({ id: id }, attrs), parent);
    list.forEach(function (stop) {
      el("stop", { offset: stop[0], "stop-color": stop[1], "stop-opacity": stop[2] === undefined ? 1 : stop[2] }, node);
    });
    return node;
  }

  function defs(svg) {
    var d = el("defs", {}, svg);
    var down = { x1: 0, y1: 0, x2: 0, y2: 1 };
    gradient(d, "linearGradient", "ds-body", down, [[0, "#3c424c"], [0.45, "#2a2e35"], [1, "#16181c"]]);
    gradient(d, "linearGradient", "ds-rim", down, [[0, "#ffffff", 0.3], [0.28, "#ffffff", 0.06], [1, "#ffffff", 0]]);
    gradient(d, "radialGradient", "ds-sheen", { cx: 0.5, cy: 0.12, r: 0.6 }, [[0, "#ffffff", 0.08], [1, "#ffffff", 0]]);
    gradient(d, "linearGradient", "ds-dish", down, [[0, "#111317"], [0.7, "#1c1f25"], [1, "#262a31"]]);
    gradient(d, "radialGradient", "ds-cap", { cx: 0.5, cy: 0.3, r: 0.75 }, [[0, "#4d535e"], [0.7, "#30343c"], [1, "#23262c"]]);
    gradient(d, "radialGradient", "ds-cap-on", { cx: 0.5, cy: 0.3, r: 0.75 }, [[0, "#7aa4ff"], [0.65, "#3a64cf"], [1, "#22408a"]]);
    gradient(d, "linearGradient", "ds-arm", down, [[0, "#4b515b"], [1, "#2a2e35"]]);
    gradient(d, "linearGradient", "ds-arm-on", down, [[0, "#7aa4ff"], [1, "#2f57bb"]]);
    gradient(d, "linearGradient", "ds-shoulder", down, [[0, "#484e58"], [0.55, "#2c3037"], [1, "#1f2227"]]);
    gradient(d, "linearGradient", "ds-shoulder-on", down, [[0, "#7aa4ff"], [1, "#2a4fae"]]);
    gradient(d, "linearGradient", "ds-fill", { x1: 0, y1: 1, x2: 0, y2: 0 }, [[0, "#2c6bed"], [1, "#9cc0ff"]]);
    gradient(d, "radialGradient", "ds-well", { cx: 0.5, cy: 0.4, r: 0.6 }, [[0, "#0a0b0e"], [0.8, "#14161a"], [1, "#24272e"]]);
    gradient(d, "linearGradient", "ds-collar", down, [[0, "#15171b"], [1, "#3c414a"]]);
    gradient(d, "radialGradient", "ds-stick", { cx: 0.45, cy: 0.32, r: 0.72 }, [[0, "#5b626d"], [0.6, "#34383f"], [1, "#1e2025"]]);
    gradient(d, "radialGradient", "ds-stick-on", { cx: 0.45, cy: 0.32, r: 0.72 }, [[0, "#86acff"], [0.6, "#3b62c4"], [1, "#1d346f"]]);
    gradient(d, "radialGradient", "ds-stick-top", { cx: 0.5, cy: 0.75, r: 0.75 }, [[0, "#41464f"], [1, "#24272d"]]);
    gradient(d, "radialGradient", "ds-led", { cx: 0.5, cy: 0.5, r: 0.5 }, [[0, "#ff6b5e", 0.55], [1, "#ff6b5e", 0]]);

    var shadow = el("filter", { id: "ds-shadow", x: -0.2, y: -0.2, width: 1.4, height: 1.5 }, d);
    el("feGaussianBlur", { in: "SourceAlpha", stdDeviation: 16 }, shadow);
    el("feOffset", { dy: 22, result: "blur" }, shadow);
    el("feFlood", { "flood-color": "#000000", "flood-opacity": 0.6 }, shadow);
    el("feComposite", { in2: "blur", operator: "in" }, shadow);
    return d;
  }

  function hitRect(group, x, y, w, h) {
    el("rect", { class: "hit", x: x, y: y, width: w, height: h }, group);
  }

  function controlGroup(parent, id, extra) {
    var button = GT4.controlsModel.find(id);
    var attrs = {
      class: "pad-ctl",
      "data-control": id,
      "data-focusable": "",
      tabindex: "0",
      role: "button",
      "aria-label": button ? button.name : id
    };
    Object.keys(extra || {}).forEach(function (key) {
      attrs[key] = extra[key];
    });
    var group = el("g", attrs, parent);
    var title = el("title", {}, group);
    title.textContent = button ? button.name : id;
    return group;
  }

  // Shoulder button (L1, L2, R1, R2): a rounded tab. L2 and R2 carry a pressure level that rises from the bottom.
  function shoulder(parent, defsNode, id, x, y, w, h, label, trigger) {
    var group = controlGroup(parent, id);
    hitRect(group, x - 6, y - 12, w + 12, h + 16);
    el("rect", { class: "halo", x: x - 5, y: y - 5, width: w + 10, height: h + 10, rx: 20 }, group);
    var press = el("g", { class: "press" }, group);
    el("rect", { class: "cap shoulder" + (trigger ? " trigger" : ""), x: x, y: y, width: w, height: h, rx: 16 }, press);
    var fill = null;
    if (trigger) {
      var clip = el("clipPath", { id: "ds-clip-" + id }, defsNode);
      el("rect", { x: x, y: y, width: w, height: h, rx: 16 }, clip);
      fill = el("rect", { class: "level", x: x, y: y + h, width: w, height: 0, "clip-path": "url(#ds-clip-" + id + ")" }, press);
    }
    el("rect", { class: "shine", x: x + 14, y: y + 3, width: w - 28, height: 2, rx: 1 }, press);
    var caption = text(press, x + w / 2, y + (trigger ? h / 2 + 7 : 25), label, "label");
    return { group: group, fill: fill, readout: caption, top: y, height: h, label: label };
  }

  function dpadArm(parent, id, angle) {
    var group = controlGroup(parent, id, { transform: "translate(" + DPAD.x + " " + DPAD.y + ") rotate(" + angle + ")" });
    hitRect(group, -27, -74, 54, 70);
    el("path", { class: "halo", d: "M-24 -18 L-24 -58 Q-24 -70 -12 -70 L12 -70 Q24 -70 24 -58 L24 -18 L0 4 Z" }, group);
    var press = el("g", { class: "press" }, group);
    el("path", { class: "cap arm", d: "M-18 -20 L-18 -56 Q-18 -64 -10 -64 L10 -64 Q18 -64 18 -56 L18 -20 L0 -4 Z" }, press);
    el("path", { class: "arm-shine", d: "M-12 -61 L12 -61" }, press);
    el("path", { class: "arrow", d: "M-6 -40 L0 -50 L6 -40 Z" }, press);
    return group;
  }

  function faceGlyph(parent, kind, cx, cy, scale) {
    var s = scale || 1;
    var cls = "glyph " + GLYPH_CLASS[kind];
    if (kind === "triangle") {
      el("path", { class: cls, d: "M" + cx + " " + (cy - 11 * s) + " L" + (cx + 10.5 * s) + " " + (cy + 7.5 * s) +
        " L" + (cx - 10.5 * s) + " " + (cy + 7.5 * s) + " Z" }, parent);
    } else if (kind === "circle") {
      el("circle", { class: cls, cx: cx, cy: cy, r: 10 * s }, parent);
    } else if (kind === "cross") {
      el("path", { class: cls, d: "M" + (cx - 9 * s) + " " + (cy - 9 * s) + " L" + (cx + 9 * s) + " " + (cy + 9 * s) +
        " M" + (cx + 9 * s) + " " + (cy - 9 * s) + " L" + (cx - 9 * s) + " " + (cy + 9 * s) }, parent);
    } else if (kind === "square") {
      el("rect", { class: cls, x: cx - 8.5 * s, y: cy - 8.5 * s, width: 17 * s, height: 17 * s, rx: 1 }, parent);
    }
  }

  function faceButton(parent, id, cx, cy) {
    var group = controlGroup(parent, id);
    el("circle", { class: "hit", cx: cx, cy: cy, r: 38 }, group);
    el("circle", { class: "halo", cx: cx, cy: cy, r: FACE.r + 6 }, group);
    el("circle", { class: "socket", cx: cx, cy: cy + 1.5, r: FACE.r + 2.5 }, group);
    var press = el("g", { class: "press" }, group);
    el("circle", { class: "cap face", cx: cx, cy: cy, r: FACE.r }, press);
    el("path", { class: "shine-arc", d: "M" + (cx - 17) + " " + (cy - 17) + " A24 24 0 0 1 " + (cx + 17) + " " + (cy - 17) }, press);
    faceGlyph(press, id, cx, cy);
    return group;
  }

  function stick(parent, id, cx, label) {
    var cy = STICK.y;
    var group = controlGroup(parent, id);
    el("circle", { class: "hit", cx: cx, cy: cy, r: STICK.well }, group);
    el("circle", { class: "halo", cx: cx, cy: cy, r: STICK.well + 7 }, group);
    var trail = el("line", { class: "trail", x1: cx, y1: cy, x2: cx, y2: cy }, group);
    var cap = el("g", { class: "stick-cap" }, group);
    var press = el("g", { class: "press" }, cap);
    el("circle", { class: "cap stick-base", cx: cx, cy: cy, r: STICK.cap }, press);
    el("circle", { class: "stick-grip", cx: cx, cy: cy + 1.5, r: STICK.cap - 5 }, press);
    el("circle", { class: "stick-top", cx: cx, cy: cy + 2, r: STICK.cap - 11 }, press);
    el("circle", { class: "stick-dot", cx: cx, cy: cy + 2, r: 3.5 }, press);
    text(press, cx, cy + 22, label, "label stick-label");
    return { group: group, cap: cap, trail: trail, cx: cx, cy: cy };
  }

  // A chip beside a button: the XInput control or key that drives it, with a short leader line.
  function badge(parent, id) {
    var spot = LAYOUT[id];
    var group = el("g", { class: "badge", "data-for": id }, parent);
    el("line", { class: "leader", x1: spot.lead[0], y1: spot.lead[1], x2: spot.badge[0], y2: spot.badge[1] }, group);
    var chip = el("g", { class: "chip" }, group);
    return { group: group, chip: chip, chips: [chip], x: spot.badge[0], y: spot.badge[1] };
  }

  // Arrow characters would fall back to a colour emoji font in SVG text, so arrows are drawn as paths.
  var ARROW_TURN = { "↑": 0, "→": 90, "↓": 180, "←": 270 };
  function chipLabel(chip, x, y, value, cls) {
    if (Object.prototype.hasOwnProperty.call(ARROW_TURN, value)) {
      el("path", { class: "chip-arrow", d: "M0 -8 L7 1 L2.5 1 L2.5 8 L-2.5 8 L-2.5 1 L-7 1 Z",
        transform: "translate(" + x + " " + y + ") rotate(" + ARROW_TURN[value] + ")" }, chip);
      return;
    }
    text(chip, x, y + 6.5, value, cls);
  }

  // Draws the chip for a label (GT4.controlsModel.label) centred on (x, y).
  function drawChip(chip, info, x, y) {
    while (chip.firstChild) {
      chip.removeChild(chip.firstChild);
    }
    chip.setAttribute("class", "chip kind-" + info.kind);
    var half = 17;
    if (info.kind === "face" || info.kind === "thumb" || info.kind === "none") {
      el("circle", { class: "chip-bg", cx: x, cy: y, r: half }, chip);
      if (info.kind === "thumb") {
        el("circle", { class: "chip-ring", cx: x, cy: y, r: half - 4 }, chip);
      }
      text(chip, x, y + 6.5, info.text, "chip-text" + (info.kind === "thumb" ? " is-small" : ""));
      return;
    }
    if (info.kind === "dpad") {
      el("rect", { class: "chip-bg", x: x - half, y: y - half, width: half * 2, height: half * 2, rx: 8 }, chip);
      chipLabel(chip, x, y, info.text, "chip-text");
      return;
    }
    var width = info.kind === "key" ? Math.max(36, 18 + info.text.length * 11) : (info.kind === "menu" ? 66 : 46);
    var height = info.kind === "key" ? 32 : 30;
    if (info.kind === "key") {
      el("rect", { class: "chip-key-base", x: x - width / 2, y: y - height / 2 + 4, width: width, height: height, rx: 7 }, chip);
    }
    el("rect", { class: "chip-bg", x: x - width / 2, y: y - height / 2, width: width, height: height, rx: info.kind === "key" ? 7 : 15 }, chip);
    chipLabel(chip, x, y, info.text, "chip-text" + (info.text.length > 3 ? " is-small" : ""));
  }

  function build(options) {
    var onPick = (options && options.onPick) || function () {};
    var svg = el("svg", {
      viewBox: [VIEW.x, VIEW.y, VIEW.w, VIEW.h].join(" "),
      class: "pad-svg",
      role: "group",
      "aria-label": GT4.t("controls.svg_label")
    });
    var defsNode = defs(svg);

    // Shoulders sit behind the body: only their top part shows, as on the pad seen from above.
    var shoulders = el("g", { class: "shoulders" }, svg);
    var parts = {};
    parts.l2 = shoulder(shoulders, defsNode, "l2", 180, 60, 136, 42, "L2", true);
    parts.r2 = shoulder(shoulders, defsNode, "r2", 684, 60, 136, 42, "R2", true);
    parts.l1 = shoulder(shoulders, defsNode, "l1", 170, 104, 156, 50, "L1", false);
    parts.r1 = shoulder(shoulders, defsNode, "r1", 674, 104, 156, 50, "R1", false);

    var d = bodyPath();
    el("path", { class: "body-shadow", d: d, filter: "url(#ds-shadow)" }, svg);
    el("path", { class: "body", d: d }, svg);
    el("path", { class: "body-sheen", d: d }, svg);
    el("path", { class: "body-rim", d: d }, svg);

    // Recessed dishes under the D-pad and the face buttons, and the collars of the sticks.
    el("circle", { class: "dish-lip", cx: DPAD.x, cy: DPAD.y + 2.5, r: 96 }, svg);
    el("circle", { class: "dish-lip", cx: FACE.x, cy: FACE.y + 2.5, r: 100 }, svg);
    el("circle", { class: "dish", cx: DPAD.x, cy: DPAD.y, r: 96 }, svg);
    el("circle", { class: "dish", cx: FACE.x, cy: FACE.y, r: 100 }, svg);
    [STICK.left, STICK.right].forEach(function (cx) {
      el("circle", { class: "collar", cx: cx, cy: STICK.y, r: STICK.well + 9 }, svg);
      el("circle", { class: "well", cx: cx, cy: STICK.y, r: STICK.well }, svg);
    });
    el("circle", { class: "pivot", cx: DPAD.x, cy: DPAD.y, r: 12 }, svg);

    var controls = el("g", { class: "controls" }, svg);
    var groups = {};
    ["dpad_up", "dpad_right", "dpad_down", "dpad_left"].forEach(function (id, index) {
      groups[id] = dpadArm(controls, id, index * 90);
    });
    groups.triangle = faceButton(controls, "triangle", FACE.x, FACE.y - FACE.offset);
    groups.circle = faceButton(controls, "circle", FACE.x + FACE.offset, FACE.y);
    groups.cross = faceButton(controls, "cross", FACE.x, FACE.y + FACE.offset);
    groups.square = faceButton(controls, "square", FACE.x - FACE.offset, FACE.y);

    // Centre: Select (a flat bar), Start (a triangle pointing right), the mode button and its LED. The mode button and
    // the LED are decoration: the game always reads the pad in analog mode.
    var select = controlGroup(controls, "select");
    hitRect(select, 380, 262, 64, 58);
    el("rect", { class: "halo", x: 381, y: 279, width: 62, height: 26, rx: 12 }, select);
    var selectPress = el("g", { class: "press" }, select);
    el("rect", { class: "cap menu", x: 388, y: 285, width: 48, height: 14, rx: 6 }, selectPress);
    groups.select = select;
    var start = controlGroup(controls, "start");
    hitRect(start, 556, 262, 64, 58);
    el("path", { class: "halo", d: "M571 272 L614 292 L571 312 Z" }, start);
    var startPress = el("g", { class: "press" }, start);
    el("path", { class: "cap menu start-tri", d: "M578 281 L603 292 L578 303 Z" }, startPress);
    groups.start = start;
    text(svg, 412, 326, "SELECT", "print");
    text(svg, 588, 326, "START", "print");

    el("circle", { class: "led-glow", cx: 500, cy: 340, r: 14 }, svg);
    el("circle", { class: "led", cx: 500, cy: 340, r: 4.5 }, svg);
    el("circle", { class: "mode-btn", cx: 500, cy: 374, r: 14 }, svg);
    text(svg, 500, 408, "ANALOG", "print");

    var sticks = {
      left: stick(controls, "l3", STICK.left, "L3"),
      right: stick(controls, "r3", STICK.right, "R3")
    };
    groups.l3 = sticks.left.group;
    groups.r3 = sticks.right.group;
    ["l1", "l2", "r1", "r2"].forEach(function (id) {
      groups[id] = parts[id].group;
    });

    var badgeLayer = el("g", { class: "badges" }, svg);
    var badges = {};
    Object.keys(LAYOUT).forEach(function (id) {
      badges[id] = badge(badgeLayer, id);
    });

    // A, Enter and Space on a focused button. nav.js dispatches a synthetic click on SVG groups (they have no click()).
    Object.keys(groups).forEach(function (id) {
      var group = groups[id];
      group.addEventListener("click", function (event) {
        event.stopPropagation();
        onPick(id, event.isTrusted ? "pointer" : "activate");
      });
      group.addEventListener("keydown", function (event) {
        if (event.key === "Enter" || event.key === " ") {
          // The key that starts a rebind must not reach the capture as well (nav.js listens on the document).
          event.preventDefault();
          event.stopPropagation();
          onPick(id, "activate");
        }
      });
    });

    // Clicks and taps on the drawing: the nearest button or chip within PICK_RANGE.
    function nearest(clientX, clientY) {
      var matrix = svg.getScreenCTM();
      if (!matrix) {
        return null;
      }
      var point = svg.createSVGPoint();
      point.x = clientX;
      point.y = clientY;
      var local = point.matrixTransform(matrix.inverse());
      var best = null;
      var bestDistance = PICK_RANGE;
      Object.keys(LAYOUT).forEach(function (id) {
        var spot = LAYOUT[id];
        [[spot.x, spot.y], spot.badge].forEach(function (p) {
          var distance = Math.sqrt((p[0] - local.x) * (p[0] - local.x) + (p[1] - local.y) * (p[1] - local.y));
          if (distance < bestDistance) {
            bestDistance = distance;
            best = id;
          }
        });
      });
      return best;
    }
    svg.addEventListener("click", function (event) {
      var id = nearest(event.clientX, event.clientY);
      if (id) {
        onPick(id, "pointer");
      }
    });

    var selected = null;
    function setSelected(id) {
      selected = id;
      Object.keys(groups).forEach(function (key) {
        groups[key].classList.toggle("is-selected", key === id);
        badges[key].group.classList.toggle("is-selected", key === id);
      });
    }

    function setCapturing(id) {
      Object.keys(groups).forEach(function (key) {
        groups[key].classList.toggle("is-capturing", key === id);
      });
      svg.classList.toggle("is-capturing", !!id);
    }

    // Chip nodes of one button: the first is the badge's own; more are added beside it when a button has several keys.
    function chipNode(spot, index) {
      while (spot.chips.length <= index) {
        spot.chips.push(el("g", { class: "chip" }, spot.group));
      }
      return spot.chips[index];
    }

    function setBadges(list, mode) {
      (list || []).forEach(function (control) {
        var spot = badges[control.id];
        if (!spot) {
          return;
        }
        var infos;
        var source = null;
        if (mode === "key") {
          var keys = control.keys || [];
          infos = keys.length ? keys.map(function (name) { return GT4.controlsModel.label("key:" + name); })
            : [GT4.controlsModel.label(null)];
        } else {
          source = (control.sources || []).filter(function (candidate) {
            return GT4.controlsModel.family(candidate) === mode;
          })[0] || null;
          infos = [GT4.controlsModel.label(source)];
        }
        var step = infos.length > 1 ? 48 : 0;
        infos.forEach(function (info, index) {
          var node = chipNode(spot, index);
          node.removeAttribute("display");
          var offset = (index - (infos.length - 1) / 2) * step;
          drawChip(node, info, spot.x + offset, spot.y);
        });
        spot.chips.slice(infos.length).forEach(function (node) {
          node.setAttribute("display", "none");
        });
        spot.group.classList.toggle("is-unbound", mode !== "key" && !source);
        spot.group.classList.toggle("is-changed", control.is_default === false);
        spot.group.setAttribute("data-label", infos.map(function (info) { return info.text; }).join(" "));
      });
    }

    var lastLive = "";
    function setLive(state) {
      var buttons = (state && state.buttons) || {};
      var sticksNow = (state && state.sticks) || { left: { x: 0, y: 0 }, right: { x: 0, y: 0 } };
      var key = JSON.stringify([buttons, sticksNow]);
      if (key === lastLive) {
        return;
      }
      lastLive = key;
      Object.keys(groups).forEach(function (id) {
        var reading = buttons[id] || { value: 0, pressed: false };
        groups[id].classList.toggle("is-down", !!reading.pressed);
        badges[id].group.classList.toggle("is-live", !!reading.pressed);
      });
      ["l2", "r2"].forEach(function (id) {
        var part = parts[id];
        var value = buttons[id] ? buttons[id].value : 0;
        var height = Math.round(part.height * value * 10) / 10;
        part.fill.setAttribute("y", String(part.top + part.height - height));
        part.fill.setAttribute("height", String(height));
        part.readout.textContent = value > 0.005 ? part.label + " · " + Math.round(value * 100) + "%" : part.label;
        part.group.classList.toggle("is-filling", value > 0.005);
      });
      [["left", sticks.left], ["right", sticks.right]].forEach(function (pair) {
        var reading = sticksNow[pair[0]] || { x: 0, y: 0 };
        var dx = Math.round(reading.x * STICK.travel * 10) / 10;
        var dy = Math.round(reading.y * STICK.travel * 10) / 10;
        pair[1].cap.setAttribute("transform", "translate(" + dx + " " + dy + ")");
        pair[1].trail.setAttribute("x2", String(pair[1].cx + dx));
        pair[1].trail.setAttribute("y2", String(pair[1].cy + dy));
        pair[1].group.classList.toggle("is-moved", Math.abs(reading.x) > 0.02 || Math.abs(reading.y) > 0.02);
      });
    }

    function flash(ids) {
      (ids || []).forEach(function (id) {
        var spot = badges[id];
        if (!spot) {
          return;
        }
        spot.group.classList.remove("is-flash");
        // Restart the highlight on the next frame so a second change shows again.
        root.requestAnimationFrame(function () {
          spot.group.classList.add("is-flash");
          root.setTimeout(function () { spot.group.classList.remove("is-flash"); }, 1400);
        });
      });
    }

    // Small drawings (short or narrow windows) get larger chips, so their labels stay readable.
    if (root.ResizeObserver) {
      new root.ResizeObserver(function (entries) {
        var width = entries[0].contentRect.width;
        svg.classList.toggle("is-small", width > 0 && width < 560);
      }).observe(svg);
    }

    setLive(null);
    return {
      svg: svg,
      groups: groups,
      badges: badges,
      selected: function () { return selected; },
      setSelected: setSelected,
      setCapturing: setCapturing,
      setBadges: setBadges,
      setLive: setLive,
      flash: flash,
      nearest: nearest
    };
  }

  // A small drawing of one button for the panel and the capture overlay (no gradients, no ids).
  function glyph(id) {
    var svg = el("svg", { viewBox: "0 0 64 64", class: "ds-glyph", "aria-hidden": "true" });
    if (["triangle", "circle", "cross", "square"].indexOf(id) >= 0) {
      el("circle", { class: "gl-cap", cx: 32, cy: 32, r: 27 }, svg);
      faceGlyph(svg, id, 32, 32, 1.3);
    } else if (id.indexOf("dpad_") === 0) {
      el("path", { class: "gl-cross", d: "M23 6 h18 v17 h17 v18 h-17 v17 h-18 v-17 h-17 v-18 h17 Z" }, svg);
      var arm = { dpad_up: [23, 6, 18, 17], dpad_down: [23, 41, 18, 17], dpad_left: [6, 23, 17, 18], dpad_right: [41, 23, 17, 18] }[id];
      el("rect", { class: "gl-on", x: arm[0], y: arm[1], width: arm[2], height: arm[3], rx: 3 }, svg);
    } else if (id === "l3" || id === "r3") {
      el("circle", { class: "gl-cap", cx: 32, cy: 32, r: 27 }, svg);
      el("circle", { class: "gl-ring", cx: 32, cy: 32, r: 18 }, svg);
      text(svg, 32, 38, id.toUpperCase(), "gl-text");
    } else if (id === "start") {
      el("path", { class: "gl-on", d: "M22 17 L49 32 L22 47 Z" }, svg);
    } else if (id === "select") {
      el("rect", { class: "gl-on", x: 10, y: 25, width: 44, height: 14, rx: 6 }, svg);
    } else {
      el("rect", { class: "gl-cap", x: 4, y: 15, width: 56, height: 34, rx: 13 }, svg);
      text(svg, 32, 39, id.toUpperCase(), "gl-text");
    }
    return svg;
  }

  GT4.controllerSvg = build;
  GT4.controllerSvg.glyph = glyph;
  GT4.controllerSvg.LAYOUT = LAYOUT;
})(window);
