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
    const up = dir ? '<tr class=up><td class=n><a href="' + hrefFor(dirOf(dir)) + '"><span class=ico>' + ICON.dir + '</span>..</a></td><td></td></tr>' : '';
    q('pfe').hidden = entries.length > 0 || failed; // a listing that failed says why instead
    const off = busy ? ' disabled' : '';
    q('pft').innerHTML = up + sd.sortEntries(entries, 'name', false).map((e) => {
      const path = join(dir, e.name);
      if (e.dir) {
        return '<tr class=d><td class=n title="' + esc(e.name) + '"><a href="' + hrefFor(path) + '"><span class=ico>' + ICON.dir + '</span>' +
          '<span class=nm>' + esc(e.name) + '</span></a></td><td></td></tr>';
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
  // /profile/SVS/S<n>_<anything>.rt4 it finds. Each input gets a combo with every profile on the card:
  // picking one makes it that file. One already in the folder without an S<k>_ is renamed, any other
  // is copied in, and the files the input had lose their S<n>_ (they stay in the folder, out of the
  // way, to be picked again).

  const SVS_DIR = 'SVS';  // the folder the RT4K looks in, when the card has none yet
  const SCAN_FOLDERS = 64; // folders read at most, SCAN_DEPTH deep under /profile
  const SCAN_DEPTH = 3;

  // The input a file in the SVS folder is for: n for "S<n>_<anything>.rt4" (0: none).
  function slotOf(name) {
    const m = /^S(\d+)_/i.exec(name);
    return m && isProfile(name) ? +m[1] : 0;
  }

  // A profile's name without its S<n>_ and extension ("S3_PS1 480i.rt4": "PS1 480i").
  const baseName = (name) => plain(name).replace(/^S\d+_/i, '');
  const extOf = (name) => (/\.rt[46]$/i.exec(name) || ['.rt4'])[0];

  // A name that isn't in taken (lower case: FAT): base + ext, else "base (2)" + ext, and on.
  function freeName(taken, base, ext) {
    for (let i = 1; ; i++) {
      const name = (i === 1 ? base : base + ' (' + i + ')') + ext;
      if (!taken.has(name.toLowerCase())) return name;
    }
  }

  // What makes choice (a profile's path under /profile; '' for none) input n's profile. svs: the SVS
  // folder's name as on the card ('' when it has none) and its profiles in the card's order. Returns
  // the steps in order ({op: 'mkdir', path} | {op: 'mv' | 'cp', from, to}, paths under /profile) and
  // where the profile ends up ('' for none). The input's other files lose their S<n>_ first, so a
  // failure halfway loses nothing; a choice that already is the input's file only drops the others.
  function plan(svs, n, choice) {
    const dir = svs.dir || SVS_DIR;
    const mine = svs.files.filter((f) => slotOf(f) === n);
    const inFolder = choice && svs.dir && sd.sameName(dirOf(choice), svs.dir) ? choice.split('/').pop() : '';
    const keep = inFolder ? mine.find((f) => sd.sameName(f, inFolder)) : undefined;
    const steps = [];
    const taken = new Set(svs.files.map((f) => f.toLowerCase()));
    for (const f of mine) {
      if (f === keep) continue;
      const to = freeName(taken, baseName(f), extOf(f));
      taken.delete(f.toLowerCase());
      taken.add(to.toLowerCase());
      steps.push({ op: 'mv', from: dir + '/' + f, to: dir + '/' + to });
    }
    if (!choice) return { steps, target: '' };
    if (keep) return { steps, target: dir + '/' + keep };
    // Free: every file named S<n>_... was the input's, and was renamed above.
    const name = 'S' + n + '_' + baseName(choice.split('/').pop()) + extOf(choice);
    if (!svs.dir) steps.unshift({ op: 'mkdir', path: dir });
    if (inFolder && !slotOf(inFolder)) steps.push({ op: 'mv', from: dir + '/' + inFolder, to: dir + '/' + name });
    else steps.push({ op: 'cp', from: choice, to: dir + '/' + name });
    return { steps, target: dir + '/' + name };
  }

  const sv = {
    total: 0, input: 0, names: [], // the switch, as the SVS tab shows it (app.js)
    tree: null,                    // every profile on the card: [{path, name, dir}] (null: not read)
    svs: null,                     // the SVS folder: {dir: its name ('' none), files: its profiles in the card's order}
    readAt: 0,
    reading: false, busy: false, failed: false, waking: false,
    shape: '',                     // what the rows were built for (rebuilt when it changes)
  };
  const SVS_FRESH = 30000; // ms: opening the SVS tab reads the profiles again after this (other views change them)

  const REFRESH_ICON = '<svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" ' +
    'stroke-linejoin="round" aria-hidden="true"><path d="M20 11a8 8 0 1 0-2.3 5.7"/><path d="M20 4v7h-7"/></svg>';

  const svsShowing = () => q('v-prof') && !q('v-prof').hidden && !q('v-prof').closest('[data-view]').hidden;

  function svsStatus(text, bad) {
    const s = q('vps');
    if (!s) return;
    s.textContent = text;
    s.classList.toggle('bad', !!bad);
  }

  async function lsProfiles(d) {
    const r = await fetch('/rt4k/ls?dir=' + encodeURIComponent(d ? ROOT + '/' + d : ROOT));
    const body = await r.text();
    if (r.status === 404) return null;
    if (!r.ok) throw new Error(body.trim() || 'HTTP ' + r.status);
    return sd.parseList(body).filter((e) => !e.name.startsWith('.')); // macOS leaves ._ files
  }

  // Reads every folder under /profile (SCAN_DEPTH deep, SCAN_FOLDERS at most), one listing each.
  async function svsRead() {
    if (sv.reading || asleep()) return;
    sv.reading = true;
    sv.failed = false;
    svsStatus('Reading the profiles on the SD card…');
    svsRender();
    const tree = [];
    let svs = { dir: '', files: [] };
    const folders = [''];
    try {
      for (let i = 0; i < folders.length && i < SCAN_FOLDERS; i++) {
        const d = folders[i];
        const list = await lsProfiles(d);
        if (!list) {
          if (!d) break; // no /profile at all
          continue;      // gone meanwhile
        }
        for (const e of list) {
          if (e.dir) { if (d.split('/').length < SCAN_DEPTH) folders.push(join(d, e.name)); }
          else if (isProfile(e.name)) tree.push({ path: join(d, e.name), name: e.name, dir: d });
        }
        if (d && !d.includes('/') && sd.sameName(d, SVS_DIR)) svs = { dir: d, files: list.filter((e) => !e.dir && isProfile(e.name)).map((e) => e.name) };
      }
      sv.tree = tree;
      sv.svs = svs;
      sv.readAt = Date.now();
      svsStatus(tree.length ? tree.length + (tree.length === 1 ? ' profile' : ' profiles') + ' on the SD card' +
        (folders.length > SCAN_FOLDERS ? ' (the first ' + SCAN_FOLDERS + ' folders)' : '') : 'The SD card has no profiles under /profile yet.');
    } catch (e) {
      sv.failed = true;
      svsStatus(e.message === 'Failed to fetch' ? 'Cruller did not answer.' : 'Could not read the profiles: ' + e.message, true);
    }
    sv.reading = false;
    sv.shape = '';
    svsRender();
  }

  // After a change: the SVS folder again (the rest of the card is as it was).
  async function svsReread() {
    const d = sv.svs.dir || SVS_DIR;
    const list = await lsProfiles(d);
    const files = list ? list.filter((e) => !e.dir && isProfile(e.name)) : [];
    sv.svs = { dir: list ? d : '', files: files.map((e) => e.name) };
    sv.tree = sv.tree.filter((p) => !sd.sameName(p.dir, d)).concat(files.map((e) => ({ path: join(d, e.name), name: e.name, dir: d })));
    sv.readAt = Date.now();
  }

  // The combo's options for input n: none, then the profiles by folder (the SVS folder first, its files
  // named for their input; then /profile itself; then the rest by name).
  function svsOptions(n, current) {
    const groups = new Map();
    for (const p of sv.tree) {
      if (!groups.has(p.dir)) groups.set(p.dir, []);
      groups.get(p.dir).push(p);
    }
    const isSvs = (d) => !!sv.svs.dir && sd.sameName(d, sv.svs.dir);
    const order = [...groups.keys()].sort((a, b) => (isSvs(b) - isSvs(a)) || ((a !== '') - (b !== '')) ||
      a.localeCompare(b, undefined, { numeric: true, sensitivity: 'base' }));
    const label = (p) => {
      if (!isSvs(p.dir)) return plain(p.name);
      const k = slotOf(p.name);
      return k === n ? baseName(p.name) : k ? baseName(p.name) + ' (input ' + k + ')' : plain(p.name);
    };
    return '<option value="">None</option>' + order.map((d) => '<optgroup label="' + esc(d ? d : '/profile') + '">' +
      sd.sortEntries(groups.get(d), 'name', false).map((p) => '<option value="' + esc(p.path) + '"' + (p.path === current ? ' selected' : '') + '>' +
        esc(label(p)) + '</option>').join('') + '</optgroup>').join('');
  }

  // Input n's file: the first of its files in the card's order, the one the RT4K finds.
  const svsCurrent = (n) => {
    const mine = sv.svs ? sv.svs.files.filter((f) => slotOf(f) === n) : [];
    return { path: mine.length ? sv.svs.dir + '/' + mine[0] : '', count: mine.length };
  };

  function svsRender() {
    const box = q('v-prof');
    if (!box) return;
    if (!q('vpl')) {
      box.innerHTML = '<div class=row><h2 class=grow>Profile for each input</h2>' +
        '<button id=vpr class=refresh title="Read the profiles again" aria-label="Read the profiles again">' + REFRESH_ICON + '</button></div>' +
        '<div id=vps class=small></div><div id=vpl class=svp></div>' +
        '<div id=vpz class=asleep hidden><p id=vpzt></p><button id=vpzb class=primary>Turn the RT4K on</button></div>' +
        '<div class=small>The RT4K keeps each input\'s profile in <span class=mono>/profile/SVS</span>, named S1_…, S2_… With Auto Load SVS on, ' +
        'it loads it when the switch changes to that input. Picking another one copies it in (or renames it, when it\'s in that folder already); ' +
        'the one the input had stays in the folder without its S1_.</div>';
      q('vpr').onclick = () => { if (!sv.busy) svsRead(); };
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
    if (!sv.tree) { q('vpl').innerHTML = ''; sv.shape = ''; return; }
    // The rows: rebuilt when the switch or the profiles change, else only the names and the input on screen.
    const shape = sv.total + '|' + JSON.stringify(sv.svs) + '|' + sv.tree.length;
    if (shape !== sv.shape) {
      sv.shape = shape;
      q('vpl').innerHTML = sv.total ? Array.from({ length: sv.total }, (_, i) => {
        const n = i + 1, cur = svsCurrent(n);
        return '<div class=svp-row data-n=' + n + '><b>' + n + '</b><span class=svp-name></span>' +
          '<select data-n=' + n + ' aria-label="Profile for input ' + n + '">' + svsOptions(n, cur.path) + '</select>' +
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
    if (s.op === 'mkdir') {
      const r = await ask('mkdir ' + ROOT + '/' + s.path, 'mkdir');
      if (!r.startsWith('mkdir ok')) throw new Error('/profile/' + s.path + ': ' + r);
    } else if (s.op === 'mv') {
      const r = await ask('mv ' + ROOT + '/' + s.from + '|' + ROOT + '/' + s.to, 'mv');
      if (!r.startsWith('mv ok')) throw new Error(s.from.split('/').pop() + ': ' + r);
    } else {
      const g = await fetch('/rt4k/get?path=' + encodeURIComponent(ROOT + '/' + s.from));
      if (!g.ok) throw new Error(s.from + ': ' + ((await g.text()).trim() || 'HTTP ' + g.status));
      const data = new Uint8Array(await g.arrayBuffer());
      const p = await fetch('/rt4k/put?path=' + encodeURIComponent(ROOT + '/' + s.to) + '&sha=' + window.sha256(data), { method: 'POST', body: data });
      if (!p.ok) throw new Error(s.to.split('/').pop() + ': ' + ((await p.text()).trim() || 'HTTP ' + p.status));
    }
  }

  const stepText = (s) => (s.op === 'mkdir' ? 'making /profile/' + s.path :
    s.op === 'mv' ? 'renaming ' + plain(s.from.split('/').pop()) + ' to ' + plain(s.to.split('/').pop()) : 'copying ' + plain(s.from.split('/').pop()) + ' in');

  async function svsChoose(n, choice) {
    if (sv.busy || !sv.svs) return;
    const p = plan(sv.svs, n, choice);
    if (!p.steps.length) return svsRender();
    const long = p.steps.find((s) => (s.to || s.path) && sd.utf8Length(ROOT + '/' + (s.to || s.path)) > PATH_MAX);
    if (long) {
      svsStatus((long.to || long.path).split('/').pop() + ': the path is too long for Cruller (' + PATH_MAX + ' bytes at most).', true);
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
      const away = p.steps.filter((s) => s.op === 'mv' && slotOf(s.from.split('/').pop()) === n).map((s) => plain(s.to.split('/').pop()));
      const said = [p.target ? 'Input ' + n + ': ' + baseName(p.target.split('/').pop()) : 'Input ' + n + ' has no profile now'];
      if (away.length) said.push('The one it had stays in the folder as ' + away.join(', '));
      // The input on screen: loaded now, as the RT4K would on switching to it.
      if (p.target && n === sv.input && power === 'on') {
        svsStatus('Input ' + n + ': loading ' + baseName(p.target.split('/').pop()) + '…');
        const r = await ask('prof load ' + p.target, 'prof', 15000);
        said.push(r === 'prof load ok' ? 'Loaded, as it\'s on screen' : 'Loading it failed: ' + r);
      }
      note = [said.join('. ') + '.'];
    } catch (e) {
      note = ['Input ' + n + ': ' + (e.message === 'Failed to fetch' ? 'Cruller did not answer' : e.message), true];
    }
    try {
      await svsReread();
    } catch (e) { // what's on the card now isn't known: nothing to pick from until it's read again
      sv.tree = null;
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
    if (!sv.tree && !sv.reading && !sv.failed && power === 'on' && sv.total && svsShowing()) svsRead();
  }

  // The SVS tab opens: the profiles are read then (and again with its refresh button).
  function svsOpen() {
    svsRender();
    if (!sv.reading && !sv.busy && power === 'on' && svsShowing() && (!sv.tree || Date.now() - sv.readAt > SVS_FRESH)) svsRead();
  }

  // The RT4K asleep or waking up: what was read may not hold once it's back (its card can change).
  function svsPower(was) {
    if (power !== 'standby') sv.waking = false;
    if (was === power) return;
    if (asleep()) { sv.tree = null; sv.failed = false; }
    svsRender();
    if (power === 'on' && !sv.reading && !sv.busy && sv.total && svsShowing()) svsRead();
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
