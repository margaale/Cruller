// The RT4K's SD card for the Cruller page (served as /sd.js, embedded at build time).
//
// Folders come from GET /rt4k/ls (the RT4K's "ls", one entry a line) and files from GET /rt4k/get
// (RTL1 get, in verified pieces), which the browser downloads on its own. Uploads go to POST
// /rt4k/put with their SHA-256 (sha256.js), which the RT4K checks; new folders, renames and deletes
// are the RT4K's own mkdir, mv and rm (POST /rt4k/ask). A profile in /profile can be loaded (its "prof
// load") or opened in the editor (#rt4k/editor/<path>), or several ticked and edited together; the one
// loaded (Cruller's status says it) is marked. Each folder has its own address (#rt4k/sd/<path>), so the browser's back button walks back up.

(() => {
  'use strict';

  const q = (id) => document.getElementById(id);

  const PATH_MAX = 160; // bytes, as Cruller takes them (rtl1.h's RTL1_PATH_MAX)

  // --- pure helpers (tests/test_sd.js) ----------------------------------------------------------------

  // GET /rt4k/ls's body: "D|F\tsize\tunix time\tname" lines.
  function parseList(body) {
    const out = [];
    for (const line of body.split('\n')) {
      const f = line.split('\t');
      if (f.length < 4 || (f[0] !== 'D' && f[0] !== 'F')) continue;
      out.push({ dir: f[0] === 'D', size: +f[1], mtime: +f[2], name: f.slice(3).join('\t') });
    }
    return out;
  }

  // Folders first, then by key (name, size, mtime), ties by name A to Z; names compare like a file
  // manager does ("Disc 2" before "Disc 10", case aside).
  function sortEntries(list, key, desc) {
    const byName = (a, b) => a.name.localeCompare(b.name, undefined, { numeric: true, sensitivity: 'base' });
    return [...list].sort((a, b) => {
      if (a.dir !== b.dir) return a.dir ? -1 : 1;
      if (key !== 'name' && a[key] !== b[key]) return desc ? b[key] - a[key] : a[key] - b[key];
      return key === 'name' && desc ? byName(b, a) : byName(a, b);
    });
  }

  const join = (dir, name) => (dir ? dir + '/' + name : name);

  const getUrl = (path) => '/rt4k/get?path=' + encodeURIComponent(path);

  // A folder's page address, and back: each part escaped on its own, so names with '/', '%', '#'
  // or spaces survive.
  const hrefFor = (dir) => '#rt4k/sd' + (dir ? '/' + dir.split('/').map(encodeURIComponent).join('/') : '');

  function dirFromParts(parts) {
    return parts.filter((p) => p).map((p) => {
      try { return decodeURIComponent(p); } catch (e) { return p; }
    }).join('/');
  }

  // What's wrong with a new file or folder name ('' if nothing): what FAT refuses, and ".." (Cruller
  // refuses it anywhere in a path).
  function nameProblem(name) {
    if (!name) return 'The name is empty.';
    if (/[\x00-\x1f\x7f/\\:*?"<>|]/.test(name)) return 'Names can\'t have / \\ : * ? " < > |.';
    if (name.includes('..')) return 'Names can\'t have "..".';
    if (/[ .]$/.test(name)) return 'Names can\'t end with a space or a dot.';
    return '';
  }

  const utf8Length = (s) => new TextEncoder().encode(s).length;

  // FAT tells names apart without case.
  const sameName = (a, b) => a.toLowerCase() === b.toLowerCase();

  function size(n) {
    if (n >= 1048576) return (n / 1048576).toFixed(1) + ' MB';
    if (n >= 1024) return (n / 1024).toFixed(n >= 10240 ? 0 : 1) + ' KB';
    return Math.round(n) + ' B'; // rates too
  }

  // FAT keeps local time without a zone and the RT4K hands it over as if UTC: show it as stored.
  function when(t) {
    if (!t) return '';
    return new Date(t * 1000).toISOString().slice(0, 16).replace('T', ' ');
  }

  // --- state ------------------------------------------------------------------------------------------

  let dir = null;          // the folder shown ('' = root; null: nothing yet)
  let loaded = '';         // the profile the RT4K has loaded, its path under /profile ('' none)
  let entries = [];
  let shown = [];          // entries as the table shows them (row buttons point into it)
  let picked = new Set();  // the profiles of this folder ticked to edit together, by name
  let total = 0;           // the RT4K's own count (more than entries when the listing didn't fit)
  let sortKey = 'name', sortDesc = false;
  let loading = 0;         // the listing request that counts (later ones win)
  let failed = false;      // the last listing failed (retried when the RT4K comes on)
  let power = '';
  let waking = false;      // "Turn the RT4K on" was clicked; waiting for it to start

  // Asleep or waking up: the RT4K answers nothing but "pwr on" then, so no listing is tried.
  const asleep = () => power === 'standby' || power === 'starting';
  let busy = false;        // an upload, new folder, rename or delete runs (one at a time)
  let onPut = null;        // the upload's progress, from each status's "put"

  const esc = (s) => String(s).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));

  const svg = (body, fill) => '<svg width="18" height="18" viewBox="0 0 24 24" ' + (fill ? 'fill="currentColor"' :
    'fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"') + ' aria-hidden="true">' + body + '</svg>';
  const ICON = {
    dir: svg('<path d="M3 6.5A1.5 1.5 0 0 1 4.5 5h4.6l2 2.2h8.4A1.5 1.5 0 0 1 21 8.7v9.8a1.5 1.5 0 0 1-1.5 1.5h-15A1.5 1.5 0 0 1 3 18.5z"/>', true),
    file: svg('<path d="M6 3h8l4 4v14H6z"/><path d="M14 3v4h4"/>'),
    down: svg('<path d="M12 4v11"/><path d="M7 10l5 5 5-5"/><path d="M5 20h14"/>'),
    ren: svg('<path d="M4 20h4L19 9l-4-4L4 16z"/><path d="M13 7l4 4"/>'),
    del: svg('<path d="M4 7h16"/><path d="M9 7V4h6v3"/><path d="M6 7l1 13h10l1-13"/>'),
    load: svg('<path d="M7 4.5v15l12-7.5z"/>'),
    edit: svg('<path d="M4 6h10"/><path d="M18 6h2"/><circle cx="16" cy="6" r="2"/><path d="M4 12h2"/><path d="M10 12h10"/><circle cx="8" cy="12" r="2"/><path d="M4 18h12"/><circle cx="18" cy="18" r="2"/>'),
  };

  // A profile the RT4K can load: a .rt4 or .rt6 under /profile. Its path from there ("SVS/S1_SNES.rt4"),
  // else null.
  const PROFILE_DIR = 'profile';
  function profilePath(dir, name) {
    const d = (dir || '').toLowerCase(); // (FAT: no case)
    if (!/\.rt[46]$/i.test(name) || !(d === PROFILE_DIR || d.startsWith(PROFILE_DIR + '/'))) return null;
    return join(dir.slice(PROFILE_DIR.length + 1), name);
  }

  // --- Cruller ----------------------------------------------------------------------------------------

  async function ask(cmd, expect, timeout) {
    const r = await fetch('/rt4k/ask?expect=' + encodeURIComponent(expect) + (timeout ? '&timeout=' + timeout : ''), { method: 'POST', body: cmd });
    const t = (await r.text()).trim();
    if (!r.ok) throw new Error(r.status === 504 ? 'the RT4K did not answer' : t);
    return t;
  }

  async function putFile(path, data, sha) {
    const r = await fetch('/rt4k/put?path=' + encodeURIComponent(path) + '&sha=' + sha, { method: 'POST', body: data });
    if (!r.ok) throw new Error((await r.text()).trim() || 'HTTP ' + r.status);
  }

  async function listing(path) {
    const r = await fetch('/rt4k/ls' + (path ? '?dir=' + encodeURIComponent(path) : ''));
    const body = await r.text();
    if (!r.ok) throw new Error(r.status === 404 ? 'There is no folder "' + path + '" on the SD card.' : body.trim() || 'HTTP ' + r.status);
    const list = parseList(body);
    return { list, total: +r.headers.get('X-Total') || list.length };
  }

  // Deletes a file, or a folder with everything in it (the RT4K's rm only takes empty ones).
  async function removeTree(path, isDir, onItem) {
    // A listing too long for Cruller has more than came (X-Total): list again until it's empty.
    for (let more = isDir; more;) {
      const { list, total: all } = await listing(path);
      for (const e of list) await removeTree(join(path, e.name), e.dir, onItem);
      more = list.length > 0 && all > list.length;
    }
    onItem(path);
    const r = await ask('rm ' + path, 'rm');
    if (!r.startsWith('rm ok')) throw new Error(path + ': ' + r);
  }

  // --- UI ---------------------------------------------------------------------------------------------

  function status(text, bad) {
    const s = q('sds');
    s.textContent = text;
    s.classList.toggle('bad', !!bad);
  }

  function summary() {
    const dirs = entries.filter((e) => e.dir).length, files = entries.length - dirs;
    const bytes = entries.reduce((s, e) => s + (e.dir ? 0 : e.size), 0);
    const parts = [];
    if (dirs) parts.push(dirs + (dirs === 1 ? ' folder' : ' folders'));
    if (files) parts.push(files + (files === 1 ? ' file' : ' files') + ' · ' + size(bytes));
    let t = parts.join(', ');
    if (total > entries.length) t += ' · showing ' + entries.length + ' of ' + total + ' (the listing is too long for Cruller)';
    return t;
  }

  function progress(text, value, max) {
    q('sdp').hidden = text === null;
    if (text === null) return;
    q('sdpt').textContent = text;
    const p = q('sdpb');
    if (max) { p.max = max; p.value = value; } else p.removeAttribute('value');
  }

  function crumbs() {
    const parts = dir ? dir.split('/') : [];
    let html = '<h2>' + (parts.length ? '<a href="' + hrefFor('') + '">SD card</a>' : 'SD card') + '</h2>';
    parts.forEach((p, i) => {
      html += '<span>/</span>';
      html += i === parts.length - 1 ? '<b>' + esc(p) + '</b>' : '<a href="' + hrefFor(parts.slice(0, i + 1).join('/')) + '">' + esc(p) + '</a>';
    });
    q('sdc').innerHTML = html;
  }

  // The RT4K asleep: in place of the folder, why there's none and a way to turn it on.
  function showAsleep() {
    const on = !asleep() || busy;
    q('sdz').hidden = on;
    q('sdtbl').hidden = !on;
    if (on) return;
    failed = true; // what it showed may not hold once it's back: read it again then
    q('sde').hidden = true;
    const starting = power === 'starting' || waking;
    q('sdzt').textContent = starting ? 'The RT4K is starting. The folder shows up as soon as it answers.' :
      'The RT4K is in standby. Its SD card can only be read with it on.';
    q('sdzb').hidden = starting;
    status('');
    ['sdu', 'sdn', 'sdr'].forEach((id) => { q(id).disabled = true; });
  }

  function render() {
    crumbs();
    // the profiles ticked: a column of ticks in a profiles' folder, every one here at its head; with any ticked,
    // a bar to edit them together
    const can = profilePath(dir, 'x.rt4') !== null, profs = entries.filter((e) => !e.dir && profilePath(dir, e.name));
    picked = new Set([...picked].filter((n) => profs.some((e) => e.name === n)));
    q('sdtbl').classList.toggle('pk', can);
    q('sdpa').checked = profs.length > 0 && picked.size === profs.length;
    q('sdpa').indeterminate = picked.size > 0 && picked.size < profs.length;
    q('sdpa').disabled = !profs.length;
    q('sdsel').hidden = !picked.size;
    q('sdseln').textContent = picked.size === 1 ? '1 profile ticked' : picked.size + ' profiles ticked';
    q('sdsele').textContent = picked.size === 1 ? 'Edit it' : 'Edit them together';
    document.querySelectorAll('#sd th[data-k]').forEach((th) => {
      th.setAttribute('aria-sort', th.dataset.k === sortKey ? (sortDesc ? 'descending' : 'ascending') : 'none');
    });
    ['sdu', 'sdn', 'sdr'].forEach((id) => { q(id).disabled = busy || asleep(); });
    // No colspan: with the date column hidden (narrow screens) it would make a fourth, empty one.
    const up = dir ? '<tr class=up><td class=ck></td><td class=n><a href="' + hrefFor(dir.split('/').slice(0, -1).join('/')) + '">' +
      '<span class=ico>' + ICON.dir + '</span>..</a></td><td class=sz></td><td class=when></td><td></td></tr>' : '';
    q('sde').hidden = entries.length > 0;
    shown = sortEntries(entries, sortKey, sortDesc);
    const off = busy ? ' disabled' : '';
    q('sdt').innerHTML = up + shown.map((e, i) => {
      const name = e.dir
        ? '<a href="' + hrefFor(join(dir, e.name)) + '"><span class=ico>' + ICON.dir + '</span><span class=nm>' + esc(e.name) + '</span></a>'
        : '<span class=fn><span class="ico f">' + ICON.file + '</span><span class=nm>' + esc(e.name) + '<small class=msz>' + size(e.size) + '</small></span></span>';
      // Downloads are plain links: the browser saves the file as it comes, in its own downloads
      // list, with the size from Content-Length (the name from Content-Disposition, and download= here).
      const pp = e.dir ? null : profilePath(dir, e.name), on = pp && loaded && sameName(pp, loaded);
      const act = (pp ? '<button class=ib data-a=load data-i=' + i + off + ' title="Load it on the RT4K" aria-label="Load ' + esc(e.name) + '">' + ICON.load + '</button>' +
        '<a class="btn ib" href="#rt4k/editor/' + pp.split('/').map(encodeURIComponent).join('/') + '" title="Edit" aria-label="Edit ' + esc(e.name) + '">' + ICON.edit + '</a>' : '') +
        (e.dir ? '' : '<a class="btn ib" href="' + getUrl(join(dir, e.name)) + '" download="' + esc(e.name) + '" title="Download" ' +
        'aria-label="Download ' + esc(e.name) + '">' + ICON.down + '</a>') +
        '<button class=ib data-a=ren data-i=' + i + off + ' title="Rename" aria-label="Rename ' + esc(e.name) + '">' + ICON.ren + '</button>' +
        '<button class="ib del" data-a=del data-i=' + i + off + ' title="Delete" aria-label="Delete ' + esc(e.name) + '">' + ICON.del + '</button>';
      const tick = pp ? '<input type=checkbox data-pick="' + esc(e.name) + '"' + (picked.has(e.name) ? ' checked' : '') + ' aria-label="Tick ' + esc(e.name) + '">' : '';
      return '<tr' + (e.dir ? ' class=d' : on ? ' class=cur title="Loaded now"' : '') + '><td class=ck>' + tick + '</td><td class=n title="' + esc(e.name) + '">' + name + '</td>' +
        '<td class="num sz">' + (e.dir ? '' : size(e.size)) + '</td><td class="num when">' + when(e.mtime) + '</td><td class=act>' + act + '</td></tr>';
    }).join('');
  }

  // note: [text, bad] to show instead of the folder's summary (how an operation went).
  async function list(d, note) {
    const mine = ++loading;
    const moved = d !== dir;
    if (moved) picked = new Set();
    dir = d;
    crumbs();
    if (asleep()) { // read once it's on (onStatus)
      failed = true;
      entries = [];
      total = 0;
      q('sdt').innerHTML = '';
      showAsleep();
      if (note) status(...note);
      return;
    }
    showAsleep();
    if (!note) status('Reading the SD card…');
    try {
      const got = await listing(d);
      if (mine !== loading) return;
      entries = got.list;
      total = got.total;
      failed = false;
      status(...(note || [summary()]));
      render();
      if (moved && q('sd').getBoundingClientRect().top < 0) q('sd').scrollIntoView(); // another folder: from its top
    } catch (e) {
      if (mine !== loading) return;
      failed = true;
      entries = [];
      total = 0;
      q('sdt').innerHTML = '';
      q('sde').hidden = true;
      if (asleep() && !note) return showAsleep(); // it went to sleep meanwhile: that says it all
      // An operation's failure says more than the listing's.
      status(note && note[1] ? note[0] : e.message === 'Failed to fetch' ? 'Cruller did not answer.' : e.message, true);
    }
  }

  // Runs one operation (fn resolves how it went), then reads the folder again.
  async function run(what, fn) {
    if (busy) return;
    busy = true;
    render();
    let note;
    try {
      note = [await fn()];
    } catch (e) {
      note = [what + ' failed: ' + (e.message === 'Failed to fetch' ? 'Cruller did not answer' : e.message), true];
    } finally {
      onPut = null;
      busy = false;
      progress(null);
    }
    await list(dir, note);
  }

  // A new name for something in this folder: its problem, or ''.
  function problemHere(name, except) {
    const p = nameProblem(name);
    if (p) return p;
    if (utf8Length(join(dir, name)) > PATH_MAX) return 'The path is too long for Cruller (' + PATH_MAX + ' bytes at most).';
    if (entries.some((e) => e !== except && sameName(e.name, name))) return 'There is already something called "' + name + '" here.';
    return '';
  }

  async function upload(fileList) {
    const files = [...fileList];
    if (!files.length || busy || dir === null || asleep()) return;
    for (const f of files) {
      const clash = entries.find((e) => sameName(e.name, f.name));
      const p = problemHere(f.name, clash && !clash.dir ? clash : undefined); // a file it replaces is fine
      if (p) return status(f.name + ': ' + p, true);
    }
    const replaced = files.filter((f) => entries.some((e) => sameName(e.name, f.name)));
    if (replaced.length && !(await window.askUser(replaced.length === 1 ? 'Replace ' + replaced[0].name + '?' : 'Replace ' + replaced.length + ' files?',
      'This folder already has ' + (replaced.length === 1 ? 'a file with that name.' : 'files with those names:\n' + replaced.map((f) => f.name).join('\n')),
      'Replace', true))) return;
    const to = dir;
    await run('Upload', async () => {
      const all = files.reduce((s, f) => s + f.size, 0);
      const t0 = Date.now();
      let done = 0;
      for (const [i, f] of files.entries()) {
        const path = join(to, f.name);
        const label = (files.length > 1 ? (i + 1) + ' of ' + files.length + ' · ' : '') + f.name;
        const show = (sent) => {
          const now = done + Math.min(sent, f.size);
          const rate = now / Math.max(0.001, (Date.now() - t0) / 1000);
          progress(label + ' · ' + size(now) + ' of ' + size(all) + (now ? ' · ' + size(rate) + '/s' : ''), now, all);
        };
        progress(label + ' · reading…', done, all);
        const data = new Uint8Array(await f.arrayBuffer());
        await new Promise((r) => setTimeout(r, 0)); // let that show before hashing
        const sha = window.sha256(data);
        show(0);
        onPut = (put) => { if (put.path === path) show(put.sent); };
        await putFile(path, data, sha);
        onPut = null;
        done += f.size;
      }
      return 'Uploaded ' + (files.length === 1 ? files[0].name : files.length + ' files') + ' · ' + size(all) + ' in ' +
        ((Date.now() - t0) / 1000).toFixed(1) + ' s';
    });
  }

  async function newFolder() {
    if (busy || dir === null || asleep()) return;
    const answer = await window.askText('New folder', dir ? 'In ' + dir : 'In the root of the SD card', '', 'Create');
    const name = answer === null ? '' : answer.trim();
    if (!name) return;
    const p = problemHere(name);
    if (p) return status(p, true);
    await run('New folder', async () => {
      const r = await ask('mkdir ' + join(dir, name), 'mkdir');
      if (!r.startsWith('mkdir ok')) throw new Error(r);
      return 'Created the folder ' + name;
    });
  }

  async function rename(e) {
    const dot = e.dir ? -1 : e.name.lastIndexOf('.');
    const answer = await window.askText('Rename ' + (e.dir ? 'folder' : 'file'), '', e.name, 'Rename', dot > 0 ? dot : undefined);
    const name = answer === null ? '' : answer.trim();
    if (!name || name === e.name) return;
    const p = problemHere(name, e);
    if (p) return status(p, true);
    await run('Rename', async () => {
      const r = await ask('mv ' + join(dir, e.name) + '|' + join(dir, name), 'mv');
      if (!r.startsWith('mv ok')) throw new Error(r);
      return 'Renamed ' + e.name + ' to ' + name;
    });
  }

  // Loads a profile of the folder shown on the RT4K: it reprograms the scaler, and can change the input
  // and the output resolution, as the profile was saved (seconds).
  async function load(e) {
    const pp = profilePath(dir, e.name);
    if (!pp) return;
    await run('Loading ' + e.name, async () => {
      progress('Loading ' + e.name, 0, 0);
      const r = await ask('prof load ' + pp, 'prof', 15000);
      if (r !== 'prof load ok') throw new Error(r);
      loaded = pp;
      return 'Loaded ' + e.name;
    });
  }

  async function remove(e) {
    const sure = await window.askUser('Delete ' + e.name + '?', (e.dir ? 'The folder and everything in it will be deleted' :
      'The file will be deleted') + ' from the RT4K\'s SD card. This can\'t be undone.', 'Delete', true);
    if (!sure) return;
    await run('Delete', async () => {
      let count = 0;
      await removeTree(join(dir, e.name), e.dir, (path) => { count++; progress('Deleting ' + path, 0, 0); });
      return 'Deleted ' + e.name + (count > 1 ? ' and the ' + (count - 1) + ' items in it' : '');
    });
  }

  function build() {
    const box = q('sd');
    box.innerHTML =
      '<div class=panel id=sdbox>' +
      '<div class="row sdh"><nav id=sdc class=crumbs aria-label="Folder"></nav><span id=sds class="small grow"></span>' +
      '<div class=row><button id=sdu class=primary>Upload</button><button id=sdn>New folder</button>' +
      '<button id=sdr title="Read the folder again">Refresh</button></div></div>' +
      '<input type=file id=sdf multiple hidden>' +
      '<div id=sdsel class=sdsel hidden><span id=sdseln class=grow></span><button id=sdselc>Clear</button><button id=sdsele class=primary></button></div>' +
      '<div id=sdp class=sdp hidden><div id=sdpt class="small mono"></div><progress id=sdpb></progress></div>' +
      '<table class=files id=sdtbl>' +
      '<thead><tr><th class=ck><input type=checkbox id=sdpa aria-label="Tick every profile here"></th><th data-k=name>Name</th><th data-k=size class="num cs sz">Size</th><th data-k=mtime class="num cw when">Modified</th><th class=ca></th></tr></thead>' +
      '<tbody id=sdt></tbody></table><div id=sde class=empty hidden>This folder is empty</div>' +
      '<div id=sdz class=asleep hidden><p id=sdzt></p><button id=sdzb class=primary>Turn the RT4K on</button></div>' +
      '<div class=small>Drop files here to upload them to this folder. ' +
      'Transfers go at about 100 KB/s (a 4 MB .rbf takes some 45 s), one after another.</div></div>';
    q('sdr').onclick = () => list(dir || '');
    q('sdu').onclick = () => q('sdf').click();
    q('sdf').onchange = () => { const files = [...q('sdf').files]; q('sdf').value = ''; upload(files); };
    q('sdn').onclick = newFolder;
    q('sdzb').onclick = () => {
      if (!window.rt4kWake()) return status('Cruller did not answer.', true);
      waking = true; // until the status says it's starting
      showAsleep();
      setTimeout(() => { if (waking) { waking = false; showAsleep(); } }, 10000); // it didn't take: offer it again
    };
    q('sdt').onchange = (ev) => {
      const c = ev.target.closest('input[data-pick]');
      if (!c) return;
      if (c.checked) picked.add(c.dataset.pick); else picked.delete(c.dataset.pick);
      render();
    };
    q('sdpa').onchange = () => {
      picked = new Set(q('sdpa').checked ? entries.filter((e) => !e.dir && profilePath(dir, e.name)).map((e) => e.name) : []);
      render();
    };
    q('sdselc').onclick = () => { picked = new Set(); render(); };
    // the ticked profiles in the editor, in the folder's order
    q('sdsele').onclick = () => {
      const paths = shown.filter((e) => picked.has(e.name)).map((e) => join(dir, e.name));
      picked = new Set();
      render();
      location.hash = '#rt4k/editor';
      window.peOpenSet(paths);
    };
    q('sdt').onclick = (ev) => {
      const b = ev.target.closest('button[data-a]');
      const e = b && shown[+b.dataset.i];
      if (!e || busy) return;
      if (b.dataset.a === 'load') load(e);
      else if (b.dataset.a === 'ren') rename(e);
      else remove(e);
    };
    box.querySelectorAll('th[data-k]').forEach((th) => {
      th.tabIndex = 0;
      th.onclick = th.onkeydown = (ev) => {
        if (ev.type === 'keydown' && ev.key !== 'Enter' && ev.key !== ' ') return;
        ev.preventDefault();
        if (sortKey === th.dataset.k) sortDesc = !sortDesc;
        else { sortKey = th.dataset.k; sortDesc = th.dataset.k !== 'name'; }
        render();
      };
    });
    // Files dropped on the panel go to the folder it shows.
    const panel = q('sdbox');
    panel.addEventListener('dragover', (ev) => {
      if (busy || asleep() || !ev.dataTransfer.types.includes('Files')) return;
      ev.preventDefault();
      panel.classList.add('drop');
    });
    panel.addEventListener('dragleave', (ev) => { if (!panel.contains(ev.relatedTarget)) panel.classList.remove('drop'); });
    panel.addEventListener('drop', (ev) => {
      ev.preventDefault();
      panel.classList.remove('drop');
      const items = [...ev.dataTransfer.items];
      if (items.some((it) => it.webkitGetAsEntry && it.webkitGetAsEntry() && it.webkitGetAsEntry().isDirectory)) {
        status('Folders can\'t be uploaded yet: make the folder here, open it and drop its files in.', true);
        return;
      }
      upload(ev.dataTransfer.files);
    });
    // Leaving the page would cut an upload or a delete short.
    addEventListener('beforeunload', (ev) => { if (busy) { ev.preventDefault(); ev.returnValue = ''; } });
    render();
  }

  // The page shows the SD card view: parts is the address after #rt4k/sd, a folder's path.
  function open(parts) {
    if (!q('sdt')) build();
    list(dirFromParts(parts || []));
  }

  // Every status: an upload's progress; the RT4K going to sleep or waking up (turned on here, from
  // the page's remote or with its own remote): the folder is read again once it's on.
  function onStatus(s) {
    if (onPut && s.put) onPut(s.put);
    if ('rt4k_profile' in s && s.rt4k_profile !== loaded) { // (the profile loaded, as Cruller keeps it)
      loaded = s.rt4k_profile;
      if (q('sdt') && !busy && !asleep() && dir !== null) render();
    }
    const was = power;
    power = s.rt4k_power;
    if (power !== 'standby') waking = false; // starting (or on): the status says so from now on
    if (!q('sdt') || was === power) return;
    if (failed && !busy && power === 'on' && dir !== null && !q('sd').hidden) list(dir);
    else if (!busy) { showAsleep(); if (!asleep()) render(); }
  }

  window.sdOpen = open;
  window.sdStatus = onStatus;
  window.sdInternals = { parseList, sortEntries, hrefFor, dirFromParts, getUrl, nameProblem, utf8Length, sameName, size, when, profilePath }; // tests/test_sd.js
})();
