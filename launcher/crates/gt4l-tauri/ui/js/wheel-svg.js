/*
 * GT4Recomp's own drawing of a racing wheel and its pedals for the Wheel page (TASK-181): plain geometry, no vendor
 * artwork, photos, logos or marks. Classes and SVG attributes only (CSP).
 *
 * Based on cbfd8b8's buildWheelStageSvg (JulioHeroZ, with luanantony): the rim with its paddles and spoke buttons that
 * turn with the wheel, the pedal base below, and a chip for each control. TASK-181: the wheel turns at its real angle
 * (a 900-degree wheel turns up to 450 degrees each way, beyond a full turn), the pedals show the raw travel and the
 * value the game gets, the chips are the focus targets (static places, so the D-pad moves predictably while the wheel
 * turns) and their leader lines show only while the wheel is near the centre.
 *
 *   GT4.wheelSvg({onPick(id, how)}) -> {svg, items, setBindings(fn), setLive(state), setSelected(id),
 *                                      setCapturing(id), flash(ids)}
 *   ids: "steering", "throttle", "brake", "clutch", "handbrake" and the wheel.toml button slots ("cross", "r2"...).
 *   setLive({degrees, steerPosition, steerBound, pedals: {id: {travel, output, bound}}, down: {slot: bool}})
 */
