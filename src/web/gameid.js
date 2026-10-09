// gameID's view for the Cruller page (served as /gameid.js, embedded at build time): the consoles Cruller
// asks which game they run, and the gameDB, each game with the RT4K profile to load (docs/GAMEID.md).
// Read from and saved to /api/v1/gameid (docs/API.md); a profile is picked from the RT4K's SD card (its
// /profile folder). A console's game can be read from here too, the browser asking it (a MemCard PRO
// answers any page), to add it to the gameDB.

(() => {
  'use strict';

  const q = (id) => document.getElementById(id);
  const esc = (s) => String(s).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));

  // --- pure helpers (tests/test_gameid_page.js) --------------------------------------------------------

  // A console's address from what's typed: an address alone is a MemCard PRO's (its /api/currentState);
  // http:// added when it's missing; anything else as it is (Cruller says what's wrong with it).
  function consoleUrl(typed) {
    let s = String(typed).trim();
    if (!s) return '';
    if (!/^[a-z]+:\/\//i.test(s)) s = 'http://' + s;
    if (/^http:\/\/[^/]+\/?$/i.test(s)) s = s.replace(/\/?$/, '/api/currentState');
    return s;
  }

  // What's wrong with a console's address ('' when nothing), as Cruller checks it.
  function urlProblem(url) {
    if (!/^http:\/\/[^/\s]/i.test(url)) return /^https:/i.test(url) ? 'https isn\'t supported yet: use http://' : 'An address starts with http://';
    if (/\s/.test(url)) return 'An address has no spaces';
    return '';
  }

  // A profile as gameID keeps it: a .rt4 or .rt6 under /profile, its path from there.
  const profileOk = (p) => !!p && !/^\//.test(p) && !/\.\./.test(p) && !/\/\//.test(p) && !/[\x00-\x1f\\]/.test(p) && /\.rt[46]$/i.test(p);

  // The games whose name, ID or profile has the text (any case).
  function filterGames(games, text) {
    const t = String(text).trim().toLowerCase();
    return t ? games.filter((g) => (g.name + '\n' + g.id + '\n' + g.profile).toLowerCase().includes(t)) : games;
  }

  // A console's answer as gameID reads it: JSON with gameID (and gameName), or the ID as text.
  function readGame(text) {
    try {
      const j = JSON.parse(text);
      if (j && typeof j === 'object') return { id: String(j.gameID || '').trim(), name: String(j.gameName || '').trim() };
    } catch (e) { /* text */ }
    return { id: String(text).trim(), name: '' };
  }

  // --- state ------------------------------------------------------------------------------------------

  let consoles = []; // as Cruller keeps them: {name, url, other, svs_input, enabled}
  let games = [];    // {id, profile, name}, in the gameDB's order
  let power = '';
  let filter = '';
  let adding = null; // the game being added: {id, name, profile}

  const asleep = () => power === 'standby' || power === 'starting';
  const plain = (p) => p.replace(/\.rt[46]$/i, '');
  const failure = (e) => (e.message === 'Failed to fetch' ? 'Cruller did not answer' : e.message);

  async function api(path, body) {
    const r = await fetch(path, body ? { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) } : undefined);
    const j = await r.json().catch(() => ({}));
    if (!r.ok || j.ok === false) throw new Error(j.error || 'HTTP ' + r.status);
    return j;
  }

  function status(id, text, bad) {
    q(id).textContent = text;
    q(id).classList.toggle('bad', !!bad);
  }

  // --- the profile picker: a folder at a time under /profile, a file picked ------------------------------

  const DIR = '<svg width="18" height="18" viewBox="0 0 24 24" fill="currentColor" aria-hidden="true"><path d="M3 6.5A1.5 1.5 0 0 1 4.5 5h4.6l2 2.2h8.4A1.5 1.5 0 0 1 21 8.7v9.8a1.5 1.5 0 0 1-1.5 1.5h-15A1.5 1.5 0 0 1 3 18.5z"/></svg>';
  let pickDir = 'profile';

  async function pickList() {
    const d = pickDir, parts = d.split('/');
    q('gidpc').innerHTML = parts.map((p, k) => (k === parts.length - 1 ? '<b>' + esc(p) + '</b>' : '<a href=# data-dir="' + esc(parts.slice(0, k + 1).join('/')) + '">' + esc(p) + '</a>')).join('<span>/</span>');
    q('gidpl').innerHTML = '';
    q('gidps').textContent = asleep() ? 'The RT4K is asleep: turn it on to read its SD card.' : 'Reading the folder…';
    if (asleep()) return;
    try {
      const r = await fetch('/rt4k/ls?dir=' + encodeURIComponent(d));
      const body = await r.text();
      if (!r.ok) throw new Error(body.trim() || 'HTTP ' + r.status);
      if (pickDir !== d) return;
      const sd = window.sdInternals, list = sd.sortEntries(sd.parseList(body), 'name', false).filter((e) => e.dir || /\.rt[46]$/i.test(e.name));
      q('gidpl').innerHTML = list.map((e) => (e.dir ? '<button type=button data-dir="' + esc(d + '/' + e.name) + '"><span class=ico>' + DIR + '</span>' + esc(e.name) + '</button>'
        : '<button type=button data-file="' + esc((d + '/' + e.name).replace(/^profile\//, '')) + '">' + esc(plain(e.name)) + '</button>')).join('') || '<div class=pe0>No profiles here</div>';
      q('gidps').textContent = '';
    } catch (e) {
      q('gidps').textContent = 'Could not read the folder: ' + failure(e);
    }
  }

  // Resolves the profile picked (its path under /profile), '' for none (when none is allowed), or null.
  function pickProfile(current, noneAllowed) {
    const dlg = q('gidpk');
    pickDir = current && current.includes('/') ? 'profile/' + current.split('/').slice(0, -1).join('/') : 'profile';
    q('gidpn').hidden = !noneAllowed;
    return new Promise((resolve) => {
      let picked = null;
      dlg.onclick = (ev) => {
        const b = ev.target.closest('[data-dir], [data-file], #gidpn');
        if (!b) return;
        ev.preventDefault();
        if (b.dataset.dir !== undefined) { pickDir = b.dataset.dir; pickList(); return; }
        picked = b.id === 'gidpn' ? '' : b.dataset.file;
        resolve(picked); // (now: the close event comes later, and a Cancel or Escape resolves null by it)
        dlg.close();
      };
      dlg.onclose = () => resolve(picked);
      dlg.showModal();
      pickList();
    });
  }

  // --- the consoles -----------------------------------------------------------------------------------

  const SVS = ['Auto', 1, 2, 3, 4, 5, 6, 7, 8];

  function renderConsoles() {
    q('gidct').innerHTML = consoles.map((c, k) => '<tr data-k=' + k + (c.enabled ? '' : ' class=off') + '>' +
      '<td class=ck><input type=checkbox data-f=enabled' + (c.enabled ? ' checked' : '') + ' aria-label="Ask ' + esc(c.name) + '"></td>' +
      '<td><input data-f=name value="' + esc(c.name) + '" maxlength=47 aria-label="Name"></td>' +
      '<td class=mono><input data-f=url value="' + esc(c.url) + '" maxlength=127 spellcheck=false aria-label="Address" title="Its address: an IP alone is a MemCard PRO\'s /api/currentState"></td>' +
      '<td><select data-f=svs_input aria-label="SVS input" title="With an SVS switch, the input it\'s on (Auto: from the SVS tab\'s consoles)">' +
      SVS.map((v, i) => '<option value=' + i + (i === c.svs_input ? ' selected' : '') + '>' + v + '</option>').join('') + '</select></td>' +
      '<td><button type=button class=gidp data-f=other title="The profile for a game the gameDB hasn\'t">' + (c.other ? esc(plain(c.other)) : '<span class=gidn>None</span>') + '</button></td>' +
      '<td class=act><button type=button class=ib data-a=read title="Read the game it runs, to add it">Its game</button>' +
      '<button type=button class="ib del" data-a=del aria-label="Remove ' + esc(c.name) + '" title="Remove it">×</button></td></tr>').join('') ||
      '<tr><td colspan=6 class=pe0>No consoles yet: add one, as its address on your network.</td></tr>';
    q('gidca').disabled = consoles.length >= 10;
  }

  async function saveConsoles(note) {
    try {
      await api('/api/v1/gameid/consoles', { consoles });
      status('gidcs', note || 'Saved');
    } catch (e) {
      status('gidcs', 'Not saved: ' + failure(e), true);
      await loadConsoles(); // back to what Cruller kept
    }
  }

  async function loadConsoles() {
    try {
      consoles = (await api('/api/v1/gameid/consoles')).consoles || [];
    } catch (e) {
      status('gidcs', 'Could not read them: ' + failure(e), true);
    }
    renderConsoles();
  }

  // The game a console runs, asked by this browser (its answer read as gameID reads it).
  async function readConsole(c) {
    const r = await fetch(c.url, { cache: 'no-store' });
    if (!r.ok) throw new Error('HTTP ' + r.status);
    return readGame(await r.text());
  }

  // --- the games --------------------------------------------------------------------------------------

  function renderGames() {
    const shown = filterGames(games, filter);
    const row = (g) => '<tr data-id="' + esc(g.id) + '"><td><input data-f=name value="' + esc(g.name) + '" maxlength=47 aria-label="Name" placeholder="(no name)"></td>' +
      '<td class=mono>' + esc(g.id) + '</td>' +
      '<td><button type=button class=gidp data-f=profile>' + esc(plain(g.profile)) + '</button></td>' +
      '<td class=act><button type=button class="ib del" data-a=del aria-label="Remove ' + esc(g.name || g.id) + '" title="Remove it">×</button></td></tr>';
    const add = adding ? '<tr class=new><td><input id=gidan value="' + esc(adding.name) + '" maxlength=47 placeholder="Name" aria-label="Name"></td>' +
      '<td class=mono><input id=gidai value="' + esc(adding.id) + '" maxlength=63 placeholder="Its ID (SCUS-97481)" spellcheck=false aria-label="ID"></td>' +
      '<td><button type=button class=gidp id=gidap>' + (adding.profile ? esc(plain(adding.profile)) : '<span class=gidn>Pick its profile…</span>') + '</button></td>' +
      '<td class=act><button type=button class=primary id=gidas>Add</button><button type=button id=gidax>Cancel</button></td></tr>' : '';
    q('gidgt').innerHTML = add + shown.map(row).join('') +
      (!shown.length && !adding ? '<tr><td colspan=4 class=pe0>' + (games.length ? 'No game matches' : 'No games yet: add one, or read a console\'s.') + '</td></tr>' : '');
    q('gidgs').textContent = (games.length === 1 ? '1 game' : games.length + ' games') + (filter.trim() ? ', ' + shown.length + ' shown' : '');
    q('gidga').disabled = !!adding || games.length >= 1000;
  }

  async function loadGames() {
    try {
      games = (await api('/api/v1/gameid/games')).games || [];
    } catch (e) {
      status('gidgs', 'Could not read them: ' + failure(e), true);
    }
    renderGames();
  }

  async function putGame(g, note) {
    try {
      const r = await api('/api/v1/gameid/games', g);
      const k = games.findIndex((x) => x.id === g.id);
      if (k >= 0) games[k] = { ...g }; else games.push({ ...g });
      renderGames();
      status('gidgm', note || (r.replaced ? 'Saved ' : 'Added ') + (g.name || g.id));
      return true;
    } catch (e) {
      status('gidgm', 'Not saved: ' + failure(e), true);
      return false;
    }
  }

  function startAdding(g) {
    adding = { id: g.id || '', name: g.name || '', profile: g.profile || '' };
    filter = '';
    q('gidq').value = '';
    renderGames();
    q(adding.id ? 'gidap' : 'gidai').focus();
  }

  // --- the view -------------------------------------------------------------------------------------

  function build() {
    q('gid').innerHTML =
      '<div class=panel>' +
      '<div class=row><h2 class=grow>Consoles</h2><span id=gidcs class="small"></span><button id=gidca>Add a console</button></div>' +
      '<div class=gidw><table class=gidt><thead><tr><th class=ck>On</th><th>Name</th><th>Address</th><th>SVS input</th><th>Game not in the gameDB</th><th></th></tr></thead>' +
      '<tbody id=gidct></tbody></table></div>' +
      '<div class=small>Cruller asks each one which game it runs: a MemCard PRO2 or PRO (with its own web page on; for the PRO2, WebUI v2 off), ' +
      'a PS1Digital or an N64Digital. Only http for now. Asking them and loading the profiles comes next.</div></div>' +
      '<div class=panel>' +
      '<div class="row sdh"><h2>Games</h2><span id=gidgs class="small grow"></span><input id=gidq class=gidq placeholder="Find a game" autocomplete=off>' +
      '<button id=gidga class=primary>Add a game</button></div>' +
      '<div id=gidgm class=small></div>' +
      '<div class=gidw><table class=gidt><thead><tr><th>Name</th><th>ID</th><th>Profile</th><th></th></tr></thead><tbody id=gidgt></tbody></table></div></div>' +
      '<dialog id=gidpk class=pick aria-labelledby=gidpt><form method=dialog><h3 id=gidpt>Pick a profile</h3>' +
      '<nav id=gidpc class=crumbs aria-label=Folder></nav><div id=gidpl class=pepl></div>' +
      '<div class=row><span id=gidps class="small grow"></span><button type=button id=gidpn>No profile</button><button value=no>Cancel</button></div></form></dialog>';

    q('gidca').onclick = () => {
      consoles.push({ name: 'Console ' + (consoles.length + 1), url: '', other: '', svs_input: 0, enabled: true });
      renderConsoles();
      const row = q('gidct').querySelector('tr:last-child [data-f=url]');
      if (row) row.focus();
      status('gidcs', 'Its address, to save it');
    };
    // a change: kept, and saved once it's whole (a console's address checked here first)
    q('gidct').onchange = (ev) => {
      const el = ev.target.closest('[data-f]'), tr = ev.target.closest('tr[data-k]');
      if (!el || !tr) return;
      const c = consoles[+tr.dataset.k], f = el.dataset.f;
      if (f === 'enabled') c.enabled = el.checked;
      else if (f === 'svs_input') c.svs_input = +el.value;
      else if (f === 'url') { c.url = consoleUrl(el.value); el.value = c.url; }
      else if (f === 'name') c.name = el.value.trim();
      tr.classList.toggle('off', !c.enabled);
      const bad = consoles.map((x) => (!x.name ? 'A console needs a name' : urlProblem(x.url))).find(Boolean);
      if (bad) return status('gidcs', bad + ': not saved yet', true);
      saveConsoles();
    };
    q('gidct').onclick = async (ev) => {
      const b = ev.target.closest('button'), tr = ev.target.closest('tr[data-k]');
      if (!b || !tr) return;
      const c = consoles[+tr.dataset.k];
      if (b.dataset.f === 'other') {
        const p = await pickProfile(c.other, true);
        if (p === null || p === c.other) return;
        c.other = p;
        renderConsoles();
        if (!urlProblem(c.url)) saveConsoles();
      } else if (b.dataset.a === 'del') {
        if (!(await window.askUser('Remove ' + c.name + '?', 'Cruller stops asking it which game it runs. Its games stay in the gameDB.', 'Remove', true))) return;
        consoles.splice(+tr.dataset.k, 1);
        renderConsoles();
        saveConsoles('Removed ' + c.name);
      } else if (b.dataset.a === 'read') {
        status('gidcs', 'Asking ' + c.name + '…');
        try {
          const g = await readConsole(c);
          if (!g.id) return status('gidcs', c.name + ' runs no game it can tell', true);
          const known = games.find((x) => x.id === g.id);
          status('gidcs', c.name + ' runs ' + (g.name || g.id) + (known ? ': in the gameDB already' : ''));
          if (!known) startAdding({ id: g.id, name: g.name, profile: c.other });
        } catch (e) {
          status('gidcs', 'Could not ask ' + c.name + ' from this browser (' + failure(e) + '): is it on, at that address?', true);
        }
      }
    };

    q('gidga').onclick = () => startAdding({});
    q('gidq').oninput = () => { filter = q('gidq').value; renderGames(); };
    q('gidgt').onchange = (ev) => {
      const el = ev.target.closest('[data-f=name]'), tr = ev.target.closest('tr[data-id]');
      if (!el || !tr) return;
      const g = games.find((x) => x.id === tr.dataset.id);
      if (g && el.value.trim() !== g.name) putGame({ ...g, name: el.value.trim() }, 'Renamed ' + (el.value.trim() || g.id));
    };
    q('gidgt').oninput = (ev) => { // the game being added: kept as typed
      if (!adding) return;
      if (ev.target.id === 'gidan') adding.name = ev.target.value;
      if (ev.target.id === 'gidai') adding.id = ev.target.value;
    };
    q('gidgt').onclick = async (ev) => {
      const b = ev.target.closest('button');
      if (!b) return;
      if (b.id === 'gidap') {
        const p = await pickProfile(adding.profile, false);
        if (p) { adding.profile = p; renderGames(); }
      } else if (b.id === 'gidax') {
        adding = null;
        renderGames();
      } else if (b.id === 'gidas') {
        const g = { id: adding.id.trim(), name: adding.name.trim(), profile: adding.profile };
        if (!g.id) return status('gidgm', 'A game needs its ID: what its console reports', true);
        if (!profileOk(g.profile)) return status('gidgm', 'Pick its profile', true);
        if (games.some((x) => x.id === g.id) && !(await window.askUser('Replace ' + g.id + '?', 'The gameDB has it already: it will load this profile instead.', 'Replace', false))) return;
        if (await putGame(g)) { adding = null; renderGames(); }
      } else {
        const tr = b.closest('tr[data-id]'), g = tr && games.find((x) => x.id === tr.dataset.id);
        if (!g) return;
        if (b.dataset.f === 'profile') {
          const p = await pickProfile(g.profile, false);
          if (p && p !== g.profile) putGame({ ...g, profile: p }, (g.name || g.id) + ' loads ' + plain(p));
        } else if (b.dataset.a === 'del') {
          if (!(await window.askUser('Remove ' + (g.name || g.id) + '?', 'Its console\'s profile for games not in the gameDB loads for it instead, if it has one.', 'Remove', true))) return;
          try {
            await api('/api/v1/gameid/games/delete', { id: g.id });
            games = games.filter((x) => x !== g);
            renderGames();
            status('gidgm', 'Removed ' + (g.name || g.id));
          } catch (e) {
            status('gidgm', 'Not removed: ' + failure(e), true);
          }
        }
      }
    };
  }

  // The page shows the view: built the first time, read every time (the API may have changed them).
  function open() {
    if (!q('gidct')) build();
    loadConsoles();
    loadGames();
  }

  function onStatus(s) {
    power = s.rt4k_power || power;
  }

  window.gidOpen = open;
  window.gidStatus = onStatus;
  window.gameidInternals = { consoleUrl, urlProblem, profileOk, filterGames, readGame }; // tests/test_gameid_page.js
})();
