// PakonX light table. Talks to the scanner only through the Rust bridge:
// read-only pakon-cli commands (src-tauri/src/cli.rs), the film pass of the
// scan window (src-tauri/src/scan.rs: checks first, Stop through the
// teardown) and `frames` on scans on disk (src-tauri/src/roll.rs: open, save as JPEG/TIFF, delete the scan
// once saved); loose JPEG, TIFF and BMP files open through
// src-tauri/src/pictures.rs. Without Tauri (a plain browser, `?demo`) it
// reads sample data from web/demo/ so the design can be reviewed.
(function () {
  var P = window.PakonX;
  var tauri = window.__TAURI__ || null;
  var demo = !tauri && /[?&]demo\b/.test(location.search);

  // Backend ---------------------------------------------------------------------
  var api = tauri ? {
    runCli: function (command) { return tauri.core.invoke('run_cli', { command: command }); },
    cliPath: function () { return tauri.core.invoke('cli_path'); },
    platform: function () { return tauri.core.invoke('platform'); },
    openLink: function (page) { return tauri.core.invoke('open_link', { page: page }); },
    pickFolder: function (start) { return tauri.dialog.open({ directory: true, multiple: false, defaultPath: start || undefined }); },
    pickFiles: function (start) {
      return tauri.dialog.open({ multiple: true, defaultPath: start || undefined,
        filters: [{ name: 'Scans and pictures', extensions: ['pakraw', 'jpg', 'jpeg', 'tif', 'tiff', 'bmp'] }] });
    },
    ask: function (message, title) {
      return tauri.dialog.ask(message, { title: title, kind: 'warning', okLabel: 'Delete scan', cancelLabel: 'Keep scan' });
    },
    newOrder: function (base, roll) { return tauri.core.invoke('new_order', { base: base, roll: roll }); },
    defaultOrderBase: function () { return tauri.core.invoke('default_order_base'); },
    scanFiles: function (prefix) { return tauri.core.invoke('scan_files', { prefix: prefix }); },
    deleteScan: function (prefix, saved) { return tauri.core.invoke('delete_scan', { prefix: prefix, saved: saved }); },
    openPictures: function (paths) { return tauri.core.invoke('open_pictures', { paths: paths }); },
    savePictures: function (prefix, tailFirst, opts, pictures, dest) {
      return tauri.core.invoke('save_pictures', { prefix: prefix, tailFirst: tailFirst, format: opts.format, quality: opts.quality,
                                                  dxNames: opts.dxNames, pictures: pictures, dest: dest,
                                                  name: opts.name, start: opts.start, digits: opts.digits });
    },
    nlpStatus: function () { return tauri.core.invoke('nlp_status'); },
    openInNlp: function (prefix, tailFirst, pictures) {
      return tauri.core.invoke('open_in_nlp', { prefix: prefix, tailFirst: tailFirst, pictures: pictures });
    },
    cancel: function () { return tauri.core.invoke('cancel_frames'); },
    fullPicture: function (prefix, tailFirst, picture, rotate) {
      return tauri.core.invoke('full_picture', { prefix: prefix, tailFirst: tailFirst, picture: picture, rotate: rotate });
    },
    oemFullPicture: function (dir, picture, rotate) {
      return tauri.core.invoke('oem_full_picture', { dir: dir, picture: picture, rotate: rotate });
    },
    rollInfo: function (prefix) { return tauri.core.invoke('roll_info', { prefix: prefix }); },
    fullPictureFile: function (path) { return tauri.core.invoke('full_picture_file', { path: path }); },
    loadAdjust: function (prefix) { return tauri.core.invoke('load_adjust', { prefix: prefix }); },
    loadPresets: function () { return tauri.core.invoke('load_presets'); },
    savePresets: function (presets) { return tauri.core.invoke('save_presets', { presets: presets }); },
    pickFile: function (start) { return tauri.dialog.open({ multiple: false, defaultPath: start || undefined }); },
    openLook: function (prefix, o) {
      return tauri.core.invoke('open_look', { prefix: prefix, oem: o.oem, capture: o.capture, eeprom: o.eeprom || null });
    },
    saveLook: function (prefix, o, pictures, dest, opts) {
      return tauri.core.invoke('save_look', { prefix: prefix, oem: o.oem, capture: o.capture, eeprom: o.eeprom || null,
                                              pictures: pictures, dest: dest, dxNames: opts.dxNames,
                                              name: opts.name, start: opts.start, digits: opts.digits, tailFirst: opts.tailFirst,
                                              format: opts.format, quality: opts.quality });
    },
    saveAdjust: function (prefix, pictures, oemLooks) {
      return tauri.core.invoke('save_adjust', { prefix: prefix, pictures: pictures, oem: oemLooks });
    },
    saveShift: function (prefix, shift) { return tauri.core.invoke('save_shift', { prefix: prefix, shift: shift }); },
    saveFilm: function (prefix, bw) { return tauri.core.invoke('save_film', { prefix: prefix, bw: bw }); },
    saveIce: function (prefix, ice) { return tauri.core.invoke('save_ice', { prefix: prefix, ice: ice }); },
    saveFrameFormat: function (prefix, format) { return tauri.core.invoke('save_frame_format', { prefix: prefix, format: format }); },
    onProgress: function (fn) { return tauri.event.listen('frames-progress', function (e) { fn(e.payload); }); },
    listRolls: function (dir) { return tauri.core.invoke('list_rolls', { dir: dir }); },
    initScanner: function () { return tauri.core.invoke('init_scanner'); },
    bundledOem: function () { return tauri.core.invoke('bundled_oem'); },
    onInitStage: function (fn) { return tauri.event.listen('scanner-init', function (e) { fn(e.payload); }); },
    onScanChecked: function (fn) { return tauri.event.listen('scan-checked', function () { fn(); }); },
    startScan: function (s) { return tauri.core.invoke('start_scan', { mode: s.mode, rows: s.rows, folder: s.folder, name: s.name, film: s.film, filmGate: s.gate, hotStart: s.hot || null }); },
    stopScan: function () { return tauri.core.invoke('stop_scan'); },
    driveFilm: function (reverse) { return tauri.core.invoke('drive_film', { reverse: reverse }); },
    onScanProgress: function (fn) { return tauri.event.listen('scan-progress', function (e) { fn(e.payload); }); },
    onScanLine: function (fn) { return tauri.event.listen('scan-line', function (e) { fn(e.payload); }); },
    checkUpdate: function () { return tauri.core.invoke('check_update'); },
    installUpdate: function () { return tauri.core.invoke('install_update'); },
    onUpdateProgress: function (fn) { return tauri.event.listen('update-progress', function (e) { fn(e.payload); }); },
    openRoll: function (prefix, tailFirst) { return tauri.core.invoke('open_roll', { prefix: prefix, tailFirst: tailFirst }); },
    img: function (path) { return tauri.core.convertFileSrc(path); }
  } : {
    runCli: function () { return Promise.reject('Not running inside PakonX: start it with `npm run dev`.'); },
    cliPath: function () { return Promise.resolve('(browser preview)'); },
    platform: function () { return Promise.resolve('windows'); },
    openLink: function (page) {
      window.open(page === 'github' ? 'https://github.com/amstrd-cpc' : 'https://www.instagram.com/amstrd_cpc', '_blank');
      return Promise.resolve();
    },
    pickFolder: function () { return Promise.resolve(demo ? 'demo' : null); },
    pickFiles: function () { return Promise.resolve(null); },
    ask: function () { return Promise.resolve(false); },
    scanFiles: function () { return Promise.resolve([]); },
    newOrder: function (base, roll) { return Promise.resolve(base + '/' + roll); },
    defaultOrderBase: function () { return Promise.resolve('Pictures/PakonX'); },
    deleteScan: function () { return Promise.reject('Not running inside PakonX.'); },
    openPictures: function () { return Promise.reject('Not running inside PakonX.'); },
    savePictures: function () { return Promise.reject('Saving needs PakonX: start it with `npm run dev`.'); },
    nlpStatus: function () { return Promise.resolve({ lightroom: null, plugin: false }); },
    openInNlp: function () { return Promise.reject('Not running inside PakonX.'); },
    cancel: function () { return Promise.resolve(false); },
    fullPicture: function (prefix, tailFirst, picture) {
      return Promise.resolve('demo/' + prefix + '/' + prefix + '.frame' + String(picture).padStart(2, '0') + '.preview.bmp');
    },
    fullPictureFile: function () { return Promise.reject('Not running inside PakonX.'); },
    oemFullPicture: function () { return Promise.reject('The OEM look needs PakonX on Windows.'); },
    rollInfo: function () { return Promise.reject('Not running inside PakonX.'); },
    loadAdjust: function () { return Promise.resolve({ pictures: {}, shift: 0 }); },
    loadPresets: function () { return Promise.resolve({}); },
    savePresets: function () { return Promise.resolve(); },
    pickFile: function () { return Promise.resolve(null); },
    openLook: function () { return Promise.reject('The OEM look needs PakonX on Windows.'); },
    saveLook: function () { return Promise.reject('The OEM look needs PakonX on Windows.'); },
    saveAdjust: function () { return Promise.resolve(); },
    saveShift: function () { return Promise.reject('Framing needs PakonX: start it with `npm run dev`.'); },
    saveFilm: function () { return Promise.resolve(); },
    onProgress: function () {},
    initScanner: function () { return Promise.reject('Not running inside PakonX: the scanner is not initialized.'); },
    bundledOem: function () { return Promise.resolve({ oem: null, capture: null, host: false }); },
    onInitStage: function () {},
    onScanChecked: function () {},
    startScan: function () { return Promise.reject('Scanning needs PakonX: start it with `npm run dev`.'); },
    stopScan: function () { return Promise.resolve(false); },
    driveFilm: function () { return Promise.reject('The film drive needs PakonX: start it with `npm run dev`.'); },
    onScanProgress: function () {},
    onScanLine: function () {},
    checkUpdate: function () { return Promise.resolve(null); },
    installUpdate: function () { return Promise.reject('Updating needs PakonX.'); },
    onUpdateProgress: function () {},
    listRolls: function () { return demo ? fetch('demo/rolls.json').then(function (r) { return r.json(); }) : Promise.resolve([]); },
    openRoll: function (prefix) { return fetch('demo/' + prefix + '/roll.json').then(function (r) { return r.json(); }); },
    img: function (path) { return path; }
  };
  function sep(cache) { return cache.indexOf('\\') >= 0 ? '\\' : '/'; }
  function dirname(path) { return path.slice(0, Math.max(path.lastIndexOf('/'), path.lastIndexOf('\\'))); }

  var state = {
    scanner: 'Not checked', tone: 'neutral', model: '—', version: '—',
    folder: null, rolls: [], roll: null, loose: null, saved: null, marked: [], looks: {},
    dev: 'pakonx', oemLook: null, oemLooks: {}, pictures: [], selected: -1, busy: false, tailFirst: false,
    // checking: a scanner command or start-up runs; scanning: a scan (its checks, then the film pass) runs.
    checking: false, scanning: false
  };

  // Theme -------------------------------------------------------------------------
  var themeSelect = document.getElementById('theme');
  try { var saved = localStorage.getItem('pakonx.theme'); if (saved) themeSelect.value = saved; } catch (e) {}
  function applyTheme() {
    document.documentElement.setAttribute('data-theme', themeSelect.value);
    try { localStorage.setItem('pakonx.theme', themeSelect.value); } catch (e) {}
  }
  themeSelect.addEventListener('change', applyTheme);
  applyTheme();

  // Helpers -------------------------------------------------------------------------
  function el(tag, cls, text) {
    var n = document.createElement(tag);
    if (cls) n.className = cls;
    if (text != null) n.textContent = text;
    return n;
  }
  function modeLabel(mode) {
    var m = /^base(\d+)(-ir)?$/.exec(mode || '');
    return m ? 'Base ' + m[1] + (m[2] ? ' · ICE' : '') : (mode || '—');
  }
  function frameLabel(f) { return f.dx_frame || ('#' + f.picture); }
  function sizeLabel(bytes) {
    return bytes >= 1048576 ? (bytes / 1048576).toFixed(1) + ' MB' : Math.max(1, Math.round(bytes / 1024)) + ' KB';
  }
  // label: what runs ("Developing", "Saving"); progress: show the bar
  // (pakon-cli reports pictures as it finishes them) with Cancel.
  function setBusy(label, progress) {
    state.busy = !!label;
    busySlot.replaceChildren(label ? P.StatusChip({ label: label, tone: 'warn' }) : document.createTextNode(''));
    progressRow.hidden = !(label && progress);
    if (label && progress) {
      progressLabel = label;
      bar.set(0, label + ' — starting', '');
      cancelBtn.disabled = false;
    }
    updateButtons();
  }
  // A cancelled run is the user's choice, not a fault: say so plainly.
  function failed(e, label) {
    var text = String(e);
    var cancelled = /^Cancelled\./.test(text);
    busySlot.replaceChildren(P.StatusChip({ label: cancelled ? 'Cancelled' : label, tone: cancelled ? 'neutral' : 'danger' }));
    write(cancelled ? 'Cancelled.' : text, cancelled ? null : 'err');
  }
  function facts(dl, rows) {
    dl.replaceChildren();
    rows.forEach(function (r) {
      dl.append(el('dt', null, r[0]), el('dd', r[2] ? 'data' : null, r[1]));
    });
  }

  // Tabs: Scan · Roll · Light table (one task on screen at a time) ------------------
  var desk = document.querySelector('.desk');
  var tabs = P.Tabs({ tabs: [{ id: 'scan', label: 'Scan' }, { id: 'roll', label: 'Roll' }, { id: 'table', label: 'Light table' }],
                      label: 'Tasks', onSelect: function (id) { showTab(id); } });
  document.getElementById('tabs').append(tabs.root);
  function showTab(id) {
    desk.dataset.tab = id;
    tabs.select(id);
    try { localStorage.setItem('pakonx.tab', id); } catch (e) {}
    if (id === 'table') fitPicture();
    if (id === 'roll' && sheet) {
      var on = sheet.root.querySelector('.is-selected');
      if (on) on.scrollIntoView({ block: 'nearest' });
    }
  }

  // Rolls task pane (Roll and Light table) --------------------------------------------
  var left = document.getElementById('pane');
  var rollsWin = P.Window({ title: 'Rolls', controls: false, mount: left });
  var folderRow = el('div', 'actions');
  var folderLabel = el('p', 'note caption folder', 'Nothing open.');
  folderRow.append(P.Button({ label: 'Open', onClick: chooseFiles }));
  var rollList = el('ul', 'rolls body');
  rollList.setAttribute('role', 'listbox');
  rollList.setAttribute('aria-label', 'Rolls in this folder');
  rollsWin.body.append(folderRow, folderLabel, rollList);

  // One picker for everything: a .film.pakraw opens its roll, JPEG, TIFF
  // and BMP files open as pictures. The folder's rolls list on the left.
  function chooseFiles() {
    if (state.busy) return;
    api.pickFiles(state.folder).then(function (paths) {
      if (!paths || !paths.length) return;
      paths = [].concat(paths);
      state.folder = dirname(paths[0]);
      try { localStorage.setItem('pakonx.folder', state.folder); } catch (e) {}
      loadRolls();
      var rolls = [];
      var pictures = [];
      paths.forEach(function (p) {
        if (/\.film\.pakraw$/i.test(p)) rolls.push(p.replace(/\.film\.pakraw$/i, ''));
        else if (/\.pakraw$/i.test(p)) write('Not a roll: ' + p + ' (only .film.pakraw scans hold pictures)', 'err');
        else pictures.push(p);
      });
      if (pictures.length) {
        if (rolls.length) write('Rolls are listed on the left; opening the pictures.');
        openPictures(pictures);
      } else if (rolls.length) {
        if (rolls.length > 1) write('Opening the first roll; the others are listed on the left.');
        openRoll(rolls[0]);
      }
    });
  }

  function loadRolls() {
    folderLabel.textContent = state.folder;
    api.listRolls(state.folder).then(function (rolls) {
      state.rolls = rolls;
      write(rolls.length + ' roll' + (rolls.length === 1 ? '' : 's') + ' in ' + state.folder);
      renderRolls();
      var want = demo && /[?&]roll=([^&]+)/.exec(location.search);
      if (want) openRoll(decodeURIComponent(want[1]));
    }).catch(function (e) { write(String(e), 'err'); });
  }

  function renderRolls() {
    rollList.replaceChildren();
    if (!state.folder) {
      rollList.append(el('li', 'rolls__empty caption', 'Open a scan (.film.pakraw) or pictures (JPEG, TIFF, BMP).'));
      return;
    }
    if (!state.rolls.length) {
      rollList.append(el('li', 'rolls__empty caption', 'No scanned rolls here. A roll is a .film.pakraw with its .scan.json.'));
      return;
    }
    state.rolls.forEach(function (r) {
      var on = !state.loose && state.roll && state.roll.prefix === r.prefix;
      var li = el('li', 'rolls__item' + (on ? ' is-selected' : ''));
      li.setAttribute('role', 'option');
      li.setAttribute('aria-selected', String(!!on));
      li.tabIndex = 0;
      li.append(el('span', 'rolls__name', r.name), el('span', 'rolls__mode caption', modeLabel(r.mode)));
      li.addEventListener('click', function () { openRoll(r.prefix); });
      li.addEventListener('keydown', function (e) { if (e.key === 'Enter') openRoll(r.prefix); });
      rollList.append(li);
    });
  }

  // Left: scanner (read-only) -----------------------------------------------------------
  var viewScan = document.getElementById('view-scan');
  var scannerWin = P.Window({ title: 'Scanner', controls: false, mount: viewScan });
  var chipSlot = el('div');
  var scanActions = el('div', 'actions');
  scanActions.style.marginTop = 'var(--space-3)';
  var scannerFacts = el('dl', 'facts body');
  // Find / Identify / Status are for troubleshooting: folded away.
  var tools = el('details', 'oembox tools');
  var toolsActions = el('div', 'actions');
  tools.append(el('summary', 'caption', 'Device tools'), toolsActions);
  scannerWin.body.append(chipSlot, scanActions, scannerFacts, tools);
  var scanButtons = [
    P.Button({ label: 'Initialize', onClick: function () { initScanner(); } }),
    P.Button({ label: 'Find', onClick: function () { run('list'); } }),
    P.Button({ label: 'Identify', onClick: function () { run('identify'); } }),
    P.Button({ label: 'Status', onClick: function () { run('status'); } })
  ];
  scanActions.append(scanButtons[0]);
  toolsActions.append(scanButtons[1], scanButtons[2], scanButtons[3]);

  // Scan film: the runbook's film pass (docs/HARDWARE_RUNBOOK.md §7-8).
  // The bridge runs the read-only checks itself and refuses the scan when
  // one fails.
  // The film end stops the pass; the row cap is only a safety bound, with
  // room for a long roll (a 36-exposure Base 16 IR roll is ~133 000 rows).
  var ROWS = { 4: 100000, 8: 150000, 16: 220000 };
  // The Scan film window comes first in the Scan tab.
  var scanWin = P.Window({ title: 'Scan film', controls: false });
  viewScan.prepend(scanWin.root);
  var scanGroup = el('div', 'scangroup');
  // Resolution is the base; ICE switches the infrared lane on (the -ir modes).
  // Older versions stored the mode itself ('base8-ir'): read it as base + ICE.
  try {
    var oldMode = localStorage.getItem('pakonx.scanMode');
    if (oldMode && /-ir$/.test(oldMode)) {
      localStorage.setItem('pakonx.scanMode', oldMode.replace(/-ir$/, ''));
      localStorage.setItem('pakonx.scanIce', '1');
    }
  } catch (e) {}
  var scanMode = choice('Resolution', 'pakonx.scanMode', [['base4', 'Base 4'], ['base8', 'Base 8'], ['base16', 'Base 16']], 'base8');
  function iceSwitch(label) {
    var row = el('label', 'row caption');
    var input = el('input', 'pk-switch');
    input.type = 'checkbox';
    input.setAttribute('role', 'switch');
    row.append(el('span', null, label), el('span', 'switchcell'));
    row.lastChild.append(input);
    return { row: row, input: input };
  }
  var scanIce = iceSwitch('Dust removal (ICE)');
  try { scanIce.input.checked = localStorage.getItem('pakonx.scanIce') === '1'; } catch (e) {}
  scanIce.input.addEventListener('change', function () {
    try { localStorage.setItem('pakonx.scanIce', scanIce.input.checked ? '1' : ''); } catch (e) {}
    renderScan();
  });
  scanIce.row.title = 'Scans the infrared lane too, so dust and scratches can be removed. Slower; not for silver black-and-white film.';
  function scanModeValue() { return scanMode.select.value + (scanIce.input.checked ? '-ir' : ''); }
  // Film gate (scan --film-gate): how the film end is decided.
  var scanGate = choice('Film end', 'pakonx.scanGate', [['auto', 'Light + DX sensor'], ['brightness', 'Light only'], ['off', 'None (row cap)']], 'auto');

  // The order (as PSI): New order asks for the roll number from the
  // sticker and makes <base folder>\<roll number>; the scan goes there
  // (1111.film.pakraw) and its pictures are named 1111_19A.jpg. The base
  // folder is set in Settings.
  // Workflow: a single roll, or a batch — one folder
  // <base>\<batch name>, rolls named from a template with a running
  // number ({batch}, {n} = 001…, {date} = YYYYMMDD), each hot-started from
  // the roll before. An open batch survives a restart until Finish batch.
  var order = null; // { roll, folder, film, bw, batch, n, template, strips }
  function rollName(template, batch, n) {
    var d = new Date();
    var date = d.getFullYear() + String(d.getMonth() + 1).padStart(2, '0') + String(d.getDate()).padStart(2, '0');
    return template.replace(/\{batch\}/g, batch).replace(/\{n\}/g, String(n).padStart(3, '0')).replace(/\{date\}/g, date)
      .replace(/\s+/g, '_').replace(/[^A-Za-z0-9_-]/g, '').slice(0, 40);
  }
  function stripName() { return order.batch ? rollName(order.template, order.roll, order.n) : order.roll; }
  function keepBatch() {
    try {
      if (order && order.batch) localStorage.setItem('pakonx.batch', JSON.stringify(order));
      else localStorage.removeItem('pakonx.batch');
    } catch (e) {}
  }
  try { var kept = JSON.parse(localStorage.getItem('pakonx.batch') || 'null'); if (kept && kept.batch && kept.folder) order = kept; } catch (e) {}
  var finishBatchBtn = P.Button({ label: 'Finish batch', onClick: function () {
    if (!order || !order.batch) return;
    write('Batch ' + order.roll + ' finished: ' + (order.strips || []).length + ' roll(s) in ' + order.folder, 'ok');
    order = null; keepBatch(); renderScan();
  } });
  var orderRow = el('div', 'row caption');
  var orderValue = el('span', 'data orderrow__value');
  orderRow.append(el('span', null, 'Order'), orderValue);
  var orderFolder = el('p', 'note caption folder');
  var newOrderBtn = P.Button({ label: 'New order', onClick: function () { askOrder(false); } });
  var orderActions = el('div', 'actions');
  orderActions.append(newOrderBtn, finishBatchBtn);
  var capNote = el('p', 'note caption');
  var scanBtn = P.Button({ label: 'Scan', variant: 'go', disabled: true, onClick: function () { startScan(); } });
  var scanBtns = el('div', 'actions');
  scanBtns.append(scanBtn);
  var scanNote = el('p', 'note caption savegroup__note');
  scanGroup.append(orderRow, orderFolder, orderActions, scanMode.row, scanIce.row, scanGate.row, scanBtns, capNote, scanNote);
  scanWin.body.append(scanGroup);

  // Film transport (PSI's Advance / Reverse Film): for film stuck in the
  // scanner. The motor alone at the Base 4 scan rate until Stop (60 s at most);
  // lamp off. Uses the scan's Stop (stdin) and its one-live-command guard.
  var driveGroup = el('fieldset', 'group scangroup');
  driveGroup.append(el('legend', 'label', 'Advance'));
  var driving = false;
  function drive(reverse) {
    if (state.busy || state.checking || driving) return;
    driving = true;
    setBusy(reverse ? 'Reversing film' : 'Advancing film', false);
    write('> pakon-cli film-drive ' + (reverse ? '--reverse' : '--forward') + ' --seconds 60 --stop-on-stdin --live', 'cmd');
    renderDrive();
    api.driveFilm(reverse).then(function (d) {
      if (d.log) write(d.log, d.code === 0 ? null : 'err');
      write(d.teardown_ok ? 'Film drive stopped; motor stopped.' : 'The film drive did not finish its teardown. Check the scanner: if the motor runs, switch it off.',
            d.teardown_ok ? 'ok' : 'err');
    }).catch(function (e) { failed(e, 'Film drive failed'); })
      .then(function () { driving = false; setBusy(null); renderDrive(); render(); });
  }
  var reverseBtn = P.Button({ label: '<', onClick: function () { drive(true); } });
  var advanceBtn = P.Button({ label: '>', onClick: function () { drive(false); } });
  reverseBtn.setAttribute('aria-label', 'Film backward');
  advanceBtn.setAttribute('aria-label', 'Film forward');
  reverseBtn.title = 'Film backward';
  advanceBtn.title = 'Film forward';
  var driveStopBtn = P.Button({ label: 'Stop', variant: 'danger', onClick: function () { api.stopScan(); } });
  var driveBtns = el('div', 'actions');
  driveBtns.append(reverseBtn, advanceBtn, driveStopBtn);
  driveGroup.append(driveBtns, el('p', 'note caption savegroup__note', 'For stuck film: < backward, > forward, at Base 4 speed until Stop (at most 60 s). The lamp stays off.'));
  scannerWin.body.append(driveGroup);
  function renderDrive() {
    advanceBtn.disabled = reverseBtn.disabled = driving || state.busy || state.checking || state.scanning;
    driveStopBtn.disabled = !driving;
  }
  [scanMode.select, scanGate.select].forEach(function (s) { s.addEventListener('change', renderScan); });

  // New order dialog: the roll number, then the folder is made.
  var orderDlg = el('dialog', 'helpdlg pk-root');
  orderDlg.setAttribute('aria-labelledby', 'order-title');
  var orderTitle = el('h2', 'heading', 'New order');
  orderTitle.id = 'order-title';
  var orderForm = el('form');
  orderForm.method = 'dialog';
  var orderInputRow = el('label', 'row caption');
  var orderInput = el('input', 'field');
  orderInput.type = 'text';
  orderInput.spellcheck = false;
  orderInput.autocomplete = 'off';
  orderInput.maxLength = 40;
  orderInput.placeholder = 'e.g. 1111';
  var orderInputLabel = el('span', null, 'Roll number');
  orderInputRow.append(orderInputLabel, orderInput);
  var orderFlowRow = el('label', 'row caption');
  var orderFlow = el('select', 'field');
  [['single', 'Single roll'], ['batch', 'Batch (numbered rolls, one folder)']].forEach(function (o) {
    var opt = el('option', null, o[1]);
    opt.value = o[0];
    orderFlow.append(opt);
  });
  orderFlowRow.append(el('span', null, 'Workflow'), orderFlow);
  var orderStartRow = el('label', 'row caption');
  var orderStart = el('input', 'field');
  orderStart.type = 'number'; orderStart.min = 1; orderStart.max = 99999; orderStart.value = 1;
  orderStartRow.append(el('span', null, 'Start number'), orderStart);
  var orderTplRow = el('label', 'row caption');
  var orderTpl = el('input', 'field');
  orderTpl.type = 'text'; orderTpl.spellcheck = false; orderTpl.maxLength = 60;
  try { orderTpl.value = localStorage.getItem('pakonx.rollTemplate') || '{batch}_{n}'; } catch (e) { orderTpl.value = '{batch}_{n}'; }
  orderTplRow.append(el('span', null, 'Roll names'), orderTpl);
  var orderHint = el('p', 'note caption savegroup__note');
  var orderOk = P.Button({ label: 'Create order', variant: 'go' });
  orderOk.type = 'submit';
  var orderCancel = P.Button({ label: 'Cancel', onClick: function () { orderDlg.close(); } });
  var orderBtns = el('div', 'actions helpdlg__actions');
  orderBtns.append(orderCancel, orderOk);
  // Film: sets the scan's exposure and film-end detection (scan --film)
  // and how the roll develops (B&W kinds turn grey, a slide is not
  // inverted); kept with the roll.
  var FILMS = [['c41', 'Colour negative (C-41)'], ['c41-unmasked', 'Colour negative, no orange mask'],
               ['e6', 'Colour slide (E-6)'], ['bw-silver', 'Black & white (silver)'],
               ['bw-c41', 'Black & white (C-41 process)']];
  function filmLabel(id) { return (FILMS.filter(function (f) { return f[0] === id; })[0] || FILMS[0])[1]; }
  var orderFilmRow = el('label', 'row caption');
  var orderFilm = el('select', 'field');
  FILMS.forEach(function (o) {
    var opt = el('option', null, o[1]);
    opt.value = o[0];
    orderFilm.append(opt);
  });
  orderFilmRow.append(el('span', null, 'Film'), orderFilm);
  orderForm.append(orderFlowRow, orderInputRow, orderStartRow, orderTplRow, orderFilmRow, orderHint, orderBtns);
  function orderFlowShow() {
    var batch = orderFlow.value === 'batch';
    orderInputLabel.textContent = batch ? 'Batch name' : 'Roll number';
    orderInput.placeholder = batch ? 'e.g. lab-0611' : 'e.g. 1111';
    orderStartRow.hidden = orderTplRow.hidden = !batch;
    orderOk.textContent = batch ? 'Start batch' : 'Create order';
    orderHintText(null);
  }
  [orderFlow, orderStart, orderTpl].forEach(function (c) { c.addEventListener('input', orderFlowShow); c.addEventListener('change', orderFlowShow); });
  orderDlg.append(orderTitle, orderForm);
  document.body.append(orderDlg);
  var orderThenScan = false;
  function orderHintText(err) {
    var batch = orderFlow.value === 'batch';
    orderHint.textContent = err || (batch
      ? 'Folder: ' + joinPath(orderBase(), orderInput.value || '…') + '. First roll: ' + (rollName(orderTpl.value, orderInput.value || 'batch', Number(orderStart.value) || 1) || '…')
      : 'Number from the roll\'s sticker. Folder: ' + joinPath(orderBase(), orderInput.value || '…'));
    orderHint.classList.toggle('is-error', !!err);
  }
  orderInput.addEventListener('input', function () { orderHintText(null); });
  function askOrder(thenScan) {
    if (state.busy || state.checking) return;
    if (order && order.batch) {
      write('Batch ' + order.roll + ' is open: Finish batch first, then make a new order.', 'err');
      return;
    }
    orderThenScan = thenScan;
    orderInput.value = '';
    orderFlowShow();
    orderDlg.showModal();
    orderInput.focus();
  }
  orderForm.addEventListener('submit', function (e) {
    e.preventDefault();
    var roll = orderInput.value.trim();
    if (!/^[A-Za-z0-9_][A-Za-z0-9_-]{0,39}$/.test(roll)) {
      orderHintText('A roll number is 1-40 letters, digits, - or _.');
      return;
    }
    var batch = orderFlow.value === 'batch';
    var start = Math.floor(Number(orderStart.value));
    if (batch && !(start >= 1 && start <= 99999)) { orderHintText('The start number is 1-99999.'); return; }
    var first = batch ? rollName(orderTpl.value, roll, start) : '';
    if (batch && (orderTpl.value.indexOf('{n}') < 0 || !/^[A-Za-z0-9_][A-Za-z0-9_-]*$/.test(first))) {
      orderHintText('Roll names need {n} and must give letters, digits, - or _.');
      return;
    }
    // Names are cut at 40 characters: the number must survive the cut.
    if (batch && (first.length >= 40 || first === rollName(orderTpl.value, roll, start + 1))) {
      orderHintText('Roll names get too long for the number to fit (40 characters): use a shorter batch name or template.');
      return;
    }
    api.newOrder(orderBase(), roll).then(function (folder) {
      order = { roll: roll, folder: folder, film: orderFilm.value, bw: /^bw/.test(orderFilm.value),
                batch: batch, n: start, template: orderTpl.value, strips: [] };
      if (batch) { try { localStorage.setItem('pakonx.rollTemplate', orderTpl.value); } catch (e) {} }
      keepBatch();
      orderDlg.close();
      write((batch ? 'Batch ' : 'Order ') + roll + ': ' + folder, 'ok');
      renderScan();
      if (orderThenScan) startScan();
    }).catch(function (err) { orderHintText(String(err)); });
  });

  // Settings: where order folders are made.
  var orderBaseDefault = '';
  api.defaultOrderBase().then(function (d) { orderBaseDefault = d; renderSettings(); renderScan(); }).catch(function () {});
  function orderBase() {
    var b = null;
    try { b = localStorage.getItem('pakonx.orderBase'); } catch (e) {}
    return b || orderBaseDefault;
  }
  function joinPath(a, b) { return a ? a + sep(a) + b : b; }
  // Settings (app bar): orders, appearance, development and the OEM look
  // files, about. Sections are filled where their parts are made.
  var settingsDlg = el('dialog', 'helpdlg settingsdlg pk-root');
  settingsDlg.setAttribute('aria-labelledby', 'settings-title');
  var settingsTitle = el('h2', 'heading', 'Settings');
  settingsTitle.id = 'settings-title';
  function settingsSection(title) {
    var s = el('section', 'settings__section');
    s.append(el('h3', 'label', title));
    return s;
  }
  var setOrders = settingsSection('Orders');
  var settingsFacts = el('dl', 'facts body');
  var settingsBtns = el('div', 'actions');
  settingsBtns.append(
    P.Button({ label: 'Change base folder', onClick: function () {
      api.pickFolder(orderBase()).then(function (p) {
        if (!p) return;
        try { localStorage.setItem('pakonx.orderBase', p); } catch (e) {}
        renderSettings();
        renderScan();
      });
    } }),
    P.Button({ label: 'Default', onClick: function () {
      try { localStorage.removeItem('pakonx.orderBase'); } catch (e) {}
      renderSettings();
      renderScan();
    } }));
  setOrders.append(settingsFacts,
    el('p', 'note caption savegroup__note', 'New order makes a folder named by the roll number here; the scan and its saved pictures go into it.'),
    settingsBtns);
  var setLooks = settingsSection('Appearance');
  var themeRow = document.querySelector('.appbar__theme');
  themeRow.className = 'row caption';
  setLooks.append(themeRow);
  var setDevelop = settingsSection('Development');
  var openWithRow = el('label', 'row caption');
  var openWithSelect = el('select', 'field');
  [['oem', 'OEM Pakon look'], ['pakonx', 'PakonX look (adjustable)']].forEach(function (o) {
    var opt = el('option', null, o[1]);
    opt.value = o[0];
    openWithSelect.append(opt);
  });
  openWithRow.append(el('span', null, 'Open rolls with'), openWithSelect);
  openWithSelect.addEventListener('change', function () {
    try { localStorage.setItem('pakonx.openWith', openWithSelect.value); } catch (e) {}
  });
  var settingsOem = el('div', 'settings__oem');
  setDevelop.append(openWithRow,
    el('p', 'note caption savegroup__note', 'The other look is developed when you choose it under Develop with (Look panel).'),
    el('h3', 'label', 'OEM look files'), settingsOem);
  var setAbout = settingsSection('About');
  var aboutFacts = el('dl', 'facts body');
  var aboutBy = el('p', 'body about__by', 'Built by Erik Muradian');
  var aboutLinks = el('div', 'actions');
  aboutLinks.append(
    P.Button({ label: 'GitHub · amstrd-cpc', onClick: function () { api.openLink('github').catch(function (e) { write(String(e), 'err'); }); } }),
    P.Button({ label: 'Instagram · amstrd_cpc', onClick: function () { api.openLink('instagram').catch(function (e) { write(String(e), 'err'); }); } }),
    P.Button({ label: 'Source code', onClick: function () { api.openLink('source').catch(function (e) { write(String(e), 'err'); }); } }),
    P.Button({ label: 'Support on Ko-fi', onClick: function () { api.openLink('kofi').catch(function (e) { write(String(e), 'err'); }); } }));
  var aboutCopy = el('p', 'note caption savegroup__note', '© 2026 Erik Muradian. All rights reserved. Kodak and Pakon are trademarks of their owners; the OEM look runs their PakonIMAu.dll.');
  // Updates (src-tauri/src/updater.rs): checked on start and from here.
  var updateNote = el('p', 'note caption savegroup__note');
  var updateBtn = P.Button({ label: 'Check for updates', onClick: function () { checkUpdate(true); } });
  var updateActions = el('div', 'actions');
  updateActions.append(updateBtn);
  setAbout.append(aboutBy, aboutLinks, aboutFacts, updateActions, updateNote, aboutCopy);
  var appVersion = '';
  if (tauri && tauri.app && tauri.app.getVersion) tauri.app.getVersion().then(function (v) { appVersion = v; renderSettings(); });
  var cliPathText = '';
  api.cliPath().then(function (p) { cliPathText = p; });
  var settingsClose = el('div', 'actions helpdlg__actions');
  settingsClose.append(P.Button({ label: 'Close', onClick: function () { settingsDlg.close(); } }));
  settingsDlg.append(settingsTitle, setOrders, setLooks, setDevelop, setAbout, settingsClose);
  settingsDlg.addEventListener('click', function (e) { if (e.target === settingsDlg) settingsDlg.close(); });
  document.body.append(settingsDlg);
  function renderSettings() {
    var own = null;
    try { own = localStorage.getItem('pakonx.orderBase'); } catch (e) {}
    facts(settingsFacts, [['Base folder', orderBase() || '—', true], ['', own ? 'Chosen' : 'Default (Pictures\\PakonX)']]);
    openWithSelect.value = openWith();
    facts(aboutFacts, [
      ['PakonX', appVersion || '—', true],
      ['Scanner', 'Kodak/Pakon F-135+ (Windows 10/11, WinUSB)'],
      ['pakon-cli', cliPathText || '—', true],
      ['Firmware', 'Bundled, loaded on start when the scanner is cold'],
      ['OEM look', 'Kodak/Pakon PakonIMAu.dll, run by oem_host.exe'],
      ['Logs', '%LOCALAPPDATA%\\app.pakonx.desktop\\logs', true]
    ]);
  }
  var settingsBtn = el('button', 'pk-btn appbar__settings', 'Settings');
  settingsBtn.type = 'button';
  settingsBtn.addEventListener('click', function () { renderSettings(); render(); settingsDlg.showModal(); });
  document.querySelector('.appbar__spacer').after(settingsBtn);
  function scanRows() {
    var base = /^base(\d+)/.exec(scanMode.select.value)[1];
    return ROWS[base];
  }
  function renderScan() {
    orderValue.textContent = order ? (order.batch ? 'Batch ' : '') + order.roll + ' · ' + filmLabel(order.film) + (order.batch ? ' · ' + (order.strips || []).length + ' roll(s), next ' + stripName() : '') : 'None yet';
    orderFolder.textContent = order ? order.folder : 'Folders go into ' + (orderBase() || 'the base folder (Settings)') + '.';
    capNote.textContent = scanGate.select.value === 'off' ? 'Runs to ' + scanRows().toLocaleString('en') + ' rows: no film end.'
      : 'Stops when the film ends (' + scanRows().toLocaleString('en') + ' rows at most).';
    scanBtn.textContent = order && order.batch ? 'Scan roll ' + stripName() : 'Scan';
    finishBatchBtn.hidden = !(order && order.batch);
    var idle = !state.busy && !state.checking;
    [scanMode.select, scanIce.input, scanGate.select, newOrderBtn, finishBatchBtn].forEach(function (c) { c.disabled = !idle; });
    scanButtons.forEach(function (b) { b.disabled = !idle; });
    // Scan stays clickable when idle: a click names what is missing (and
    // asks for the folder when none is chosen) rather than a greyed-out
    // button nobody can explain.
    scanBtn.disabled = !idle;
    scanNote.textContent = state.scanning ? 'Scanning. Stop ends the pass through the teardown: lamp off, motor stopped.'
      : state.checking ? (/^Initializing/.test(state.scanner) ? 'Initializing the scanner: it boots when cold, then identifies.' : 'Asking the scanner (read-only).')
      : !order ? 'New order: enter the roll number first (Scan asks for it too).'
      : 'Scan checks the scanner first (VM off, warm, identify, status, EEPROM), then runs the film pass.';
  }

  // Start-up (src-tauri/src/boot.rs): find the scanner, boot it with the
  // frozen reference script when it is cold, identify it. Runs when the
  // app opens and on Initialize; state.checking holds the other commands.
  var INIT_RESULT = {
    ready: ['Ready', 'ok'], vm: ['XP VM running', 'danger'], not_found: ['Not found', 'neutral'],
    boot_failed: ['Boot failed', 'danger'], not_f135: ['Not an F-135+', 'danger'], error: ['No session', 'danger']
  };
  function initScanner() {
    if (state.busy || state.checking) return;
    state.checking = true;
    state.scanner = 'Initializing';
    state.tone = 'warn';
    write('> initialize scanner: find, boot if cold, identify', 'cmd');
    render();
    api.initScanner().then(function (r) {
      state.checking = false;
      if (r.log) write(r.log);
      var shown = INIT_RESULT[r.state] || ['No session', 'danger'];
      state.scanner = shown[0];
      state.tone = shown[1];
      if (r.model) state.model = r.model;
      if (r.version) state.version = r.version.replace(/\s{2,}/g, ' · ');
      write((r.booted && r.state === 'ready' ? 'Firmware loaded. ' : '') + r.detail, r.state === 'ready' ? 'ok' : 'err');
      render();
    }).catch(function (e) {
      state.checking = false;
      write(String(e), 'err');
      state.scanner = 'Not initialized';
      state.tone = 'danger';
      render();
    });
  }
  api.onInitStage(function (s) {
    if (!state.checking) return;
    state.scanner = 'Initializing — ' + s.text.toLowerCase();
    write(s.text + '…');
    render();
  });

  // Hot-plug: `pakon-cli list` (read-only enumeration) every few seconds
  // while idle. A scanner that appears (powered on, plugged in) is
  // initialized; one that disappears shows Not found. Only edges count, so
  // a failed boot is not retried until the scanner is power-cycled.
  var scannerSeen = null; // unknown until the first poll
  function pollScanner() {
    if (state.busy || state.checking) return;
    api.runCli('list').then(function (r) {
      if (state.busy || state.checking) return;
      var seen = /Pakon F-X35 detected/.test(r.stdout);
      var was = scannerSeen;
      scannerSeen = seen;
      if (was === false && seen) {
        write('Scanner connected.', 'cmd');
        initScanner();
      } else if (was && !seen) {
        write('Scanner disconnected or switched off.', 'err');
        state.scanner = 'Not found';
        state.tone = 'neutral';
        render();
      }
    }).catch(function () {});
  }
  if (tauri) setInterval(pollScanner, 3000);

  function startScan() {
    if (state.busy || state.checking) return;
    if (!order) { askOrder(true); return; }
    if (zoomAt) exitZoom();
    var last = order.strips && order.strips.length ? order.strips[order.strips.length - 1] : null;
    var s = { mode: scanModeValue(), rows: scanRows(), folder: order.folder, name: stripName(), bw: order.bw, film: order.film,
              gate: scanGate.select.value, hot: last && last.mode === scanModeValue() ? last.name : null };
    if (s.film === 'bw-silver' && /-ir$/.test(s.mode)) {
      write('Silver black-and-white film blocks infrared, so dust removal cannot work on it: switch Dust removal (ICE) off.', 'err');
      return;
    }
    flushLook().then(function () {
      state.scanning = true;
      openLog(true); // the scan's own lines are the progress to watch
      setBusy('Scanning', true);
      cancelBtn.textContent = 'Stop scan';
      bar.set(0, 'Checking the scanner', '');
      state.scanner = 'Scanning';
      state.tone = 'warn';
      write('> pakon-cli scan --live-scan --config ' + s.mode + ' --film-rows ' + s.rows + ' --out-prefix ' +
            s.folder + sep(s.folder) + s.name + ' --stop-on-stdin --film ' + s.film + ' --film-gate ' + s.gate +
            (s.hot ? ' --hot-start ' + s.folder + sep(s.folder) + s.hot + '.scan.json' : ''), 'cmd');
      render();
      return api.startScan(s);
    }).then(function (done) {
      scanEnded();
      var lampSafe = done.teardown_ok;
      if (!lampSafe) {
        state.scanner = 'Teardown failed';
        state.tone = 'danger';
        busySlot.replaceChildren(P.StatusChip({ label: 'Teardown failed', tone: 'danger' }));
        write('The teardown did not finish. Look at the scanner: if the lamp is on or the motor runs, switch it off. ' +
              'Keep the log and the .scan-stats.txt.', 'err');
      } else {
        state.scanner = done.stopped ? 'Stopped' : done.code === 0 ? 'Scanned' : 'Scan failed';
        state.tone = done.code === 0 ? 'ok' : done.stopped ? 'neutral' : 'danger';
        write(done.stopped ? 'Scan stopped; lamp off, motor stopped.' : done.code === 0 ? 'Scan done: ' + done.prefix
              : 'Scan failed (exit ' + done.code + '); lamp off, motor stopped.', done.code === 0 ? 'ok' : 'err');
      }
      // The order is done with its scan; the next roll is a new order.
      // Batch: the order stays and moves on to the next number (hot start
      // from this roll); a single roll is done with its scan.
      if (done.roll && done.code === 0 && order && order.batch) {
        order.strips = (order.strips || []).concat([{ name: s.name, mode: s.mode }]);
        order.n += 1;
        keepBatch();
        write('Roll ' + s.name + ' done: feed the next roll and press Scan roll ' + stripName() + ', or Finish batch.', 'ok');
      } else if (done.roll) order = null;
      render();
      if (done.roll) {
        state.folder = s.folder;
        try { localStorage.setItem('pakonx.folder', state.folder); } catch (e) {}
        loadRolls();
        // The roll's film goes with it before it opens (develops in B&W).
        // The roll's film and its dust removal (scanned with ICE: on) go with
        // it before it opens.
        if (done.code === 0) api.saveFilm(done.prefix, !!s.bw)
          .then(function () { return /-ir$/.test(s.mode) ? api.saveIce(done.prefix, 'normal') : null; })
          .then(function () { openRoll(done.prefix); }, function (e) { write(String(e), 'err'); openRoll(done.prefix); });
      }
    }).catch(function (e) {
      scanEnded();
      if (order && order.batch && /already scanned/.test(String(e))) {
        order.n += 1; keepBatch();
        write('Roll ' + s.name + ' is already in this batch folder; the batch moves on to ' + stripName() + '.', 'cmd');
      }
      state.scanner = 'Not scanned';
      state.tone = 'danger';
      failed(e, 'Not scanned');
      render();
    });
  }
  function scanEnded() {
    state.scanning = false;
    cancelBtn.textContent = 'Cancel';
    setBusy(null);
  }
  api.onScanProgress(function (p) {
    if (!state.scanning) return;
    bar.set(100 * p.rows / p.cap, 'Scanning — film rows ' + p.rows.toLocaleString('en') + ' of at most ' + p.cap.toLocaleString('en'));
  });
  api.onScanChecked(function () {
    if (state.scanning) bar.set(0, 'Scanning — lamp warm-up and calibration', '');
  });
  api.onScanLine(function (l) { write(l.text, l.err ? 'err' : null); });

  // OEM look files ---------------------------------------------------------------------------
  // The OEM's own PakonIMAu.dll develops the roll (tools/oem_host, Windows):
  // it needs a copy of the OEM install and TLB's captured descriptors; an
  // EEPROM dump only for scans older than the sidecar's unit.neg_matrix.
  // The app carries the install, the descriptors and oem_host.exe
  // (src-tauri/src/look.rs): '' means the bundled one. Other folders can be
  // chosen in Settings, Development.
  var oem = { oem: '', capture: '', eeprom: '' };
  // Bundled by default: only folders chosen in this version count
  // ('pakonx.oemFiles'); the older 'pakonx.oem' choices are dropped.
  try {
    localStorage.removeItem('pakonx.oem');
    Object.assign(oem, JSON.parse(localStorage.getItem('pakonx.oemFiles') || '{}'));
  } catch (e) {}
  var bundledOem = { oem: null, capture: null, host: false };
  api.bundledOem().then(function (b) { bundledOem = b; render(); }).catch(function () {});
  // The OEM look is a 32-bit Windows DLL: macOS and Linux develop with PakonX only.
  api.platform().then(function (os) {
    if (os === 'windows') return;
    var opt = devSelect.querySelector('option[value="oem"]');
    if (opt) opt.remove();
    settingsOem.parentNode.hidden = true;
    render();
  }).catch(function () {});
  var oemChip = el('div');
  var oemFacts = el('dl', 'facts body');
  var oemActions = el('div', 'actions');
  oemActions.style.marginTop = 'var(--space-2)';
  oemActions.append(
    P.Button({ label: 'OEM folder', onClick: function () { chooseOem('oem', true); } }),
    P.Button({ label: 'Capture folder', onClick: function () { chooseOem('capture', true); } }),
    P.Button({ label: 'EEPROM dump', onClick: function () { chooseOem('eeprom', false); } }),
    P.Button({ label: 'Use bundled', onClick: function () { useBundledOem(); } }),
    P.Button({ label: 'How it works', onClick: function () { oemHelp.showModal(); } }));
  settingsOem.append(oemChip, oemFacts, oemActions);

  // How it works: what the OEM look uses and how to swap in other files.
  var oemHelp = el('dialog', 'helpdlg pk-root');
  oemHelp.setAttribute('aria-labelledby', 'oem-help-title');
  var helpTitle = el('h2', 'heading', 'OEM look: how it works');
  helpTitle.id = 'oem-help-title';
  var helpBody = el('div', 'body helpdlg__body');
  [
    ['p', 'The OEM Pakon look develops your roll with Pakon\'s own image software (PakonIMAu.dll), the way the original scanner software did: ' +
          'Ansel balances the colours of the whole roll, then each picture is scaled, colour-adjusted and saved as JPEG.'],
    ['h3', 'What it uses'],
    ['ul', [
      'OEM folder: a copy of the Pakon install (PakonIMAu.dll, its DLLs, Config, anselinstalldir).',
      'Capture folder: settings recorded from the original software (settings.bin and descriptor files). They tell the DLL how to run.',
      'EEPROM dump: only for scans made before 2026-10-20. Newer scans carry the scanner\'s colour data themselves.',
      'oem_host.exe: a small 32-bit helper that runs the DLL. It always comes with PakonX.'
    ]],
    ['h3', 'Bundled files'],
    ['p', 'PakonX comes with all of these. "Bundled" means it uses its own copy, so nothing needs setting up. ' +
          'On first use it copies the OEM install into its own data folder, because the DLL writes logs and a cache next to itself.'],
    ['h3', 'Using different files'],
    ['ul', [
      'OEM folder / Capture folder: pick another folder. It is used only if it holds PakonIMAu.dll / settings.bin; otherwise the bundled one is used.',
      'The capture must come from the same kind of recording as the bundled one (file names like baldesc_42.bin, coloradjust44_a0.bin), or the look stops with an error in the log.',
      'EEPROM dump: a file saved with "pakon-cli eeprom --out eeprom.bin" on the scanner the roll was scanned with.',
      'Use bundled: forget your choices and go back to the bundled files.'
    ]],
    ['p', 'Rolls open with the OEM look unless Settings, Development says otherwise. In this mode the light table shows and saves the OEM\'s JPEGs, with their own look sliders ' +
          '(kept apart from the PakonX look), turns, Roll 180°, framing and names; TIFF is PakonX only.']
  ].forEach(function (b) {
    if (b[0] === 'ul') {
      var ul = el('ul');
      b[1].forEach(function (s) { ul.append(el('li', null, s)); });
      helpBody.append(ul);
    } else helpBody.append(el(b[0], b[0] === 'h3' ? 'label' : null, b[1]));
  });
  var helpClose = P.Button({ label: 'Close', onClick: function () { oemHelp.close(); } });
  var helpActions = el('div', 'actions helpdlg__actions');
  helpActions.append(helpClose);
  oemHelp.append(helpTitle, helpBody, helpActions);
  oemHelp.addEventListener('click', function (e) { if (e.target === oemHelp) oemHelp.close(); });
  document.body.append(oemHelp);
  function oemReady() { return !!((oem.oem || bundledOem.oem) && (oem.capture || bundledOem.capture) && (bundledOem.host || !tauri)); }
  function useBundledOem() {
    oem = { oem: '', capture: '', eeprom: '' };
    try { localStorage.removeItem('pakonx.oemFiles'); } catch (e) {}
    state.oemLook = null;
    render();
  }
  function oemLabel(own, bundled) { return own ? shortPath(own) + (bundled ? ' (bundled if this lacks the files)' : '') : bundled ? 'Bundled' : 'Not set'; }
  function chooseOem(key, folder) {
    (folder ? api.pickFolder(oem[key] || state.folder) : api.pickFile(oem[key] || state.folder)).then(function (p) {
      if (!p) return;
      oem[key] = p;
      try { localStorage.setItem('pakonx.oemFiles', JSON.stringify(oem)); } catch (e) {}
      state.oemLook = null;
      render();
    });
  }
  function shortPath(p) { return p ? p.slice(dirname(dirname(p)).length + 1) : 'Not set'; }

  // Centre: light table -------------------------------------------------------------------
  var table = P.Window({ title: 'Light table', controls: false, mount: document.getElementById('view-table') });
  var head = el('div', 'tablehead');
  var headText = el('div', 'tablehead__text');
  var frameTitle = el('div', 'display', '—');
  var frameSub = el('div', 'caption tablehead__sub', 'No roll open');
  headText.append(frameTitle, frameSub);
  var busySlot = el('span');
  var prevBtn = P.Button({ label: 'Previous', disabled: true, onClick: function () { step(-1); } });
  var nextBtn = P.Button({ label: 'Next', disabled: true, onClick: function () { step(1); } });
  var strip = null;
  var turnLeftBtn = P.Button({ label: '⟲ 90°', disabled: true, onClick: function () { rotate(-90); } });
  var turnRightBtn = P.Button({ label: '⟳ 90°', disabled: true, onClick: function () { rotate(90); } });
  turnLeftBtn.title = 'Turn the picture (or the marked pictures) left';
  turnRightBtn.title = 'Turn the picture (or the marked pictures) right';
  turnLeftBtn.setAttribute('aria-label', turnLeftBtn.title);
  turnRightBtn.setAttribute('aria-label', turnRightBtn.title);
  // Roll 180°: a tail-first picture is exactly the head-first one turned
  // half round (process/positive.hpp picture_row / picture_col), so the
  // screen turns it at once and nothing is developed again; saving passes
  // --tail-first. Kept per roll.
  var rotateBtn = P.Button({ label: 'Roll 180°', disabled: true, onClick: function () {
    if (state.busy || !state.roll) return;
    state.tailFirst = !state.tailFirst;
    try { localStorage.setItem('pakonx.tail.' + state.roll.prefix, state.tailFirst ? '1' : ''); } catch (e) {}
    applyLooks();
    showShift();
    showFrame();
  } });
  function storedTailFirst(prefix) {
    try { return localStorage.getItem('pakonx.tail.' + prefix) === '1'; } catch (e) { return false; }
  }
  var zoomSlot = el('span');
  rotateBtn.title = 'The film went in tail first: turn the whole roll';
  var clearBtn = P.Button({ label: 'Clear', onClick: function () { clearAll(); } });
  clearBtn.title = 'Close the roll and start over: the light table empties and the scanner is initialized again';
  head.append(headText, zoomSlot, prevBtn, nextBtn, turnLeftBtn, turnRightBtn);
  var loupe = el('div', 'loupe');
  var matte = el('div', 'loupe__matte');
  var empty = el('div', 'loupe__empty');
  empty.append(el('div', 'heading', 'Nothing open'), el('p', 'body', 'Open a scan (.film.pakraw) or pictures (JPEG, TIFF, BMP) on the left.'));
  var picture = el('img', 'loupe__img');
  picture.alt = '';
  picture.hidden = true;
  matte.append(empty, picture);
  // 100% view: the full-size picture, one image pixel per screen pixel,
  // scrolled and dragged inside the loupe.
  var zoom = el('div', 'loupe__zoom');
  zoom.hidden = true;
  zoom.tabIndex = 0;
  zoom.setAttribute('aria-label', 'Picture at 100 %; drag to move, click or Esc to fit');
  var full = el('img', 'loupe__full');
  full.alt = '';
  full.draggable = false;
  zoom.append(full);
  loupe.append(matte, zoom);
  picture.title = 'Click to see it at 100 %';
  var stripSlot = el('div', 'stripslot');
  // Progress of a pakon-cli run: real picture counts only, never a guess.
  var progressRow = el('div', 'progressrow');
  var bar = P.ScanProgress({ label: 'Progress' });
  var progressLabel = '';
  var cancelBtn = P.Button({ label: 'Cancel', variant: 'danger', onClick: function () {
    cancelBtn.disabled = true;
    if (state.scanning) {
      write('Stopping the scan: teardown next.', 'cmd');
      api.stopScan();
    } else api.cancel();
  } });
  bar.root.classList.add('progressrow__bar');
  progressRow.append(bar.root, cancelBtn);
  progressRow.hidden = true;
  api.onProgress(function (p) {
    if (!state.busy || !p.total) return;
    bar.set(100 * p.done / p.total, progressLabel + ' — picture ' + p.done + ' / ' + p.total);
  });
  table.body.append(head, loupe, stripSlot);
  document.getElementById('busyrow').append(busySlot, progressRow);

  // Roll: the contact sheet, strips of 6 like a sleeve (FilmStrip perRow) -----------
  var rollWin = P.Window({ title: 'Roll', controls: false, mount: document.getElementById('view-roll') });
  var sheetHead = el('div', 'tablehead');
  var sheetText = el('div', 'tablehead__text');
  var sheetTitle = el('div', 'display', '—');
  var sheetSub = el('div', 'caption tablehead__sub', 'No roll open');
  sheetText.append(sheetTitle, sheetSub);
  var sizeRow = el('label', 'sheetsize caption');
  var sizeInput = el('input', 'lookrow__range');
  sizeInput.type = 'range';
  sizeInput.min = 96; sizeInput.max = 288; sizeInput.step = 8; sizeInput.value = 160;
  try { var savedSize = Number(localStorage.getItem('pakonx.sheetSize')); if (savedSize >= 96 && savedSize <= 288) sizeInput.value = savedSize; } catch (e) {}
  sizeInput.setAttribute('aria-label', 'Picture size');
  sizeRow.append(el('span', null, 'Size'), sizeInput);
  function sizeSheet() { if (sheet) sheet.root.style.setProperty('--pk-frame-w', sizeInput.value + 'px'); }
  sizeInput.addEventListener('input', function () {
    sizeSheet();
    try { localStorage.setItem('pakonx.sheetSize', sizeInput.value); } catch (e) {}
  });
  var selectAllBtn = P.Button({ label: 'Select all', disabled: true, onClick: function () { markAll(); } });
  sheetHead.append(sheetText, sizeRow, selectAllBtn, rotateBtn, clearBtn);
  var sheetSlot = el('div', 'sheetslot');
  var sheetEmpty = el('div', 'loupe__empty');
  sheetEmpty.append(el('div', 'heading', 'Nothing open'), el('p', 'body', 'Pick a roll on the left, or Open a scan (.film.pakraw) or pictures.'));
  sheetSlot.append(sheetEmpty);
  rollWin.body.append(sheetHead, sheetSlot);
  var sheet = null;
  function markAll() {
    if (strip && !state.loose && state.pictures.length) strip.mark(state.pictures.map(function (_, i) { return i; }));
  }

  // Updates -------------------------------------------------------------------------
  // A newer version shows as a button in the app bar; installing closes
  // PakonX, runs the installer and starts the new version. Never during a scan.
  var available = null;
  var updateChip = el('button', 'pk-btn pk-btn--go appbar__update');
  updateChip.type = 'button';
  updateChip.hidden = true;
  updateChip.addEventListener('click', function () { installUpdate(); });
  document.querySelector('.appbar__spacer').after(updateChip);
  function checkUpdate(asked) {
    if (asked) { updateBtn.disabled = true; updateNote.textContent = 'Checking…'; }
    return api.checkUpdate().then(function (u) {
      updateBtn.disabled = false;
      available = u;
      updateChip.hidden = !u;
      if (u) {
        updateChip.textContent = 'Update to ' + u.version;
        updateNote.textContent = 'PakonX ' + u.version + ' is available.' + (u.notes ? ' ' + u.notes : '');
        write('PakonX ' + u.version + ' is available: Update to ' + u.version + ' (app bar) installs it.', 'ok');
      } else if (asked) {
        updateNote.textContent = 'PakonX ' + (appVersion || '') + ' is the newest version.';
      }
    }).catch(function (e) {
      updateBtn.disabled = false;
      if (asked) updateNote.textContent = String(e);
    });
  }
  function installUpdate() {
    if (!available) return;
    if (state.scanning || state.busy || state.checking) {
      write('Finish the scan or save first; then Update to ' + available.version + '.', 'err');
      return;
    }
    flushLook().then(function () {
      return api.ask('Install PakonX ' + available.version + ' now? PakonX closes, installs it and starts again. Your scans and pictures stay.',
        'Update PakonX');
    }).then(function (yes) {
      if (!yes) return;
      setBusy('Updating to ' + available.version, true);
      cancelBtn.disabled = true;
      write('Downloading PakonX ' + available.version + '…', 'cmd');
      return api.installUpdate();
    }).catch(function (e) {
      setBusy(null);
      failed(e, 'Not updated');
    });
  }
  api.onUpdateProgress(function (p) {
    if (!p.total) return;
    bar.set(100 * p.done / p.total, 'Downloading PakonX ' + (available ? available.version : ''), sizeLabel(p.done) + ' / ' + sizeLabel(p.total));
  });

  // Right: frame + log ------------------------------------------------------------------------
  var frameWin = P.Window({ title: 'Picture', controls: false, mount: document.getElementById('insp-table') });
  var rollInsp = P.Window({ title: 'Roll settings', controls: false, mount: document.getElementById('insp-roll') });
  var frameFacts = el('dl', 'facts body');
  // Save: the stage-1 positive of this picture or the whole roll.
  // Look: the user's exposure, colour balance and contrast per picture.
  // pakon-cli applies them when saving (--adjust, process/positive.hpp
  // Adjust); on screen the same curves run as an SVG lookup table over the
  // 8-bit previews, exact because each is a function of the output value.
  var LOOK_RANGE = 2.2; // PositiveOptions::range_decades
  var LOOK_FIELDS = [
    { key: 'exposure', label: 'Exposure', min: -2, max: 2, step: 0.1, neutral: 0, unit: ' EV' },
    { key: 'red', label: 'Red', min: -1, max: 1, step: 0.05, neutral: 0, unit: '' },
    { key: 'green', label: 'Green', min: -1, max: 1, step: 0.05, neutral: 0, unit: '' },
    { key: 'blue', label: 'Blue', min: -1, max: 1, step: 0.05, neutral: 0, unit: '' },
    { key: 'contrast', label: 'Contrast', min: 0.6, max: 1.6, step: 0.05, neutral: 1, unit: '' }
  ];
  var lookGroup = el('fieldset', 'group lookgroup');
  lookGroup.append(el('legend', 'label', 'Look'));
  var devRow = el('label', 'row caption');
  var devSelect = el('select', 'field');
  [['oem', 'OEM Pakon look'], ['pakonx', 'PakonX (adjustable)']].forEach(function (o) {
    var opt = el('option', null, o[1]);
    opt.value = o[0];
    devSelect.append(opt);
  });
  devSelect.addEventListener('change', function () { setDev(devSelect.value); });
  devRow.append(el('span', null, 'Develop with'), devSelect);
  var filmRow = el('label', 'row caption');
  var filmSelect = el('select', 'field');
  [['colour', 'Colour negative'], ['bw', 'Black & white']].forEach(function (o) {
    var opt = el('option', null, o[1]);
    opt.value = o[0];
    filmSelect.append(opt);
  });
  filmSelect.addEventListener('change', function () { setFilm(filmSelect.value === 'bw'); });
  filmRow.append(el('span', null, 'Film'), filmSelect);
  // Dust removal from the scan's infrared plane (frames --ice), -ir scans only.
  var rollIce = iceSwitch('Dust removal (ICE)');
  var iceRow = rollIce.row;
  // On = the Normal strength (frames --ice normal); a roll saved earlier
  // with Light or Strong still counts as on.
  function iceOn(r) { return !!r && !!r.ice && r.ice !== 'off'; }
  rollIce.input.addEventListener('change', function () { setIce(rollIce.input.checked ? 'normal' : 'off'); });
  // Development: what applies to the whole roll (Roll tab).
  var devGroup = el('fieldset', 'group');
  devGroup.append(el('legend', 'label', 'Development'), filmRow, iceRow, devRow);
  var lookInputs = {};
  LOOK_FIELDS.forEach(function (d) {
    // A number field with − / + steps (not a label: it would forward clicks to −).
    var row = el('div', 'row lookrow lookrow--num caption');
    var input = el('input', 'field data lookrow__num');
    input.type = 'number';
    input.min = d.min; input.max = d.max; input.step = d.step; input.value = lookNum(d, d.neutral);
    input.setAttribute('aria-label', d.label);
    function commit(v) {
      if (!isFinite(v)) v = d.neutral;
      v = Math.min(d.max, Math.max(d.min, Math.round(v / d.step) * d.step));
      input.value = lookNum(d, v);
      setLook(d.key, Number(input.value));
    }
    input.addEventListener('change', function () { commit(Number(input.value)); });
    var minus = P.Button({ label: '−', onClick: function () { commit(Number(input.value) - d.step); } });
    var plus = P.Button({ label: '+', onClick: function () { commit(Number(input.value) + d.step); } });
    minus.classList.add('lookrow__step');
    plus.classList.add('lookrow__step');
    minus.setAttribute('aria-label', d.label + ' down');
    plus.setAttribute('aria-label', d.label + ' up');
    var value = el('span', 'caption lookrow__value', d.key === 'contrast' ? '×' : d.unit.trim());
    row.append(el('span', null, d.label), minus, input, plus, value);
    lookInputs[d.key] = { input: input, minus: minus, plus: plus, def: d };
    lookGroup.append(row);
  });
  var lookActions = el('div', 'actions');
  var lookReset = P.Button({ label: 'Reset', onClick: function () { resetLook(); } });
  var lookAll = P.Button({ label: 'Apply to all', onClick: function () { lookToAll(); } });
  lookActions.append(lookReset, lookAll);
  lookGroup.append(lookActions, el('p', 'note caption savegroup__note', 'Type a value (Enter) or step with − / +. Saved pictures carry the look.'));
  // Film presets (PakonX look): the selected picture's look saved for the
  // roll's DX code (roll.rs load_presets / save_presets), put on every
  // picture of a roll of that film when it opens without a look of its own.
  var presets = {};
  api.loadPresets().then(function (p) { presets = p || {}; render(); }).catch(function (e) { write('Film presets not loaded: ' + e, 'err'); });
  var presetActions = el('div', 'actions');
  var presetSave = P.Button({ label: 'Save film preset', onClick: function () { savePreset(); } });
  var presetApply = P.Button({ label: 'Apply film preset', onClick: function () { applyPreset(false); } });
  presetActions.append(presetSave, presetApply);
  var presetNote = el('p', 'note caption savegroup__note');
  lookGroup.append(presetActions, presetNote);
  var looksSvg = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
  looksSvg.setAttribute('aria-hidden', 'true');
  looksSvg.setAttribute('class', 'looks');
  document.body.append(looksSvg);

  var saveGroup = el('fieldset', 'group savegroup');
  saveGroup.append(el('legend', 'label', 'Save'));
  var formatRow = el('label', 'row caption');
  var formatSelect = el('select', 'field');
  [['jpeg', 'JPEG'], ['tiff', 'TIFF (16-bit PakonX, 8-bit OEM)']].forEach(function (o) {
    var opt = el('option', null, o[1]);
    opt.value = o[0];
    formatSelect.append(opt);
  });
  try { var savedFormat = localStorage.getItem('pakonx.saveFormat'); if (savedFormat) formatSelect.value = savedFormat; } catch (e) {}
  formatSelect.addEventListener('change', function () {
    try { localStorage.setItem('pakonx.saveFormat', formatSelect.value); } catch (e) {}
  });
  formatRow.append(el('span', null, 'Format'), formatSelect);
  function choice(name, key, options, fallback) {
    var row = el('label', 'row caption');
    var select = el('select', 'field');
    options.forEach(function (o) {
      var opt = el('option', null, o[1]);
      opt.value = o[0];
      select.append(opt);
    });
    select.value = fallback;
    try { var v = localStorage.getItem(key); if (v && options.some(function (o) { return o[0] === v; })) select.value = v; } catch (e) {}
    select.addEventListener('change', function () {
      try { localStorage.setItem(key, select.value); } catch (e) {}
      updateButtons();
    });
    row.append(el('span', null, name), select);
    return { row: row, select: select };
  }
  var quality = choice('JPEG quality', 'pakonx.saveQuality',
    [['100', 'Maximum (100)'], ['95', 'High (95)'], ['92', 'Standard (92)'], ['85', 'Smaller (85)']], '92');
  var naming = choice('Names', 'pakonx.saveNames',
    [['dx', 'Roll + film number (1111_19A)'], ['picture', 'Picture numbers (roll.frame01)'], ['custom', 'Your name + number']], 'dx');
  // Your name + number: BASE + the picture's position on the roll from
  // Start, zero-padded (Trip_001, Trip_002, …), the same for any picture
  // whether the whole roll or a few are saved.
  var nameBase = el('input', 'field');
  nameBase.type = 'text';
  nameBase.spellcheck = false;
  nameBase.maxLength = 80;
  var nameBaseRow = el('label', 'row caption');
  nameBaseRow.append(el('span', null, 'Name'), nameBase);
  var nameStart = el('input', 'field');
  nameStart.type = 'number';
  nameStart.min = 0; nameStart.max = 999999; nameStart.value = 1;
  var nameStartRow = el('label', 'row caption');
  nameStartRow.append(el('span', null, 'First number'), nameStart);
  var nameDigits = choice('Digits', 'pakonx.saveDigits', [['1', '1'], ['2', '01'], ['3', '001'], ['4', '0001']], '3');
  var nameExample = el('p', 'note caption savegroup__note');
  function customNaming() { return naming.select.value === 'custom'; }
  function nameOpts() {
    var start = Number(nameStart.value);
    return { base: nameBase.value, start: start, digits: Number(nameDigits.select.value) };
  }
  function nameError() {
    var o = nameOpts();
    if (!o.base || o.base.length > 80 || o.base[0] === '-' || /[<>:"\/\\|?*]|[^ -~]/.test(o.base)) return 'Name: letters, digits and punctuation, without < > : " / \\ | ? *';
    if (!(Number.isInteger(o.start) && o.start >= 0 && o.start <= 999999)) return 'First number: 0 to 999999';
    return null;
  }
  function renderNaming() {
    var on = customNaming();
    [nameBaseRow, nameStartRow, nameDigits.row, nameExample].forEach(function (n) { n.hidden = !on; });
    if (!on) return;
    var o = nameOpts(), err = nameError();
    var pad = function (n) { return String(n).padStart(o.digits, '0'); };
    nameExample.textContent = err || (o.base + pad(o.start) + ', ' + o.base + pad(o.start + 1) + ', … by position on the roll');
    nameExample.classList.toggle('is-error', !!err);
  }
  [nameBase, nameStart].forEach(function (i) { i.addEventListener('input', function () { renderNaming(); updateButtons(); }); });
  [naming.select, nameDigits.select].forEach(function (s) { s.addEventListener('change', renderNaming); });
  formatSelect.addEventListener('change', function () { updateButtons(); render(); });
  var saveActions = el('div', 'actions');
  var savePictureBtn = P.Button({ label: 'Save picture', disabled: true, onClick: function () { save(true); } });
  var saveRollBtn = P.Button({ label: 'Save roll', variant: 'go', disabled: true, onClick: function () { save(false); } });
  saveActions.append(saveRollBtn);
  var pictureSave = el('div', 'actions picturesave');
  pictureSave.append(savePictureBtn);
  var saveNote = el('p', 'note caption savegroup__note', 'Open a roll to save its pictures.');
  var markHint = el('p', 'note caption savegroup__note', 'Ctrl-click pictures to pick several, Shift-click picks a run, Ctrl+A picks all; a plain click or Esc clears.');
  saveGroup.append(formatRow, quality.row, naming.row, nameBaseRow, nameStartRow, nameDigits.row, nameExample, saveActions, markHint, saveNote);
  // Negative Lab Pro: shown only when Lightroom Classic is installed
  // (src-tauri/src/nlp.rs). The picked pictures, else the whole roll.
  var nlpBtn = P.Button({ label: 'Open in Negative Lab Pro', onClick: function () {
    openInNlp(state.marked.map(function (i) { return state.pictures[i].picture; }));
  } });
  // Light table: this picture alone.
  var nlpPicBtn = P.Button({ label: 'Open in Negative Lab Pro', onClick: function () {
    var f = state.pictures[state.selected];
    if (f) openInNlp([f.picture]);
  } });
  nlpPicBtn.hidden = true;
  pictureSave.append(nlpPicBtn);
  var nlpRow = el('div', 'actions');
  nlpRow.append(nlpBtn);
  var nlpNote = el('p', 'note caption savegroup__note', 'Writes the flat negatives (16-bit TIFF) beside the scan and opens them in Lightroom Classic. In Negative Lab Pro, set the source to Scanner - Linear.');
  nlpRow.hidden = nlpNote.hidden = true;
  saveGroup.append(nlpRow, nlpNote);
  api.nlpStatus().then(function (s) {
    nlpRow.hidden = nlpNote.hidden = nlpPicBtn.hidden = !s.lightroom;
  }).catch(function () {});
  // Framing: the whole roll's frame windows moved along the film, for
  // pictures cut off at one edge. On screen it shows at once (the preview
  // slides); Apply redevelops the roll with `frames --shift`.
  var frameGroup = el('fieldset', 'group lookgroup');
  frameGroup.append(el('legend', 'label', 'Framing'));
  var shiftRow = el('label', 'row lookrow caption');
  var shiftInput = el('input', 'lookrow__range');
  shiftInput.type = 'range';
  shiftInput.step = 5;
  var shiftValue = el('span', 'data lookrow__value');
  shiftRow.append(el('span', null, 'Offset'), shiftInput, shiftValue);
  var shiftActions = el('div', 'actions');
  var shiftApply = P.Button({ label: 'Apply', variant: 'go', onClick: function () { applyShift(); } });
  var shiftReset = P.Button({ label: 'Reset', onClick: function () { shiftInput.value = 0; previewShift(); } });
  shiftActions.append(shiftApply, shiftReset);
  // Frame format (frames --format): full 24x36, half 18x24, or auto.
  var frameFmtRow = el('label', 'row caption');
  var frameFmtSelect = el('select', 'field');
  [['auto', 'Auto (from the gaps)'], ['full', 'Full frame (24x36)'], ['half', 'Half frame (18x24)'], ['pano', 'Panoramic (gap to gap)']].forEach(function (o) {
    var opt = el('option', null, o[1]);
    opt.value = o[0];
    frameFmtSelect.append(opt);
  });
  frameFmtSelect.addEventListener('change', function () { setFrameFormat(frameFmtSelect.value); });
  frameFmtRow.append(el('span', null, 'Frame format'), frameFmtSelect);
  devGroup.insertBefore(frameFmtRow, devRow);
  // What the scan says the film is (frames.json film_detected, filmid.hpp).
  var detectedNote = el('p', 'note caption savegroup__note');
  var useBwBtn = P.Button({ label: 'Develop as B&W', onClick: function () { setFilm(true); } });
  useBwBtn.hidden = true;
  devGroup.append(detectedNote, useBwBtn);
  var FILM_NAMES = { 'c41': 'colour negative (C-41)', 'c41-unmasked': 'colour negative without orange mask',
                     'e6': 'colour slide (E-6)', 'bw-silver': 'black & white (silver)', 'bw-c41': 'black & white (C-41 process)' };
  function showDetected() {
    var fr = state.roll && !state.loose && state.roll.frames;
    var kind = fr && fr.film_detected;
    var bwFilm = /^bw/.test(kind || '');
    detectedNote.textContent = !fr ? '' : (kind ? 'Detected: ' + FILM_NAMES[kind] : 'Film not detected') +
      (fr.iso ? ' · ISO ' + fr.iso : '') + '.' +
      (/clipped/.test(fr.film_why || '') ? ' Its clear base was scanned with the colour-negative exposure and clips: scan it again with the film kind set to ' + (bwFilm ? 'Black & white (silver)' : 'Colour negative, no orange mask') + '.' : '') +
      (kind === 'e6' ? ' A slide develops right only when it was scanned as E-6 slide.' : '');
    detectedNote.title = fr && fr.film_why ? fr.film_why : '';
    useBwBtn.hidden = !(bwFilm && state.roll && !state.roll.bw);
  }
  frameGroup.append(shiftRow, shiftActions,
    el('p', 'note caption savegroup__note', 'Slide right to move the pictures right inside their frames, then Apply: the roll is developed again.'));
  shiftInput.addEventListener('input', previewShift);
  shiftInput.addEventListener('dblclick', function () { shiftInput.value = shiftOnScreen(); previewShift(); });
  frameWin.body.append(frameFacts, lookGroup, pictureSave);
  rollInsp.body.append(devGroup, frameGroup, saveGroup);

  // Log: a drawer over the foot of the window, opened from the status bar;
  // the status bar always shows the last line.
  var logDrawer = document.getElementById('logdrawer');
  var log = P.Window({ title: 'Log', controls: false, mount: logDrawer });
  var consoleEl = el('pre', 'console data');
  consoleEl.setAttribute('aria-live', 'polite');
  log.body.append(consoleEl);
  var lastMsg = el('span', 'appstatus__msg');
  var logToggle = el('button', 'appstatus__log');
  logToggle.type = 'button';
  logToggle.addEventListener('click', function () { openLog(logDrawer.hidden); });
  function openLog(on) {
    logDrawer.hidden = !on;
    logToggle.textContent = on ? 'Log ▾' : 'Log ▴';
    logToggle.setAttribute('aria-expanded', String(on));
    try { localStorage.setItem('pakonx.log', on ? '1' : ''); } catch (e) {}
    if (on) consoleEl.scrollTop = consoleEl.scrollHeight;
  }

  function write(text, cls) {
    consoleEl.append(el('span', cls || null, text.endsWith('\n') ? text : text + '\n'));
    consoleEl.scrollTop = consoleEl.scrollHeight;
    var line = text.trim().split('\n').pop();
    lastMsg.textContent = line;
    lastMsg.title = line;
    lastMsg.className = 'appstatus__msg' + (cls === 'err' ? ' is-err' : cls === 'ok' ? ' is-ok' : '');
  }

  // Rolls -> light table -----------------------------------------------------------------------
  // Rolls open with the look chosen in Settings: the OEM look by default,
  // straight from the scan (one run, no PakonX previews first), else
  // PakonX's own positive. The other look is developed when chosen.
  function openWith() {
    var v = null;
    try { v = localStorage.getItem('pakonx.openWith'); } catch (e) {}
    return v === 'pakonx' ? 'pakonx' : 'oem';
  }
  function openRoll(prefix) {
    if (state.busy) return;
    if (desk.dataset.tab === 'scan') showTab('roll');
    if (zoomAt) exitZoom();
    flushLook();
    state.tailFirst = storedTailFirst(prefix);
    if (openWith() === 'oem' && oemReady() && tauri) openRollOem(prefix);
    else openRollPakonx(prefix);
  }
  // A roll on the light table: the pictures listed by whichever look ran.
  function showRoll(roll) {
    state.roll = roll;
    state.loose = null;
    state.saved = null;
    state.pictures = (roll.frames.frames || []).filter(function (f) { return f.kind === 'picture'; });
    state.selected = state.pictures.length ? 0 : -1;
    state.looks = {};
    state.oemLooks = {};
    renderRolls();
    renderTable();
    showShift();
    api.loadAdjust(roll.prefix).then(function (adj) {
      if (state.roll !== roll) return;
      state.looks = (adj && adj.pictures) || {};
      state.oemLooks = (adj && adj.oem) || {};
      roll.ice = (adj && adj.ice) || 'off';
      roll.format = (adj && adj.format) || 'auto';
      applyPreset(true);
      applyLooks();
      showLook();
      render();
    }).catch(function (e) { write(String(e), 'err'); });
  }
  function openRollOem(prefix) {
    setBusy('Developing (OEM look)', true);
    write('> pakon-cli frames ' + prefix + ' --pakon-look', 'cmd');
    Promise.all([api.rollInfo(prefix), api.openLook(prefix, oem)]).then(function (got) {
      setBusy(null);
      var info = got[0], look = got[1];
      if (look.log) write(look.log, look.cached ? 'ok' : null);
      look.prefix = prefix;
      state.oemLook = look;
      state.dev = 'oem';
      showRoll({ prefix: prefix, name: info.name, mode: info.mode, shift: info.shift, bw: info.bw, frames: look.frames, cache: null });
    }).catch(function (e) {
      setBusy(null);
      write('OEM look not made: ' + e, 'err');
      write('Opening with the PakonX look instead.');
      openRollPakonx(prefix);
    });
  }
  function openRollPakonx(prefix, then) {
    setBusy('Developing', true);
    write('> pakon-cli frames ' + prefix + ' --preview', 'cmd');
    api.openRoll(prefix, false).then(function (roll) {
      setBusy(null);
      if (roll.log) write(roll.log, roll.cached ? 'ok' : null);
      if (then) return then(roll);
      if (!state.roll || state.roll.prefix !== roll.prefix) state.oemLook = null;
      state.dev = 'pakonx';
      showRoll(roll);
    }).catch(function (e) {
      setBusy(null);
      failed(e, 'Not opened');
    });
  }


  function openPictures(paths) {
    if (zoomAt) exitZoom();
    setBusy('Opening');
    write('Opening ' + paths.length + ' picture' + (paths.length === 1 ? '' : 's'), 'cmd');
    api.openPictures(paths).then(function (set) {
      setBusy(null);
      set.skipped.forEach(function (s) { write('Not opened: ' + s, 'err'); });
      if (!set.pictures.length) return;
      state.loose = set;
      state.pictures = set.pictures;
      state.selected = 0;
      write(set.pictures.length + ' picture' + (set.pictures.length === 1 ? '' : 's') + ' open from ' + set.name, 'ok');
      renderRolls();
      renderTable();
    }).catch(function (e) {
      setBusy(null);
      failed(e, 'Not opened');
    });
  }

  function file(f, kind) {
    if (state.loose) return api.img(kind === 'thumb' ? f.thumb : f.preview);
    if (state.dev === 'oem' && state.oemLook) {
      var d = state.oemLook.dir;
      return api.img(d + sep(d) + 'look' + String(f.picture).padStart(2, '0') + '.jpg');
    }
    var r = state.roll;
    var n = String(f.picture).padStart(2, '0');
    return api.img(r.cache + sep(r.cache) + r.name + '.frame' + n + '.' + kind + '.bmp');
  }

  function label(f, i) { return state.loose ? String(i + 1) : frameLabel(f); }

  function renderTable() {
    if (!state.loose && !nameBase.value) nameBase.value = state.roll.name + '_';
    var name = state.loose ? state.loose.name : state.roll.name;
    table.root.querySelector('.pk-titlebar__text').textContent = 'Light table — ' + name;
    rollWin.root.querySelector('.pk-titlebar__text').textContent = 'Roll — ' + name;
    sheetTitle.textContent = name;
    sheetSub.textContent = state.pictures.length + (state.loose ? ' pictures' : ' frames') + (state.loose ? '' : ' · ' + modeLabel(state.roll.mode));
    var frames = function () { return state.pictures.map(function (f, i) { return { no: label(f, i), src: file(f, 'thumb') }; }); };
    var listName = (state.loose ? 'Pictures ' : 'Roll ') + name;
    state.marked = [];
    strip = P.FilmStrip({ frames: frames(), selected: state.selected, label: listName, onSelect: select,
                          multiple: !state.loose, onMark: markFrames });
    stripSlot.replaceChildren(strip.root);
    sheet = P.FilmStrip({ frames: frames(), selected: state.selected, label: listName + ', contact sheet', perRow: 6, onSelect: select,
                          multiple: !state.loose, onMark: markFrames, onOpen: function (i) { select(i); showTab('table'); } });
    sheetSlot.replaceChildren(sheet.root);
    sizeSheet();
    [strip, sheet].forEach(function (s) { s.root.querySelectorAll('.pk-frame__img').forEach(function (img) { img.decoding = 'async'; }); });
    applyLooks();
    showFrame();
  }

  // Look ----------------------------------------------------------------------------------------
  function lookNum(d, v) { return v.toFixed(d.step < 0.1 ? 2 : 1); }

  // 8-bit sRGB in -> 8-bit sRGB out for one channel, the C++ steps in x.
  function lookTable(look, c) {
    var floor = Math.pow(10, -LOOK_RANGE);
    var grey = 1 + Math.log10(0.18 * (1 - floor) + floor) / LOOK_RANGE;
    var k = look.contrast == null ? 1 : look.contrast;
    var shift = ((look.exposure || 0) + (look[['red', 'green', 'blue'][c]] || 0)) * Math.log10(2) / LOOK_RANGE;
    var out = [];
    for (var i = 0; i < 256; i++) {
      var v = i / 255;
      var y = v <= 0.04045 ? v / 12.92 : Math.pow((v + 0.055) / 1.055, 2.4);
      var x = 1 + Math.log10(y * (1 - floor) + floor) / LOOK_RANGE;
      x = Math.min(1, Math.max(0, grey + (x - grey) * k + shift));
      var y2 = Math.min(1, Math.max(0, (Math.pow(10, LOOK_RANGE * (x - 1)) - floor) / (1 - floor)));
      out.push((y2 <= 0.0031308 ? 12.92 * y2 : 1.055 * Math.pow(y2, 1 / 2.4) - 0.055).toFixed(4));
    }
    return out.join(' ');
  }

  // Each development has its own looks (the OEM's JPEGs and PakonX's
  // positive start from different colours): PakonX looks with the turns in
  // state.looks ("pictures", read by pakon-cli), OEM-look tones in
  // state.oemLooks ("oem", applied by PakonX to the OEM JPEGs on save).
  function toneMap() { return state.dev === 'oem' ? state.oemLooks : state.looks; }
  function toneOf(n) {
    var look = Object.assign({}, toneMap()[String(n)]);
    delete look.rotate;
    return look;
  }

  // CSS filter for a picture's look: url(#look-D-N), or '' when it has none.
  function lookFilter(n) {
    var look = !state.loose && toneOf(n);
    if (!look || !Object.keys(look).length) return '';
    var id = 'look-' + state.dev + '-' + n;
    var filter = looksSvg.querySelector('#' + id);
    if (!filter) {
      filter = document.createElementNS('http://www.w3.org/2000/svg', 'filter');
      filter.id = id;
      filter.setAttribute('color-interpolation-filters', 'sRGB');
      var ct = document.createElementNS('http://www.w3.org/2000/svg', 'feComponentTransfer');
      ['feFuncR', 'feFuncG', 'feFuncB'].forEach(function (name) {
        var fn = document.createElementNS('http://www.w3.org/2000/svg', name);
        fn.setAttribute('type', 'table');
        ct.append(fn);
      });
      filter.append(ct);
      looksSvg.append(filter);
    }
    // The tables only change with the tone: skip the 3 x 256 values when
    // this picture's look is what the filter already holds.
    var key = [look.exposure, look.red, look.green, look.blue, look.contrast].join('|');
    if (filter.getAttribute('data-key') !== key) {
      var fns = filter.firstChild.childNodes;
      for (var c = 0; c < 3; c++) fns[c].setAttribute('tableValues', lookTable(look, c));
      filter.setAttribute('data-key', key);
    }
    return 'url(#' + id + ')';
  }

  function applyLooks() {
    if (!strip) return;
    [strip, sheet].forEach(function (s) {
      var imgs = s ? s.root.querySelectorAll('.pk-frame__img') : [];
      state.pictures.forEach(function (f, i) { if (imgs[i]) { imgs[i].style.filter = lookFilter(f.picture); turnThumb(imgs[i], f.picture); } });
    });
    var f = state.pictures[state.selected];
    picture.style.filter = full.style.filter = f ? lookFilter(f.picture) : '';
    fitPicture();
  }

  // Rotation ------------------------------------------------------------------------------------
  // Clockwise degrees per picture, kept with its look ("rotate"); saving
  // turns the file, the screen turns the preview.
  function rotationOf(n) {
    var look = !state.loose && state.looks[String(n)];
    return (look && look.rotate) || 0;
  }
  // How a picture is turned on screen: its own turn, plus half a turn for
  // a roll fed tail first.
  function viewTurn(n) { return (rotationOf(n) + (state.tailFirst && !state.loose ? 180 : 0)) % 360; }
  function turnThumb(img, n) {
    var r = viewTurn(n);
    var apply = function () {
      var k = r % 180 && img.naturalWidth ? Math.min(img.naturalWidth, img.naturalHeight) / Math.max(img.naturalWidth, img.naturalHeight) : 1;
      img.style.transform = r ? 'rotate(' + r + 'deg) scale(' + k.toFixed(3) + ')' : '';
    };
    if (img.complete) apply(); else img.addEventListener('load', apply, { once: true });
  }
  // The loupe picture fitted to the matte as turned, slid by the framing
  // preview along the film (its own x axis, before the turn).
  function fitPicture() {
    var f = state.pictures[state.selected];
    if (!f || picture.hidden || !picture.naturalWidth) return;
    var r = viewTurn(f.picture);
    var cs = getComputedStyle(matte);
    var W = matte.clientWidth - parseFloat(cs.paddingLeft) - parseFloat(cs.paddingRight);
    var H = matte.clientHeight - parseFloat(cs.paddingTop) - parseFloat(cs.paddingBottom);
    var w = picture.naturalWidth, h = picture.naturalHeight;
    var side = r % 180 !== 0;
    var s = Math.min(W / (side ? h : w), H / (side ? w : h));
    picture.style.maxWidth = picture.style.maxHeight = 'none';
    picture.style.width = (w * s) + 'px';
    picture.style.height = (h * s) + 'px';
    var lines = !state.loose && f.end_line - f.first_line;
    // Along the film: the preview's own x axis (head-first), before the turn.
    var slide = lines ? (shiftDir() * Number(shiftInput.value) - (state.roll.shift || 0)) * (w * s) / lines : 0;
    picture.style.transform = (r ? 'rotate(' + r + 'deg) ' : '') + (slide ? 'translateX(' + slide.toFixed(1) + 'px)' : '');
  }
  picture.addEventListener('load', fitPicture);
  if (window.ResizeObserver) new ResizeObserver(function () { fitPicture(); }).observe(matte);

  // Turns the marked pictures (or the selected one) by 90° steps.
  function rotate(by) {
    if (state.busy || state.loose || !state.roll) return;
    var targets = state.marked.length > 1 ? state.marked.map(function (i) { return state.pictures[i]; }) : [state.pictures[state.selected]];
    targets = targets.filter(Boolean);
    if (!targets.length) return;
    targets.forEach(function (g) {
      var n = String(g.picture);
      var look = Object.assign({}, state.looks[n]);
      var r = (((look.rotate || 0) + by) % 360 + 360) % 360;
      if (r) look.rotate = r; else delete look.rotate;
      if (Object.keys(look).length) state.looks[n] = look; else delete state.looks[n];
    });
    lookChanged(targets);
    if (zoomAt) { var c = zoomCentre(); enterZoom(c.fx, c.fy); }
  }

  // Framing -------------------------------------------------------------------------------------
  // The shift is stored in film lines; on screen "right" is towards later
  // lines for a tail-first feed and earlier lines head first
  // (process/positive.hpp picture_col).
  function shiftDir() { return state.tailFirst ? -1 : 1; }
  function shiftOnScreen() { return state.roll ? shiftDir() * (state.roll.shift || 0) : 0; }
  function showShift() {
    var pitch = (state.roll && state.roll.frames.pitch) || 2000;
    var lim = Math.max(50, Math.round(pitch * 0.2 / 5) * 5);
    shiftInput.min = -lim;
    shiftInput.max = lim;
    shiftInput.value = shiftOnScreen();
    previewShift();
  }
  function previewShift() {
    var v = Number(shiftInput.value);
    shiftValue.textContent = (v > 0 ? '+' : v < 0 ? '−' : '±') + Math.abs(v);
    shiftValue.title = 'film lines';
    updateButtons();
    fitPicture();
  }
  function applyShift() {
    var r = state.roll;
    if (state.busy || !r || state.loose) return;
    var lines = shiftDir() * Number(shiftInput.value);
    if (lines === (r.shift || 0)) return;
    write('Framing: every frame moved ' + lines + ' film lines; developing the roll again.', 'cmd');
    flushLook().then(function () { return api.saveShift(r.prefix, lines); }).then(function () {
      // Developed again in the look on screen (the shift moves both).
      var oemOn = state.dev === 'oem' && oemReady();
      state.oemLook = null;
      if (oemOn) openRollOem(r.prefix); else openRollPakonx(r.prefix);
    }).catch(function (e) { failed(e, 'Framing not saved'); });
  }

  function showLook() {
    var f = state.pictures[state.selected];
    var look = f ? toneOf(f.picture) : {};
    LOOK_FIELDS.forEach(function (d) {
      var v = look[d.key] == null ? d.neutral : look[d.key];
      lookInputs[d.key].input.value = lookNum(d, v);
    });
  }

  function setLook(key, v) {
    var f = state.pictures[state.selected];
    if (!f || state.loose) return;
    var n = String(f.picture);
    var map = toneMap();
    var look = Object.assign({}, map[n]);
    var d = lookInputs[key].def;
    if (Math.abs(v - d.neutral) < 1e-9) delete look[key]; else look[key] = v;
    if (Object.keys(look).length) map[n] = look; else delete map[n];
    lookChanged([f]);
  }

  // Tone back to neutral; the picture's rotation stays.
  function resetLook() {
    var f = state.pictures[state.selected];
    if (!f) return;
    var n = String(f.picture);
    if (state.dev === 'oem') delete state.oemLooks[n];
    else {
      var r = rotationOf(f.picture);
      if (r) state.looks[n] = { rotate: r }; else delete state.looks[n];
    }
    lookChanged([f]);
  }

  // The roll's DX code as a preset key ("39/1"), or null when not read.
  function rollDx() {
    var dx = state.roll && !state.loose && state.roll.frames.dx;
    return dx && dx.product >= 0 ? dx.product + '/' + dx.specifier : null;
  }
  function savePreset() {
    var f = state.pictures[state.selected], key = rollDx();
    if (!f || !key || state.dev !== 'pakonx') return;
    var look = toneOf(f.picture);
    var next = Object.assign({}, presets);
    if (Object.keys(look).length) next[key] = look; else delete next[key];
    api.savePresets(next).then(function () {
      presets = next;
      write(next[key] ? 'Film preset for DX ' + key + ' saved from ' + frameLabel(f) + ': rolls of this film open with it.'
                      : 'Film preset for DX ' + key + ' removed (the look was neutral).', 'ok');
      render();
    }).catch(function (e) { write('Film preset not saved: ' + e, 'err'); });
  }
  // Puts the roll's film preset on every picture (each keeps its turn).
  // auto: on opening, only when no picture has a PakonX look yet.
  function applyPreset(auto) {
    var key = rollDx(), look = key && presets[key];
    if (!look || !state.pictures.length) return;
    if (auto && Object.keys(state.looks).some(function (n) { return Object.keys(state.looks[n]).some(function (k) { return k !== 'rotate'; }); })) return;
    state.pictures.forEach(function (g) {
      var mine = Object.assign({}, look);
      var r = rotationOf(g.picture);
      if (r) mine.rotate = r;
      state.looks[String(g.picture)] = mine;
    });
    lookChanged(state.pictures);
    write('Film preset for DX ' + key + ' applied to ' + state.pictures.length + ' pictures.', 'ok');
  }

  function lookToAll() {
    var f = state.pictures[state.selected];
    if (!f) return;
    // The tone only: every picture keeps its own rotation.
    var look = toneOf(f.picture);
    var map = toneMap();
    state.pictures.forEach(function (g) {
      var mine = Object.assign({}, look);
      var r = state.dev === 'oem' ? 0 : rotationOf(g.picture);
      if (r) mine.rotate = r;
      if (Object.keys(mine).length) map[String(g.picture)] = mine; else delete map[String(g.picture)];
    });
    lookChanged(state.pictures);
    write('Look of ' + frameLabel(f) + ' applied to all ' + state.pictures.length + ' pictures.');
  }

  var lookTimer = null;
  var lookPending = null; // { prefix, looks }: what the timer will write
  var lookSaving = Promise.resolve();
  // A look change shows on the next frame: the changed pictures' thumbnails
  // and the loupe only (a slider fires many inputs per frame; filtering
  // every thumbnail each time made dragging stutter).
  var lookFrame = 0;
  var lookDirty = [];
  function lookChanged(pictures) {
    pictures.forEach(function (g) { if (lookDirty.indexOf(g) < 0) lookDirty.push(g); });
    lookPending = { prefix: state.roll.prefix, looks: JSON.parse(JSON.stringify(state.looks)),
                    oemLooks: JSON.parse(JSON.stringify(state.oemLooks)) };
    clearTimeout(lookTimer);
    lookTimer = setTimeout(flushLook, 400);
    if (lookFrame) return;
    lookFrame = requestAnimationFrame(function () {
      lookFrame = 0;
      var imgs = strip ? strip.root.querySelectorAll('.pk-frame__img') : [];
      lookDirty.forEach(function (g) {
        var k = state.pictures.indexOf(g);
        if (imgs[k]) { imgs[k].style.filter = lookFilter(g.picture); turnThumb(imgs[k], g.picture); }
      });
      lookDirty = [];
      var f = state.pictures[state.selected];
      picture.style.filter = full.style.filter = f ? lookFilter(f.picture) : '';
      fitPicture();
      showLook();
    });
  }


  // Writes <prefix>.adjust.json now if a change is pending.
  function flushLook() {
    if (!lookPending) return lookSaving;
    clearTimeout(lookTimer);
    var pending = lookPending;
    lookTimer = lookPending = null;
    lookSaving = lookSaving.then(function () { return api.saveAdjust(pending.prefix, pending.looks, pending.oemLooks); })
      .catch(function (e) { write('Look not saved: ' + e, 'err'); });
    return lookSaving;
  }

  // The sheet and the strip show one selection and one set of marks.
  function select(i) {
    if (i === state.selected) return;
    state.selected = i;
    if (strip) strip.select(i);
    if (sheet) sheet.select(i);
    showFrame();
  }
  function markFrames(m) {
    if (String(m) === String(state.marked)) return;
    state.marked = m;
    if (strip) strip.mark(m);
    if (sheet) sheet.mark(m);
    updateButtons();
  }

  function step(d) {
    var i = state.selected + d;
    if (state.busy || !strip || i < 0 || i >= state.pictures.length) return;
    strip.select(i);
    var on = strip.root.querySelector('.is-selected');
    if (on) on.scrollIntoView({ block: 'nearest', inline: 'nearest' });
  }

  // Zoom -------------------------------------------------------------------------------------
  var zoomAt = null;   // { fx, fy }: the picture point to centre, 0..1
  var zoomToken = 0;   // the latest request wins

  picture.addEventListener('click', function (e) {
    if (state.busy || state.selected < 0) return;
    var r = picture.getBoundingClientRect();
    enterZoom((e.clientX - r.left) / r.width, (e.clientY - r.top) / r.height);
  });

  function enterZoom(fx, fy) {
    var f = state.pictures[state.selected];
    if (!f) return;
    zoomAt = { fx: fx, fy: fy };
    var token = ++zoomToken;
    var req = state.loose ? api.fullPictureFile(f.path)
      : state.dev === 'oem' && state.oemLook ? api.oemFullPicture(state.oemLook.dir, f.picture, viewTurn(f.picture))
      : api.fullPicture(state.roll.prefix, false, f.picture, viewTurn(f.picture));
    setBusy('Loading full size');
    req.then(function (path) {
      if (token !== zoomToken) return;
      full.onload = function () {
        if (token !== zoomToken) return;
        setBusy(null);
        var dpr = window.devicePixelRatio || 1;
        full.style.width = full.naturalWidth / dpr + 'px';
        full.style.height = full.naturalHeight / dpr + 'px';
        zoom.hidden = false;
        matte.hidden = true;
        full.style.marginLeft = Math.max(0, (zoom.clientWidth - full.offsetWidth) / 2) + 'px';
        full.style.marginTop = Math.max(0, (zoom.clientHeight - full.offsetHeight) / 2) + 'px';
        zoom.scrollLeft = zoomAt.fx * full.offsetWidth - zoom.clientWidth / 2;
        zoom.scrollTop = zoomAt.fy * full.offsetHeight - zoom.clientHeight / 2;
        zoom.focus({ preventScroll: true });
        render();
      };
      full.onerror = function () { setBusy(null); write('Could not show the full-size picture: ' + path, 'err'); };
      full.src = api.img(path);
    }).catch(function (e) {
      if (token !== zoomToken) return;
      setBusy(null);
      failed(e, 'Not loaded');
    });
  }

  function exitZoom() {
    zoomToken++;
    zoomAt = null;
    zoom.hidden = true;
    matte.hidden = false;
    full.removeAttribute('src');
    render();
  }

  // Drag pans; a click without a drag goes back to the fitted view.
  var drag = null;
  zoom.addEventListener('pointerdown', function (e) {
    drag = { x: e.clientX, y: e.clientY, left: zoom.scrollLeft, top: zoom.scrollTop, moved: false };
    zoom.setPointerCapture(e.pointerId);
    zoom.classList.add('is-dragging');
  });
  zoom.addEventListener('pointermove', function (e) {
    if (!drag) return;
    var dx = e.clientX - drag.x, dy = e.clientY - drag.y;
    if (Math.abs(dx) + Math.abs(dy) > 4) drag.moved = true;
    zoom.scrollLeft = drag.left - dx;
    zoom.scrollTop = drag.top - dy;
  });
  zoom.addEventListener('pointerup', function () {
    var click = drag && !drag.moved;
    drag = null;
    zoom.classList.remove('is-dragging');
    if (click) exitZoom();
  });

  // The centre of the 100 % view, to keep when stepping to another picture.
  function zoomCentre() {
    if (zoom.hidden || !full.offsetWidth) return zoomAt;
    return { fx: (zoom.scrollLeft + zoom.clientWidth / 2) / full.offsetWidth,
             fy: (zoom.scrollTop + zoom.clientHeight / 2) / full.offsetHeight };
  }

  // No page zoom: Ctrl+click picks pictures, and a scroll with Ctrl still
  // held zoomed the whole interface (WebView2's Ctrl+wheel / Ctrl+plus).
  document.addEventListener('wheel', function (e) { if (e.ctrlKey) e.preventDefault(); }, { passive: false });
  document.addEventListener('keydown', function (e) {
    if ((e.ctrlKey || e.metaKey) && /^[-+=0]$/.test(e.key)) e.preventDefault();
  });
  document.addEventListener('keydown', function (e) {
    var t = e.target;
    if (e.key === 'Escape' && zoomAt) { exitZoom(); e.preventDefault(); return; }
    var typing = t && (t.tagName === 'INPUT' || t.tagName === 'TEXTAREA' || t.tagName === 'SELECT' || t.isContentEditable);
    if ((e.ctrlKey || e.metaKey) && !e.altKey && /^[123]$/.test(e.key)) {
      showTab(['scan', 'roll', 'table'][Number(e.key) - 1]);
      e.preventDefault();
      return;
    }
    // Ctrl+A marks every picture of the roll; Esc clears the marks.
    if ((e.ctrlKey || e.metaKey) && !e.altKey && (e.key === 'a' || e.key === 'A') && !typing && strip && !state.loose && state.pictures.length) {
      markAll();
      e.preventDefault();
      return;
    }
    if (e.key === 'Escape' && !typing && strip && state.marked.length) { strip.mark([]); e.preventDefault(); return; }
    if (e.key === 'Escape' && !typing && desk.dataset.tab === 'table') { showTab('roll'); e.preventDefault(); return; }
    if (e.defaultPrevented || e.altKey || e.ctrlKey || e.metaKey) return;
    if (t && (t.tagName === 'SELECT' || t.tagName === 'INPUT' || t.closest('[role="listbox"]') || t.closest('.pk-strip'))) return;
    if (e.key === 'ArrowRight' || e.key === 'PageDown') { step(1); e.preventDefault(); }
    if (e.key === 'ArrowLeft' || e.key === 'PageUp') { step(-1); e.preventDefault(); }
  });

  function showPicture() {
    var f = state.pictures[state.selected];
    frameTitle.textContent = f.name;
    frameSub.textContent = 'Picture ' + (state.selected + 1) + ' of ' + state.pictures.length + ' · ' + f.format;
    picture.src = file(f, 'preview');
    picture.alt = f.name;
    if (zoomAt) { var c = zoomCentre(); enterZoom(c.fx, c.fy); }
    picture.hidden = false;
    empty.hidden = true;
    facts(frameFacts, [
      ['File', f.name, true],
      ['Format', f.format],
      ['Size', f.width + ' × ' + f.height, true],
      ['Depth', f.depth],
      ['On disk', sizeLabel(f.bytes), true],
      ['Folder', f.path.slice(0, f.path.length - f.name.length - 1), true]
    ]);
    render();
  }

  function showFrame() {
    if (state.loose) return showPicture();
    var r = state.roll;
    var f = state.pictures[state.selected];
    var dx = r.frames.dx || {};
    var film = dx.product >= 0 ? 'DX ' + dx.product + ' / ' + dx.specifier : 'Not read';
    if (!f) {
      frameTitle.textContent = '—';
      frameSub.textContent = 'No pictures found on this roll';
      picture.hidden = true;
      empty.hidden = false;
      facts(frameFacts, [['Roll', r.name], ['Mode', modeLabel(r.mode) + (r.bw ? ' · B&W' : '')], ['Film', film, true]]);
      render();
      return;
    }
    frameTitle.textContent = frameLabel(f);
    frameSub.textContent = 'Frame ' + (state.selected + 1) + ' of ' + state.pictures.length + ' · ' + modeLabel(r.mode);
    picture.src = file(f, 'preview');
    picture.alt = 'Frame ' + frameLabel(f);
    picture.style.filter = full.style.filter = lookFilter(f.picture);
    showLook();
    if (zoomAt) { var c = zoomCentre(); enterZoom(c.fx, c.fy); }
    picture.hidden = false;
    empty.hidden = true;
    facts(frameFacts, [
      ['Frame', frameLabel(f), true],
      ['Picture', String(f.picture), true],
      ['Lines', f.first_line + '–' + f.end_line, true],
      ['Placed', f.placement],
      ['Roll', r.name],
      ['Mode', modeLabel(r.mode) + (r.bw ? ' · B&W' : '')],
      ['Film', film, true],
      ['Feed', state.tailFirst ? 'Tail first' : 'Head first']
    ]);
    render();
  }

  // Development: PakonX's own positive or the OEM look ------------------------------------------
  // Colour negative or black-and-white: stored with the roll, which is
  // developed again in the look on screen.
  function setFilm(bw) {
    var r = state.roll;
    if (state.busy || !r || state.loose || !!r.bw === bw) { filmSelect.value = r && r.bw ? 'bw' : 'colour'; return; }
    write('Film: ' + (bw ? 'black & white' : 'colour negative') + '; developing the roll again.', 'cmd');
    flushLook().then(function () { return api.saveFilm(r.prefix, bw); }).then(function () {
      var oemOn = state.dev === 'oem' && oemReady();
      state.oemLook = null;
      if (oemOn) openRollOem(r.prefix); else openRollPakonx(r.prefix);
    }).catch(function (e) { failed(e, 'Film not saved'); filmSelect.value = r.bw ? 'bw' : 'colour'; });
  }
  // Infrared dust removal: stored with the roll, which is developed again.
  function hasIr(r) { return !!r && /-ir$/.test(r.mode || ''); }
  function setIce(ice) {
    var r = state.roll;
    if (state.busy || !r || state.loose || !hasIr(r) || iceOn(r) === (ice !== 'off')) { rollIce.input.checked = iceOn(r); return; }
    write('Dust removal ' + (ice === 'off' ? 'off' : 'on') + '; developing the roll again.', 'cmd');
    flushLook().then(function () { return api.saveIce(r.prefix, ice); }).then(function () {
      var oemOn = state.dev === 'oem' && oemReady();
      state.oemLook = null;
      if (oemOn) openRollOem(r.prefix); else openRollPakonx(r.prefix);
    }).catch(function (e) { failed(e, 'Dust removal not saved'); rollIce.input.checked = iceOn(r); });
  }
  // Frame format: stored with the roll, which is framed and developed again.
  function setFrameFormat(format) {
    var r = state.roll;
    if (state.busy || !r || state.loose || (r.format || 'auto') === format) { frameFmtSelect.value = (r && r.format) || 'auto'; return; }
    write('Frame format: ' + format + '; framing the roll again.', 'cmd');
    flushLook().then(function () { return api.saveFrameFormat(r.prefix, format); }).then(function () {
      var oemOn = state.dev === 'oem' && oemReady() && format !== 'half' && format !== 'pano';
      state.oemLook = null;
      if (oemOn) openRollOem(r.prefix); else openRollPakonx(r.prefix);
    }).catch(function (e) { failed(e, 'Frame format not saved'); frameFmtSelect.value = r.format || 'auto'; });
  }
  function setDev(dev) {
    if (state.busy || !state.roll) { devSelect.value = state.dev; return; }
    if (zoomAt) exitZoom();
    if (dev === 'pakonx') {
      if (state.roll.cache) {
        state.dev = 'pakonx';
        renderTable();
        return;
      }
      var opened = state.roll;
      openRollPakonx(opened.prefix, function (roll) {
        if (state.roll !== opened) return;
        opened.cache = roll.cache;
        opened.frames = roll.frames;
        state.dev = 'pakonx';
        renderTable();
      });
      return;
    }
    if (!oemReady()) {
      devSelect.value = state.dev;
      write('The OEM look files are missing: choose them in Settings, OEM look' +
            (bundledOem.host ? '.' : ' (oem_host.exe was not found either).'), 'err');
      return;
    }
    if (state.oemLook && state.oemLook.prefix === state.roll.prefix) {
      state.dev = 'oem';
      renderTable();
      return;
    }
    var roll = state.roll;
    setBusy('Developing (OEM look)', true);
    write('> pakon-cli frames ' + roll.prefix + ' --pakon-look --oem ' + (oem.oem || bundledOem.oem) + ' --capture ' + (oem.capture || bundledOem.capture) +
          (oem.eeprom ? ' --eeprom ' + oem.eeprom : ''), 'cmd');
    api.openLook(roll.prefix, oem).then(function (look) {
      setBusy(null);
      if (look.log) write(look.log, look.cached ? 'ok' : null);
      if (state.roll !== roll) return;
      look.prefix = roll.prefix;
      state.oemLook = look;
      state.dev = 'oem';
      renderTable();
    }).catch(function (e) {
      setBusy(null);
      devSelect.value = state.dev;
      failed(e, 'No OEM look');
    });
  }

  // Save ----------------------------------------------------------------------------------------
  function save(onlySelected) {
    var r = state.roll;
    var f = state.pictures[state.selected];
    if (state.busy || !r || state.loose || (onlySelected && !f)) return;
    var opts = { format: formatSelect.value, quality: Number(quality.select.value), dxNames: naming.select.value === 'dx',
                 name: null, start: 1, digits: 3, tailFirst: state.tailFirst };
    if (customNaming()) {
      if (nameError()) { write(nameError(), 'err'); return; }
      var no = nameOpts();
      opts.name = no.base; opts.start = no.start; opts.digits = no.digits;
    }
    var marked = state.marked.map(function (i) { return state.pictures[i].picture; });
    // Pictures go next to their scan by default (the order folder), as PSI.
    var start = dirname(r.prefix);
    flushLook().then(function () { return api.pickFolder(start); }).then(function (dest) {
      if (!dest) return;
      var pictures = !onlySelected ? [] : marked.length ? marked : [f.picture];
      setBusy('Saving', true);
      write('> pakon-cli frames ' + r.prefix + ' --save ' + opts.format + ' --quality ' + opts.quality + ' --out-dir ' + dest +
            (opts.name != null ? ' --name "' + opts.name + '" --start ' + opts.start + ' --digits ' + opts.digits
              : opts.dxNames ? ' --dx-names' : '') + (pictures.length ? ' --pictures ' + pictures.join(',') : '') +
            (state.tailFirst ? ' --tail-first' : '') + (r.shift ? ' --shift ' + r.shift : ''), 'cmd');
      var run = state.dev === 'oem' ? api.saveLook(r.prefix, oem, pictures, dest, opts)
        : api.savePictures(r.prefix, state.tailFirst, opts, pictures, dest);
      return run.then(function (res) {
        setBusy(null);
        if (res.log) write(res.log);
        var n = res.written.length;
        state.saved = 'Saved ' + n + ' picture' + (n === 1 ? '' : 's') + ' to ' + dest + '.';
        write(state.saved, 'ok');
        render();
        // The whole roll is out: offer to free the scan's space.
        if (!onlySelected && n === state.pictures.length && n > 0) return offerDelete(r, res.written);
      });
    }).catch(function (e) {
      setBusy(null);
      failed(e, 'Not saved');
    });
  }

  // pictures: numbers from 1; empty = the whole roll.
  function openInNlp(pictures) {
    var r = state.roll;
    if (state.busy || !r || state.loose) return;
    setBusy('Writing negatives', true);
    write('> pakon-cli frames ' + r.prefix + ' --save negative --dx-names --out-dir ' + r.prefix + '_negatives' +
          (pictures.length ? ' --pictures ' + pictures.join(',') : '') + (state.tailFirst ? ' --tail-first' : '') +
          (r.shift ? ' --shift ' + r.shift : ''), 'cmd');
    api.openInNlp(r.prefix, state.tailFirst, pictures).then(function (res) {
      setBusy(null);
      if (res.log) write(res.log);
      var n = res.written.length;
      write('Opened ' + n + ' negative' + (n === 1 ? '' : 's') + ' in Lightroom Classic: import them, then run Negative Lab Pro.', 'ok');
      if (!res.plugin) write('Negative Lab Pro was not found in Lightroom\'s Modules folder: install it, or check Lightroom\'s Plug-in Manager.', 'err');
    }).catch(function (e) {
      setBusy(null);
      failed(e, 'Not opened');
    });
  }

  function offerDelete(r, written) {
    return api.scanFiles(r.prefix).then(function (files) {
      if (!files.length) return;
      var total = files.reduce(function (s, f) { return s + f.bytes; }, 0);
      var names = files.map(function (f) { return f.path.slice(dirname(f.path).length + 1) + ' (' + sizeLabel(f.bytes) + ')'; });
      var msg = 'All ' + written.length + ' pictures of ' + r.name + ' are saved. Delete the scan to free ' + sizeLabel(total) + '?\n\n' +
                names.join('\n') + '\n\nThe saved pictures stay. The roll cannot be opened, rotated or saved again.';
      return api.ask(msg, 'Delete the scan of ' + r.name).then(function (yes) {
        if (!yes) { write('Scan kept: ' + r.prefix); return; }
        return api.deleteScan(r.prefix, written).then(function (removed) {
          removed.forEach(function (p) { write('deleted: ' + p); });
          write('Scan of ' + r.name + ' deleted; showing its saved pictures.', 'ok');
          state.roll = null;
          state.saved = null;
          loadRolls();
          openPictures(written);
        });
      });
    });
  }

  // Clear: back to how PakonX opens. Closes the roll or pictures (files on
  // disk stay; looks are saved first), empties the light table and the roll
  // number, and initializes the scanner again.
  function clearAll() {
    if (state.busy || state.checking) return;
    if (zoomAt) exitZoom();
    flushLook().then(function () {
      state.roll = null;
      state.loose = null;
      state.pictures = [];
      state.selected = -1;
      state.marked = [];
      state.looks = {};
      state.oemLooks = {};
      state.oemLook = null;
      state.dev = 'pakonx';
      state.tailFirst = false;
      state.saved = null;
      strip = null;
      sheet = null;
      stripSlot.replaceChildren();
      sheetSlot.replaceChildren(sheetEmpty);
      sheetTitle.textContent = '—';
      sheetSub.textContent = 'No roll open';
      rollWin.root.querySelector('.pk-titlebar__text').textContent = 'Roll';
      picture.hidden = true;
      picture.removeAttribute('src');
      empty.hidden = false;
      frameTitle.textContent = '—';
      frameSub.textContent = 'No roll open';
      table.root.querySelector('.pk-titlebar__text').textContent = 'Light table';
      facts(frameFacts, []);
      if (!(order && order.batch)) order = null; // an open batch survives Clear
      nameBase.value = '';
      write('Cleared.', 'cmd');
      renderRolls();
      render();
      initScanner();
    });
  }

  function updateButtons() {
    renderDrive();
    prevBtn.disabled = state.busy || state.selected <= 0;
    nextBtn.disabled = state.busy || state.selected < 0 || state.selected >= state.pictures.length - 1;
    var roll = !!state.roll && !state.loose;
    rotateBtn.disabled = state.busy || !roll;
    clearBtn.disabled = state.busy || state.checking;
    turnLeftBtn.disabled = turnRightBtn.disabled = state.busy || !roll || state.selected < 0;
    var shiftChanged = roll && Number(shiftInput.value) !== shiftOnScreen();
    shiftInput.disabled = state.busy || !roll || !state.pictures.length;
    shiftApply.disabled = state.busy || !shiftChanged;
    shiftReset.disabled = state.busy || !roll || Number(shiftInput.value) === 0;
    var badName = customNaming() && !!nameError();
    var n = state.marked.length;
    savePictureBtn.textContent = n > 1 ? 'Save ' + n + ' pictures' : 'Save picture';
    savePictureBtn.disabled = state.busy || !roll || state.selected < 0 || badName;
    quality.select.disabled = state.busy || !roll || formatSelect.value !== 'jpeg';
    naming.select.disabled = state.busy || !roll;
    saveRollBtn.disabled = state.busy || !roll || !state.pictures.length || badName;
    nlpBtn.disabled = state.busy || !roll || !!state.loose || !state.pictures.length;
    nlpPicBtn.disabled = state.busy || !roll || !!state.loose || state.selected < 0;
    nlpBtn.textContent = n ? 'Open ' + n + ' in Negative Lab Pro' : 'Open roll in Negative Lab Pro';
    [nameBase, nameStart, nameDigits.select].forEach(function (c) { c.disabled = state.busy || !roll; });
    formatSelect.disabled = state.busy || !roll;
    renderScan();
  }

  // Scanner commands ---------------------------------------------------------------------------
  function absorb(run) {
    var out = run.stdout;
    var model = /^Model: (.+)$/m.exec(out);
    var version = /^Version (.+)$/m.exec(out);
    if (run.code === 0) {
      if (run.command === 'list') {
        if (/\(operational\)/.test(out)) { state.scanner = 'Warm'; state.tone = 'ok'; }
        else if (/cold\/bootstrap/.test(out)) { state.scanner = 'Cold ROM'; state.tone = 'neutral'; }
        else { state.scanner = 'Not found'; state.tone = 'neutral'; }
      } else { state.scanner = 'Warm'; state.tone = 'ok'; }
      if (model) state.model = model[1];
      if (version) state.version = version[1].replace(/\s{2,}/g, ' · ');
    } else {
      state.scanner = run.command === 'list' ? 'Not found' : 'No session';
      state.tone = 'danger';
    }
    render();
  }

  // state.checking also covers these: one scanner command at a time.
  function run(command) {
    if (state.busy || state.checking) return;
    write('> pakon-cli ' + command, 'cmd');
    state.checking = true;
    state.scanner = 'Working';
    state.tone = 'warn';
    render();
    api.runCli(command).then(function (r) {
      state.checking = false;
      if (r.stdout) write(r.stdout);
      if (r.stderr) write(r.stderr, 'err');
      write('exit ' + r.code + ' · ' + r.millis + ' ms', r.code === 0 ? 'ok' : 'err');
      absorb(r);
    }).catch(function (e) {
      state.checking = false;
      write(String(e), 'err');
      state.scanner = 'CLI missing';
      state.tone = 'danger';
      render();
    });
  }


  // Chrome --------------------------------------------------------------------------------------
  var statusbar = document.getElementById('statusbar');
  var rollChip = document.getElementById('roll-chip');

  function render() {
    chipSlot.replaceChildren(P.StatusChip({ label: state.scanner, tone: state.tone }));
    facts(scannerFacts, [['Model', state.model], ['Firmware', state.version, true]]);
    var loose = state.loose;
    rollChip.replaceChildren(loose ? P.StatusChip({ label: 'Pictures ' + loose.name, tone: 'neutral' })
      : state.roll ? P.StatusChip({ label: 'Roll ' + state.roll.name, tone: 'film' }) : document.createTextNode(''));
    statusbar.replaceChildren();
    var dx = !loose && state.roll && state.roll.frames.dx;
    [state.scanner,
     loose ? 'Pictures ' + loose.name : state.roll ? 'Roll ' + state.roll.name : 'No roll',
     state.pictures.length + (loose ? ' pictures' : ' frames'),
     loose ? 'Opened from disk' : dx && dx.product >= 0 ? 'Film DX ' + dx.product + ' / ' + dx.specifier : 'Film DX not read'
    ].forEach(function (s) { statusbar.append(el('span', null, s)); });
    statusbar.append(lastMsg, logToggle);
    updateButtons();
    zoomSlot.replaceChildren(zoom.hidden ? document.createTextNode('') : P.StatusChip({ label: '100 % · Esc to fit', tone: 'neutral' }));
    saveGroup.hidden = !!loose;
    lookGroup.hidden = !!loose || !state.roll;
    devGroup.hidden = !!loose || !state.roll;
    showDetected();
    useBwBtn.disabled = state.busy;
    selectAllBtn.disabled = state.busy || !state.roll || !!loose || !state.pictures.length;
    frameGroup.hidden = !!loose || !state.roll;
    var noPicture = state.busy || !state.roll || state.selected < 0;
    var fixed = noPicture;
    LOOK_FIELDS.forEach(function (d) { var l = lookInputs[d.key]; l.input.disabled = l.minus.disabled = l.plus.disabled = fixed; });
    lookReset.disabled = lookAll.disabled = fixed;
    var dxKey = rollDx(), pakonx = state.dev === 'pakonx';
    presetSave.disabled = fixed || !dxKey || !pakonx;
    presetApply.disabled = state.busy || !dxKey || !presets[dxKey] || !pakonx;
    presetNote.textContent = !state.roll || state.loose ? 'Film presets: open a roll.'
      : !pakonx ? 'Film presets apply to the PakonX look.'
      : !dxKey ? 'Film presets: this scan read no DX code.'
      : presets[dxKey] ? 'Film preset for DX ' + dxKey + ' is set; new rolls of this film open with it.'
      : 'No preset for DX ' + dxKey + ' yet: grade a picture, then Save film preset.';
    devSelect.value = state.dev;
    devSelect.disabled = state.busy || !state.roll;
    filmSelect.value = state.roll && state.roll.bw ? 'bw' : 'colour';
    filmSelect.disabled = state.busy || !state.roll || !!state.loose;
    rollIce.input.checked = iceOn(state.roll);
    frameFmtSelect.value = (state.roll && state.roll.format) || 'auto';
    frameFmtSelect.disabled = state.busy || !state.roll || !!state.loose;
    rollIce.input.disabled = state.busy || !hasIr(state.roll) || !!state.loose;
    iceRow.title = state.roll && !hasIr(state.roll) ? 'Needs a scan made with Dust removal (ICE) switched on.' : '';
    oemChip.replaceChildren(P.StatusChip({ label: oemReady() ? 'Ready' : 'Not set up', tone: oemReady() ? 'ok' : 'neutral' }));
    facts(oemFacts, [['OEM', oemLabel(oem.oem, bundledOem.oem), true], ['Capture', oemLabel(oem.capture, bundledOem.capture), true],
                     ['EEPROM', oem.eeprom ? shortPath(oem.eeprom) : 'From the scan', true],
                     ['Host', bundledOem.host ? 'Found' : 'Not found', true]]);
    saveNote.textContent = !state.roll ? 'Open a roll to save its pictures.'
      : state.saved || (state.dev === 'oem' ? (formatSelect.value === 'tiff' ? 'Saves the OEM look as 8-bit TIFF (what the OEM pipeline makes), with your look, turns and names.'
          : 'Saves the OEM look as JPEG, with your look, turns and names; at quality 90 and unchanged it is the OEM\'s own file.')
        : 'Saves the pictures as developed here, look and rotation included.');
  }

  renderNaming();
  try { openLog(localStorage.getItem('pakonx.log') === '1'); } catch (e) { openLog(false); }
  var startTab = 'scan';
  try { startTab = localStorage.getItem('pakonx.tab') || 'scan'; } catch (e) {}
  var urlTab = demo && /[?&]tab=(\w+)/.exec(location.search); // demo: ?tab=roll for reviews
  if (urlTab) startTab = urlTab[1];
  showTab(/^(scan|roll|table)$/.test(startTab) ? startTab : 'scan');
  render();
  write('PakonX 1.0.0 (free software, GPL-3.0) — the scanner is initialized on start (booted when cold); Scan film checks the scanner, then runs the film pass; rolls open and save offline.');
  api.cliPath().then(function (p) { write('pakon-cli: ' + p); });
  try { state.folder = localStorage.getItem('pakonx.folder'); } catch (e) {}
  if (demo) state.folder = 'demo';
  if (state.folder) loadRolls();
  if (tauri) initScanner();
  if (tauri) setTimeout(function () { checkUpdate(false); }, 4000);
})();
