// The RT4K's profiles for the Cruller page (served as /profiles.js, embedded at build time): each SVS
// input's profile, picked in the SVS tab from the RT4K's /profile/SVS (GET /rt4k/ls, as sd.js reads
// them; renamed with the RT4K's mv, copied with GET /rt4k/get and POST /rt4k/put). The profiles
// themselves are browsed, loaded and opened in the editor in the SD card view (sd.js). Uses sd.js's
// helpers (window.sdInternals).

(() => {
  'use strict';

  const q = (id) => document.getElementById(id);
  const sd = window.sdInternals;

  const ROOT = 'profile';   // on the SD card
  const PATH_MAX = 160;     // bytes of the SD path, as Cruller takes them (sd.js)

  // --- pure helpers (tests/test_profiles.js) ----------------------------------------------------------

  const isProfile = (name) => /\.rt[46]$/i.test(name);
  const plain = (name) => name.replace(/\.rt[46]$/i, '');

  // --- state ------------------------------------------------------------------------------------------

  let power = '';
  const asleep = () => power === 'standby' || power === 'starting';

  const esc = (s) => String(s).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));

  // --- Cruller ----------------------------------------------------------------------------------------

  async function ask(cmd, expect, timeout) {
    const r = await fetch('/rt4k/ask?expect=' + encodeURIComponent(expect) + (timeout ? '&timeout=' + timeout : ''), { method: 'POST', body: cmd });
    const t = (await r.text()).trim();
    if (!r.ok) throw new Error(r.status === 504 ? 'the RT4K did not answer' : t);
    return t;
  }

  // Copies a file on the SD card (paths from its root) through the page: read whole, written with its
  // SHA-256 (the RT4K checks it).
  async function copyFile(from, to) {
    const g = await fetch('/rt4k/get?path=' + encodeURIComponent(from));
    if (!g.ok) throw new Error(from.split('/').pop() + ': ' + ((await g.text()).trim() || 'HTTP ' + g.status));
    const data = new Uint8Array(await g.arrayBuffer());
    const p = await fetch('/rt4k/put?path=' + encodeURIComponent(to) + '&sha=' + window.sha256(data), { method: 'POST', body: data });
    if (!p.ok) throw new Error(to.split('/').pop() + ': ' + ((await p.text()).trim() || 'HTTP ' + p.status));
  }

  // --- each SVS input's profile (the SVS tab) ---------------------------------------------------------
  //
  // The RT4K's own way (Auto Load SVS): when the switch tells it input n is on, it loads the first
  // /profile/SVS/S<n>_<anything>.rt4 it finds, and only from that folder. Each input gets a combo with
  // the profiles there; picking one makes it the input's file: renamed to S<n>_<name>, or copied when
  // it's another input's (each input keeps its own). The files the input had become unassigned: X_ in
  // place of their S<n>_ (S1_SNES: X_SNES, or X_SNES (2) when that's taken), kept in the folder to be
  // picked again.

  const SVS_DIR = 'SVS'; // under /profile (FAT: whatever its case on the card)
  // An unassigned profile's prefix. Not an S: were the RT4K to read the input with atoi(), "SX_"
  // would read as input 0 (and "S0_" is a real one, for no input).
  const UNSET = 'X_';

  // The input a file in the SVS folder is for: n for "S<n>_<anything>.rt4" (0: none).
  function slotOf(name) {
    const m = /^S(\d+)_/i.exec(name);
    return m && isProfile(name) ? +m[1] : 0;
  }

  // A profile's name without its X_, S<n>_ and extension ("X_S3_PS1 480i.rt4": "PS1 480i").
  const baseName = (name) => plain(name).replace(/^(X_)?(S\d+_)?/i, '');
  const extOf = (name) => (/\.rt[46]$/i.exec(name) || ['.rt4'])[0];

  // A name that isn't in taken (lower case: FAT): base + ext, else "base (2)" + ext, and on.
  function freeName(taken, base, ext) {
    for (let i = 1; ; i++) {
      const name = (i === 1 ? base : base + ' (' + i + ')') + ext;
      if (!taken.has(name.toLowerCase())) return name;
    }
  }

  // What makes name (a profile in the SVS folder; '' for none) input n's profile. files: the folder's
  // profiles in the card's order. Returns the steps in order ({op: 'mv' | 'cp', from, to}, names in
  // the folder) and the name it ends up with ('' for none). The input's other files become X_ first,
  // so a failure halfway loses nothing; picking the input's own file only drops the others.
  function plan(files, n, name) {
    const mine = files.filter((f) => slotOf(f) === n);
    const keep = name ? mine.find((f) => sd.sameName(f, name)) : undefined;
    const steps = [];
    const taken = new Set(files.map((f) => f.toLowerCase()));
    for (const f of mine) {
      if (f === keep) continue;
      const to = freeName(taken, UNSET + baseName(f), extOf(f));
      taken.delete(f.toLowerCase());
      taken.add(to.toLowerCase());
      steps.push({ op: 'mv', from: f, to });
    }
    if (!name) return { steps, target: '' };
    if (keep) return { steps, target: keep };
    // Free: every file named S<n>_... was the input's, and was renamed above.
    const to = 'S' + n + '_' + baseName(name) + extOf(name);
    steps.push({ op: slotOf(name) ? 'cp' : 'mv', from: name, to });
    return { steps, target: to };
  }

  const sv = {
    input: 0, total: 0,  // the switch: the input on screen (0: none), how many it has (app.js)
    files: null,         // the SVS folder's profiles in the card's order (null: not read)
    none: false,         // the card has no /profile/SVS
    reading: false, busy: false, failed: false, waking: false,
    key: '',             // what the cards' combos were drawn for (app.js draws them again when it changes)
    kept: null,          // each input's profile as Cruller keeps it ({"1": "S1_PS1.rt4"}; null: not known)
  };

  // The folder read now: the combos change it. Else each input's profile as Cruller keeps it, only to
  // show (the RT4K asleep, or the folder being read again).
  const svsLive = () => !!sv.files && !sv.none && !asleep();

  // Each input's profile as Cruller keeps it (GET /api/v1/svs "profiles", from app.js).
  function svsKept(profiles) {
    sv.kept = profiles && typeof profiles === 'object' ? profiles : null;
    svsRender();
  }

  // The folder just read: each input's profile (its first S<n>_ file, the one the RT4K loads) to Cruller,
  // when it isn't what it keeps (its flash is written only then).
  function svsSend() {
    const now = {};
    for (const f of sv.files) {
      const n = slotOf(f);
      if (n && n <= 32 && !now[n]) now[n] = f;
    }
    const same = (a, b) => JSON.stringify(Object.entries(a).sort()) === JSON.stringify(Object.entries(b).sort());
    if (sv.kept && same(sv.kept, now)) return;
    sv.kept = now;
    const text = Object.keys(now).sort((a, b) => a - b).map((n) => n + '\t' + now[n]).join('\n');
    fetch('/api/v1/svs/profiles', { method: 'POST', body: text }).catch(() => { /* sent again with the next read */ });
  }

  const svsShowing = () => q('v-prof') && !q('v-prof').hidden && !q('v-prof').closest('[data-view]').hidden;
  const svsPath = (name) => ROOT + '/' + SVS_DIR + '/' + name;

  function svsStatus(text, bad) {
    const s = q('vps');
    if (!s) return;
    s.textContent = text;
    s.classList.toggle('bad', !!bad);
  }

  // The SVS folder's profiles, in the card's order (macOS's ._ files aside).
  async function svsList() {
    const r = await fetch('/rt4k/ls?dir=' + encodeURIComponent(ROOT + '/' + SVS_DIR));
    const body = await r.text();
    sv.none = r.status === 404;
    if (!r.ok && !sv.none) throw new Error(body.trim() || 'HTTP ' + r.status);
    sv.files = sv.none ? [] : sd.parseList(body).filter((e) => !e.dir && isProfile(e.name) && !e.name.startsWith('.')).map((e) => e.name);
    svsSend();
  }

  // Reads the SVS folder (one listing): on opening the tab, with the refresh button, when the RT4K comes on.
  async function svsRead() {
    if (sv.reading || sv.busy || asleep()) return;
    sv.reading = true;
    sv.failed = false;
    svsStatus('Reading /profile/SVS…');
    svsRender();
    try {
      await svsList();
      svsStatus(sv.none ? 'The SD card has no /profile/SVS folder: make it in the SD card view, and save profiles there.' : '');
    } catch (e) {
      sv.files = null;
      sv.failed = true;
      svsStatus(e.message === 'Failed to fetch' ? 'Cruller did not answer.' : 'Could not read /profile/SVS: ' + e.message, true);
    }
    sv.reading = false;
    svsRender();
  }

  // The combo's options for input n: none, its own, the unassigned ones (renamed when picked), and the
  // other inputs' (copied). Each by its name alone; two unassigned ones of the same name (X_SNES and a
  // loose SNES) by their whole names.
  function svsOptions(n, current) {
    const opt = (f, label) => '<option value="' + esc(f) + '"' + (f === current ? ' selected' : '') + '>' + esc(label) + '</option>';
    const group = (label, list, text) => (list.length ? '<optgroup label="' + label + '">' + list.map((f) => opt(f, text(f))).join('') + '</optgroup>' : '');
    const files = sd.sortEntries(sv.files.map((name) => ({ name, dir: false })), 'name', false).map((e) => e.name);
    const unset = files.filter((f) => !slotOf(f));
    const twice = (f) => unset.filter((g) => baseName(g).toLowerCase() === baseName(f).toLowerCase()).length > 1;
    return '<option value="">None</option>' + files.filter((f) => slotOf(f) === n).map((f) => opt(f, baseName(f))).join('') +
      group('Unassigned', unset, (f) => (twice(f) ? plain(f) : baseName(f))) +
      group('Copy another input\'s', files.filter((f) => slotOf(f) && slotOf(f) !== n).sort((a, b) => slotOf(a) - slotOf(b)),
        (f) => baseName(f) + ' (input ' + slotOf(f) + ')');
  }

  // Input n's file: the first of its files in the card's order, the one the RT4K finds (or as kept).
  const svsCurrent = (n) => {
    if (!svsLive()) return { name: (sv.kept && sv.kept[n]) || '', count: 0 };
    const mine = sv.files.filter((f) => slotOf(f) === n);
    return { name: mine.length ? mine[0] : '', count: mine.length };
  };

  // What the cards' combos show ('' for no combos: the folder missing or empty, or nothing kept).
  const svsKey = () => {
    if (svsLive()) return sv.files.length ? 'now/' + sv.files.join('/') : '';
    return sv.kept && Object.keys(sv.kept).length ? 'kept/' + JSON.stringify(sv.kept) : '';
  };

  // Input n's combo, for its card in the SVS tab's grid (app.js); '' while there's none to show. As
  // Cruller keeps it, it only shows.
  function svsSelect(n) {
    if (!svsKey()) return '';
    const cur = svsCurrent(n);
    if (!svsLive()) {
      return '<select data-n=' + n + ' aria-label="Profile for input ' + n + '" disabled title="Turn the RT4K on to change it">' +
        '<option>' + esc(cur.name ? baseName(cur.name) : 'None') + '</option></select>';
    }
    return '<select data-n=' + n + ' aria-label="Profile for input ' + n + '"' + (sv.busy || sv.reading ? ' disabled' : '') + '>' +
      svsOptions(n, cur.name) + '</select>' +
      (cur.count > 1 ? '<small class=svp-warn title="The RT4K loads the first it finds: this one. Pick it again to leave it alone.">' +
        cur.count + ' S' + n + '_ files</small>' : '');
  }

  // Back to what input n has (a pick that changed nothing).
  function svsRevert(n) {
    const sel = q('v-grid') && q('v-grid').querySelector('select[data-n="' + n + '"]');
    if (sel) sel.value = svsCurrent(n).name;
  }

  // Under the grid: how it went, the RT4K asleep.
  function svsBuild() {
    const box = q('v-prof');
    if (!box || q('vps')) return;
    box.innerHTML = '<div id=vps class=small></div>' +
      '<div id=vpz class=svp-asleep hidden><span id=vpzt></span><button id=vpzb class=primary>Turn the RT4K on</button></div>';
    q('vpr').onclick = svsRead;
    q('vpzb').onclick = () => {
      if (!window.rt4kWake()) return svsStatus('Cruller did not answer.', true);
      sv.waking = true;
      svsRender();
      setTimeout(() => { if (sv.waking) { sv.waking = false; svsRender(); } }, 10000);
    };
    q('v-grid').addEventListener('change', (ev) => {
      const sel = ev.target.closest('select[data-n]');
      if (sel) svsChoose(+sel.dataset.n, sel.value);
    });
  }

  function svsRender() {
    svsBuild();
    if (!q('vps')) return;
    const sleeping = asleep() && !sv.busy;
    q('vpz').hidden = !sleeping;
    q('vpr').disabled = sv.busy || sv.reading || asleep();
    if (sleeping) {
      const starting = power === 'starting' || sv.waking;
      q('vpzt').textContent = svsKey() ?
        (starting ? 'The RT4K is starting: the profiles can be changed once it answers.' :
          'The RT4K is in standby: each input\'s profile as Cruller kept it. Turn it on to change them.') :
        (starting ? 'The RT4K is starting: the profiles show up as soon as it answers.' :
          'The RT4K is in standby: each input\'s profile shows up once it\'s on.');
      q('vpzb').hidden = starting;
      svsStatus('');
    }
    // The combos: drawn again when what they list changes (app.js, with the cards), else only on or off.
    if (svsKey() !== sv.key) {
      sv.key = svsKey();
      if (window.svsRedraw) window.svsRedraw();
    }
    q('v-grid').querySelectorAll('select[data-n]').forEach((s) => { s.disabled = sv.busy || sv.reading || !svsLive(); });
  }

  async function svsStep(s) {
    if (s.op === 'mv') {
      const r = await ask('mv ' + svsPath(s.from) + '|' + svsPath(s.to), 'mv');
      if (!r.startsWith('mv ok')) throw new Error(s.from + ': ' + r);
      return;
    }
    await copyFile(svsPath(s.from), svsPath(s.to));
  }

  const stepText = (s) => (s.op === 'mv' ? 'renaming ' + plain(s.from) + ' to ' + plain(s.to) : 'copying ' + plain(s.from) + ' to ' + plain(s.to));

  async function svsChoose(n, name) {
    if (sv.busy || !svsLive()) return svsRevert(n);
    const p = plan(sv.files, n, name);
    if (!p.steps.length) return svsRevert(n);
    const long = p.steps.find((s) => sd.utf8Length(svsPath(s.to)) > PATH_MAX);
    if (long) {
      svsStatus(long.to + ': the path is too long for Cruller (' + PATH_MAX + ' bytes at most).', true);
      return svsRevert(n);
    }
    sv.busy = true;
    svsRender();
    // Only what went wrong is said: the combos show how it ended.
    let note = [''];
    try {
      for (const s of p.steps) {
        svsStatus('Input ' + n + ': ' + stepText(s) + '…');
        await svsStep(s);
      }
      // The input on screen: loaded now, as the RT4K would on switching to it.
      if (p.target && n === sv.input && power === 'on') {
        svsStatus('Input ' + n + ': loading ' + baseName(p.target) + '…');
        const r = await ask('prof load ' + SVS_DIR + '/' + p.target, 'prof', 15000);
        if (r !== 'prof load ok') note = ['Input ' + n + ': loading ' + baseName(p.target) + ' failed: ' + r, true];
      }
    } catch (e) {
      note = ['Input ' + n + ': ' + (e.message === 'Failed to fetch' ? 'Cruller did not answer' : e.message), true];
    }
    try {
      await svsList();
    } catch (e) { // what's in the folder now isn't known: nothing to pick from until it's read again
      sv.files = null;
      sv.failed = true;
      note = [(note[0] ? note[0] + ' ' : '') + 'Could not read /profile/SVS again: use the refresh button.', true];
    }
    sv.busy = false;
    svsRender();
    svsStatus(...note);
  }

  // The SVS tab's switch (app.js, with every status): which input is on screen, how many there are.
  function svsSwitch(info) {
    sv.input = info.input;
    sv.total = info.total;
    if (!sv.files && !sv.reading && !sv.failed && power === 'on' && sv.total && svsShowing()) svsRead();
  }

  // The SVS tab opens: the folder is read again (the SD card view may have changed it).
  function svsOpen() {
    svsRender();
    if (power === 'on' && svsShowing()) svsRead();
  }

  // The RT4K asleep or waking up: what was read may not hold once it's back (its card can change).
  function svsPower(was) {
    if (power !== 'standby') sv.waking = false;
    if (was === power) return;
    if (asleep()) { sv.files = null; sv.failed = false; }
    svsRender();
    if (power === 'on' && sv.total && svsShowing()) svsRead();
  }

  // Every status: the RT4K going to sleep or waking up (the SVS folder read again once it's on).
  function onStatus(s) {
    const was = power;
    power = s.rt4k_power;
    svsPower(was);
  }

  window.profStatus = onStatus;
  window.profSvsSwitch = svsSwitch;
  window.profSvsOpen = svsOpen;
  window.profSvsSelect = svsSelect;
  window.profSvsKey = svsKey;
  window.profSvsKept = svsKept;
  window.profInternals = { isProfile, plain, slotOf, baseName, freeName, plan }; // tests/test_profiles.js
})();
