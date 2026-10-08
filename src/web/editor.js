// The RT4K profile editor for the Cruller page (served as /editor.js, embedded at build time).
//
// A profile (.rt4, .rt6) is a 128-byte header ("RT4K Profile", then at 0x20 a CRC-16/XMODEM of the
// body, little-endian) and the body: the settings as the RT4K runs them (what sget sends, 22876 bytes
// on 1.92 and 1.93). The settings map (/rt4k_settings.json, made with the Debug tab's Settings map)
// says where each setting lives and how; the editor shows them as the RT4K's menus do, writes the ones
// changed, and leaves every other byte as it was. Opened from the SD card (GET /rt4k/get) or from a
// file on this computer, saved to either (POST /rt4k/put, or a download).

(() => {
  'use strict';

  const q = (id) => document.getElementById(id);
  const esc = (s) => String(s).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));

  // --- pure helpers (tests/test_editor.js) --------------------------------------------------------------

  const HEADER = 128;
  const MAGIC = 'RT4K Profile';
  const CRC_AT = 0x20;

  // CRC-16/XMODEM (poly 0x1021, from 0): what the RT4K checks a profile's body with.
  function crc16(bytes) {
    let c = 0;
    for (const x of bytes) {
      c ^= x << 8;
      for (let k = 0; k < 8; k++) c = c & 0x8000 ? ((c << 1) ^ 0x1021) & 0xffff : (c << 1) & 0xffff;
    }
    return c;
  }

  // A profile file: {header, body, crcOk}, or null when it isn't one.
  function parseProfile(data) {
    const d = data instanceof Uint8Array ? data : new Uint8Array(data);
    if (d.length <= HEADER || String.fromCharCode(...d.slice(0, MAGIC.length)) !== MAGIC) return null;
    const header = d.slice(0, HEADER), body = d.slice(HEADER);
    return { header, body, crcOk: (header[CRC_AT] | header[CRC_AT + 1] << 8) === crc16(body) };
  }

  // The file again: the header as it came, with the body's CRC.
  function buildProfile(header, body) {
    const out = new Uint8Array(HEADER + body.length);
    out.set(header.slice(0, HEADER));
    out.set(body, HEADER);
    const c = crc16(body);
    out[CRC_AT] = c & 0xff;
    out[CRC_AT + 1] = c >> 8;
    return out;
  }

  // The map for a firmware: its own, else the newest (same layout so far: sget's ver says).
  function mapFor(doc, firmware) {
    const maps = (doc && doc.maps) || [];
    const newer = (a, b) => b.firmware.localeCompare(a.firmware, undefined, { numeric: true });
    return maps.find((m) => m.firmware === firmware) || maps.slice().sort(newer)[0] || null;
  }

  // A value as a number ("+3", "-0.50", "1.000 (Rmax: 30)", "+-100", "3432 dots/line"), as mapper.js.
  const asNumber = (v) => { const m = /^([+-]?\d+(?:\.\d+)?)\s*[a-z%/]*(?:\s*\(.*\))?$/i.exec(String(v).trim().replace(/^\+-/, '-')); return m ? +m[1] : NaN; };

  const hexOf = (body, bytes) => bytes.map(([off, len]) => Array.from(body.slice(off, off + len), (b) => b.toString(16).padStart(2, '0')).join('')).join('|');

  function readRaw(body, off, len, kind) {
    const v = new DataView(body.buffer, body.byteOffset + off, len);
    if (kind === 'f32') return v.getFloat32(0, true);
    let n = 0;
    for (let i = len - 1; i >= 0; i--) n = n * 256 + body[off + i];
    return kind === 'int' && n >= 2 ** (8 * len - 1) ? n - 2 ** (8 * len) : n;
  }

  function writeRaw(body, off, len, kind, raw) {
    if (kind === 'f32') return new DataView(body.buffer, body.byteOffset + off, 4).setFloat32(0, raw, true);
    let n = Math.round(raw);
    if (n < 0) n += 2 ** (8 * len);
    for (let i = 0; i < len; i++) { body[off + i] = n % 256; n = Math.floor(n / 256); }
  }

  const rawOfHex = (hex, kind) => {
    const b = Uint8Array.from(hex.match(/../g).map((h) => parseInt(h, 16)));
    return readRaw(b, 0, b.length, kind);
  };

  // How a number is kept, from the values the map saw: raw = a * value + b, raw a little-endian int
  // (signed or not) or a float32. Null when none fits them all (or there are fewer than two).
  function fitNumber(setting) {
    if (setting.min === undefined || !setting.bytes || setting.bytes.length !== 1) return null;
    const len = setting.bytes[0][1];
    const pts = (setting.values || []).map(([shown, hex]) => [asNumber(shown), hex]).filter(([v, hex]) => !isNaN(v) && /^[0-9a-f]+$/.test(hex) && hex.length === len * 2);
    if (pts.length < 2) return null;
    const lo = pts.reduce((a, p) => (p[0] < a[0] ? p : a)), hi = pts.reduce((a, p) => (p[0] > a[0] ? p : a));
    if (lo[0] === hi[0]) return null;
    for (const kind of len === 4 ? ['int', 'uint', 'f32'] : ['int', 'uint']) {
      const r1 = rawOfHex(lo[1], kind), r2 = rawOfHex(hi[1], kind);
      let a = (r2 - r1) / (hi[0] - lo[0]), b = r1 - a * lo[0];
      if (!a || !isFinite(a)) continue;
      if (kind === 'f32') { // (the RT4K's floats drift as it steps them: Input Gain 1.00 is 0, not -5e-7)
        a = +a.toPrecision(6);
        b = Math.round(b / (setting.step * Math.abs(a))) * setting.step * Math.abs(a);
      }
      const tol = kind === 'f32' ? (r) => 1e-3 * Math.max(1, Math.abs(r)) : () => 0.5;
      if (pts.every(([v, hex]) => { const r = rawOfHex(hex, kind); return Math.abs(a * v + b - r) <= tol(r); })) return { kind, a, b };
    }
    return null;
  }

  const decimals = (step) => { const s = String(step); return s.includes('.') ? s.split('.')[1].length : 0; };

  // Each setting's codec: {type: 'number', fit, min, max, step} or {type: 'list', values} or {type: 'raw'}.
  // A number with no values of its own borrows the fit of one with the same range and step that lives in
  // the same bytes (Sample Rate Detection's ADC Sample Rate is the ADC's Samples per Line), or that sits
  // beside it with as many (the colour matrix's eight walked one step).
  function codecs(settings) {
    const fits = settings.map(fitNumber);
    return settings.map((s, i) => ({ ...codecOf(s, i), readonly: !!s.readonly })); // (readonly: shown, never written)
    function codecOf(s, i) {
      let fit = fits[i];
      if (!fit && s.min !== undefined && s.bytes && s.bytes.length === 1) {
        const like = (o) => o.min === s.min && o.max === s.max && o.step === s.step && o.bytes[0][1] === s.bytes[0][1];
        const j = [(o) => o.bytes[0][0] === s.bytes[0][0], (o) => o.path === s.path && o.section === s.section]
          .map((how) => settings.findIndex((o, k) => fits[k] && like(o) && how(o))).find((k) => k >= 0);
        if (j !== undefined) fit = fits[j];
      }
      if (fit) return { type: 'number', fit, min: s.min, max: s.max, step: s.step };
      if (s.values && s.values.length) {
        const parts = (s.bytes || []).map((r, k) => k).filter((k) => !derived(s.bytes[k]));
        return { type: 'list', values: s.values, parts: parts.length ? parts : s.bytes.map((r, k) => k) };
      }
      return { type: 'raw' };
    }
  }

  // The scaler's factors (0xca8..0xcc8 on 1.92/1.93): worked out from the other settings, so a list
  // seen with them (Scaling Mode, Buffer Length) is read and written without them.
  const derived = ([off, len]) => off < 0xcc8 && off + len > 0xca8;
  const pick = (hex, parts) => { const p = hex.split('|'); return parts.map((k) => p[k]).join('|'); };

  // The value a profile's body holds for a setting: a number, a list's value as shown, or null (bytes
  // the map never saw, then hex says them).
  function decode(setting, codec, body) {
    if (!setting.bytes) return { value: null, hex: '' };
    const hex = hexOf(body, setting.bytes);
    if (codec.type === 'number') {
      const [off, len] = setting.bytes[0], f = codec.fit;
      const v = (readRaw(body, off, len, f.kind) - f.b) / f.a, d = decimals(codec.step);
      return { value: +v.toFixed(d), hex };
    }
    if (codec.type === 'list') {
      const mine = pick(hex, codec.parts), hit = codec.values.find((x) => pick(x[1], codec.parts) === mine);
      return { value: hit ? hit[0] : null, hex };
    }
    return { value: null, hex };
  }

  // Writes a value into the body (a copy is the caller's): a number within its range (an int to the
  // nearest the RT4K keeps, its step shown rounded: the ADC gains' 0.004 is 0.0039; a float to its step),
  // a list's value as shown. False when it can't.
  function encode(setting, codec, body, value) {
    if (codec.readonly) return false;
    if (codec.type === 'number') {
      const [off, len] = setting.bytes[0], f = codec.fit;
      let v = Math.min(codec.max, Math.max(codec.min, +value));
      if (isNaN(v)) return false;
      if (f.kind === 'f32') v = +(codec.min + Math.round((v - codec.min) / codec.step) * codec.step).toFixed(decimals(codec.step));
      writeRaw(body, off, len, f.kind, f.a * v + f.b);
      return true;
    }
    if (codec.type === 'list') {
      const hit = codec.values.find((x) => x[0] === value);
      if (!hit) return false;
      const parts = hit[1].split('|');
      for (const i of codec.parts) {
        const [off, len] = setting.bytes[i];
        for (let k = 0; k < len; k++) body[off + k] = parseInt(parts[i].substr(k * 2, 2), 16);
      }
      return true;
    }
    return false;
  }

  // --- state ------------------------------------------------------------------------------------------

  let doc = null;          // the settings map, once read
  let map = null, cs = []; // the map used and its codecs
  let pf = null;           // the profile open: {name, path (on the SD card, from its root) or '', header, body, orig}
  let power = '', fw = '';
  let busy = '';
  let filter = '';

  const asleep = () => power === 'standby' || power === 'starting';
  const plain = (name) => name.replace(/\.rt[46]$/i, '');

  // --- UI ---------------------------------------------------------------------------------------------

  function status(text, bad) {
    const s = q('pes');
    s.textContent = text;
    s.classList.toggle('bad', !!bad);
  }

  const changed = () => !!pf && pf.body.some((b, i) => b !== pf.orig[i]);

  function build() {
    q('pe').innerHTML =
      '<div class=panel style="max-width:1100px">' +
      '<div class="row sdh"><h2 class=grow id=pen>Profile editor</h2>' +
      '<div class=row><button id=peo>Open a file…</button><input type=file id=pef accept=".rt4,.rt6" hidden>' +
      '<button id=ped>Download</button><button id=pesv class=primary>Save to the SD card…</button></div></div>' +
      '<div id=pes class=small></div>' +
      '<div id=pem class=small></div>' +
      '<div id=pebody hidden><div class=row><input id=peq placeholder="Find a setting" class=grow autocomplete=off>' +
      '<button id=peu>Undo the changes</button></div><div id=peg></div></div>' +
      '<div id=pe0 class=empty>Open a profile from this computer, or pick one to edit in the <a class=more href="#rt4k/profiles">Profiles</a> view.</div>' +
      '<div class=small>Only the settings changed are written; every other byte stays as the profile had it. The settings ' +
      'and where they live come from mapping the RT4K\'s menus (Debug tab, Settings map).</div></div>';
    q('peo').onclick = () => q('pef').click();
    q('pef').onchange = async () => {
      const f = q('pef').files[0];
      q('pef').value = '';
      if (f) openData(f.name, '', new Uint8Array(await f.arrayBuffer()));
    };
    q('ped').onclick = download;
    q('pesv').onclick = saveToSd;
    q('peu').onclick = () => { if (pf) { pf.body = pf.orig.slice(); render(); } };
    q('peq').oninput = () => { filter = q('peq').value.trim().toLowerCase(); render(); };
    q('peg').onchange = (ev) => {
      const el = ev.target.closest('[data-i]');
      if (!el || !pf) return;
      const i = +el.dataset.i;
      if (!encode(map.settings[i], cs[i], pf.body, cs[i].type === 'number' ? +el.value : el.value)) status('That value can\'t be written.', true);
      render();
    };
    addEventListener('beforeunload', (ev) => { if (changed() || busy) { ev.preventDefault(); ev.returnValue = ''; } });
  }

  const label = (s) => (s.section ? s.section + ' › ' : '') + s.label;

  function control(s, c, i) {
    const d = decode(s, c, pf.body), off = busy || c.readonly ? ' disabled' : '';
    if (c.type === 'number') {
      return '<input type=number data-i=' + i + ' min=' + c.min + ' max=' + c.max + ' step=' + c.step + ' value="' + (d.value === null ? '' : d.value) + '"' + off + '>' +
        '<span class=small>' + c.min + ' to ' + c.max + '</span>';
    }
    if (c.type === 'list') {
      const opts = c.values.map(([v]) => '<option' + (v === d.value ? ' selected' : '') + '>' + esc(v) + '</option>').join('');
      return '<select data-i=' + i + off + '>' + (d.value === null ? '<option selected disabled>? (' + esc(d.hex) + ')</option>' : '') + opts + '</select>';
    }
    return '<span class="small mono">' + esc(d.hex) + '</span>';
  }

  function render() {
    const has = !!pf;
    q('pebody').hidden = !has;
    q('pe0').hidden = has;
    q('pen').textContent = has ? plain(pf.name) + (changed() ? ' (changed)' : '') : 'Profile editor';
    q('ped').disabled = !has || !!busy;
    q('pesv').disabled = !has || !!busy || asleep();
    q('peu').disabled = !changed() || !!busy;
    if (!has) return;
    q('pem').textContent = map ? 'Settings from firmware ' + map.firmware + '\'s map' + (fw && fw !== map.firmware ? ' (the RT4K runs ' + fw + ': same layout)' : '') + ' · ' +
      map.settings.length + ' settings' : '';
    const groups = new Map();
    map.settings.forEach((s, i) => {
      if (filter && !(label(s) + ' ' + s.path).toLowerCase().includes(filter)) return;
      const menu = s.path.replace(/^RetroTINK-\S+ \S+ (Advanced Menu › )?/, ''); // "Scaling/Crop Setup", "Main Menu › HDMI• Output"
      if (!groups.has(menu)) groups.set(menu, []);
      groups.get(menu).push(i);
    });
    const was = new Set([...q('peg').querySelectorAll('details[open]')].map((d) => d.dataset.m)); // (kept open as they were)
    q('peg').innerHTML = [...groups].map(([menu, idx]) => '<details data-m="' + esc(menu) + '"' + (filter || was.has(menu) ? ' open' : '') + '><summary>' + esc(menu) + ' <span class=small>' + idx.length + '</span></summary><table class="tbl pet">' +
      idx.map((i) => {
        const s = map.settings[i], c = cs[i], mod = s.bytes && s.bytes.some(([o, n]) => pf.body.slice(o, o + n).some((b, k) => b !== pf.orig[o + k]));
        return '<tr' + (mod ? ' class=chg' : '') + '><td>' + esc(label(s)) + (s.asks ? ' <span class="small bad" title="' + esc(s.asks) + '">asks first on the RT4K</span>' : '') +
          '</td><td class=r>' + control(s, c, i) + '</td></tr>';
      }).join('') + '</table></details>').join('') || '<div class=empty>No setting matches</div>';
  }

  async function readMap() {
    if (doc) return;
    const r = await fetch('/rt4k_settings.json');
    if (!r.ok) throw new Error('the settings map: HTTP ' + r.status);
    doc = await r.json();
  }

  async function openData(name, path, data) {
    try {
      await readMap();
      const p = parseProfile(data);
      if (!p) throw new Error(name + ' is not an RT4K profile');
      map = mapFor(doc, fw);
      if (!map) throw new Error('there is no settings map');
      if (p.body.length !== map.size) throw new Error(name + ' has ' + p.body.length + ' bytes of settings, the map knows ' + map.size);
      cs = codecs(map.settings);
      pf = { name, path, header: p.header, body: p.body.slice(), orig: p.body.slice() };
      status(p.crcOk ? 'Opened ' + plain(name) + (path ? ' from the SD card' : ' from this computer') : plain(name) + '\'s CRC doesn\'t match: the RT4K wouldn\'t load it as it is (saving writes it right).', !p.crcOk);
    } catch (e) {
      status('Could not open it: ' + e.message, true);
    }
    render();
  }

  async function openSd(path) {
    if (asleep()) return status('The RT4K is asleep: turn it on to read its SD card, or open a file from this computer.', true);
    status('Reading ' + plain(path.split('/').pop()) + '…');
    try {
      const r = await fetch('/rt4k/get?path=' + encodeURIComponent(path));
      if (!r.ok) throw new Error((await r.text()).trim() || 'HTTP ' + r.status);
      await openData(path.split('/').pop(), path, new Uint8Array(await r.arrayBuffer()));
    } catch (e) {
      status('Could not read it: ' + (e.message === 'Failed to fetch' ? 'Cruller did not answer' : e.message), true);
    }
  }

  function download() {
    if (!pf) return;
    const a = document.createElement('a');
    a.href = URL.createObjectURL(new Blob([buildProfile(pf.header, pf.body)], { type: 'application/octet-stream' }));
    a.download = pf.name;
    a.click();
    setTimeout(() => URL.revokeObjectURL(a.href), 1000);
  }

  // Saves to the SD card: where it came from, or /profile; under the name given (the same replaces it,
  // asked first), and loaded if wanted.
  async function saveToSd() {
    if (!pf || busy || asleep()) return;
    const dir = pf.path ? pf.path.split('/').slice(0, -1).join('/') : 'profile';
    const typed = await window.askText('Save to the SD card', 'The profile goes to /' + dir + ' under this name.', plain(pf.name), 'Save');
    if (typed === null || !typed.trim()) return;
    const name = /\.rt[46]$/i.test(typed.trim()) ? typed.trim() : typed.trim() + (pf.name.match(/\.rt[46]$/i) || ['.rt4'])[0];
    const path = dir + '/' + name;
    if (path.toLowerCase() === (pf.path || '').toLowerCase() &&
      !(await window.askUser('Replace ' + plain(name) + '?', 'The profile on the SD card will hold these settings instead.', 'Replace', true))) return;
    const load = await window.askUser('Load it now?', 'The RT4K can load ' + plain(name) + ' once it\'s saved.', 'Load it', false);
    busy = 'save';
    render();
    try {
      const data = buildProfile(pf.header, pf.body);
      status('Saving ' + plain(name) + '…');
      const r = await fetch('/rt4k/put?path=' + encodeURIComponent(path) + '&sha=' + window.sha256(data), { method: 'POST', body: data });
      if (!r.ok) throw new Error((await r.text()).trim() || 'HTTP ' + r.status);
      pf = { ...pf, name, path, orig: pf.body.slice() };
      let note = 'Saved ' + plain(name);
      if (load) {
        status('Loading ' + plain(name) + '…');
        const a = await fetch('/rt4k/ask?expect=prof&timeout=15000', { method: 'POST', body: 'prof load ' + path.replace(/^profile\//, '') });
        const t = (await a.text()).trim();
        note += t === 'prof load ok' ? ', and loaded' : ', but the RT4K did not load it (' + t + ')';
      }
      status(note, / did not /.test(note));
    } catch (e) {
      status('Saving failed: ' + (e.message === 'Failed to fetch' ? 'Cruller did not answer' : e.message), true);
    }
    busy = '';
    render();
  }

  // The page shows the editor: parts, a profile's path under /profile (#rt4k/editor/<path>), or none.
  async function open(parts) {
    if (!q('pebody')) build();
    render();
    const path = (parts || []).map(decodeURIComponent).join('/');
    if (!path || (pf && pf.path === 'profile/' + path)) return;
    if (changed() && !(await window.askUser('Leave the changes unsaved?', plain(pf.name) + ' has changes not saved: opening another drops them.', 'Open it', true))) return;
    openSd('profile/' + path);
  }

  function onStatus(s) {
    const was = power;
    power = s.rt4k_power || power;
    fw = s.rt4k_fw || fw;
    if (q('pebody') && was !== power) render();
  }

  window.peOpen = open;
  window.peStatus = onStatus;
  window.editorInternals = { crc16, parseProfile, buildProfile, mapFor, fitNumber, codecs, decode, encode, asNumber, HEADER }; // tests/test_editor.js
})();
