// The RT4K's profiles for the Cruller page (served as /profiles.js, embedded at build time).
//
// The folders under /profile on its SD card (GET /rt4k/ls, as sd.js reads them), the profile it has
// loaded, loading one and saving its current settings as a new one: the console's "prof get",
// "prof load <path>" and "prof save <path>" (POST /rt4k/ask), with paths relative to /profile. Each
// folder has its own address (#rt4k/profiles/<path>). Uses sd.js's helpers (window.sdInternals).

(() => {
  'use strict';

  const q = (id) => document.getElementById(id);
  const sd = window.sdInternals;

  const ROOT = 'profile';   // on the SD card
  const PATH_MAX = 160;     // bytes of the SD path, as Cruller takes them (sd.js)
  const READ_EVERY = 10000; // ms: the loaded profile is read again while the view shows (the IR remote changes it too)

  // --- pure helpers (tests/test_profiles.js) ----------------------------------------------------------

  // "prof get"'s reply: the loaded profile's path under /profile, '' for none, null when it's not that.
  function parseLoaded(line) {
    if (/^prof loaded=0\b/.test(line)) return '';
    const m = /^prof loaded=1 dir=\/profile file=(.+)$/.exec(line);
    return m ? m[1] : null;
  }

  const isProfile = (name) => /\.rt[46]$/i.test(name);

  // The extension the RT4K saves with: .rt6 for the 6X, .rt4 for the 4Ks ("model" as it says it).
  const extFor = (model) => (/6x/i.test(model || '') ? '.rt6' : '.rt4');

  // A new profile's file name: the extension added when it has none of a profile's.
  const withExt = (name, model) => (isProfile(name) ? name : name + extFor(model));

  const plain = (name) => name.replace(/\.rt[46]$/i, '');

  const join = (dir, name) => (dir ? dir + '/' + name : name);

  // A folder's page address (dir: under /profile), and back: as sd.js does it.
  const hrefFor = (dir) => '#rt4k/profiles' + (dir ? '/' + dir.split('/').map(encodeURIComponent).join('/') : '');

  // The folder a loaded profile's path is in.
  const dirOf = (path) => path.split('/').slice(0, -1).join('/');

  // --- state ------------------------------------------------------------------------------------------

  let dir = null;          // the folder shown, under /profile ('' = /profile itself; null: nothing yet)
  let entries = [];        // its folders and profiles
  let others = 0;          // its other files (not shown)
  let total = 0, listed = 0;
  let loaded = null;       // the loaded profile's path under /profile ('' none; null not known)
  let loadedAt = 0;
  let loading = 0;         // the listing request that counts
  let failed = false;
  let busy = '';           // what runs: a load or a save (one at a time)
  let power = '', model = '';
  let waking = false;

  const asleep = () => power === 'standby' || power === 'starting';

  const esc = (s) => String(s).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));

  const svg = (body, fill) => '<svg width="18" height="18" viewBox="0 0 24 24" ' + (fill ? 'fill="currentColor"' :
    'fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"') + ' aria-hidden="true">' + body + '</svg>';
  const ICON = {
    dir: svg('<path d="M3 6.5A1.5 1.5 0 0 1 4.5 5h4.6l2 2.2h8.4A1.5 1.5 0 0 1 21 8.7v9.8a1.5 1.5 0 0 1-1.5 1.5h-15A1.5 1.5 0 0 1 3 18.5z"/>', true),
    prof: svg('<rect x="3" y="5" width="18" height="12" rx="2"/><path d="M8 21h8"/><path d="M12 17v4"/>'),
  };

  // --- Cruller ----------------------------------------------------------------------------------------

  async function ask(cmd, expect, timeout) {
    const r = await fetch('/rt4k/ask?expect=' + encodeURIComponent(expect) + (timeout ? '&timeout=' + timeout : ''), { method: 'POST', body: cmd });
    const t = (await r.text()).trim();
    if (!r.ok) throw new Error(r.status === 504 ? 'the RT4K did not answer' : t);
    return t;
  }

  async function readLoaded() {
    try {
      const got = parseLoaded(await ask('prof get', 'prof loaded'));
      loaded = got;
    } catch (e) {
      loaded = null;
    }
    loadedAt = Date.now();
    showLoaded();
    if (!busy) render();
  }

  // --- UI ---------------------------------------------------------------------------------------------

  function status(text, bad) {
    const s = q('pfs');
    s.textContent = text;
    s.classList.toggle('bad', !!bad);
  }

  function summary() {
    const dirs = entries.filter((e) => e.dir).length, profs = entries.length - dirs;
    const parts = [];
    if (dirs) parts.push(dirs + (dirs === 1 ? ' folder' : ' folders'));
    if (profs) parts.push(profs + (profs === 1 ? ' profile' : ' profiles'));
    let t = parts.join(', ');
    if (total > listed) t += ' · showing the first ' + listed + ' of ' + total + ' entries (the listing is too long for Cruller)';
    return t;
  }

  function crumbs() {
    const parts = dir ? dir.split('/') : [];
    let html = parts.length ? '<a href="' + hrefFor('') + '">Profiles</a>' : '<b>Profiles</b>';
    parts.forEach((p, i) => {
      html += '<span>/</span>';
      html += i === parts.length - 1 ? '<b>' + esc(p) + '</b>' : '<a href="' + hrefFor(parts.slice(0, i + 1).join('/')) + '">' + esc(p) + '</a>';
    });
    q('pfc').innerHTML = html;
  }

  function showLoaded() {
    const box = q('pfl');
    if (!box) return;
    if (asleep() || loaded === null) {
      box.innerHTML = '<span class=small>Loaded now</span><span class=pfn>' + (asleep() ? '–' : 'Not known') + '</span>';
      return;
    }
    if (!loaded) {
      box.innerHTML = '<span class=small>Loaded now</span><span class=pfn>None</span><span class=small>The RT4K runs on settings that aren\'t a saved profile.</span>';
      return;
    }
    const where = dirOf(loaded);
    box.innerHTML = '<span class=small>Loaded now</span><span class=pfn title="/profile/' + esc(loaded) + '">' + esc(plain(loaded.split('/').pop())) + '</span>' +
      '<a class=more href="' + hrefFor(where) + '">' + esc(where ? 'in ' + where : 'in /profile') + '</a>';
  }

  // The RT4K asleep: in place of the folder, why there's none and a way to turn it on.
  function showAsleep() {
    const on = !asleep() || !!busy;
    q('pfz').hidden = on;
    q('pftbl').hidden = !on;
    showLoaded();
    if (on) return;
    failed = true;
    q('pfe').hidden = true;
    const starting = power === 'starting' || waking;
    q('pfzt').textContent = starting ? 'The RT4K is starting. Its profiles show up as soon as it answers.' :
      'The RT4K is in standby. Its profiles can only be read with it on.';
    q('pfzb').hidden = starting;
    status('');
    ['pfn', 'pfr'].forEach((id) => { q(id).disabled = true; });
  }

  const sameLoaded = (path) => loaded && path.toLowerCase() === loaded.toLowerCase(); // FAT: no case

  function render() {
    crumbs();
    ['pfn', 'pfr'].forEach((id) => { q(id).disabled = !!busy || asleep(); });
    const up = dir ? '<tr class=up><td class=n colspan=2><a href="' + hrefFor(dirOf(dir)) + '"><span class=ico>' + ICON.dir + '</span>..</a></td></tr>' : '';
    q('pfe').hidden = entries.length > 0 || failed; // a listing that failed says why instead
    const off = busy ? ' disabled' : '';
    q('pft').innerHTML = up + sd.sortEntries(entries, 'name', false).map((e) => {
      const path = join(dir, e.name);
      if (e.dir) {
        return '<tr class=d><td class=n colspan=2 title="' + esc(e.name) + '"><a href="' + hrefFor(path) + '"><span class=ico>' + ICON.dir + '</span>' +
          '<span class=nm>' + esc(e.name) + '</span></a></td></tr>';
      }
      const on = sameLoaded(path);
      const act = on ? '<span class="pill on">Loaded</span>' :
        '<button class=pfload data-p="' + esc(path) + '"' + off + ' aria-label="Load ' + esc(plain(e.name)) + '">' + (busy === path ? 'Loading…' : 'Load') + '</button>';
      return '<tr' + (on ? ' class=cur' : '') + '><td class=n title="' + esc(e.name) + '"><span class=fn><span class="ico f">' + ICON.prof + '</span>' +
        '<span class=nm>' + esc(plain(e.name)) + '</span></span></td><td class=act>' + act + '</td></tr>';
    }).join('');
  }

  // note: [text, bad] to show instead of the folder's summary (how an operation went).
  async function list(d, note) {
    const mine = ++loading;
    const moved = d !== dir;
    dir = d;
    crumbs();
    if (asleep()) {
      failed = true;
      entries = [];
      q('pft').innerHTML = '';
      showAsleep();
      if (note) status(...note);
      return;
    }
    showAsleep();
    if (!note) status('Reading the profiles…');
    try {
      const r = await fetch('/rt4k/ls?dir=' + encodeURIComponent(d ? ROOT + '/' + d : ROOT));
      const body = await r.text();
      if (mine !== loading) return;
      if (!r.ok) throw new Error(r.status === 404 ? (d ? 'There is no folder "' + d + '" in /profile.' : 'The SD card has no /profile folder.') : body.trim() || 'HTTP ' + r.status);
      const all = sd.parseList(body);
      entries = all.filter((e) => e.dir || isProfile(e.name));
      others = all.length - entries.length;
      listed = all.length;
      total = +r.headers.get('X-Total') || all.length;
      failed = false;
      status(...(note || [summary()]));
      render();
      if (moved && q('pf').getBoundingClientRect().top < 0) q('pf').scrollIntoView();
    } catch (e) {
      if (mine !== loading) return;
      failed = true;
      entries = [];
      q('pft').innerHTML = '';
      q('pfe').hidden = true;
      if (asleep() && !note) return showAsleep();
      status(note && note[1] ? note[0] : e.message === 'Failed to fetch' ? 'Cruller did not answer.' : e.message, true);
    }
  }

  // Runs a load or a save (fn resolves how it went), then reads the loaded profile and the folder again.
  async function run(what, label, fn) {
    if (busy) return;
    busy = what;
    render();
    let note;
    try {
      note = [await fn()];
    } catch (e) {
      note = [label + ' failed: ' + (e.message === 'Failed to fetch' ? 'Cruller did not answer' : e.message), true];
    } finally {
      busy = '';
    }
    await readLoaded();
    await list(dir, note);
  }

  function load(path) {
    return run(path, 'Loading ' + plain(path.split('/').pop()), async () => {
      status('Loading ' + plain(path.split('/').pop()) + '…');
      // A load reprograms the scaler, and can change the input and output resolution: it takes seconds.
      const r = await ask('prof load ' + path, 'prof', 15000);
      if (r !== 'prof load ok') throw new Error(r);
      return 'Loaded ' + plain(path.split('/').pop());
    });
  }

  // Saves the RT4K's current settings as a new profile in the folder shown.
  async function saveNew() {
    if (busy || dir === null || asleep()) return;
    const start = loaded ? plain(loaded.split('/').pop()) + ' copy' : 'New profile';
    const answer = await window.askText('Save as a new profile',
      'The RT4K\'s current settings, as it runs now, go to a new profile in ' + (dir ? '/profile/' + dir : '/profile') + '.', start, 'Save');
    const typed = answer === null ? '' : answer.trim();
    if (!typed) return;
    const name = withExt(typed, model);
    const p = sd.nameProblem(name) ||
      (sd.utf8Length(join(ROOT, join(dir, name))) > PATH_MAX ? 'The path is too long for Cruller (' + PATH_MAX + ' bytes at most).' : '') ||
      (entries.some((e) => e.dir && sd.sameName(e.name, name)) ? 'There is a folder called "' + name + '" here.' : '');
    if (p) return status(p, true);
    if (entries.some((e) => !e.dir && sd.sameName(e.name, name)) &&
      !(await window.askUser('Replace ' + plain(name) + '?', 'This folder already has a profile with that name: it will hold the RT4K\'s current settings instead.', 'Replace', true))) return;
    const path = join(dir, name);
    await run(path, 'Saving ' + plain(name), async () => {
      status('Saving ' + plain(name) + '…');
      const r = await ask('prof save ' + path, 'prof', 10000);
      if (r !== 'prof save ok') throw new Error(r);
      return 'Saved ' + plain(name);
    });
  }

  function build() {
    q('pf').innerHTML =
      '<div class=panel style="max-width:1100px">' +
      '<div id=pfl class=pfl></div>' +
      '<div class="row sdh"><nav id=pfc class="crumbs grow" aria-label="Folder"></nav>' +
      '<div class=row><button id=pfn class=primary>Save as new…</button>' +
      '<button id=pfr title="Read the folder and the loaded profile again">Refresh</button></div></div>' +
      '<div id=pfs class=small></div>' +
      '<table class="files pft" id=pftbl><colgroup><col><col class=ca></colgroup><tbody id=pft></tbody></table><div id=pfe class=empty hidden>No profiles in this folder</div>' +
      '<div id=pfz class=asleep hidden><p id=pfzt></p><button id=pfzb class=primary>Turn the RT4K on</button></div>' +
      '<div class=small>Loading a profile can change the RT4K\'s input and output resolution, as it was saved. ' +
      'Folders, renames and deletes are in the <a class=more href="#rt4k/sd/profile">SD card</a> view.</div></div>';
    q('pfr').onclick = () => { readLoaded(); list(dir || ''); };
    q('pfn').onclick = saveNew;
    q('pfzb').onclick = () => {
      if (!window.rt4kWake()) return status('Cruller did not answer.', true);
      waking = true;
      showAsleep();
      setTimeout(() => { if (waking) { waking = false; showAsleep(); } }, 10000);
    };
    q('pft').onclick = (ev) => {
      const b = ev.target.closest('button.pfload');
      if (b && !busy) load(b.dataset.p);
    };
    addEventListener('beforeunload', (ev) => { if (busy) { ev.preventDefault(); ev.returnValue = ''; } });
  }

  const showing = () => q('pf') && !q('pf').hidden && !q('pf').closest('[data-view]').hidden && document.visibilityState === 'visible';

  // The page shows the profiles view: parts is the address after #rt4k/profiles, a folder's path.
  function open(parts) {
    if (!q('pft')) build();
    if (!asleep()) readLoaded();
    list(sd.dirFromParts(parts || []));
  }

  // --- each SVS input's profile (the SVS tab) ---------------------------------------------------------
  //
  // The RT4K's own way (Auto Load SVS): when the switch tells it input n is on, it loads the first
  // /profile/SVS/S<n>_<anything>.rt4 it finds, and only from that folder. Each input gets a combo with
  // the profiles there; picking one makes it the input's file: renamed to S<n>_<name>, or copied when
  // it's another input's (each input keeps its own). The files the input had become unassigned: X_ in
  // front of their whole name (S1_SNES: X_S1_SNES, which no other file has), kept in the folder to be
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
      const to = freeName(taken, UNSET + plain(f), extOf(f));
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
    total: 0, input: 0, names: [], // the switch, as the SVS tab shows it (app.js)
    files: null,                   // the SVS folder's profiles in the card's order (null: not read; none: no folder)
    none: false,                   // the card has no /profile/SVS
    reading: false, busy: false, failed: false, waking: false,
    shape: '',                     // what the rows were built for (rebuilt when it changes)
  };

  const REFRESH_ICON = '<svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" ' +
    'stroke-linejoin="round" aria-hidden="true"><path d="M20 11a8 8 0 1 0-2.3 5.7"/><path d="M20 4v7h-7"/></svg>';

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
      const n = sv.files.length;
      svsStatus(sv.none ? 'The SD card has no /profile/SVS folder.' : n ? n + (n === 1 ? ' profile' : ' profiles') + ' in /profile/SVS' : '/profile/SVS has no profiles.');
    } catch (e) {
      sv.files = null;
      sv.failed = true;
      svsStatus(e.message === 'Failed to fetch' ? 'Cruller did not answer.' : 'Could not read /profile/SVS: ' + e.message, true);
    }
    sv.reading = false;
    sv.shape = '';
    svsRender();
  }

  // The combo's options for input n: none, its own, the unassigned ones (renamed when picked), and the
  // other inputs' (copied).
  function svsOptions(n, current) {
    const opt = (f, label) => '<option value="' + esc(f) + '"' + (f === current ? ' selected' : '') + '>' + esc(label) + '</option>';
    const group = (label, list, text) => (list.length ? '<optgroup label="' + label + '">' + list.map((f) => opt(f, text(f))).join('') + '</optgroup>' : '');
    const files = sd.sortEntries(sv.files.map((name) => ({ name, dir: false })), 'name', false).map((e) => e.name);
    return '<option value="">None</option>' + files.filter((f) => slotOf(f) === n).map((f) => opt(f, baseName(f))).join('') +
      group('Unassigned', files.filter((f) => !slotOf(f)), (f) => plain(f).replace(/^X_/i, '')) +
      group('Copy another input\'s', files.filter((f) => slotOf(f) && slotOf(f) !== n).sort((a, b) => slotOf(a) - slotOf(b)),
        (f) => baseName(f) + ' (input ' + slotOf(f) + ')');
  }

  // Input n's file: the first of its files in the card's order, the one the RT4K finds.
  const svsCurrent = (n) => {
    const mine = sv.files ? sv.files.filter((f) => slotOf(f) === n) : [];
    return { name: mine.length ? mine[0] : '', count: mine.length };
  };

  function svsRender() {
    const box = q('v-prof');
    if (!box) return;
    if (!q('vpl')) {
      box.innerHTML = '<div class=row><h2 class=grow>Profile for each input</h2>' +
        '<button id=vpr class=refresh title="Read /profile/SVS again" aria-label="Read /profile/SVS again">' + REFRESH_ICON + '</button></div>' +
        '<div id=vps class=small></div><div id=vpl class=svp></div>' +
        '<div id=vpz class=asleep hidden><p id=vpzt></p><button id=vpzb class=primary>Turn the RT4K on</button></div>' +
        '<div class=small>With Auto Load SVS on, the RT4K loads an input\'s profile from <span class=mono>/profile/SVS</span> when the switch ' +
        'changes to it: the one named S1_… for input 1, S2_… for input 2, and on. Picking an unassigned one renames it so, and another input\'s ' +
        'is copied. The one the input had becomes unassigned: X_ in front of its name, in the same folder. New profiles go there from the ' +
        '<a class=more href="#rt4k/profiles/SVS">Profiles</a> view.</div>';
      q('vpr').onclick = svsRead;
      q('vpzb').onclick = () => {
        if (!window.rt4kWake()) return svsStatus('Cruller did not answer.', true);
        sv.waking = true;
        svsRender();
        setTimeout(() => { if (sv.waking) { sv.waking = false; svsRender(); } }, 10000);
      };
      q('vpl').onchange = (ev) => {
        const sel = ev.target.closest('select[data-n]');
        if (sel) svsChoose(+sel.dataset.n, sel.value);
      };
    }
    const sleeping = asleep() && !sv.busy;
    q('vpz').hidden = !sleeping;
    q('vpl').hidden = sleeping;
    q('vpr').disabled = sv.busy || sv.reading || asleep();
    if (sleeping) {
      const starting = power === 'starting' || sv.waking;
      q('vpzt').textContent = starting ? 'The RT4K is starting. Its profiles show up as soon as it answers.' :
        'The RT4K is in standby. Its profiles can only be read with it on.';
      q('vpzb').hidden = starting;
      svsStatus('');
      return;
    }
    if (!sv.files || sv.none) { q('vpl').innerHTML = ''; sv.shape = ''; return; }
    // The rows: rebuilt when the switch or the folder changes, else only the names and the input on screen.
    const shape = sv.total + '|' + sv.files.join('/');
    if (shape !== sv.shape) {
      sv.shape = shape;
      q('vpl').innerHTML = sv.total ? Array.from({ length: sv.total }, (_, i) => {
        const n = i + 1, cur = svsCurrent(n);
        return '<div class=svp-row data-n=' + n + '><b>' + n + '</b><span class=svp-name></span>' +
          '<select data-n=' + n + ' aria-label="Profile for input ' + n + '">' + svsOptions(n, cur.name) + '</select>' +
          (cur.count > 1 ? '<span class=svp-warn>' + cur.count + ' profiles start with S' + n + '_ and the RT4K loads only one: this one. ' +
            'Pick it again to leave it alone.</span>' : '') + '</div>';
      }).join('') : '<div class=small>The inputs show up once the SVS Bridge says how many this switch has.</div>';
    }
    q('vpl').querySelectorAll('.svp-row').forEach((row) => {
      const n = +row.dataset.n;
      row.classList.toggle('on', n === sv.input);
      row.querySelector('.svp-name').textContent = (sv.names[n - 1] || 'Empty') + (n === sv.input ? ' · on screen' : '');
      row.querySelector('select').disabled = sv.busy || sv.reading;
    });
  }

  async function svsStep(s) {
    if (s.op === 'mv') {
      const r = await ask('mv ' + svsPath(s.from) + '|' + svsPath(s.to), 'mv');
      if (!r.startsWith('mv ok')) throw new Error(s.from + ': ' + r);
      return;
    }
    const g = await fetch('/rt4k/get?path=' + encodeURIComponent(svsPath(s.from)));
    if (!g.ok) throw new Error(s.from + ': ' + ((await g.text()).trim() || 'HTTP ' + g.status));
    const data = new Uint8Array(await g.arrayBuffer());
    const p = await fetch('/rt4k/put?path=' + encodeURIComponent(svsPath(s.to)) + '&sha=' + window.sha256(data), { method: 'POST', body: data });
    if (!p.ok) throw new Error(s.to + ': ' + ((await p.text()).trim() || 'HTTP ' + p.status));
  }

  const stepText = (s) => (s.op === 'mv' ? 'renaming ' + plain(s.from) + ' to ' + plain(s.to) : 'copying ' + plain(s.from) + ' to ' + plain(s.to));

  async function svsChoose(n, name) {
    if (sv.busy || !sv.files) return;
    const p = plan(sv.files, n, name);
    if (!p.steps.length) { sv.shape = ''; return svsRender(); }
    const long = p.steps.find((s) => sd.utf8Length(svsPath(s.to)) > PATH_MAX);
    if (long) {
      svsStatus(long.to + ': the path is too long for Cruller (' + PATH_MAX + ' bytes at most).', true);
      sv.shape = '';
      return svsRender();
    }
    sv.busy = true;
    svsRender();
    let note;
    try {
      for (const s of p.steps) {
        svsStatus('Input ' + n + ': ' + stepText(s) + '…');
        await svsStep(s);
      }
      const away = p.steps.filter((s) => s.op === 'mv' && slotOf(s.from) === n).map((s) => plain(s.to));
      const said = [p.target ? 'Input ' + n + ': ' + baseName(p.target) : 'Input ' + n + ' has no profile now'];
      if (away.length) said.push('The one it had is unassigned now: ' + away.join(', '));
      // The input on screen: loaded now, as the RT4K would on switching to it.
      if (p.target && n === sv.input && power === 'on') {
        svsStatus('Input ' + n + ': loading ' + baseName(p.target) + '…');
        const r = await ask('prof load ' + SVS_DIR + '/' + p.target, 'prof', 15000);
        said.push(r === 'prof load ok' ? 'Loaded, as it\'s on screen' : 'Loading it failed: ' + r);
      }
      note = [said.join('. ') + '.'];
    } catch (e) {
      note = ['Input ' + n + ': ' + (e.message === 'Failed to fetch' ? 'Cruller did not answer' : e.message), true];
    }
    try {
      await svsList();
    } catch (e) { // what's in the folder now isn't known: nothing to pick from until it's read again
      sv.files = null;
      sv.failed = true;
      note = [note[0] + ' Could not read /profile/SVS again: use the refresh button.', true];
    }
    sv.busy = false;
    sv.shape = '';
    svsRender();
    svsStatus(...note);
  }

  // The SVS tab's switch (app.js, with every status): how many inputs, which is on screen, their names.
  function svsSwitch(info) {
    sv.total = info.total;
    sv.input = info.input;
    sv.names = info.names;
    svsRender();
    if (!sv.files && !sv.reading && !sv.failed && power === 'on' && sv.total && svsShowing()) svsRead();
  }

  // The SVS tab opens: the folder is read again (the Profiles and SD card views may have changed it).
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

  // Every status: the RT4K going to sleep or waking up (both read again once it's on), and while the
  // view shows, the loaded profile now and then (its own remote changes it too).
  function onStatus(s) {
    const was = power;
    power = s.rt4k_power;
    model = s.rt4k_model || model;
    if (power !== 'standby') waking = false;
    svsPower(was);
    if (!q('pft')) return;
    if (was !== power) {
      if (asleep()) loaded = null;
      if (!busy && power === 'on' && showing()) { readLoaded(); if (failed && dir !== null) list(dir); }
      else if (!busy) { showAsleep(); if (!asleep()) render(); }
      return;
    }
    if (!busy && power === 'on' && showing() && Date.now() - loadedAt >= READ_EVERY) readLoaded();
  }

  window.profOpen = open;
  window.profStatus = onStatus;
  window.profSvsSwitch = svsSwitch;
  window.profSvsOpen = svsOpen;
  window.profInternals = { parseLoaded, isProfile, extFor, withExt, plain, hrefFor, dirOf, slotOf, baseName, freeName, plan }; // tests/test_profiles.js
})();
