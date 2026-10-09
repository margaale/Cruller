// gameID in the Consoles view of the Cruller page (served as /gameid.js, embedded at build time): the game on
// screen and the profile for it; under each SVS input's card (app.js) the console that tells the game on it;
// the consoles not on the SVS; and your games, each with the RT4K profile to load (docs/GAMEID.md). Read
// from and saved to /api/v1/gameid (docs/API.md); a profile is picked from the RT4K's SD card (its /profile
// folder). What Cruller knows as it asks the consoles (GET /api/v1/gameid/state): every 2 s while the view
// shows.

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

  // A console's address as shown and typed: a MemCard PRO's by its host alone, http:// left out.
  const shortUrl = (url) => String(url).replace(/^http:\/\//i, '').replace(/\/api\/currentState$/, '');

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

  // A console as Cruller last asked it (state.consoles[k]): what it runs, as its card says it.
  function liveText(l, enabled) {
    if (!enabled) return 'Not asked';
    if (!l) return '…';
    if (!l.on) return 'Off';
    return l.game ? (l.game_name || l.game) : 'No game';
  }

  // The SVS input a console is on: the one it's set to, or on Auto (0) the one whose console is of its
  // kind (as Cruller matches them: its MemCard's mode, else its name), when just one is; 0: not on it.
  function inputOf(c, kind, inputs) {
    if (c.svs_input >= 1 && c.svs_input <= inputs.length) return c.svs_input;
    if (c.svs_input || !kind) return 0;
    const m = inputs.map((p, i) => (p.device === kind ? i + 1 : 0)).filter(Boolean);
    return m.length === 1 ? m[0] : 0;
  }

  const plain = (p) => p.replace(/\.rt[46]$/i, '');
  const same = (a, b) => !!a && !!b && a.toLowerCase() === b.toLowerCase();
  const ago = (s) => (s < 5 ? 'just now' : s < 60 ? s + ' s ago' : s < 3600 ? Math.round(s / 60) + ' min ago' : Math.round(s / 3600) + ' h ago');

  // What's on screen, as On screen says it: from gameID's state (s, null until read), the switch (sv:
  // known, input, inputs [{name, device}], files {n: its profile in /profile/SVS}), the consoles, the games
  // and the RT4K's power. title (dim: nothing playing), sub, the profile for it and how it stands (badge,
  // tone: ok, wait, bad), why; add: the game to add to your games; addFor: the input to give a gameID.
  function onScreen(s, sv, consoles, games, power) {
    const svs = !!(sv && sv.known);
    const input = svs ? (s ? s.svs_input : sv.input) : 0;
    const inName = (n) => (sv.inputs[n - 1] && sv.inputs[n - 1].name) || 'Input ' + n;
    const own = (n) => (svs && n && sv.files[n] ? 'SVS/' + sv.files[n] : '');
    const r = { title: '', dim: false, sub: '', profile: '', badge: '', tone: '', why: '', add: null, addFor: 0 };
    const p = s && s.playing;
    if (p) {
      r.title = p.game_name || p.game;
      r.sub = (svs && input ? inName(input) + ' on input ' + input : p.console) + ' · ' + p.game;
      if (!games.some((g) => g.id === p.game)) r.add = { id: p.game, name: p.game_name || '' };
      r.profile = s.profile || own(input);
      r.why = s.from === 'gamedb' ? 'From your games.' + (own(input) ? ' Input ' + input + ' loads its own ' + plain(sv.files[input]) + ' first, this one 3 s after.' : '') :
        s.from === 'other' ? 'Not in your games: its console\'s profile for those.' :
          'Not in your games: ' + (r.profile ? 'the input\'s own stays.' : 'the RT4K keeps the profile it has.');
    } else if (svs && !input) {
      Object.assign(r, { title: 'No input active', dim: true, sub: 'The SVS shows nothing', why: 'The RT4K keeps the profile it has.' });
    } else if (svs) {
      const mine = consoles.map((c, k) => ({ c, l: s && s.consoles[k] && s.consoles[k].name === c.name ? s.consoles[k] : null }))
        .filter((x) => inputOf(x.c, x.l && x.l.kind, sv.inputs) === input);
      const asked = mine.filter((x) => x.c.enabled);
      r.title = inName(input);
      r.sub = 'Input ' + input + ' · ' + (!asked.length ? 'nothing on it tells its game' : asked.some((x) => x.l && x.l.on) ? 'no game it can tell' : 'its gameID doesn\'t answer');
      r.addFor = mine.length ? 0 : input;
      r.profile = s && s.from === 'svs' ? s.profile : own(input);
      if (s && s.from === 'svs') r.why = 'Its console went off: the input\'s own again.';
    } else {
      const asked = consoles.filter((c) => c.enabled).length;
      Object.assign(r, { title: 'No game on screen', dim: true, why: 'The RT4K keeps the profile it has.',
        sub: !consoles.length ? 'No consoles yet' : asked ? 'Asking ' + (asked === 1 ? 'its console which game it runs' : asked + ' consoles which game they run') : 'No console asked' });
    }
    if (r.profile) {
      if (s && same(s.pending, r.profile)) {
        r.badge = power === 'standby' || power === 'starting' ? 'Waiting for the RT4K' : 'Loading…';
        r.tone = 'wait';
      } else if (s && /^could not load /.test(s.note) && s.note.includes(r.profile)) {
        Object.assign(r, { badge: 'Could not load', tone: 'bad', why: s.note });
      } else if (s && same(s.loaded, r.profile)) {
        r.badge = 'Loaded' + (s.note && s.note.includes(r.profile) && s.note_age_s >= 0 ? ' ' + ago(s.note_age_s) : '');
        r.tone = 'ok';
      } else if (r.profile === own(input)) {
        r.badge = 'The input\'s own';
      }
    }
    return r;
  }

  // --- state ------------------------------------------------------------------------------------------

  let consoles = []; // as Cruller keeps them: {name, url, other, svs_input, enabled}
  let games = [];    // {id, profile, name}, in the gameDB's order
  let power = '';
  let filter = '';
  let adding = null; // the game being added: {id, name, profile}
  let live = null;   // Cruller's state, as last read
  let liveFail = false;
  let built = false;
  let playing = '';  // the game on screen, as your games last showed it
  let nowAdd = null; // the game On screen offers to add

  const asleep = () => power === 'standby' || power === 'starting';
  const failure = (e) => (e.message === 'Failed to fetch' ? 'Cruller did not answer' : e.message);
  const liveOf = (k) => (live && live.consoles[k] && consoles[k] && live.consoles[k].name === consoles[k].name ? live.consoles[k] : null);

  // The switch, as app.js and profiles.js last read it.
  function svsInfo() {
    const sv = window.svsNow ? window.svsNow() : { bridge: false, known: false, input: 0, inputs: [] };
    sv.files = {};
    for (let n = 1; n <= sv.inputs.length; n++) sv.files[n] = window.profSvsFile ? window.profSvsFile(n) : '';
    return sv;
  }
  const inputName = (sv, n) => (sv.inputs[n - 1] && sv.inputs[n - 1].name) || 'Input ' + n;
  const kindOf = (k) => { const l = liveOf(k); return l ? l.kind : ''; };

  async function api(path, body) {
    const r = await fetch(path, body ? { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) } : undefined);
    const j = await r.json().catch(() => ({}));
    if (!r.ok || j.ok === false) throw new Error(j.error || 'HTTP ' + r.status);
    return j;
  }

  function status(id, text, bad) {
    q(id).textContent = text;
    q(id).classList.toggle('bad', !!bad);
    q(id).gidHtml = undefined; // (set() writes it again)
  }

  // An element's HTML, set only when it changes (a focused button stays).
  function set(el, html) {
    if (el && el.gidHtml !== html) { el.innerHTML = html; el.gidHtml = html; }
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

  // Resolves the profile picked (its path under /profile), or null.
  function pickProfile(current) {
    const dlg = q('gidpk');
    pickDir = current && current.includes('/') ? 'profile/' + current.split('/').slice(0, -1).join('/') : 'profile';
    return new Promise((resolve) => {
      let picked = null;
      dlg.onclick = (ev) => {
        const b = ev.target.closest('[data-dir], [data-file]');
        if (!b) return;
        ev.preventDefault();
        if (b.dataset.dir !== undefined) { pickDir = b.dataset.dir; pickList(); return; }
        picked = b.dataset.file;
        resolve(picked); // (now: the close event comes later, and a Cancel or Escape resolves null by it)
        dlg.close();
      };
      dlg.onclose = () => resolve(picked);
      dlg.showModal();
      pickList();
    });
  }

  // --- what's on screen ---------------------------------------------------------------------------------

  function renderNow() {
    const sv = svsInfo(), r = onScreen(live, sv, consoles, games, power);
    nowAdd = r.add;
    const state = !consoles.length ? ['', 'No consoles to ask'] : liveFail ? ['warn', 'Cruller did not answer'] : live ? ['ok', 'Asking every 2 s'] : ['', '…'];
    set(q('gon'), '<div class=row><h2 class=grow>On screen</h2><span class="dot ' + state[0] + '"></span><span class=small>' + state[1] + '</span></div>' +
      '<div class=gonb><div class=gonl><div class="gont' + (r.dim ? ' dim' : '') + '">' + esc(r.title) + '</div><div class=gons>' + esc(r.sub) + '</div></div>' +
      '<div class=gonr><div class=small>Profile</div><div class=gonp><span>' + (r.profile ? esc(plain(r.profile)) : '—') + '</span>' +
      (r.badge ? '<span class="gbad ' + r.tone + '">' + esc(r.badge) + '</span>' : '') + '</div>' + (r.why ? '<div class=small>' + esc(r.why) + '</div>' : '') +
      (r.add ? '<button type=button class=primary data-a=addgame>Add ' + esc(r.add.name || r.add.id) + ' to your games</button>' :
        r.addFor ? '<button type=button data-a=addfor data-n=' + r.addFor + (consoles.length >= 10 ? ' disabled' : '') + '>Add its gameID</button>' : '') + '</div></div>');
  }

  // --- each console's gameID: in its input's card, or with the consoles not on the SVS -----------------

  // Console k: its address and what it runs (it opens its gameID); a game not in your games, added in a click.
  function device(k) {
    const c = consoles[k], l = liveOf(k), tone = !c.enabled ? '' : l && l.on ? ' ok' : l ? ' bad' : '';
    const game = l && l.on && l.game;
    return '<button type=button class="gdev' + (game ? '' : ' off') + '" data-k=' + k + ' title="Change its gameID">' +
      '<span class="dot' + tone + '"></span><span><b>' + esc(shortUrl(c.url)) + '</b><i>' + esc(liveText(l, c.enabled)) + '</i></span></button>' +
      (game && !games.some((g) => g.id === l.game) ? '<button type=button class=gaddg data-a=addgame data-k=' + k + '>Add ' + esc(l.game_name || l.game) + ' to your games</button>' : '');
  }

  const full = () => consoles.length >= 10;

  function renderSlots() {
    const sv = svsInfo();
    document.querySelectorAll('#v-grid .gslot[data-n]').forEach((el) => {
      const n = +el.dataset.n, ks = consoles.map((c, k) => k).filter((k) => inputOf(consoles[k], kindOf(k), sv.inputs) === n);
      set(el, '<div class=gcap>gameID</div>' + (ks.length ? ks.map(device).join('') :
        '<button type=button class=gadd data-a=addfor data-n=' + n + (full() ? ' disabled' : '') + '>+ Add a MemCard or Digital</button>'));
    });
  }

  function renderOthers() {
    const sv = svsInfo(), ins = sv.known ? sv.inputs : [];
    const ks = consoles.map((c, k) => k).filter((k) => inputOf(consoles[k], kindOf(k), ins) === 0);
    q('gcon').hidden = sv.bridge && !ks.length;
    const where = (c) => (!sv.bridge ? '' : c.svs_input ? 'Input ' + c.svs_input + ': this switch has fewer' : 'Not on an input');
    set(q('gcon'), '<div class=row><h2 class=grow>' + (sv.bridge ? 'Not on the SVS' : 'Consoles') + '</h2>' +
      '<button type=button data-a=addfor data-n=0' + (full() ? ' disabled' : '') + '>Add a console</button></div>' +
      (ks.length ? '<div class="svs-grid gcards">' + ks.map((k) => '<div><span>' + esc(consoles[k].name) + '</span><small>' + esc(where(consoles[k])) + '</small>' +
        '<div class=gslot><div class=gcap>gameID</div>' + device(k) + '</div></div>').join('') + '</div>' :
        '<div class=small>None yet: add one by its MemCard\'s or Digital\'s address, and each game\'s profile loads by itself.</div>') +
      (sv.bridge ? '' : '<div class=small>With an SVS switch, its SVS Bridge on your network shows its inputs here by itself, each with its gameID.</div>'));
    set(q('gnot'), sv.bridge && !ks.length ? 'A console that isn\'t on the SVS? <button type=button class=link data-a=addfor data-n=0' + (full() ? ' disabled' : '') + '>Add it</button>' : '');
  }

  // What Cruller knows, everywhere it shows; your games again when the one on screen changes.
  function renderLive() {
    if (!built) return;
    renderNow();
    renderSlots();
    renderOthers();
    const p = live && live.playing ? live.playing.game : '';
    if (p !== playing) { playing = p; renderGames(); }
  }

  // Every 2 s while the view shows.
  async function tick() {
    if (!q('cons') || q('cons').hidden || document.hidden) return;
    try {
      const r = await fetch('/api/v1/gameid/state', { cache: 'no-store' });
      if (!r.ok) throw new Error('HTTP ' + r.status);
      live = await r.json();
      liveFail = false;
    } catch (e) {
      liveFail = true;
    }
    renderLive();
  }

  async function loadConsoles() {
    try {
      consoles = (await api('/api/v1/gameid/consoles')).consoles || [];
    } catch (e) {
      status('gnot', 'Could not read the consoles: ' + failure(e), true);
    }
    renderLive();
  }

  async function saveConsoles(list) {
    try {
      await api('/api/v1/gameid/consoles', { consoles: list });
      consoles = list;
      renderLive();
      return true;
    } catch (e) {
      status('gdm', 'Not saved: ' + failure(e), true);
      return false;
    }
  }

  // --- a console's gameID, added or changed ------------------------------------------------------------

  let draft = null, draftK = -1;

  // Console k (-1: a new one, on input n or none).
  function openConsole(k, n) {
    const sv = svsInfo(), ins = sv.known ? sv.inputs : [];
    draftK = k;
    draft = k >= 0 ? { ...consoles[k], svs_input: inputOf(consoles[k], kindOf(k), ins) || consoles[k].svs_input } :
      { name: '', url: '', other: '', svs_input: n || 0, enabled: true };
    q('gdil').hidden = !ins.length;
    q('gdi').innerHTML = '<option value=0>Not on the SVS</option>' + ins.map((p, i) => '<option value=' + (i + 1) + '>Input ' + (i + 1) + (p.name ? ' · ' + esc(p.name) : '') + '</option>').join('');
    q('gdi').value = String(draft.svs_input <= ins.length ? draft.svs_input : 0);
    q('gdu').value = shortUrl(draft.url);
    q('gdn').value = draft.name;
    q('gde').checked = draft.enabled;
    q('gdr').hidden = k < 0;
    q('gdsv').textContent = k < 0 ? 'Add' : 'Save';
    status('gdm', '');
    dialogFields();
    q('gdlg').showModal();
    q('gdu').focus();
  }

  // What depends on the input it's on: the title, its name (an input's console's own), what a game not in
  // your games loads.
  function dialogFields() {
    const sv = svsInfo(), n = q('gdil').hidden ? 0 : +q('gdi').value;
    q('gdt').textContent = n ? 'gameID for input ' + n + ' · ' + inputName(sv, n) : draftK < 0 ? 'Add a console' : draft.name;
    q('gds').textContent = 'The game it says it runs picks the profile' + (n ? ', while input ' + n + ' is on screen.' : '.');
    q('gdnl').hidden = !!n;
    const own = n && sv.files[n] ? plain(sv.files[n]) : '';
    q('gdokt').textContent = own ? 'Keeps the input\'s own, ' + own : n ? 'Keeps the input\'s own profile' : 'Keeps the profile the RT4K has';
    q('gdok').checked = !draft.other;
    q('gdoo').checked = !!draft.other;
    q('gdoot').textContent = draft.other ? 'Loads ' + plain(draft.other) : 'Loads another profile';
    q('gdop').textContent = draft.other ? 'Change…' : 'Pick…';
  }

  async function pickOther() {
    const p = await pickProfile(draft.other);
    if (p) draft.other = p;
    dialogFields();
  }

  async function saveConsole() {
    const sv = svsInfo(), n = q('gdil').hidden ? draft.svs_input : +q('gdi').value;
    const c = { name: n && !q('gdil').hidden ? inputName(sv, n) : q('gdn').value.trim(), url: consoleUrl(q('gdu').value), other: draft.other, svs_input: n, enabled: q('gde').checked };
    if (!c.name) return status('gdm', 'It needs a name', true);
    const bad = urlProblem(c.url);
    if (bad) return status('gdm', bad, true);
    const list = consoles.slice();
    if (draftK >= 0) list[draftK] = c; else list.push(c);
    if (await saveConsoles(list)) q('gdlg').close();
  }

  async function removeConsole() {
    const c = consoles[draftK];
    if (!(await window.askUser('Remove ' + c.name + '\'s gameID?', 'Cruller stops asking it which game it runs. Your games stay.', 'Remove', true))) return;
    if (await saveConsoles(consoles.filter((x, k) => k !== draftK))) q('gdlg').close();
  }

  // --- your games -------------------------------------------------------------------------------------

  function renderGames() {
    if (!built) return;
    const shown = filterGames(games, filter);
    const row = (g) => '<tr data-id="' + esc(g.id) + '"' + (g.id === playing ? ' class=os title="On screen"' : '') + '><td><input data-f=name value="' + esc(g.name) + '" maxlength=47 aria-label="Name" placeholder="(no name)"></td>' +
      '<td class=mono>' + esc(g.id) + '</td>' +
      '<td><button type=button class=gidp data-f=profile>' + esc(plain(g.profile)) + '</button></td>' +
      '<td class=act><button type=button class="ib del" data-a=del aria-label="Remove ' + esc(g.name || g.id) + '" title="Remove it">×</button></td></tr>';
    const add = adding ? '<tr class=new><td><input id=gidan value="' + esc(adding.name) + '" maxlength=47 placeholder="Name" aria-label="Name"></td>' +
      '<td class=mono><input id=gidai value="' + esc(adding.id) + '" maxlength=63 placeholder="Its ID (SCUS-97481)" spellcheck=false aria-label="ID"></td>' +
      '<td><button type=button class=gidp id=gidap>' + (adding.profile ? esc(plain(adding.profile)) : '<span class=gidn>Pick its profile…</span>') + '</button></td>' +
      '<td class=act><button type=button class=primary id=gidas>Add</button><button type=button id=gidax>Cancel</button></td></tr>' : '';
    q('gidgt').innerHTML = add + shown.map(row).join('') +
      (!shown.length && !adding ? '<tr><td colspan=4 class=pe0>' + (games.length ? 'No game matches' : 'No games yet: when one is on screen, On screen adds it in a click.') + '</td></tr>' : '');
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
    renderLive();
  }

  async function putGame(g, note) {
    try {
      const r = await api('/api/v1/gameid/games', g);
      const k = games.findIndex((x) => x.id === g.id);
      if (k >= 0) games[k] = { ...g }; else games.push({ ...g });
      renderGames();
      renderLive();
      status('gidgm', note || (r.replaced ? 'Saved ' : 'Added ') + (g.name || g.id));
      return true;
    } catch (e) {
      status('gidgm', 'Not saved: ' + failure(e), true);
      return false;
    }
  }

  // A game to add (its ID and name from what's on screen or a console), its profile next.
  function startAdding(g) {
    adding = { id: g.id || '', name: g.name || '', profile: g.profile || '' };
    filter = '';
    q('gidq').value = '';
    renderGames();
    q('ggam').scrollIntoView({ block: 'nearest', behavior: 'smooth' });
    q(adding.id ? 'gidap' : 'gidai').focus();
  }

  // --- the view -------------------------------------------------------------------------------------

  function build() {
    built = true;
    q('ggam').innerHTML =
      '<div class="row sdh"><h2>Your games</h2><span id=gidgs class="small grow"></span><input id=gidq class=gidq placeholder="Find a game" autocomplete=off aria-label="Find a game">' +
      '<button id=gidga class=primary>Add a game</button></div>' +
      '<div id=gidgm class=small></div>' +
      '<div class=gidw><table class=gidt><thead><tr><th>Name</th><th>ID</th><th>Profile</th><th></th></tr></thead><tbody id=gidgt></tbody></table></div>' +
      '<div class=small>A game that isn\'t here keeps its input\'s own profile, or its gameID\'s for those.</div>';
    q('cons').insertAdjacentHTML('beforeend',
      '<dialog id=gdlg class=pick aria-labelledby=gdt><form method=dialog class=gdf>' +
      '<div><h3 id=gdt></h3><div id=gds class=small></div></div>' +
      '<label id=gdil>On the SVS<select id=gdi></select></label>' +
      '<label id=gdnl>Its name<input id=gdn maxlength=47 autocomplete=off placeholder="N64, PS1…: the console it is"></label>' +
      '<label>Its address<input id=gdu class=mono maxlength=120 spellcheck=false autocomplete=off placeholder="192.168.1.50"></label>' +
      '<div class=small style="margin-top:-6px">A MemCard PRO2 or PRO (its own web page on; on the PRO2, WebUI v2 off), a PS1Digital or an N64Digital: ' +
      'its IP alone will do. Only http for now.</div>' +
      '<fieldset><legend>A game that isn\'t in your games</legend>' +
      '<label class=gdo><input type=radio name=gdo id=gdok><span id=gdokt></span></label>' +
      '<label class=gdo><input type=radio name=gdo id=gdoo><span id=gdoot></span><button type=button id=gdop class=link></button></label></fieldset>' +
      '<label class=gdc><input type=checkbox id=gde>Ask it which game it runs</label>' +
      '<div id=gdm class=small></div>' +
      '<div class=gdb><button type=button id=gdr class=danger>Remove</button><span class=grow></span><button value=no>Cancel</button><button type=button id=gdsv class=primary></button></div>' +
      '</form></dialog>' +
      '<dialog id=gidpk class=pick aria-labelledby=gidpt><form method=dialog><h3 id=gidpt>Pick a profile</h3>' +
      '<nav id=gidpc class=crumbs aria-label=Folder></nav><div id=gidpl class=pepl></div>' +
      '<div class=row><span id=gidps class="small grow"></span><button value=no>Cancel</button></div></form></dialog>');

    // a console's gameID: opened from its card, added to an input or not on the SVS; a game added from it
    q('cons').addEventListener('click', (ev) => {
      const b = ev.target.closest('button');
      if (!b || b.disabled) return;
      if (b.classList.contains('gdev')) openConsole(+b.dataset.k);
      else if (b.dataset.a === 'addfor') openConsole(-1, +b.dataset.n);
      else if (b.dataset.a === 'addgame' && b.dataset.k !== undefined) {
        const k = +b.dataset.k, l = liveOf(k);
        if (l && l.game) startAdding({ id: l.game, name: l.game_name, profile: consoles[k].other });
      } else if (b.dataset.a === 'addgame' && nowAdd) startAdding(nowAdd);
    });
    q('gdi').onchange = dialogFields;
    q('gdok').onchange = () => { draft.other = ''; dialogFields(); };
    q('gdoo').onchange = pickOther;
    q('gdop').onclick = pickOther;
    q('gdsv').onclick = saveConsole;
    q('gdr').onclick = removeConsole;

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
        const p = await pickProfile(adding.profile);
        if (p) { adding.profile = p; renderGames(); }
      } else if (b.id === 'gidax') {
        adding = null;
        renderGames();
      } else if (b.id === 'gidas') {
        const g = { id: adding.id.trim(), name: adding.name.trim(), profile: adding.profile };
        if (!g.id) return status('gidgm', 'A game needs its ID: what its console reports', true);
        if (!profileOk(g.profile)) return status('gidgm', 'Pick its profile', true);
        if (games.some((x) => x.id === g.id) && !(await window.askUser('Replace ' + g.id + '?', 'Your games have it already: it will load this profile instead.', 'Replace', false))) return;
        if (await putGame(g)) { adding = null; renderGames(); }
      } else {
        const tr = b.closest('tr[data-id]'), g = tr && games.find((x) => x.id === tr.dataset.id);
        if (!g) return;
        if (b.dataset.f === 'profile') {
          const p = await pickProfile(g.profile);
          if (p && p !== g.profile) putGame({ ...g, profile: p }, (g.name || g.id) + ' loads ' + plain(p));
        } else if (b.dataset.a === 'del') {
          if (!(await window.askUser('Remove ' + (g.name || g.id) + '?', 'Its input\'s own profile stays for it instead, or its gameID\'s for games not in your games.', 'Remove', true))) return;
          try {
            await api('/api/v1/gameid/games/delete', { id: g.id });
            games = games.filter((x) => x !== g);
            renderGames();
            renderLive();
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
    if (!built) { build(); setInterval(tick, 2000); }
    tick();
    loadConsoles();
    loadGames();
  }

  function onStatus(s) {
    power = s.rt4k_power || power;
  }

  window.gidOpen = open;
  window.gidStatus = onStatus;
  window.gidSvs = renderLive; // app.js: the switch as the bridge said it now (the cards may be new)
  window.gameidInternals = { consoleUrl, shortUrl, urlProblem, profileOk, filterGames, readGame, liveText, inputOf, onScreen }; // tests/test_gameid_page.js
})();
