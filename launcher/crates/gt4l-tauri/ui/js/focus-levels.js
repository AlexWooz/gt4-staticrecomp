/*
 * Two-level focus for the Settings page, as console menus use it (pure logic, no DOM; tested in
 * dev/nav-test.html).
 *
 *   side     focus is on the tab list. Up/down moves between tabs and the content follows the tab.
 *            A or D-pad right enters the tab. B goes back to Home. LB/RB change the tab.
 *   content  focus is inside the tab's rows. Up/down moves between rows, left/right changes a value,
 *            A activates. B returns to the side level on the same tab. LB/RB change the tab and keep the level.
 *
 * state = {level, tab, rows}. rows maps a tab id to the key of the row last focused in that tab; it lasts for
 * the session. Every function returns a new state and never changes its input.
 */
(function (root) {
  "use strict";
  var GT4 = (root.GT4 = root.GT4 || {});

  var SIDE = "side";
  var CONTENT = "content";

  function assign(state, changes) {
    var next = { level: state.level, tab: state.tab, rows: state.rows };
    Object.keys(changes).forEach(function (key) {
      next[key] = changes[key];
    });
    return next;
  }

  // Opening Settings (or a new route): the side level, on the given tab. Remembered rows are kept.
  function open(state, tab) {
    return assign(state, { level: SIDE, tab: tab });
  }

  function start(tab) {
    return { level: SIDE, tab: tab, rows: {} };
  }

  // A, or D-pad right, on a tab of the side level.
  function enter(state) {
    return assign(state, { level: CONTENT });
  }

  // Focus landed inside a tab's rows. A focus that comes from rendering the preview (previewing) never moves the
  // level: the tab list keeps focus while its content is drawn. Returns the same state object when nothing changes.
  function onContentFocus(state, previewing) {
    if (previewing || state.level === CONTENT) {
      return state;
    }
    return enter(state);
  }

  // B inside the content: the side level, on the same tab. Nothing else changes.
  function back(state) {
    return assign(state, { level: SIDE });
  }

  // The tab shown (the preview follows the highlighted tab). The level is kept.
  function select(state, tab) {
    return assign(state, { tab: tab });
  }

  // LB/RB or swipe: the next (delta 1) or previous (delta -1) tab, wrapping. The level is kept.
  function moveTab(state, delta, tabIds) {
    var index = tabIds.indexOf(state.tab);
    var next = tabIds[(((index + delta) % tabIds.length) + tabIds.length) % tabIds.length];
    return select(state, next);
  }

  // Focus landed on a row of the current tab.
  function remember(state, rowKey) {
    var rows = {};
    Object.keys(state.rows).forEach(function (key) {
      rows[key] = state.rows[key];
    });
    rows[state.tab] = rowKey;
    return assign(state, { rows: rows });
  }

  // The row to focus when the content is entered: the one last focused in this tab, or null (first row).
  function rowFor(state) {
    return Object.prototype.hasOwnProperty.call(state.rows, state.tab) ? state.rows[state.tab] : null;
  }

  // Footer labels for a level. The tab adds its own keys (for example Y Reset, X Rebind) on the content level.
  function footer(level) {
    if (level === CONTENT) {
      return [{ key: "A", label: GT4.t("hint.select") }, { key: "B", label: GT4.t("hint.tabs") }];
    }
    return [{ key: "A", label: GT4.t("hint.enter") }, { key: "B", label: GT4.t("hint.back") }, { key: "LB RB", label: GT4.t("hint.tabs") }];
  }

  GT4.focusLevels = {
    SIDE: SIDE,
    CONTENT: CONTENT,
    start: start,
    open: open,
    enter: enter,
    onContentFocus: onContentFocus,
    back: back,
    select: select,
    moveTab: moveTab,
    remember: remember,
    rowFor: rowFor,
    footer: footer
  };
})(window);
