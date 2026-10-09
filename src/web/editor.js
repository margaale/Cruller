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

  // A setting kept once per input mode the RT4K detects (the trims, scaling, the ADC: 128) or per audio
  // input port (8) has each: {by, count, stride}, its first element in bytes[0]. Its bytes for element el.
  // With Sample Rate Detection locked on a rate, the RT4K keeps some per-mode settings (the trims, scaling,
  // Sub-Phase) per rate instead: each.srd is where their 32 slots start (same stride), slot the one picked
  // (-1: none, the mode's own).
  function bytesAt(setting, el, slot = -1) {
    const e = setting.each;
    if (e && e.srd !== undefined && slot >= 0) return setting.bytes.map(([o, l], i) => (i ? [o, l] : [e.srd + slot * e.stride, l]));
    if (!e || !el) return setting.bytes;
    return setting.bytes.map(([o, l], i) => (i ? [o, l] : [o + el * e.stride, l]));
  }

  // The 32 slots: 4 groups of 8, each a rate (the ADC's samples per line / 10, 8, 7, 6, 5, 4: the first 6) for
  // 240p, 480i, 288p and 576i (measured: 480i at 1/8, 1/5 and 1/4 in slots 9, 12 and 13; 240p as the 240p
  // consoles' profiles use the first group, 288p and 576i as the PAL profiles use the last two). Named as
  // the RT4K's mode line puts the rate ("A 686.40"), from the samples per line (spl) when known.
  const DIVS = [10, 8, 7, 6, 5, 4];
  const GROUPS = ['240p', '480i', '288p', '576i'];
  function rateName(k, spl) {
    const n = k & 7;
    if (n >= DIVS.length) return 'Slot ' + k;
    return GROUPS[k >> 3] + ' · ' + (spl > 0 ? (spl / DIVS[n]).toFixed(3) + ' (1/' + DIVS[n] + ')' : '1/' + DIVS[n]);
  }

  // The input modes the RT4K has named on its mode line here (0 with no signal, 1 a PS2's 480i component).
  const MODES = { 0: 'No Signal', 1: 'CP 480i' };
  const modeName = (k) => 'Mode ' + k + (MODES[k] ? ' · ' + MODES[k] : '');

  // A setting that applies only while another has some values (what the RT4K shows, or marks N/A, as that
  // one changes): when {path (if another menu's), section, label, is: [values]}. For each setting, the
  // index of the one it depends on, or -1.
  function links(settings) {
    return settings.map((s) => {
      const w = s.when;
      if (!w) return -1;
      return settings.findIndex((o) => o.path === (w.path || s.path) && o.label === w.label && (o.section || '') === (w.section || ''));
    });
  }

  // Where each setting shows: the main menu's as tiles; the advanced menu's in its tab and submenu (the
  // map's tabs: [tab, [[item, title]]], as the menu has them), in the map's order; any other in "Other".
  function layoutOf(m) {
    const tiles = [], tabs = (m.tabs || []).map(([tab, items]) => ({ tab, menus: items.map(([item, title]) => ({ item, title, idx: [] })) }));
    const menus = tabs.flatMap((t) => t.menus), other = { item: 'Other', title: '', idx: [] };
    m.settings.forEach((s, i) => {
      if (s.hidden) return; // (not laid out: the saving device's ID has a line of its own)
      if (/^RetroTINK-\S+ \S+ Main Menu/.test(s.path)) return tiles.push(i);
      const title = s.path.split(' › ').pop();
      (menus.find((x) => x.title === title) || other).idx.push(i);
    });
    if (other.idx.length) tabs.push({ tab: 'Other', menus: [other] });
    // the tiles as the main menu goes: Input Source, HDMI Output (its resolution first), the rest, Profiles
    const rank = (s) => (s.label === 'Input Source' ? 0 : / HDMI• Output$/.test(s.path) ? (s.label === 'Resolution' ? 1 : 2) : / Profiles$/.test(s.path) ? (s.readonly ? 5 : 4) : 3);
    tiles.sort((a, b) => rank(m.settings[a]) - rank(m.settings[b]) || a - b);
    return { tiles, tabs };
  }

  // The slots a profile has settings in: those where any per-rate setting isn't 0.
  function slotsUsed(settings, body) {
    const used = new Set();
    for (const s of settings) {
      if (!s.each || s.each.srd === undefined) continue;
      for (let k = 0; k < 32; k++) {
        const [o, l] = bytesAt(s, 0, k)[0];
        if (body.slice(o, o + l).some((b) => b)) used.add(k);
      }
    }
    return [...used].sort((a, b) => a - b);
  }

  // The input modes a profile has settings of its own for: those where any per-mode setting isn't 0.
  function modesUsed(settings, body) {
    const used = new Set();
    for (const s of settings) {
      if (!s.each || s.each.by !== 'mode') continue;
      for (let el = 0; el < s.each.count; el++) {
        const [o, l] = bytesAt(s, el)[0];
        if (body.slice(o, o + l).some((b) => b)) used.add(el);
      }
    }
    return [...used].sort((a, b) => a - b);
  }

  // The value a profile's body holds for a setting (for el, its element: mode or port; slot, a detected
  // rate's): a number, a list's value as shown, or null (bytes the map never saw, then hex says them).
  function decode(setting, codec, body, el = 0, slot = -1) {
    if (!setting.bytes) return { value: null, hex: '' };
    const bytes = bytesAt(setting, el, slot), hex = hexOf(body, bytes);
    if (codec.type === 'number') {
      const [off, len] = bytes[0], f = codec.fit;
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
  function encode(setting, codec, body, value, el = 0, slot = -1) {
    if (codec.readonly) return false;
    const bytes = bytesAt(setting, el, slot);
    if (codec.type === 'number') {
      const [off, len] = bytes[0], f = codec.fit;
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
        const [off, len] = bytes[i];
        for (let k = 0; k < len; k++) body[off + k] = parseInt(parts[i].substr(k * 2, 2), 16);
      }
      return true;
    }
    return false;
  }

  // --- state ------------------------------------------------------------------------------------------

  let doc = null;          // the settings map, once read
  let map = null, cs = []; // the map used and its codecs
  let pf = null;           // the profile open: {name, path (on the SD card, from its root) or '', header, body, orig, mode, port, slot}
  let power = '', fw = '';
  let busy = '';
  let filter = '';

  const asleep = () => power === 'standby' || power === 'starting';
  const plain = (name) => name.replace(/\.rt[46]$/i, '');

  // --- UI ---------------------------------------------------------------------------------------------
  //
  // Laid out as the RT4K's menus read: the main menu's settings as tiles, then the signal picked, then the
  // advanced menu (its tabs, its submenus, each with its headings), every value shown as the OSD shows
  // it and changed in place (a list's value opens its list, a number takes a new one: arrows step it).
  // What a setting means or needs is its line's tooltip; what doesn't apply is dimmed.

  let ui = { tab: '', menu: '' }; // the tab and submenu shown (the submenu's title)
  let lay = null, dep = [];        // the map's layout (layoutOf) and links (links), with map

  function status(text, bad) {
    const s = q('pes');
    s.textContent = text;
    s.classList.toggle('bad', !!bad);
  }

  const changed = () => !!pf && pf.body.some((b, i) => b !== pf.orig[i]);

  function build() {
    q('pe').innerHTML =
      '<div class="panel pep">' +
      '<div class="row sdh"><h2 class=grow id=pen>Profile editor</h2>' +
      '<div class=row><button id=peo>Open a file…</button><input type=file id=pef accept=".rt4,.rt6" hidden>' +
      '<button id=peu>Undo</button><button id=ped>Download</button><button id=pesv class=primary>Save to the SD card…</button></div></div>' +
      '<div id=pes class=small></div>' +
      '<div id=pebody class=peg hidden>' +
      '<div class=pehd><span id=pename class=pename></span><span class=grow></span>' +
      '<span id=pedev class=pedev><span class=pelab>Device ID</span><span id=pedid class=mono></span><button id=pedc class=pemini title="Empty the ID of the RT4K that saved it">Clear</button></span></div>' +
      '<div id=petl class=petl></div>' +
      '<div class=pebar><span class=pegrp><span class=pelab>Input signal mode</span><select id=pesig class=pev></select></span>' +
      '<label class=pelab title="A change to a setting kept per mode or per rate goes to every mode and rate"><input type=checkbox id=peall> Every mode</label>' +
      '<span class=grow></span><span class=pegrp><span class=pelab>Audio input</span><select id=peport class=pev title="The settings kept per audio input (Audio Input) show this one\'s"></select></span></div>' +
      '<div class=pebar><span class=pelab>Advanced settings</span><div id=petabs class=peseg></div><span class=grow></span>' +
      '<input id=peq class=pefind placeholder="Find a setting" autocomplete=off></div>' +
      '<div class=pew><nav id=penav class=penav></nav><div id=pepane class=pepane></div></div></div>' +
      '<div id=pe0 class=pe0>Open a profile from this computer, or pick one to edit in the <a class=more href="#rt4k/profiles">Profiles</a> view.</div>' +
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
    q('pesig').onchange = () => {
      if (!pf) return;
      const v = q('pesig').value, k = +v.slice(1);
      if (v[0] === 'm') { pf.mode = k; pf.slot = -1; } else pf.slot = k;
      render();
    };
    q('peport').onchange = () => { if (pf) { pf.port = +q('peport').value; render(); } };
    q('pedc').onclick = () => {
      const s = map && map.settings[deviceAt()];
      if (!pf || !s) return;
      for (const [o, n] of s.bytes) pf.body.fill(0, o, o + n);
      render();
    };
    q('pe').addEventListener('change', (ev) => {
      const el = ev.target.closest('[data-i]');
      if (!el || !pf) return;
      const i = +el.dataset.i;
      write(i, cs[i].type === 'number' ? +el.value : el.value);
    });
    q('pe').addEventListener('click', (ev) => {
      const b = ev.target.closest('button');
      if (!b || !pf) return;
      if (b.dataset.tab !== undefined) {
        ui.tab = b.dataset.tab;
        ui.menu = lay.tabs.find((t) => t.tab === ui.tab).menus[0].title;
        render();
      } else if (b.dataset.menu !== undefined) {
        ui.menu = b.dataset.menu;
        render();
      }
    });
    addEventListener('beforeunload', (ev) => { if (changed() || busy) { ev.preventDefault(); ev.returnValue = ''; } });
  }

  // Writes setting i's value where the signal picked keeps it (every mode and rate, if asked).
  function write(i, v) {
    const s = map.settings[i];
    const every = s.each && s.each.by === 'mode' && q('peall').checked;
    const where = every ? Array.from({ length: s.each.count }, (x, k) => [k, -1]) : [at(s)];
    if (every && s.each.srd !== undefined) for (let k = 0; k < 32; k++) if ((k & 7) < DIVS.length) where.push([0, k]);
    if (!where.every(([k, slot]) => encode(s, cs[i], pf.body, v, k, slot))) status('That value can\'t be written.', true);
    render();
  }

  const label = (s) => (s.section ? s.section + ' › ' : '') + s.label;

  // The audio input ports, as the input source's port byte says them (measured: HD-15 0, RCA 1, SCART 2,
  // HDMI 4).
  const PORTS = ['HD-15', 'RCA', 'SCART', 'Port 3', 'HDMI', 'Port 5', 'Port 6', 'Port 7'];
  const PORT_AT = 0x57eb; // input_port, beside the input source (0x57e9) on struct ver 109

  // The saving device's ID: in the map, not laid out with the rest (only Clear changes it).
  const deviceAt = () => map.settings.findIndex((s) => s.hidden && s.label === 'Saved on device');

  const elementOf = (s) => (!s.each ? 0 : s.each.by === 'mode' ? pf.mode : pf.port);
  const slotOf = (s) => (s.each && s.each.srd !== undefined ? pf.slot : -1);
  const at = (s) => [elementOf(s), slotOf(s)];

  // The ADC's samples per line in the mode picked: what the rates are a fraction of.
  function samplesPerLine() {
    const i = map.settings.findIndex((s) => s.label === 'Samples per Line' && s.each);
    return i < 0 ? 0 : decode(map.settings[i], cs[i], pf.body, pf.mode).value || 0;
  }

  // Whether setting i applies, as the one it depends on is set (where the signal picked has it): '' when it
  // does, else why not.
  function whyNot(i) {
    const j = dep[i];
    if (j < 0) return '';
    const s = map.settings[i], o = map.settings[j], v = decode(o, cs[j], pf.body, ...at(o)).value;
    if (s.when.is.includes(v)) return '';
    const not = (o.values || []).map(([x]) => x).filter((x) => !s.when.is.includes(x)); // (the shorter way to say it)
    return not.length < s.when.is.length ? 'Not with ' + o.label + ': ' + not.join(' or ') : 'Only with ' + o.label + ': ' + s.when.is.join(' or ');
  }

  // Whether setting i differs from the profile as opened: where the signal picked keeps it (here), or
  // anywhere (any mode, rate or input).
  const differs = (o, n) => pf.body.slice(o, o + n).some((b, k) => b !== pf.orig[o + k]);
  const changedHere = (i) => { const s = map.settings[i]; return !!s.bytes && bytesAt(s, ...at(s)).some(([o, n]) => differs(o, n)); };
  function changedAnywhere(i) {
    const s = map.settings[i];
    if (!s.bytes) return false;
    const e = s.each, ranges = s.bytes.slice(e ? 1 : 0).filter((r) => !derived(r));
    if (e) ranges.push([s.bytes[0][0], e.count * e.stride]);
    if (e && e.srd !== undefined) ranges.push([e.srd, 32 * e.stride]);
    return ranges.some(([o, n]) => differs(o, n));
  }

  // A value as the OSD shows it, changed in place: a list opens its values, a number takes one (arrows step).
  function control(s, c, i) {
    const d = decode(s, c, pf.body, ...at(s)), off = busy || c.readonly ? ' disabled' : '';
    const tone = /^Off\b/.test(d.value) ? ' off' : '';
    if (c.type === 'number') {
      const w = Math.max(String(c.min).length, String(c.max).length, String(d.value).length) + 2;
      return '<input type=number class="pev' + tone + '" data-i=' + i + ' min=' + c.min + ' max=' + c.max + ' step=' + c.step + ' style="width:' + w + 'ch" value="' +
        (d.value === null ? '' : d.value) + '" title="' + c.min + ' to ' + c.max + '"' + off + '>';
    }
    if (c.type === 'list') {
      const opts = c.values.map(([v]) => '<option' + (v === d.value ? ' selected' : '') + '>' + esc(v) + '</option>').join('');
      return '<select class="pev' + tone + '" data-i=' + i + off + '>' + (d.value === null ? '<option selected disabled>? (' + esc(d.hex) + ')</option>' : '') + opts + '</select>';
    }
    return '<span class="pev mono">' + esc(d.hex) + '</span>';
  }

  // What a line's tooltip says: why it doesn't apply, what it means, what it's kept per, what it asks.
  function tip(i) {
    const s = map.settings[i];
    const per = !s.each ? '' : slotOf(s) >= 0 ? 'Kept per detected rate' : s.each.by === 'mode' ? 'Kept per input mode' : 'Kept per audio input';
    return [whyNot(i), s.note, per, s.asks ? 'The RT4K asks first: ' + s.asks : ''].filter(Boolean).join('\n');
  }

  // One setting's line: its name, its value.
  function line(i, name) {
    const s = map.settings[i], t = tip(i);
    return '<div class="per' + (changedHere(i) ? ' chg' : '') + (whyNot(i) ? ' na' : '') + '"' + (t ? ' title="' + esc(t) + '"' : '') + '>' +
      '<span class="pln' + (s.note || s.asks ? ' tip' : '') + '">' + esc(name || s.label) + '</span>' + control(s, cs[i], i) + '</div>';
  }

  // Settings under their headings: the one above each in the menu, or what named says.
  function groups(idx, named) {
    const by = new Map();
    for (const i of idx) {
      const s = map.settings[i], h = named ? named(i) : s.heading || s.section || '';
      if (!by.has(h)) by.set(h, []);
      by.get(h).push(i);
    }
    return [...by].map(([h, l]) => '<section class=peh>' + (h ? '<h3>' + esc(h) + '</h3>' : '') + l.map((i) => line(i, named ? label(map.settings[i]) : '')).join('') + '</section>').join('');
  }

  function render() {
    const has = !!pf;
    q('pebody').hidden = !has;
    q('pe0').hidden = has;
    q('pen').textContent = 'Profile editor';
    q('ped').disabled = !has || !!busy;
    q('pesv').disabled = !has || !!busy || asleep();
    q('peu').disabled = !changed() || !!busy;
    if (!has) return;
    q('pename').textContent = plain(pf.name) + (changed() ? ' (changed)' : '');

    // the ID of the RT4K that saved it: shown, and emptied with Clear (nothing else writes it)
    const dev = map.settings[deviceAt()], id = dev ? hexOf(pf.body, dev.bytes) : '';
    q('pedev').hidden = !dev;
    q('pedid').textContent = /[1-9a-f]/.test(id) ? id : '(empty)';
    q('pedc').disabled = !/[1-9a-f]/.test(id) || !!busy;
    q('pedev').classList.toggle('chg', !!dev && dev.bytes.some(([o, n]) => differs(o, n)));

    // the main menu's settings, as tiles
    q('petl').innerHTML = lay.tiles.map((i) => {
      const s = map.settings[i], t = tip(i);
      return '<div class="pti' + (changedHere(i) ? ' chg' : '') + (whyNot(i) ? ' na' : '') + '"' + (t ? ' title="' + esc(t) + '"' : '') + '><span class=pelab>' + esc(s.label) + '</span>' + control(s, cs[i], i) + '</div>';
    }).join('');

    // the signal: the input modes and rates the profile has settings for, then the others
    const modes = modesUsed(map.settings, pf.body), rates = slotsUsed(map.settings, pf.body), spl = samplesPerLine();
    const picked = (v) => (v[0] === 'm' ? pf.slot < 0 && +v.slice(1) === pf.mode : +v.slice(1) === pf.slot);
    const opt = (v, text) => '<option value=' + v + (picked(v) ? ' selected' : '') + '>' + esc(text) + '</option>';
    const allRates = Array.from({ length: 32 }, (x, k) => k).filter((k) => (k & 7) < DIVS.length);
    q('pesig').innerHTML =
      '<optgroup label="In this profile">' + [...modes.map((k) => opt('m' + k, modeName(k))), ...rates.map((k) => opt('s' + k, rateName(k, spl)))].join('') + '</optgroup>' +
      '<optgroup label="Input modes">' + Array.from({ length: 128 }, (x, k) => k).filter((k) => !modes.includes(k)).map((k) => opt('m' + k, modeName(k))).join('') + '</optgroup>' +
      '<optgroup label="Detected sample rates">' + allRates.filter((k) => !rates.includes(k)).map((k) => opt('s' + k, rateName(k, spl))).join('') + '</optgroup>';
    q('pesig').title = pf.slot >= 0
      ? 'With Sample Rate Detection locked on this rate, the RT4K takes the trims, scaling and Sub-Phase from it, the rest from ' + modeName(pf.mode) + ' (pick a mode to change that)'
      : 'The settings kept per input mode show this one\'s';
    q('peport').innerHTML = PORTS.map((n, k) => '<option value=' + k + (k === pf.port ? ' selected' : '') + '>' + n + (k === pf.body[PORT_AT] ? ' (this profile\'s)' : '') + '</option>').join('');

    // a search: every setting found, under its submenu; else the tab and submenu picked
    const menus = lay.tabs.flatMap((t) => t.menus);
    q('penav').hidden = !!filter;
    if (!menus.some((m) => m.title === ui.menu)) { ui.tab = lay.tabs[0].tab; ui.menu = lay.tabs[0].menus[0].title; }
    const count = (idx) => idx.filter(changedAnywhere).length, badge = (n) => (n ? ' <span class=peb>' + n + '</span>' : '');
    q('petabs').innerHTML = lay.tabs.map((t) => '<button data-tab="' + esc(t.tab) + '"' + (!filter && t.tab === ui.tab ? ' aria-selected=true' : '') + '>' + esc(t.tab) + badge(count(t.menus.flatMap((m) => m.idx))) + '</button>').join('');
    if (filter) {
      const where = new Map(menus.flatMap((m) => m.idx.map((i) => [i, m.item])));
      const found = map.settings.map((s, i) => i).filter((i) => !map.settings[i].hidden && (label(map.settings[i]) + ' ' + map.settings[i].path).toLowerCase().includes(filter));
      q('pepane').innerHTML = groups(found, (i) => where.get(i) || 'Main menu') || '<div class=pe0>No setting matches</div>';
      return;
    }
    const tab = lay.tabs.find((t) => t.tab === ui.tab);
    q('penav').innerHTML = tab.menus.map((m) => '<button data-menu="' + esc(m.title) + '"' + (m.title === ui.menu ? ' aria-current=true' : '') + '>' + esc(m.item) + badge(count(m.idx)) + '</button>').join('');
    q('pepane').innerHTML = groups(menus.find((m) => m.title === ui.menu).idx) || '<div class=pe0>No settings mapped here</div>';
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
      lay = layoutOf(map);
      dep = links(map.settings);
      // the mode shown first: the profile's own (one with settings, other than 0, the one with none); no
      // detected rate (the mode's own); the audio input: the one the profile is for
      const used = modesUsed(map.settings, p.body);
      pf = { name, path, header: p.header, body: p.body.slice(), orig: p.body.slice(), mode: used.find((k) => k) || 0, slot: -1, port: p.body[PORT_AT] < PORTS.length ? p.body[PORT_AT] : 0 };
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
  window.editorInternals = { crc16, parseProfile, buildProfile, mapFor, fitNumber, codecs, decode, encode, asNumber, bytesAt, modesUsed, slotsUsed, rateName, modeName, links, layoutOf, HEADER }; // tests/test_editor.js
})();
