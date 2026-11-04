/*
 * A small Markdown reader for the release notes panel (TASK-099). It builds DOM nodes with GT4.ui.h and text nodes
 * only: no innerHTML, so any HTML in the text shows as text (CSP and safety). The subset is what docs/release-notes
 * uses: headings (#, ##, ###), paragraphs, bullet lists (one nesting level, "- " or "* "), pipe tables, bold, italic and
 * inline code. Links keep their text and drop the address (the panel never opens a URL from the notes).
 * GT4.markdown.render(text, {skipTitle}) returns a DocumentFragment.
 */
(function (root) {
  "use strict";
  var GT4 = (root.GT4 = root.GT4 || {});
  var h = GT4.ui.h;

  // Inline markup inside one line: `code`, **bold**, *italic* or _italic_, [text](url) -> text.
  var INLINE = /(`[^`]+`)|(\*\*[^*]+\*\*)|(\*[^*\s][^*]*\*)|(_[^_\s][^_]*_)|(\[[^\]]+\]\([^)]*\))/g;

  function inline(text, parent) {
    // A fresh pattern per call: bold and italic recurse, and a shared global pattern would lose its place.
    var pattern = new RegExp(INLINE.source, "g");
    var at = 0;
    var match;
    while ((match = pattern.exec(text)) !== null) {
      if (match.index > at) {
        parent.appendChild(document.createTextNode(text.slice(at, match.index)));
      }
      var token = match[0];
      if (match[1]) {
        parent.appendChild(h("code", { text: token.slice(1, -1) }));
      } else if (match[2]) {
        parent.appendChild(inlineInto(h("strong"), token.slice(2, -2)));
      } else if (match[3] || match[4]) {
        parent.appendChild(inlineInto(h("em"), token.slice(1, -1)));
      } else {
        parent.appendChild(document.createTextNode(token.slice(1, token.indexOf("]"))));
      }
      at = match.index + token.length;
    }
    if (at < text.length) {
      parent.appendChild(document.createTextNode(text.slice(at)));
    }
    return parent;
  }

  function inlineInto(element, text) {
    return inline(text, element);
  }

  function tableRow(line) {
    return line.replace(/^\s*\|/, "").replace(/\|\s*$/, "").split("|").map(function (cell) { return cell.trim(); });
  }

  function isSeparatorRow(cells) {
    return cells.every(function (cell) { return /^:?-{2,}:?$/.test(cell); });
  }

  function render(text, options) {
    var skipTitle = !!(options && options.skipTitle);
    var fragment = document.createDocumentFragment();
    var lines = String(text || "").replace(/\r\n/g, "\n").split("\n");
    var paragraph = [];
    var list = null;      // the open <ul>
    var lastItem = null;  // its last <li>, for a nested list
    var nested = null;    // the open nested <ul>
    var titleSkipped = false;

    function flushParagraph() {
      if (paragraph.length) {
        fragment.appendChild(inline(paragraph.join(" "), h("p")));
        paragraph = [];
      }
    }

    function closeList() {
      list = null;
      lastItem = null;
      nested = null;
    }

    for (var i = 0; i < lines.length; i++) {
      var line = lines[i];
      var heading = /^(#{1,3})\s+(.*)$/.exec(line);
      var item = /^(\s*)[-*]\s+(.*)$/.exec(line);
      if (!line.trim()) {
        flushParagraph();
        closeList();
        continue;
      }
      if (heading) {
        flushParagraph();
        closeList();
        if (skipTitle && !titleSkipped && heading[1].length === 1) {
          titleSkipped = true;
          continue;
        }
        fragment.appendChild(inline(heading[2], h("h" + Math.min(6, heading[1].length + 2), { class: "md-h" + heading[1].length })));
        continue;
      }
      if (/^\s*\|/.test(line)) {
        flushParagraph();
        closeList();
        var table = h("table");
        var head = tableRow(line);
        var next = i + 1 < lines.length ? tableRow(lines[i + 1]) : [];
        var start = i + 1;
        if (next.length && isSeparatorRow(next)) {
          var headRow = h("tr");
          head.forEach(function (cell) { headRow.appendChild(inline(cell, h("th"))); });
          table.appendChild(h("thead", null, [headRow]));
          start = i + 2;
        } else {
          start = i;
        }
        var body = h("tbody");
        var j = start;
        while (j < lines.length && /^\s*\|/.test(lines[j])) {
          var row = h("tr");
          tableRow(lines[j]).forEach(function (cell) { row.appendChild(inline(cell, h("td"))); });
          body.appendChild(row);
          j++;
        }
        table.appendChild(body);
        fragment.appendChild(h("div", { class: "md-table" }, [table]));
        i = j - 1;
        continue;
      }
      if (item) {
        flushParagraph();
        var indented = item[1].length >= 2 && lastItem;
        if (indented) {
          if (!nested) {
            nested = h("ul");
            lastItem.appendChild(nested);
          }
          nested.appendChild(inline(item[2], h("li")));
        } else {
          if (!list) {
            list = h("ul");
            fragment.appendChild(list);
          }
          nested = null;
          lastItem = inline(item[2], h("li"));
          list.appendChild(lastItem);
        }
        continue;
      }
      if (/^\s{2,}\S/.test(line) && lastItem) {
        // A wrapped line of the last list item.
        (nested ? nested.lastChild : lastItem).appendChild(document.createTextNode(" "));
        inline(line.trim(), nested ? nested.lastChild : lastItem);
        continue;
      }
      closeList();
      paragraph.push(line.trim());
    }
    flushParagraph();
    return fragment;
  }

  GT4.markdown = { render: render };
})(window);
