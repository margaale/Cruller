// The RT4K's SD card for the Cruller page (served as /sd.js, embedded at build time).
//
// Folders come from GET /rt4k/ls (the RT4K's "ls", one entry a line) and files from GET /rt4k/get
// (RTL1 get, in verified pieces), which the browser downloads on its own. Each folder has its own
// address (#rt4k/sd/<path>), so the browser's back button walks back up.

(() => {
  'use strict';

  const q = (id) => document.getElementById(id);

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

  function size(n) {
    if (n >= 1048576) return (n / 1048576).toFixed(1) + ' MB';
    if (n >= 1024) return (n / 1024).toFixed(n >= 10240 ? 0 : 1) + ' KB';
    return n + ' B';
  }

  // FAT keeps local time without a zone and the RT4K hands it over as if UTC: show it as stored.
  function when(t) {
    if (!t) return '';
    return new Date(t * 1000).toISOString().slice(0, 16).replace('T', ' ');
  }

  // --- state ------------------------------------------------------------------------------------------

  let dir = null;          // the folder shown ('' = root; null: nothing yet)
  let entries = [];
  let total = 0;           // the RT4K's own count (more than entries when the listing didn't fit)
  let sortKey = 'name', sortDesc = false;
  let loading = 0;         // the listing request that counts (later ones win)
  let failed = false;      // the last listing failed (retried when the RT4K comes on)
  let power = '';

  const esc = (s) => String(s).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));

  const ICON = {
    dir: '<svg width="18" height="18" viewBox="0 0 24 24" fill="currentColor" aria-hidden="true"><path d="M3 6.5A1.5 1.5 0 0 1 4.5 5h4.6l2 2.2h8.4A1.5 1.5 0 0 1 21 8.7v9.8a1.5 1.5 0 0 1-1.5 1.5h-15A1.5 1.5 0 0 1 3 18.5z"/></svg>',
    file: '<svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linejoin="round" aria-hidden="true"><path d="M6 3h8l4 4v14H6z"/><path d="M14 3v4h4"/></svg>',
    down: '<svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M12 4v11"/><path d="M7 10l5 5 5-5"/><path d="M5 20h14"/></svg>',
  };

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
    // No colspan: with the date column hidden (narrow screens) it would make a fourth, empty one.
    const up = dir ? '<tr class=up><td class=n><a href="' + hrefFor(dir.split('/').slice(0, -1).join('/')) + '">' +
      '<span class=ico>' + ICON.dir + '</span>..</a></td><td></td><td class=when></td><td></td></tr>' : '';
    q('sde').hidden = entries.length > 0;
    const sorted = sortEntries(entries, sortKey, sortDesc);
    q('sdt').innerHTML = up + sorted.map((e) => {
      const name = e.dir
        ? '<a href="' + hrefFor(join(dir, e.name)) + '"><span class=ico>' + ICON.dir + '</span><span class=nm>' + esc(e.name) + '</span></a>'
        : '<span class="ico f">' + ICON.file + '</span><span class=nm>' + esc(e.name) + '</span>';
      // A plain link: the browser saves the file as it comes, in its own downloads list, with the
      // size from Content-Length (the name from Content-Disposition, and download= here).
      const act = e.dir ? '' : '<a class="btn ib" href="' + getUrl(join(dir, e.name)) + '" download="' + esc(e.name) + '" title="Download" ' +
        'aria-label="Download ' + esc(e.name) + '">' + ICON.down + '</a>';
      return '<tr' + (e.dir ? ' class=d' : '') + '><td class=n title="' + esc(e.name) + '">' + name + '</td>' +
        '<td class=num>' + (e.dir ? '' : size(e.size)) + '</td><td class="num when">' + when(e.mtime) + '</td><td class=act>' + act + '</td></tr>';
    }).join('');
  }

  async function list(d) {
    const mine = ++loading;
    const moved = d !== dir;
    dir = d;
    crumbs();
    status('Reading the SD card…');
    try {
      const r = await fetch('/rt4k/ls' + (d ? '?dir=' + encodeURIComponent(d) : ''));
      const body = await r.text();
      if (mine !== loading) return;
      if (!r.ok) throw new Error(r.status === 404 ? 'There is no folder "' + d + '" on the SD card.' : body.trim() || 'HTTP ' + r.status);
      entries = parseList(body);
      total = +r.headers.get('X-Total') || entries.length;
      failed = false;
      status(summary());
      render();
      if (moved && q('sd').getBoundingClientRect().top < 0) q('sd').scrollIntoView(); // another folder: from its top
    } catch (e) {
      if (mine !== loading) return;
      failed = true;
      entries = [];
      total = 0;
      q('sdt').innerHTML = '';
      q('sde').hidden = true;
      status(e.message === 'Failed to fetch' ? 'Cruller did not answer.' : e.message, true);
    }
  }

  function build() {
    const box = q('sd');
    box.innerHTML =
      '<div class=panel style="max-width:1100px">' +
      '<div class=row><nav id=sdc class="crumbs grow" aria-label="Folder"></nav>' +
      '<button id=sdr title="Read the folder again">Refresh</button></div>' +
      '<div id=sds class=small></div>' +
      '<table class=files>' +
      '<thead><tr><th data-k=name>Name</th><th data-k=size class="num cs">Size</th><th data-k=mtime class="num cw when">Modified</th><th class=ca></th></tr></thead>' +
      '<tbody id=sdt></tbody></table><div id=sde class=empty hidden>This folder is empty</div>' +
      '<div class=small>The RT4K has to be on to read its SD card. Downloads come at about 100 KB/s (a 4 MB .rbf takes ' +
      'some 45 s); several go one after another.</div></div>';
    q('sdr').onclick = () => list(dir || '');
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
  }

  // The page shows the SD card view: parts is the address after #rt4k/sd, a folder's path.
  function open(parts) {
    if (!q('sdt')) build();
    list(dirFromParts(parts || []));
  }

  // Every status: a listing that failed because the RT4K slept is read again once it's on.
  function onStatus(s) {
    const was = power;
    power = s.rt4k_power;
    if (failed && was && was !== 'on' && power === 'on' && dir !== null && !q('sd').hidden) list(dir);
  }

  window.sdOpen = open;
  window.sdStatus = onStatus;
  window.sdInternals = { parseList, sortEntries, hrefFor, dirFromParts, getUrl, size, when }; // for tests/test_sd.js
})();
