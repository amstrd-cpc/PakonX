/* @ds-bundle: {"format":4,"namespace":"PakonX","components":[{"name":"Window"},{"name":"Button"},{"name":"StatusChip"},{"name":"ScanProgress"},{"name":"FilmStrip"},{"name":"Tabs"}]} */
(function () {
  function el(tag, cls, text) {
    var n = document.createElement(tag);
    if (cls) n.className = cls;
    if (text != null) n.textContent = text;
    return n;
  }

  // Window: title bar + body + optional status bar. Returns the body element to fill.
  function Window(opts) {
    opts = opts || {};
    var w = el('section', 'pk-window pk-root' + (opts.inactive ? ' is-inactive' : ''));
    w.setAttribute('aria-label', opts.title || 'Window');
    var bar = el('header', 'pk-titlebar');
    bar.appendChild(el('span', 'pk-titlebar__icon'));
    bar.appendChild(el('span', 'pk-titlebar__text', opts.title || ''));
    if (opts.controls !== false) {
      [['_', 'Minimize'], ['□', 'Maximize'], ['×', 'Close']].forEach(function (c, i) {
        var b = el('button', 'pk-caption-btn' + (i === 2 ? ' pk-caption-btn--close' : ''), c[0]);
        b.setAttribute('aria-label', c[1]);
        bar.appendChild(b);
      });
    }
    var body = el('div', 'pk-window__body');
    w.appendChild(bar);
    w.appendChild(body);
    if (opts.status) {
      var sb = el('footer', 'pk-statusbar');
      opts.status.forEach(function (s) { sb.appendChild(el('span', null, s)); });
      w.appendChild(sb);
    }
    if (opts.mount) opts.mount.appendChild(w);
    return { root: w, body: body };
  }

  // Button: variant 'default' | 'go' | 'danger'.
  function Button(opts) {
    opts = opts || {};
    var b = el('button', 'pk-btn' + (opts.variant && opts.variant !== 'default' ? ' pk-btn--' + opts.variant : ''), opts.label || '');
    b.type = 'button';
    if (opts.disabled) b.disabled = true;
    if (opts.onClick) b.addEventListener('click', opts.onClick);
    return b;
  }

  // StatusChip: tone 'neutral' | 'ok' | 'warn' | 'danger' | 'film'. The label always carries the meaning.
  function StatusChip(opts) {
    opts = opts || {};
    var tone = opts.tone && opts.tone !== 'neutral' ? ' pk-chip--' + opts.tone : '';
    return el('span', 'pk-chip' + tone, opts.label || '');
  }

  // ScanProgress: segmented XP bar. Returns { root, set(value, left, right) }.
  function ScanProgress(opts) {
    opts = opts || {};
    var root = el('div', 'pk-root');
    var trough = el('div', 'pk-progress' + (opts.tone === 'film' ? ' pk-progress--film' : ''));
    trough.setAttribute('role', 'progressbar');
    trough.setAttribute('aria-valuemin', '0');
    trough.setAttribute('aria-valuemax', '100');
    if (opts.label) trough.setAttribute('aria-label', opts.label);
    var fill = el('div', 'pk-progress__fill');
    trough.appendChild(fill);
    var meta = el('div', 'pk-progress-meta');
    var l = el('span'), r = el('span');
    meta.appendChild(l); meta.appendChild(r);
    root.appendChild(trough); root.appendChild(meta);
    function set(v, left, right) {
      v = Math.max(0, Math.min(100, v));
      fill.style.width = v + '%';
      trough.setAttribute('aria-valuenow', String(Math.round(v)));
      l.textContent = left != null ? left : '';
      r.textContent = right != null ? right : Math.round(v) + '%';
    }
    set(opts.value || 0, opts.left, opts.right);
    return { root: root, set: set };
  }

  // FilmStrip: frames [{ no, src?, flagged? }], selected index, onSelect(i).
  // multiple: Ctrl/Cmd-click or Space marks frames, Shift-click marks a run,
  // a plain click clears the marks; onMark(indices) reports the marked set.
  function FilmStrip(opts) {
    opts = opts || {};
    var per = opts.perRow > 0 ? opts.perRow : 0;
    var root = el('div', 'pk-strip pk-root' + (per ? ' pk-strip--sheet' : ''));
    // perRow: the roll as a contact sheet, strips of `per` frames one under
    // another, still one list (selection, marks and keys span every strip).
    var list = el('div', per ? 'pk-strip__sheet' : 'pk-strip__film');
    list.setAttribute('role', 'listbox');
    list.setAttribute('aria-label', opts.label || 'Frames');
    var frames = null;
    function row() {
      var film = per ? el('div', 'pk-strip__film') : list;
      film.appendChild(el('div', 'pk-strip__sprockets'));
      frames = el('div', 'pk-strip__frames');
      film.appendChild(frames);
      film.appendChild(el('div', 'pk-strip__sprockets'));
      if (per) list.appendChild(film);
    }
    var items = [];
    var marked = [];
    var anchor = null;
    if (opts.multiple) list.setAttribute('aria-multiselectable', 'true');
    function mark(list) {
      marked = list.filter(function (i, k) { return items[i] && list.indexOf(i) === k; }).sort(function (a, b) { return a - b; });
      items.forEach(function (n, j) {
        var on = marked.indexOf(j) >= 0;
        n.classList.toggle('is-marked', on);
        if (opts.multiple) n.setAttribute('aria-checked', String(on));
      });
      if (opts.onMark) opts.onMark(marked.slice());
    }
    function toggle(i) {
      anchor = i;
      mark(marked.indexOf(i) >= 0 ? marked.filter(function (j) { return j !== i; }) : marked.concat([i]));
    }
    function markRun(i) {
      var from = anchor == null ? i : anchor;
      var run = [];
      for (var j = Math.min(from, i); j <= Math.max(from, i); j++) run.push(j);
      mark(marked.concat(run));
    }
    function select(i) {
      items.forEach(function (n, j) {
        n.classList.toggle('is-selected', i === j);
        n.setAttribute('aria-selected', String(i === j));
      });
      if (opts.onSelect) opts.onSelect(i);
    }
    function go(j) { if (items[j]) { items[j].focus(); select(j); } }
    (opts.frames || []).forEach(function (f, i) {
      if (!frames || (per && i % per === 0)) row();
      var n = el('div', 'pk-frame' + (f.flagged ? ' is-flagged' : ''));
      n.setAttribute('role', 'option');
      n.tabIndex = 0;
      if (f.src) { var img = el('img', 'pk-frame__img'); img.src = f.src; img.alt = 'Frame ' + f.no; n.appendChild(img); }
      n.appendChild(el('span', 'pk-frame__no', f.no));
      n.addEventListener('click', function (e) {
        if (opts.multiple && (e.ctrlKey || e.metaKey)) toggle(i);
        else if (opts.multiple && e.shiftKey) markRun(i);
        else {
          anchor = i;
          if (marked.length) mark([]);
        }
        select(i);
      });
      n.addEventListener('keydown', function (e) {
        if (opts.multiple && e.key === ' ') { e.preventDefault(); toggle(i); }
        if (e.key === 'ArrowRight') go(i + 1);
        if (e.key === 'ArrowLeft') go(i - 1);
        if (per && e.key === 'ArrowDown') { e.preventDefault(); go(i + per); }
        if (per && e.key === 'ArrowUp') { e.preventDefault(); go(i - per); }
        if (e.key === 'Enter' && opts.onOpen) opts.onOpen(i);
      });
      if (opts.onOpen) n.addEventListener('dblclick', function () { opts.onOpen(i); });
      items.push(n);
      frames.appendChild(n);
    });
    if (!frames) row();
    root.appendChild(list);
    if (opts.selected != null) select(opts.selected);
    return { root: root, select: select, mark: mark };
  }

  // Tabs: XP property-sheet tabs. tabs [{ id, label }], selected id,
  // onSelect(id). The consumer shows and hides the panels.
  function Tabs(opts) {
    opts = opts || {};
    var root = el('div', 'pk-tabs pk-root');
    root.setAttribute('role', 'tablist');
    if (opts.label) root.setAttribute('aria-label', opts.label);
    var tabs = (opts.tabs || []).map(function (t) {
      var b = el('button', 'pk-tab', t.label);
      b.type = 'button';
      b.setAttribute('role', 'tab');
      b.id = 'pk-tab-' + t.id;
      b.addEventListener('click', function () { select(t.id, true); });
      root.appendChild(b);
      return { id: t.id, b: b };
    });
    function select(id, user) {
      tabs.forEach(function (t) {
        var on = t.id === id;
        t.b.classList.toggle('is-selected', on);
        t.b.setAttribute('aria-selected', String(on));
        t.b.tabIndex = on ? 0 : -1;
      });
      if (user && opts.onSelect) opts.onSelect(id);
    }
    root.addEventListener('keydown', function (e) {
      var i = tabs.findIndex(function (t) { return t.b === document.activeElement; });
      if (i < 0 || (e.key !== 'ArrowRight' && e.key !== 'ArrowLeft')) return;
      var j = (i + (e.key === 'ArrowRight' ? 1 : tabs.length - 1)) % tabs.length;
      tabs[j].b.focus();
      select(tabs[j].id, true);
    });
    if (opts.selected != null) select(opts.selected);
    return { root: root, select: function (id) { select(id, false); } };
  }

  window.PakonX = { Window: Window, Button: Button, StatusChip: StatusChip, ScanProgress: ScanProgress, FilmStrip: FilmStrip, Tabs: Tabs };
})();
