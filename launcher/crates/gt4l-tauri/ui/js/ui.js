/*
 * Shared DOM helpers: element builder, hint bar, toast, dialogs, dates.
 * No inline event handlers and no style attributes (CSP: style-src 'self').
 */
(function (root) {
  "use strict";
  var GT4 = (root.GT4 = root.GT4 || {});

  // h("button", {class: "x", on: {click: fn}, "aria-label": "..."}, [children])
  function h(tag, props, children) {
    var el = document.createElement(tag);
    var key;
    if (props) {
      for (key in props) {
        if (!Object.prototype.hasOwnProperty.call(props, key)) {
          continue;
        }
        var value = props[key];
        if (value === null || value === undefined || value === false) {
          continue;
        }
        if (key === "class") {
          el.className = value;
        } else if (key === "text") {
          el.textContent = String(value);
        } else if (key === "on") {
          Object.keys(value).forEach(function (name) {
            el.addEventListener(name, value[name]);
          });
        } else if (key === "data") {
          Object.keys(value).forEach(function (name) {
            el.dataset[name] = String(value[name]);
          });
        } else if (value === true) {
          el.setAttribute(key, "");
        } else {
          el.setAttribute(key, String(value));
        }
      }
    }
    append(el, children);
    return el;
  }

  function append(el, children) {
    if (children === null || children === undefined) {
      return;
    }
    if (!Array.isArray(children)) {
      children = [children];
    }
    children.forEach(function (child) {
      if (child === null || child === undefined || child === false) {
        return;
      }
      if (Array.isArray(child)) {
        append(el, child);
      } else if (child instanceof Node) {
        el.appendChild(child);
      } else {
        el.appendChild(document.createTextNode(String(child)));
      }
    });
  }

  function clear(el) {
    while (el.firstChild) {
      el.removeChild(el.firstChild);
    }
  }

  // Hint bar: [{key: "A", label: "Select", run: fn?}]. Clicks run the action
  // for mouse users; the bar itself is not focusable.
  function setHints(list) {
    var bar = document.getElementById("hints");
    if (!bar) {
      return;
    }
    clear(bar);
    (list || []).forEach(function (hint) {
      var item = h("span", { class: "hint", on: { click: function () { if (hint.run) { hint.run(); } } } }, [
        h("span", { class: "key", text: hint.key }),
        h("span", { text: hint.label })
      ]);
      bar.appendChild(item);
    });
  }

  var toastTimer = null;
  function toast(text, isError) {
    var box = document.getElementById("toast");
    if (!box) {
      return;
    }
    box.textContent = text;
    box.className = "toast is-shown" + (isError ? " is-error" : "");
    clearTimeout(toastTimer);
    toastTimer = setTimeout(function () {
      box.className = "toast";
    }, 3200);
  }

  var dialogId = 0;

  // openDialog({title, text, body: [nodes], buttons: [{label, primary, danger, run}],
  //             list: bool, dismissable: bool, onClose: fn})
  // Returns {el, close}. The top-most .modal owns navigation while open.
  function openDialog(options) {
    var previous = document.activeElement;
    dialogId += 1;
    var titleId = "dialog-title-" + dialogId;
    var closed = false;
    var overlay = h("div", { class: "modal", role: "dialog", "aria-modal": "true", "aria-labelledby": titleId }, [
      h("div", { class: "modal-box" + (options.wide ? " is-wide" : "") }, [
        h("h3", { id: titleId, text: options.title }),
        options.text ? h("p", { class: "modal-text", text: options.text }) : null,
        options.body || null,
        h("div", { class: "modal-actions" }, (options.buttons || []).map(function (button) {
          return h("button", {
            class: "btn" + (button.primary ? " primary" : "") + (button.danger ? " danger" : ""),
            "data-default": button.primary ? true : null,
            on: {
              click: function () {
                if (button.run) {
                  button.run(api);
                }
                if (button.closes !== false) {
                  api.close();
                }
              }
            }
          }, button.label);
        }))
      ])
    ]);
    var api = {
      el: overlay,
      close: function () {
        if (closed) {
          return;
        }
        closed = true;
        overlay.remove();
        if (options.onClose) {
          options.onClose();
        }
        if (previous && previous.focus && document.contains(previous)) {
          previous.focus({ preventScroll: true });
        }
      },
      // Back (B or Escape) closes the dialog unless it is not dismissable.
      back: function () {
        if (options.dismissable !== false) {
          api.close();
        }
      }
    };
    overlay.__dialog = api;
    document.body.appendChild(overlay);
    var first = overlay.querySelector("[data-default]") || overlay.querySelector("button");
    if (first) {
      first.focus({ preventScroll: true });
    }
    return api;
  }

  function topDialog() {
    var modals = document.querySelectorAll(".modal");
    return modals.length ? modals[modals.length - 1].__dialog || null : null;
  }

  // Help tips ("i") open on tap or click and close on a second tap or a tap outside.
  // Returns true when a tip was open, so B can close it before it goes back.
  function closeTips(except) {
    var open = document.querySelectorAll(".info.is-open");
    var closed = false;
    Array.prototype.forEach.call(open, function (el) {
      if (el !== except) {
        el.classList.remove("is-open");
        el.setAttribute("aria-expanded", "false");
        closed = true;
      }
    });
    return closed;
  }

  function toggleTip(info) {
    var open = !info.classList.contains("is-open");
    closeTips(info);
    info.classList.toggle("is-open", open);
    info.setAttribute("aria-expanded", String(open));
  }

  document.addEventListener("pointerdown", function (event) {
    var target = event.target;
    Array.prototype.forEach.call(document.querySelectorAll(".info.is-open"), function (el) {
      if (!el.contains(target)) {
        el.classList.remove("is-open");
        el.setAttribute("aria-expanded", "false");
      }
    });
  }, true);

  // Formats an ISO timestamp from Rust in the interface language (issue #89: the Saves dates followed en-GB, so pt-BR
  // showed English month names). The formatter is built per call, so a language change applies at once. Returns the
  // raw text when it cannot parse.
  function formatDate(iso) {
    if (!iso) {
      return "";
    }
    var date = new Date(iso);
    if (isNaN(date.getTime())) {
      return String(iso);
    }
    try {
      return new Intl.DateTimeFormat(GT4.i18n.intlLocale(), {
        day: "numeric", month: "short", year: "numeric", hour: "2-digit", minute: "2-digit"
      }).format(date);
    } catch (err) {
      return String(iso);
    }
  }

  // Keeps the page shell and the app-level hooks in one place.
  GT4.ui = {
    h: h,
    append: append,
    clear: clear,
    setHints: setHints,
    toast: toast,
    openDialog: openDialog,
    topDialog: topDialog,
    closeTips: closeTips,
    toggleTip: toggleTip,
    formatDate: formatDate
  };
})(window);