(function (root) {
  "use strict";
  var GT4 = (root.GT4 = root.GT4 || {});
  var NS = "http://www.w3.org/2000/svg";

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
    var node = el("text", { x: x, y: y, class: cls || "" }, parent);
    node.textContent = value;
    return node;
  }

  function gradient(defs, id, stops, vertical) {
    var node = el("linearGradient", { id: id, x1: 0, y1: 0, x2: vertical === false ? 1 : 0, y2: vertical === false ? 0 : 1 }, defs);
    stops.forEach(function (stop) {
      el("stop", { offset: stop[0], "stop-color": stop[1], "stop-opacity": stop[2] === undefined ? 1 : stop[2] }, node);
    });
    return node;
  }

  var CX = 500;
  var CY = 225;

  // Where each control sits on the wheel at rest (it turns with the rim), and where its chip is.
  var BUTTONS = {
    gear_down: { x: 336, y: 168, side: "left", row: 0 },
    l2: { x: 457, y: 203, side: "left", row: 1 },
    dpad_up: { x: 408, y: 210, side: "left", row: 2 },
    dpad_left: { x: 388, y: 230, side: "left", row: 3 },
    dpad_right: { x: 428, y: 230, side: "left", row: 4 },
    dpad_down: { x: 408, y: 250, side: "left", row: 5 },
    l3: { x: 466, y: 292, side: "left", row: 6 },
    select: { x: 474, y: 330, side: "left", row: 7 },
    gear_up: { x: 664, y: 168, side: "right", row: 0 },
    r2: { x: 543, y: 203, side: "right", row: 1 },
    triangle: { x: 592, y: 210, side: "right", row: 2 },
    square: { x: 572, y: 230, side: "right", row: 3 },
    circle: { x: 612, y: 230, side: "right", row: 4 },
    cross: { x: 592, y: 250, side: "right", row: 5 },
    r3: { x: 534, y: 292, side: "right", row: 6 },
    start: { x: 526, y: 330, side: "right", row: 7 }
  };
  var CHIP_W = 160;
  var CHIP_H = 36;
  var CHIP_X = { left: 152, right: 848 };
  function chipY(row) {
    return 66 + row * 45;
  }
  var PEDAL_X = { clutch: 392, brake: 500, throttle: 608 };
  var PEDAL_TOP = 490;
  var PEDAL_H = 104;

  function build(options) {
    var model = GT4.wheelModel;
    var onPick = (options && options.onPick) || function () {};
    var svg = el("svg", { viewBox: "55 -8 890 690", class: "pad-svg wheel-svg", preserveAspectRatio: "xMidYMid meet", "aria-hidden": "false" });
    var defs = el("defs", {}, svg);
    gradient(defs, "wh-base", [[0, "#2a2f37"], [1, "#101216"]]);
    gradient(defs, "wh-rim", [[0, "#3c424c"], [0.5, "#23272e"], [1, "#121418"]]);
    gradient(defs, "wh-spoke", [[0, "#4b525d"], [0.55, "#2b3037"], [1, "#191c21"]]);
    gradient(defs, "wh-paddle", [[0, "#7a8494"], [0.5, "#4a515d"], [1, "#2a2e35"]]);
    gradient(defs, "wh-cap", [[0, "#3a3f48"], [1, "#1b1e23"]]);
    gradient(defs, "wh-pedal", [[0, "#5d6470"], [1, "#2a2e35"]]);
    gradient(defs, "wh-out-throttle", [[0, "#5ce096"], [1, "#1f7a4c"]]);
    gradient(defs, "wh-out-brake", [[0, "#ff7b7b"], [1, "#a82828"]]);
    gradient(defs, "wh-out-clutch", [[0, "#85b2ff"], [1, "#245bc4"]]);
    gradient(defs, "wh-out-handbrake", [[0, "#f6c35c"], [1, "#9e6a14"]]);

    var items = {};
    var chips = {};
    var leaders = {};
    var controls = {};

    // ---- the base behind the wheel (it does not turn) ----
    el("ellipse", { class: "wh-floor", cx: CX, cy: CY + 8, rx: 205, ry: 196 }, svg);
    el("rect", { class: "wh-base", x: CX - 96, y: CY - 92, width: 192, height: 184, rx: 34 }, svg);

    // ---- the turning wheel ----
    var turning = el("g", { class: "wh-turning" }, svg);
    function paddle(id, mirror) {
      var g = el("g", { class: "wh-ctl", "data-control": id }, turning);
      var d = mirror
        ? "M 628 150 C 650 142 672 140 690 146 C 698 160 698 182 690 196 C 672 200 652 198 632 190 Z"
        : "M 372 150 C 350 142 328 140 310 146 C 302 160 302 182 310 196 C 328 200 348 198 368 190 Z";
      el("path", { class: "wh-paddle", d: d }, g);
      text(g, mirror ? 672 : 328, 178, mirror ? "+" : "−", "wh-paddle-label");
      controls[id] = g;
    }
    paddle("gear_down", false);
    paddle("gear_up", true);
    el("circle", { class: "wh-rim", cx: CX, cy: CY, r: 150 }, turning);
    el("circle", { class: "wh-rim-grip", cx: CX, cy: CY, r: 150 }, turning);
    el("circle", { class: "wh-rim-edge", cx: CX, cy: CY, r: 166 }, turning);
    el("circle", { class: "wh-rim-edge inner", cx: CX, cy: CY, r: 134 }, turning);
    el("rect", { class: "wh-top-mark", x: CX - 7, y: CY - 166, width: 14, height: 32, rx: 4 }, turning);
    el("path", {
      class: "wh-spokes",
      d: "M 366 200 L 444 196 C 466 180 534 180 556 196 L 634 200 L 634 258 L 552 268 L 532 360 L 468 360 L 448 268 L 366 258 Z"
    }, turning);
    el("path", { class: "wh-spokes-shine", d: "M 372 205 L 446 201 C 468 186 532 186 554 201 L 628 205" }, turning);
    el("circle", { class: "wh-hub", cx: CX, cy: CY + 6, r: 32 }, turning);
    el("circle", { class: "wh-hub-ring", cx: CX, cy: CY + 6, r: 23 }, turning);
    el("circle", { class: "wh-hub-dot", cx: CX, cy: CY + 6, r: 5 }, turning);

    function capButton(id, x, y, r, draw) {
      var g = el("g", { class: "wh-ctl", "data-control": id }, turning);
      el("circle", { class: "wh-cap", cx: x, cy: y, r: r }, g);
      draw(g, x, y);
      controls[id] = g;
    }
    function arrow(direction) {
      return function (g, x, y) {
        var points = {
          up: [[x, y - 5], [x - 5, y + 3], [x + 5, y + 3]],
          down: [[x, y + 5], [x - 5, y - 3], [x + 5, y - 3]],
          left: [[x - 5, y], [x + 3, y - 5], [x + 3, y + 5]],
          right: [[x + 5, y], [x - 3, y - 5], [x - 3, y + 5]]
        }[direction];
        el("polygon", { class: "wh-arrow", points: points.map(function (p) { return p.join(","); }).join(" ") }, g);
      };
    }
    function face(kind) {
      return function (g, x, y) {
        if (kind === "triangle") {
          el("polygon", { class: "glyph g-triangle wh-glyph", points: x + "," + (y - 5) + " " + (x - 5.5) + "," + (y + 4) + " " + (x + 5.5) + "," + (y + 4) }, g);
        } else if (kind === "circle") {
          el("circle", { class: "glyph g-circle wh-glyph", cx: x, cy: y, r: 5 }, g);
        } else if (kind === "cross") {
          el("path", { class: "glyph g-cross wh-glyph", d: "M" + (x - 4.5) + " " + (y - 4.5) + " L" + (x + 4.5) + " " + (y + 4.5) + " M" + (x + 4.5) + " " + (y - 4.5) + " L" + (x - 4.5) + " " + (y + 4.5) }, g);
        } else {
          el("rect", { class: "glyph g-square wh-glyph", x: x - 4.5, y: y - 4.5, width: 9, height: 9, rx: 1 }, g);
        }
      };
    }
    function label(value) {
      return function (g, x, y) {
        text(g, x, y + 3.5, value, "wh-cap-label");
      };
    }
    ["up", "left", "right", "down"].forEach(function (direction) {
      var id = "dpad_" + direction;
      capButton(id, BUTTONS[id].x, BUTTONS[id].y, 9.5, arrow(direction));
    });
    ["triangle", "square", "circle", "cross"].forEach(function (kind) {
      capButton(kind, BUTTONS[kind].x, BUTTONS[kind].y, 10, face(kind));
    });
    capButton("l2", BUTTONS.l2.x, BUTTONS.l2.y, 9, label("L2"));
    capButton("r2", BUTTONS.r2.x, BUTTONS.r2.y, 9, label("R2"));
    capButton("l3", BUTTONS.l3.x, BUTTONS.l3.y, 9, label("L3"));
    capButton("r3", BUTTONS.r3.x, BUTTONS.r3.y, 9, label("R3"));
    capButton("select", BUTTONS.select.x, BUTTONS.select.y, 9, function (g, x, y) {
      el("rect", { class: "wh-arrow", x: x - 5, y: y - 2, width: 10, height: 4, rx: 2 }, g);
    });
    capButton("start", BUTTONS.start.x, BUTTONS.start.y, 9, function (g, x, y) {
      el("polygon", { class: "wh-arrow", points: (x - 3) + "," + (y - 5) + " " + (x + 5) + "," + y + " " + (x - 3) + "," + (y + 5) }, g);
    });

    // ---- the angle readout and the steering bar (they do not turn) ----
    var readout = text(svg, CX, 430, "0°", "wh-angle");
    var lockText = text(svg, CX, 449, "", "wh-lock");
    el("rect", { class: "wh-bar-track", x: CX - 160, y: 460, width: 320, height: 5, rx: 2.5 }, svg);
    var barFill = el("rect", { class: "wh-bar-fill", x: CX, y: 460, width: 0, height: 5, rx: 2.5 }, svg);
    el("rect", { class: "wh-bar-tick", x: CX - 1, y: 456, width: 2, height: 13 }, svg);

    // ---- the pedals and the handbrake ----
    el("rect", { class: "wh-pedal-base", x: 322, y: PEDAL_TOP - 8, width: 356, height: PEDAL_H + 56, rx: 18 }, svg);
    var pedalParts = {};
    function pedal(id) {
      var x = PEDAL_X[id];
      var g = el("g", { class: "wh-item wh-pedal", "data-control": id, "data-focusable": "1", tabindex: "0", role: "button" }, svg);
      el("rect", { class: "wh-halo", x: x - 46, y: PEDAL_TOP - 4, width: 92, height: PEDAL_H + 48, rx: 14 }, g);
      var plate = el("g", { class: "wh-plate" }, g);
      el("rect", { class: "wh-plate-back", x: x - 30, y: PEDAL_TOP + 4, width: 60, height: PEDAL_H - 8, rx: 10 }, plate);
      var raw = el("rect", { class: "wh-plate-raw", x: x - 27, y: PEDAL_TOP + PEDAL_H - 7, width: 54, height: 0, rx: 7 }, plate);
      var out = el("rect", { class: "wh-plate-out", x: x - 27, y: PEDAL_TOP + PEDAL_H - 7, width: 54, height: 0, rx: 7, fill: "url(#wh-out-" + id + ")" }, plate);
      [22, 42, 62].forEach(function (offset) {
        el("line", { class: "wh-plate-rib", x1: x - 16, y1: PEDAL_TOP + offset, x2: x + 16, y2: PEDAL_TOP + offset }, plate);
      });
      var name = text(g, x, PEDAL_TOP + PEDAL_H + 14, "", "wh-pedal-name");
      var value = text(g, x, PEDAL_TOP + PEDAL_H + 34, "0%", "wh-pedal-value");
      pedalParts[id] = { plate: plate, raw: raw, out: out, name: name, value: value, x: x };
      items[id] = g;
    }
    pedal("clutch");
    pedal("brake");
    pedal("throttle");
    (function handbrake() {
      var x = 760;
      var g = el("g", { class: "wh-item wh-pedal", "data-control": "handbrake", "data-focusable": "1", tabindex: "0", role: "button" }, svg);
      el("rect", { class: "wh-halo", x: x - 50, y: PEDAL_TOP - 4, width: 100, height: PEDAL_H + 48, rx: 14 }, g);
      el("rect", { class: "wh-hb-base", x: x - 26, y: PEDAL_TOP + PEDAL_H - 22, width: 52, height: 18, rx: 6 }, g);
      var lever = el("g", { class: "wh-hb-lever" }, g);
      el("rect", { class: "wh-hb-arm", x: x - 6, y: PEDAL_TOP + 6, width: 12, height: PEDAL_H - 20, rx: 6 }, lever);
      el("rect", { class: "wh-hb-grip", x: x - 9, y: PEDAL_TOP + 2, width: 18, height: 34, rx: 8 }, lever);
      var raw = el("rect", { class: "wh-plate-raw", x: x + 18, y: PEDAL_TOP + PEDAL_H - 7, width: 8, height: 0, rx: 3 }, g);
      var out = el("rect", { class: "wh-plate-out", x: x + 18, y: PEDAL_TOP + PEDAL_H - 7, width: 8, height: 0, rx: 3, fill: "url(#wh-out-handbrake)" }, g);
      var name = text(g, x, PEDAL_TOP + PEDAL_H + 14, "", "wh-pedal-name");
      var value = text(g, x, PEDAL_TOP + PEDAL_H + 34, "0%", "wh-pedal-value");
      pedalParts.handbrake = { lever: lever, raw: raw, out: out, name: name, value: value, x: x, slim: true };
      items.handbrake = g;
    })();

    // ---- chips: one per control, the focus targets ----
    function chipGlyph(g, dfp, x, y) {
      var glyph = el("g", { class: "wh-chip-glyph" }, g);
      el("circle", { class: "wh-chip-glyph-bg", cx: x, cy: y, r: 13 }, glyph);
      if (["cross", "circle", "square", "triangle"].indexOf(dfp) >= 0) {
        face(dfp)(glyph, x, y);
      } else if (dfp.indexOf("dpad_") === 0) {
        arrow(dfp.slice(5))(glyph, x, y);
      } else if (dfp === "gear_up" || dfp === "gear_down") {
        text(glyph, x, y + 5.5, dfp === "gear_up" ? "+" : "−", "wh-chip-sign");
      } else if (dfp === "start") {
        el("polygon", { class: "wh-arrow", points: (x - 3) + "," + (y - 5) + " " + (x + 5) + "," + y + " " + (x - 3) + "," + (y + 5) }, glyph);
      } else if (dfp === "select") {
        el("rect", { class: "wh-arrow", x: x - 6, y: y - 2.5, width: 12, height: 5, rx: 2.5 }, glyph);
      } else {
        text(glyph, x, y + 4, dfp.toUpperCase(), "wh-chip-tag");
      }
    }
    function chip(id, dfp, x, y, width, leaderTo) {
      var g = el("g", { class: "wh-item badge", "data-control": id, "data-focusable": "1", tabindex: "0", role: "button" }, svg);
      if (leaderTo) {
        var fromX = x < CX ? x + width / 2 : x - width / 2;
        leaders[id] = el("line", { class: "leader wh-leader", x1: fromX, y1: y, x2: leaderTo[0], y2: leaderTo[1] }, g);
      }
      var box = el("g", { class: "chip" }, g);
      el("rect", { class: "chip-bg", x: x - width / 2, y: y - CHIP_H / 2, width: width, height: CHIP_H, rx: CHIP_H / 2 }, box);
      if (dfp) {
        chipGlyph(box, dfp, x - width / 2 + 20, y);
      }
      var value = text(box, dfp ? x + 18 : x, y + 5.5, "", "chip-text wh-chip-text");
      chips[id] = { group: g, text: value, width: width };
      items[id] = g;
    }
    Object.keys(BUTTONS).forEach(function (dfp) {
      var place = BUTTONS[dfp];
      var slot = model.controlOfDfp(dfp).slot;
      chip(slot, dfp, CHIP_X[place.side], chipY(place.row), CHIP_W, [place.x, place.y]);
      controls[slot] = controls[dfp];
    });
    chip("steering", "", CX, 14, 200, null);
    Object.keys(pedalParts).forEach(function (id) {
      var part = pedalParts[id];
      var holder = el("g", { class: "badge wh-pedal-chip" }, items[id]);
      var width = 104;
      var y = PEDAL_TOP + PEDAL_H + 60;
      el("rect", { class: "chip-bg", x: part.x - width / 2, y: y - 14, width: width, height: 28, rx: 14 }, holder);
      chips[id] = { group: items[id], text: text(holder, part.x, y + 5, "", "chip-text wh-chip-text is-small"), width: width, holder: holder };
    });

    // ---- picking: a click on the drawing selects the nearest control or chip ----
    function rotated(point, degrees) {
      var radians = degrees * Math.PI / 180;
      var dx = point.x - CX;
      var dy = point.y - CY;
      return { x: CX + dx * Math.cos(radians) - dy * Math.sin(radians), y: CY + dx * Math.sin(radians) + dy * Math.cos(radians) };
    }
    var angleNow = 0;
    svg.addEventListener("click", function (event) {
      var matrix = svg.getScreenCTM();
      if (!matrix) {
        return;
      }
      var point = svg.createSVGPoint();
      point.x = event.clientX;
      point.y = event.clientY;
      var at = point.matrixTransform(matrix.inverse());
      var best = null;
      var bestDistance = 70;
      function consider(id, x, y) {
        var distance = Math.hypot(at.x - x, at.y - y);
        if (distance < bestDistance) {
          bestDistance = distance;
          best = id;
        }
      }
      Object.keys(BUTTONS).forEach(function (dfp) {
        var place = BUTTONS[dfp];
        var slot = model.controlOfDfp(dfp).slot;
        var spot = rotated(place, angleNow);
        consider(slot, spot.x, spot.y);
        consider(slot, CHIP_X[place.side], chipY(place.row));
      });
      Object.keys(PEDAL_X).forEach(function (id) { consider(id, PEDAL_X[id], PEDAL_TOP + PEDAL_H / 2); });
      consider("handbrake", 760, PEDAL_TOP + PEDAL_H / 2);
      consider("steering", CX, 14);
      if (!best && Math.hypot(at.x - CX, at.y - CY) < 175) {
        best = "steering";
      }
      if (best) {
        // TASK-189: a click that landed on a control (its chip, its pedal or the rim) opens the capture popup at once; a
        // click on the empty drawing only selects (the page decides in `onPick`).
        var onControl = Object.keys(items).some(function (id) { return items[id].contains(event.target); });
        onPick(best, onControl ? "chip" : "pointer");
      }
    });
    Object.keys(items).forEach(function (id) {
      items[id].addEventListener("click", function (event) {
        // A pad or key activation (nav.js dispatches the click on the focused item): bind. A mouse click lands on the svg.
        if (event.target === items[id] && event.detail === 0) {
          event.stopPropagation();
          onPick(id, "activate");
        }
      });
    });

    return {
      svg: svg,
      items: items,
      // fn(id) -> {text, unbound, aux, title}
      setBindings: function (describe) {
        Object.keys(chips).forEach(function (id) {
          var info = describe(id) || { text: "", unbound: true };
          chips[id].text.textContent = info.text;
          chips[id].group.classList.toggle("is-unbound", !!info.unbound);
          chips[id].group.classList.toggle("is-aux", !!info.aux);
          if (info.title) {
            items[id].setAttribute("aria-label", info.title);
          }
        });
      },
      setPedalNames: function (names) {
        Object.keys(pedalParts).forEach(function (id) {
          pedalParts[id].name.textContent = names[id] || id;
        });
      },
      setLockText: function (value) {
        lockText.textContent = value;
      },
      setLive: function (state) {
        angleNow = state.degrees || 0;
        turning.setAttribute("transform", "rotate(" + angleNow.toFixed(2) + " " + CX + " " + CY + ")");
        readout.textContent = state.steerBound ? model.angleText(angleNow) : "—";
        svg.classList.toggle("is-turned", Math.abs(angleNow) > 6);
        var position = Math.max(-1, Math.min(1, state.steerPosition || 0));
        barFill.setAttribute("x", String(position < 0 ? CX + position * 160 : CX));
        barFill.setAttribute("width", String(Math.abs(position) * 160));
        Object.keys(pedalParts).forEach(function (id) {
          var part = pedalParts[id];
          var reading = (state.pedals || {})[id] || { travel: 0, output: 0, bound: false };
          var full = PEDAL_H - 14;
          var rawHeight = reading.travel * full;
          var outHeight = reading.output * full;
          part.raw.setAttribute("y", String(PEDAL_TOP + PEDAL_H - 7 - rawHeight));
          part.raw.setAttribute("height", String(rawHeight));
          part.out.setAttribute("y", String(PEDAL_TOP + PEDAL_H - 7 - outHeight));
          part.out.setAttribute("height", String(outHeight));
          part.value.textContent = reading.bound ? Math.round(reading.output * 100) + "%" : "—";
          if (part.plate) {
            part.plate.setAttribute("transform", "translate(0 " + (reading.travel * 6).toFixed(1) + ")");
          }
          if (part.lever) {
            part.lever.setAttribute("transform", "rotate(" + (-reading.travel * 26).toFixed(1) + " " + part.x + " " + (PEDAL_TOP + PEDAL_H - 14) + ")");
          }
          items[id].classList.toggle("is-down", reading.output > 0.02);
          items[id].classList.toggle("is-unbound", !reading.bound);
        });
        Object.keys(chips).forEach(function (id) {
          if (BUTTONS[(model.controlOfSlot(id) || {}).dfp]) {
            var down = !!(state.down || {})[id];
            chips[id].group.classList.toggle("is-live", down);
            if (controls[id]) {
              controls[id].classList.toggle("is-down", down);
            }
          }
        });
      },
      setSelected: function (id) {
        Object.keys(items).forEach(function (key) {
          items[key].classList.toggle("is-selected", key === id);
          if (controls[key]) {
            controls[key].classList.toggle("is-selected", key === id);
          }
        });
        turning.classList.toggle("is-selected", id === "steering");
      },
      setCapturing: function (id) {
        Object.keys(items).forEach(function (key) {
          items[key].classList.toggle("is-capturing", key === id);
          if (controls[key]) {
            controls[key].classList.toggle("is-capturing", key === id);
          }
        });
        turning.classList.toggle("is-capturing", id === "steering");
      },
      flash: function (ids) {
        ids.forEach(function (id) {
          var item = items[id];
          if (!item) {
            return;
          }
          item.classList.remove("is-flash");
          void item.getBoundingClientRect();
          item.classList.add("is-flash");
        });
      }
    };
  }

  GT4.wheelSvg = build;
  GT4.wheelSvg.BUTTONS = BUTTONS;

  // A small drawing of a wheel control for the panel and the wizard: a pedal, the steering wheel, or a DFP button
  // (the DualShock 2 glyph of controller-svg.js, which the DFP's buttons share).
  GT4.wheelSvg.glyph = function (id) {
    var model = GT4.wheelModel;
    if (id === "steering") {
      var wheel = el("svg", { viewBox: "0 0 64 64", class: "ds-glyph wh-mini", "aria-hidden": "true" });
      el("circle", { class: "wh-mini-rim", cx: 32, cy: 32, r: 24 }, wheel);
      el("path", { class: "wh-mini-spoke", d: "M10 30 L54 30 M32 30 L32 54" }, wheel);
      el("circle", { class: "wh-mini-hub", cx: 32, cy: 32, r: 6 }, wheel);
      el("rect", { class: "wh-mini-mark", x: 29.5, y: 6, width: 5, height: 8, rx: 1.5 }, wheel);
      return wheel;
    }
    if (model.PEDALS.indexOf(id) >= 0) {
      var pedalSvg = el("svg", { viewBox: "0 0 64 64", class: "ds-glyph wh-mini wh-mini-" + id, "aria-hidden": "true" });
      if (id === "handbrake") {
        el("rect", { class: "wh-mini-hb", x: 28, y: 10, width: 8, height: 40, rx: 4, transform: "rotate(-18 32 50)" }, pedalSvg);
        el("rect", { class: "wh-mini-base", x: 18, y: 48, width: 28, height: 8, rx: 3 }, pedalSvg);
      } else {
        el("rect", { class: "wh-mini-pedal", x: 18, y: 8, width: 28, height: 44, rx: 7 }, pedalSvg);
        el("path", { class: "wh-mini-rib", d: "M24 20 H40 M24 30 H40 M24 40 H40" }, pedalSvg);
      }
      return pedalSvg;
    }
    var control = model.controlOfSlot(id);
    return GT4.controllerSvg.glyph(control ? control.glyph : id);
  };
})(window);
