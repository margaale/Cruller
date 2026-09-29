// The RT4K's SD card for the Cruller page (served as /sd.js, embedded at build time).
//
// Folders come from GET /rt4k/ls (the RT4K's "ls", one entry a line) and files from GET /rt4k/get
// (RTL1 get, in verified pieces), which the browser downloads on its own. Uploads go to POST
// /rt4k/put with their SHA-256 (sha256.js), which the RT4K checks; new folders, renames and deletes
// are the RT4K's own mkdir, mv and rm (POST /rt4k/ask). Each folder has its own address
// (#rt4k/sd/<path>), so the browser's back button walks back up.

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
  let entries = [];
  let shown = [];          // entries as the table shows them (row buttons point into it)
  let total = 0;           // the RT4K's own count (more than entries when the listing didn't fit)
  let sortKey = 'name', sortDesc = false;
  let loading = 0;         // the listing request that counts (later ones win)
  let failed = false;      // the last listing failed (retried when the RT4K comes on)
  let power = '';
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
  };

  // --- Cruller ----------------------------------------------------------------------------------------

  async function ask(cmd, expect) {
    const r = await fetch('/rt4k/ask?expect=' + encodeURIComponent(expect), { method: 'POST', body: cmd });
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
    let html = parts.length ? '<a href="' + hrefFor('') + '">SD card</a>' : '<b>SD card</b>';
    parts.forEach((p, i) => {
      html += '<span>/</span>';
      html += i === parts.length - 1 ? '<b>' + esc(p) + '</b>' : '<a href="' + hrefFor(parts.slice(0, i + 1).join('/')) + '">' + esc(p) + '</a>';
    });
    q('sdc').innerHTML = html;
  }

  function render() {
    crumbs();
    document.querySelectorAll('#sd th[data-k]').forEach((th) => {
      th.setAttribute('aria-sort', th.dataset.k === sortKey ? (sortDesc ? 'descending' : 'ascending') : 'none');
    });
    ['sdu', 'sdn', 'sdr'].forEach((id) => { q(id).disabled = busy; });
    // No colspan: with the date column hidden (narrow screens) it would make a fourth, empty one.
    const up = dir ? '<tr class=up><td class=n><a href="' + hrefFor(dir.split('/').slice(0, -1).join('/')) + '">' +
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
      const act = (e.dir ? '' : '<a class="btn ib" href="' + getUrl(join(dir, e.name)) + '" download="' + esc(e.name) + '" title="Download" ' +
        'aria-label="Download ' + esc(e.name) + '">' + ICON.down + '</a>') +
        '<button class=ib data-a=ren data-i=' + i + off + ' title="Rename" aria-label="Rename ' + esc(e.name) + '">' + ICON.ren + '</button>' +
        '<button class="ib del" data-a=del data-i=' + i + off + ' title="Delete" aria-label="Delete ' + esc(e.name) + '">' + ICON.del + '</button>';
      return '<tr' + (e.dir ? ' class=d' : '') + '><td class=n title="' + esc(e.name) + '">' + name + '</td>' +
        '<td class="num sz">' + (e.dir ? '' : size(e.size)) + '</td><td class="num when">' + when(e.mtime) + '</td><td class=act>' + act + '</td></tr>';
    }).join('');
  }

  // note: [text, bad] to show instead of the folder's summary (how an operation went).
  async function list(d, note) {
    const mine = ++loading;
    const moved = d !== dir;
    dir = d;
    crumbs();
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
    if (!files.length || busy || dir === null) return;
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
    if (busy || dir === null) return;
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
      '<div class=panel id=sdbox style="max-width:1100px">' +
      '<div class="row sdh"><nav id=sdc class="crumbs grow" aria-label="Folder"></nav>' +
      '<div class=row><button id=sdu class=primary>Upload</button><button id=sdn>New folder</button>' +
      '<button id=sdr title="Read the folder again">Refresh</button></div></div>' +
      '<input type=file id=sdf multiple hidden>' +
      '<div id=sds class=small></div>' +
      '<div id=sdp class=sdp hidden><div id=sdpt class="small mono"></div><progress id=sdpb></progress></div>' +
      '<table class=files>' +
      '<thead><tr><th data-k=name>Name</th><th data-k=size class="num cs sz">Size</th><th data-k=mtime class="num cw when">Modified</th><th class=ca></th></tr></thead>' +
      '<tbody id=sdt></tbody></table><div id=sde class=empty hidden>This folder is empty</div>' +
      '<div class=small>The RT4K has to be on to read its SD card. Drop files here to upload them to this folder. ' +
      'Transfers go at about 100 KB/s (a 4 MB .rbf takes some 45 s), one after another.</div></div>';
    q('sdr').onclick = () => list(dir || '');
    q('sdu').onclick = () => q('sdf').click();
    q('sdf').onchange = () => { const files = [...q('sdf').files]; q('sdf').value = ''; upload(files); };
    q('sdn').onclick = newFolder;
    q('sdt').onclick = (ev) => {
      const b = ev.target.closest('button[data-a]');
      const e = b && shown[+b.dataset.i];
      if (!e || busy) return;
      if (b.dataset.a === 'ren') rename(e);
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
      if (busy || !ev.dataTransfer.types.includes('Files')) return;
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

  // Every status: an upload's progress, and a listing that failed because the RT4K slept is read
  // again once it's on.
  function onStatus(s) {
    if (onPut && s.put) onPut(s.put);
    const was = power;
    power = s.rt4k_power;
    if (failed && !busy && was && was !== 'on' && power === 'on' && dir !== null && !q('sd').hidden) list(dir);
  }

  window.sdOpen = open;
  window.sdStatus = onStatus;
  window.sdInternals = { parseList, sortEntries, hrefFor, dirFromParts, getUrl, nameProblem, utf8Length, sameName, size, when }; // tests/test_sd.js
})();
