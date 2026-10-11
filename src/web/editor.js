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

  const bytesOfHex = (hex) => Uint8Array.from(hex.match(/../g) || [], (h) => parseInt(h, 16));
  const rawOfHex = (hex, kind) => { const b = bytesOfHex(hex); return readRaw(b, 0, b.length, kind); };

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
        const all = s.bytes.map((r, k) => k), own = all.filter((k) => !derived(s.bytes[k]));
        const parts = own.length ? own : all;
        return { type: 'list', values: s.values, parts, match: s.match || parts }; // (match: the parts a value is read by; all are written)
      }
      return { type: 'raw' };
    }
  }

  // The scaler's factors (0xca8..0xcc4 on 1.92/1.93, the TW9912's Brightness after them): worked out from
  // the other settings, so a list seen with them (Scaling Mode) is read and written without them.
  const derived = ([off, len]) => off < 0xcc4 && off + len > 0xca8;
  const pick = (hex, parts) => { const p = hex.split('|'); return parts.map((k) => p[k]).join('|'); };

  // A setting kept once per input mode the RT4K detects (the trims, scaling, the ADC: 128) or per audio
  // input port (8) has each: {by, count, stride}, its first element in bytes[0]. Its bytes for element el.
  // With Sample Rate Detection locked on a rate, the RT4K keeps some per-mode settings (the trims, scaling,
  // Sub-Phase) per rate instead: each.srd is where their 32 slots start (same stride), slot the one picked
  // (-1: none, the mode's own).
  function bytesAt(setting, el, slot = -1) {
    const e = setting.each, rate = e && e.srd !== undefined && slot >= 0;
    if (!rate && (!e || !el)) return setting.bytes;
    return setting.bytes.map(([o, l], i) => (i ? [o, l] : [rate ? e.srd + slot * e.stride : o + el * e.stride, l]));
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

  // The input modes' names, and the one each group of rates belongs to (its samples per line name the rates;
  // the RT4K takes the settings not kept per rate from it): taken from PIPe's RT4K Profiler
  // (https://rt4k-profiler.pipe.hr/, its build of 2026-10-08), as the user chose on 2026-10-09. Measured here
  // too: 0 with no signal, 1 a PS2's 480i component. Slots 20 to 68 it has as reserved for future modes, and
  // none for 94, 95, 125 to 127.
  const MODES = {
    0: 'No Signal', 1: 'CP 480i', 2: 'CP 240p', 3: 'CP 576i', 4: 'CP 288p', 5: 'CP 480p', 6: 'CP 576p', 7: 'CP 720p', 8: 'CP 1080i', 9: 'CP 1080p',
    10: 'DOS 400p70', 11: 'DOS 350p70', 12: 'SVGA 800x600', 13: 'XGA 1024x768', 14: 'VESA 1280x960', 15: 'VESA 1280x1024', 16: 'VESA 1600x1200',
    17: 'VGA 640x480', 18: 'Unknown', 19: 'PC 1368x768', 69: 'Custom Input Mode No Slot',
    96: 'SDP 240p', 97: 'SDP 480i', 98: 'SDP 288p', 99: 'SDP 576i',
    100: 'HDMI® Custom', 101: 'HDMI® 480i', 102: 'HDMI® 240p', 103: 'HDMI® 480p', 104: 'HDMI® 576i', 105: 'HDMI® 288p', 106: 'HDMI® 480i (SR)',
    107: 'HDMI® 240p (SR)', 108: 'HDMI® 576i (SR)', 109: 'HDMI® 288p (SR)', 110: 'HDMI® 576p', 111: 'HDMI® 720p', 112: 'HDMI® 1080i', 113: 'HDMI® 1080p',
    114: 'HDMI® 640x480', 115: 'HDMI® 800x600', 116: 'HDMI® 1024x768', 117: 'HDMI® 1280x1024', 118: 'HDMI® GBI', 119: 'HDMI® 960i',
    120: 'MiSTer 240p', 121: 'MiSTer 480i', 122: 'MiSTer 288p', 123: 'MiSTer 576i', 124: 'MiSTer Gen.',
  };
  for (let k = 0; k < 24; k++) MODES[70 + k] = 'Custom Input Mode ' + (k + 1);
  const modeName = (k) => 'Mode ' + k + (MODES[k] ? ' · ' + MODES[k] : '');
  const GROUP_MODE = [2, 1, 4, 3]; // 240p, 480i, 288p, 576i
  const modeOfRate = (k) => GROUP_MODE[k >> 3];

  // A setting that applies only while another has some values (what the RT4K shows, or marks N/A, as that
  // one changes): when {path (if another menu's), section, label, is: [values]}. The index of the one it
  // depends on (in path's menu when it doesn't say), or -1.
  const whenAt = (settings, w, path) => settings.findIndex((o) => o.path === (w.path || path) && o.label === w.label && (o.section || '') === (w.section || ''));
  const links = (settings) => settings.map((s) => (s.when ? whenAt(settings, s.when, s.path) : -1));

  // Where each setting shows: the main menu's as tiles; the advanced menu's in its tab and submenu (the
  // map's tabs: [tab, [[item, title, when]]], as the menu has them; when: the input it needs, as a setting's,
  // dep the setting's index), in the map's order; any other in "Other".
  function layoutOf(m) {
    const tiles = [], tabs = (m.tabs || []).map(([tab, items]) => ({ tab, menus: items.map(([item, title, when]) => ({ item, title, when, dep: when ? whenAt(m.settings, when) : -1, idx: [] })) }));
    const menus = tabs.flatMap((t) => t.menus), other = { item: 'Other', title: '', dep: -1, idx: [] };
    m.settings.forEach((s, i) => {
      if (s.hidden) return; // (not laid out: the saving device's ID has a line of its own)
      if (/^RetroTINK-\S+ \S+ Main Menu/.test(s.path)) return tiles.push(i);
      const title = s.path.split(' › ').pop();
      (menus.find((x) => x.title === title) || other).idx.push(i);
    });
    if (other.idx.length) tabs.push({ tab: 'Other', menus: [other] });
    // the tiles as the main menu goes: Input Source, HDMI Output (its resolution first), the rest, Profiles
    const rank = (s) => (s.label === 'Input Source' ? 0 : / HDMI• Output$/.test(s.path) ? (s.label === 'Output Resolution' ? 1 : 2) : / Profiles$/.test(s.path) ? (s.readonly ? 5 : 4) : 3);
    tiles.sort((a, b) => rank(m.settings[a]) - rank(m.settings[b]) || a - b);
    return { tiles, tabs };
  }

  // Whether a setting is kept per input mode (perRate false) or per detected rate (true).
  const keptPer = (s, perRate) => !!s.each && (perRate ? s.each.srd !== undefined : s.each.by === 'mode');

  // The input modes (perRate false) or detected rates a profile has settings for: those where any setting
  // kept per mode (or per rate) isn't 0.
  function used(settings, body, perRate) {
    const out = new Set();
    for (const s of settings) {
      if (!keptPer(s, perRate)) continue;
      for (let k = 0; k < (perRate ? 32 : s.each.count); k++) {
        if (out.has(k)) continue;
        const [o, l] = perRate ? bytesAt(s, 0, k)[0] : bytesAt(s, k)[0];
        for (let i = o; i < o + l; i++) if (body[i]) { out.add(k); break; }
      }
    }
    return [...out].sort((a, b) => a - b);
  }
  const slotsUsed = (settings, body) => used(settings, body, true);
  const modesUsed = (settings, body) => used(settings, body, false);

  // Empties an input mode's settings (el), or a detected rate's (slot): back to none, so the RT4K uses its
  // defaults there, as a profile that never had them. Only that element of each setting kept per mode (or
  // per rate); true when anything was set.
  function clearSignal(settings, body, el, slot = -1) {
    let any = false;
    for (const s of settings) {
      if (!keptPer(s, slot >= 0)) continue;
      const [o, l] = bytesAt(s, el, slot)[0];
      for (let k = o; k < o + l; k++) if (body[k]) { body[k] = 0; any = true; }
    }
    return any;
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
      const mine = pick(hex, codec.match), hit = codec.values.find((x) => pick(x[1], codec.match) === mine);
      return { value: hit ? hit[0] : null, hex };
    }
    return { value: null, hex };
  }

  // What several profiles hold for a setting (decode's, one each): mixed when they differ (by value; by
  // bytes where the map doesn't know them), and which they have (each value once, in their order).
  function agree(ds) {
    const key = (d) => (d.value === null ? 'h' + d.hex : 'v' + d.value);
    const kinds = [...new Map(ds.map((d) => [key(d), d])).values()];
    return { mixed: kinds.length > 1, kinds };
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
      for (const i of codec.parts) body.set(bytesOfHex(parts[i]).subarray(0, bytes[i][1]), bytes[i][0]);
      return true;
    }
    return false;
  }

  // --- state ------------------------------------------------------------------------------------------

  let doc = null;          // the settings map, once read
  let map = null, cs = []; // the map used and its codecs
  let files = [];          // the profiles open, listed beside the editor: {name, path (on the SD card, from its root) or '', header, body, orig, crcOk}
  let pf = null;           // the one shown: edited alone, or with Multi Edit the one whose settings say what applies
  let multi = false;       // Multi Edit: the profiles ticked are edited together (pf one of them)
  let ticked = new Set();
  const group = () => (!pf ? [] : multi ? files.filter((p) => ticked.has(p)) : [pf]); // the profiles a change goes to
  let view = { mode: 0, slot: -1, port: 0 }; // the input mode, detected rate and audio input whose settings show
  let power = '', fw = '';
  let busy = false;
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
  let devAt = -1, splAt = -1;      // with map: the saving device's ID (hidden: only Clear changes it), the ADC's samples per line

  function status(text, bad) {
    const s = q('pes');
    s.textContent = text;
    s.classList.toggle('bad', !!bad);
  }

  // Whether a profile's bytes from o (n of them; all by default) differ from those it was opened with.
  const edited = (p, o = 0, n = p.body.length) => { for (let k = o; k < o + n; k++) if (p.body[k] !== p.orig[k]) return true; return false; };
  const touched = (p) => edited(p);
  const changed = () => files.some(touched);

  function build() {
    q('pe').innerHTML =
      '<div class="panel pep">' +
      '<div class="row sdh"><h2 id=pen>Profile editor</h2><span id=pes class="small grow"></span></div>' +
      '<div class=pewrap>' +
      // the profiles open, and what's done with them
      '<div class=peside><aside class=pefiles aria-label="Profiles open">' +
      '<div class=pefh><span class=pelab>Profiles</span><label class=pemt title="Tick several profiles: a change goes to all of them"><input type=checkbox id=pem role=switch> Multi Edit</label></div>' +
      '<div id=pefl class=pefl></div>' +
      '<div class=pefb><div class=pefa><button id=pea>Add from the SD card…</button><button id=peo>Add from this computer…</button>' +
      '<input type=file id=pef accept=".rt4,.rt6" multiple hidden></div>' +
      '<div class=pefa><div class=row><button id=peu class=grow>Undo</button><button id=ped class=grow>Download</button></div>' +
      '<button id=pesv class=primary>Save to the SD card…</button></div></div></aside></div>' +
      '<div id=pebody class=peg hidden>' +
      '<div class=pehd><span id=peset class=pename></span><span class=grow></span>' +
      '<span id=pedev class=pedev><span class=pelab>Device ID</span><span id=pedid class=mono></span><button id=pedc class=pemini title="Empty the ID of the RT4K that saved it">Clear</button></span></div>' +
      '<div id=petl class=petl></div>' +
      '<div class=pebar><span class=pegrp><span class=pelab>Input signal mode</span><button type=button id=pesig class="pev pdd" aria-haspopup=listbox></button></span>' +
      '<button type=button id=pemadd class=pemini aria-haspopup=listbox title="Edit a mode or rate the profile has no settings for yet">Add mode…</button>' +
      '<button type=button id=pemdel class=pemini title="Empty its settings: the RT4K uses its defaults there">Delete mode</button>' +
      '<label class=pelab title="A change to a setting kept per mode or per rate goes to every mode and rate"><input type=checkbox id=peall> Every mode</label>' +
      '</div>' +
      '<div class=pebar><span class=pelab>Advanced Settings</span><div id=petabs class=peseg></div><span class=grow></span>' +
      '<input id=peq class=pefind placeholder="Find a setting" autocomplete=off></div>' +
      '<div class=pew><nav id=penav class=penav></nav><div id=pepane class=pepane></div></div></div>' +
      '<div id=pe0 class=pe0>Add a profile from the SD card or this computer, or pick some in the <a class=more href="#rt4k/sd/profile">SD card</a> ' +
      'view (its /profile folder): one to edit, or several ticked to edit together.</div></div>' +
      '</div>' +
      '<dialog id=peadd class=pick aria-labelledby=pept><form method=dialog><h3 id=pept>Add from the SD card</h3>' +
      '<nav id=pepc class=crumbs aria-label=Folder></nav><div id=pepl class=pepl></div>' +
      '<div class=row><span id=peps class="small grow"></span><button value=no>Cancel</button><button value=yes id=pepok class=primary disabled>Add</button></div></form></dialog>';
    q('peo').onclick = () => q('pef').click();
    q('pef').onchange = async () => {
      const chosen = [...q('pef').files];
      q('pef').value = '';
      for (const f of chosen) await openData(f.name, '', new Uint8Array(await f.arrayBuffer()));
    };
    q('ped').onclick = download;
    q('pesv').onclick = () => (group().length > 1 ? saveAll() : saveToSd());
    q('peu').onclick = () => { for (const p of group()) p.body = p.orig.slice(); render(); };
    q('pea').onclick = addProfiles;
    // Multi Edit: on, every profile open ticked to start with; off, the one shown alone
    q('pem').onchange = () => {
      multi = q('pem').checked;
      if (multi) ticked = new Set(files);
      render();
    };
    q('peq').oninput = () => { filter = q('peq').value.trim().toLowerCase(); render(); };
    q('pesig').onclick = () => { if (pf) openList(q('pesig'), signals(), view.slot >= 0 ? 's' + view.slot : 'm' + view.mode, toSignal); };
    q('pemadd').onclick = () => { if (pf) openList(q('pemadd'), others(), undefined, toSignal); };
    q('pemdel').onclick = deleteSignal;
    q('pedc').onclick = () => {
      const s = map && map.settings[devAt];
      if (!pf || !s) return;
      for (const p of group()) for (const [o, n] of s.bytes) p.body.fill(0, o, o + n);
      render();
    };
    // the list: a name shows that profile (with Multi Edit, it's ticked too); its tick adds it to those edited
    // together or leaves it out (one stays); × closes it
    q('pefl').onclick = (ev) => {
      const b = ev.target.closest('button');
      if (!b || busy) return;
      const p = files[+b.dataset.k];
      if (b.dataset.show !== undefined) showOne(p);
      else if (b.dataset.close !== undefined) closeFile(p);
    };
    q('pefl').onchange = (ev) => {
      const c = ev.target.closest('input[data-k]'), p = c && files[+c.dataset.k];
      if (!p) return;
      if (c.checked) ticked.add(p);
      else if (ticked.size > 1) {
        ticked.delete(p);
        if (pf === p) pf = files.find((x) => ticked.has(x));
      }
      render();
    };
    q('pe').addEventListener('change', (ev) => {
      const el = ev.target.closest('[data-i]');
      if (!el || !pf) return;
      const i = +el.dataset.i;
      if (cs[i].type === 'number' && el.value.trim() === '') return render(); // (emptied: nothing to write)
      write(i, cs[i].type === 'number' ? +el.value : el.value);
    });
    // − and +: one step, and on while held (a setting's whole range, the trims' ±4096, takes a while one by one)
    let held = null;
    const stop = () => { if (held) { clearTimeout(held.t); clearInterval(held.r); held = null; } };
    const step = (i, dir) => { // (from the value shown: when they differ, the one pf has)
      const c = cs[i], d = valueOf(i);
      const v = +Math.min(c.max, Math.max(c.min, (d.value === null ? c.min : d.value) + dir * c.step)).toFixed(decimals(c.step));
      if (v !== d.value || d.mixed) write(i, v);
    };
    q('pe').addEventListener('pointerdown', (ev) => {
      const b = ev.target.closest('button.pestep');
      if (!b || b.disabled || !pf || ev.button) return;
      ev.preventDefault();
      stop();
      const i = +b.dataset.i, dir = +b.dataset.step;
      step(i, dir);
      held = { t: setTimeout(() => { held.r = setInterval(() => step(i, dir), 60); }, 400) };
    });
    addEventListener('pointerup', stop);
    addEventListener('scroll', fitSide, { passive: true }); // (the list beside the editor kept in sight)
    addEventListener('resize', fitSide);
    addEventListener('pointercancel', stop);
    q('pe').addEventListener('click', (ev) => {
      const b = ev.target.closest('button');
      if (!b || !pf) return;
      if (b.classList.contains('pestep')) {
        if (!ev.detail) step(+b.dataset.i, +b.dataset.step); // (a key, not the pointer: that stepped already)
      } else if (b.id === 'peport') {
        openList(b, PORTS.map((n, k) => ({ value: k, label: n })), view.port, (v) => { view.port = v; render(); });
      } else if (b.classList.contains('pdd') && b.dataset.i !== undefined) {
        const i = +b.dataset.i, d = valueOf(i);
        openList(b, cs[i].values.map(([v]) => ({ value: v, label: v })), d.mixed ? undefined : d.value, (v) => write(i, v));
      } else if (b.dataset.tab !== undefined) {
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

  // Writes setting i's value where the signal picked keeps it (every mode and rate, if asked), in the
  // profile shown or, with Multi Edit, every one ticked.
  function write(i, v) {
    const s = map.settings[i];
    const every = s.each && s.each.by === 'mode' && q('peall').checked;
    const where = every ? Array.from({ length: s.each.count }, (x, k) => [k, -1]) : [at(s)];
    if (every && s.each.srd !== undefined) for (const k of RATES) where.push([0, k]);
    if (!group().every((p) => where.every(([k, slot]) => encode(s, cs[i], p.body, v, k, slot)))) status('That value can\'t be written.', true);
    if (s.label === 'Input Source' && pf.body[PORT_AT] < PORTS.length) view.port = pf.body[PORT_AT]; // (its audio settings show, as the new input's)
    render();
  }

  // What the profiles edited hold for setting i, where the signal picked keeps it: pf's value and bytes,
  // mixed when theirs differ, all: each one's.
  function valueOf(i) {
    const g = group(), s = map.settings[i], w = at(s), all = g.map((p) => decode(s, cs[i], p.body, ...w));
    return { ...all[g.indexOf(pf)], mixed: g.length > 1 && agree(all).mixed, all };
  }
  const shownAs = (d) => (d.value === null ? '? (' + d.hex + ')' : String(d.value));

  // Lines for a tooltip or a question: the first 12, and how many more.
  const first12 = (lines) => (lines.length > 12 ? lines.slice(0, 12).concat('and ' + (lines.length - 12) + ' more') : lines).join('\n');

  // Who holds what, for a tooltip: a name a line.
  const whoHas = (values) => first12(group().map((p, k) => plain(p.name) + ': ' + values[k]));

  const label = (s) => (s.section ? s.section + ' › ' : '') + s.label;

  // The audio input ports, as the input source's port byte says them (measured with every input: HD-15 0,
  // RCA 1, SCART 2, Front 3, HDMI 4; the arrays have 8, the last 3 no input's).
  const PORTS = ['HD-15', 'RCA', 'SCART', 'Front', 'HDMI'];
  const PORT_AT = 0x57eb; // input_port, beside the input source (0x57e9) on struct ver 109

  const elementOf = (s) => (!s.each ? 0 : s.each.by === 'mode' ? view.mode : view.port);
  const slotOf = (s) => (s.each && s.each.srd !== undefined ? view.slot : -1);
  const at = (s) => [elementOf(s), slotOf(s)];

  // The ADC's samples per line in an input mode: what that mode's rates are a fraction of.
  // With Multi Edit, the first one edited that has settings for that mode says it (one without has the
  // default), so a rate is named as when its profile is open alone.
  function samplesPerLine(mode) {
    if (splAt < 0) return 0;
    const p = group().find((x) => modesUsed(map.settings, x.body).includes(mode)) || pf;
    return decode(map.settings[splAt], cs[splAt], p.body, mode).value || 0;
  }
  const rateLabel = (k) => rateName(k, samplesPerLine(modeOfRate(k))); // (its group's mode's)

  // The value pf holds for setting j, where the signal picked keeps it.
  const pfValue = (j) => decode(map.settings[j], cs[j], pf.body, ...at(map.settings[j])).value;

  // Whether setting i applies, as the one it depends on is set (where the signal picked has it): '' when it
  // does, else why not.
  function whyNot(i) {
    const j = dep[i];
    if (j < 0) return '';
    const s = map.settings[i], o = map.settings[j];
    if (s.when.is.includes(pfValue(j))) return '';
    const not = (o.values || []).map(([x]) => x).filter((x) => !s.when.is.includes(x)); // (the shorter way to say it)
    return not.length < s.when.is.length ? 'Not with ' + o.label + ': ' + not.join(' or ') : 'Only with ' + o.label + ': ' + s.when.is.join(' or ');
  }

  // Whether setting i differs from the profiles edited as they were opened (any of them): where the signal
  // picked keeps it (here), or anywhere (any mode, rate or input).
  const differs = (o, n) => group().some((p) => edited(p, o, n));
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
  // Profiles that differ show Mixed (who has what in the line's tooltip) until a value is picked for all.
  // d: what they hold (valueOf).
  function control(i, d) {
    const c = cs[i], off = busy || c.readonly ? ' disabled' : '';
    const tone = d.mixed ? ' mix' : /^Off\b/.test(d.value) ? ' off' : '';
    if (c.type === 'number') {
      const w = Math.max(String(c.min).length, String(c.max).length, String(d.value).length, d.mixed ? 5 : 0) + 2;
      return '<span class=pen><button type=button class=pestep data-step=-1 data-i=' + i + off + ' aria-label="Less" tabindex=-1>−</button>' +
        '<input type=number class="pev' + tone + '" data-i=' + i + ' min=' + c.min + ' max=' + c.max + ' step=' + c.step + ' style="width:' + w + 'ch" value="' +
        (d.value === null || d.mixed ? '' : d.value) + '"' + (d.mixed ? ' placeholder=Mixed' : '') + ' title="' + c.min + ' to ' + c.max + '"' + off + '>' +
        '<button type=button class=pestep data-step=1 data-i=' + i + off + ' aria-label="More" tabindex=-1>+</button></span>';
    }
    if (c.type === 'list') {
      return '<button type=button class="pev pdd' + tone + '" data-i=' + i + ' aria-haspopup=listbox' + off + '><span>' + esc(d.mixed ? 'Mixed' : shownAs(d)) + '</span></button>';
    }
    return '<span class="pev mono' + tone + '">' + esc(d.mixed ? 'Mixed' : d.hex) + '</span>';
  }

  // A list's values under the button that opens it: the one set marked, the rest to pick from (a click,
  // or the arrows and Enter); Escape or a click elsewhere closes it. items: [{value, label} | {group}].
  let pop = null;
  function closeList() {
    if (!pop) return;
    pop.el.remove();
    pop.anchor.setAttribute('aria-expanded', 'false');
    removeEventListener('mousedown', pop.away, true);
    removeEventListener('scroll', pop.away, true);
    removeEventListener('resize', pop.away);
    pop = null;
  }
  function openList(anchor, items, current, pick) {
    const again = pop && pop.anchor === anchor;
    closeList();
    if (again) return; // (a second click on it closes it)
    const el = document.createElement('div');
    el.className = 'pelist';
    el.setAttribute('role', 'listbox');
    el.tabIndex = -1;
    el.innerHTML = items.map((it, k) => (it.group !== undefined ? '<div class=pelg>' + esc(it.group) + '</div>' :
      '<div role=option data-k=' + k + ' class="peo' + (it.value === current ? ' on' : '') + '" aria-selected=' + (it.value === current) + '>' + esc(it.label) + '</div>')).join('');
    document.body.appendChild(el);
    const r = anchor.getBoundingClientRect(), below = innerHeight - r.bottom - 8, above = r.top - 8;
    const h = Math.min(el.scrollHeight, 340, Math.max(below, above));
    el.style.minWidth = Math.max(r.width, 140) + 'px';
    el.style.maxHeight = h + 'px';
    el.style.left = Math.max(8, Math.min(r.left, innerWidth - el.offsetWidth - 8)) + scrollX + 'px';
    el.style.top = (below >= h || below >= above ? r.bottom + 4 : r.top - 4 - h) + scrollY + 'px';
    const opts = [...el.querySelectorAll('.peo')];
    let act = Math.max(0, opts.findIndex((o) => o.classList.contains('on')));
    const show = () => { opts.forEach((o, k) => o.classList.toggle('act', k === act)); if (opts[act]) opts[act].scrollIntoView({ block: 'nearest' }); };
    show();
    const choose = (o) => { const it = items[+o.dataset.k]; closeList(); anchor.focus(); if (it.value !== current) pick(it.value); };
    el.onclick = (ev) => { const o = ev.target.closest('.peo'); if (o) choose(o); };
    el.onmousemove = (ev) => { const o = ev.target.closest('.peo'); if (o && opts.indexOf(o) !== act) { act = opts.indexOf(o); opts.forEach((x, k) => x.classList.toggle('act', k === act)); } };
    el.onkeydown = (ev) => {
      if (ev.key === 'ArrowDown' || ev.key === 'ArrowUp') { act = Math.max(0, Math.min(opts.length - 1, act + (ev.key === 'ArrowDown' ? 1 : -1))); show(); }
      else if (ev.key === 'Home' || ev.key === 'End') { act = ev.key === 'Home' ? 0 : opts.length - 1; show(); }
      else if (ev.key === 'Enter' || ev.key === ' ') { if (opts[act]) choose(opts[act]); }
      else if (ev.key === 'Escape' || ev.key === 'Tab') { closeList(); anchor.focus(); }
      else return;
      ev.preventDefault();
    };
    const away = (ev) => { const t = ev && ev.target; if (!(t instanceof Node) || !(el.contains(t) || anchor.contains(t))) closeList(); }; // (a resize's target is the window)
    addEventListener('mousedown', away, true);
    addEventListener('scroll', away, true);
    addEventListener('resize', away);
    anchor.setAttribute('aria-expanded', 'true');
    pop = { el, anchor, away };
    el.focus({ preventScroll: true });
  }

  // The input modes and detected rates the profiles edited have settings for (any of them).
  const usedIn = (used) => [...new Set(group().flatMap((p) => used(map.settings, p.body)))].sort((a, b) => a - b);
  const RATES = Array.from({ length: 32 }, (x, k) => k).filter((k) => (k & 7) < DIVS.length);

  // The signal picker's items: those the profiles have settings for (and the one shown); Add mode… has the others.
  function signals() {
    const modes = usedIn(modesUsed), rates = usedIn(slotsUsed);
    if (!modes.includes(view.mode)) modes.push(view.mode);
    if (view.slot >= 0 && !rates.includes(view.slot)) rates.push(view.slot);
    const by = (a, b) => a - b;
    return [{ group: 'Input modes' }, ...modes.sort(by).map((k) => ({ value: 'm' + k, label: modeName(k) })),
      ...(rates.length ? [{ group: 'Detected sample rates' }, ...rates.sort(by).map((k) => ({ value: 's' + k, label: rateLabel(k) }))] : [])];
  }
  function others() {
    const modes = usedIn(modesUsed), rates = usedIn(slotsUsed);
    return [{ group: 'Input modes' }, ...Array.from({ length: 128 }, (x, k) => k).filter((k) => MODES[k] && !modes.includes(k) && k !== view.mode).map((k) => ({ value: 'm' + k, label: modeName(k) })),
      { group: 'Detected sample rates' }, ...RATES.filter((k) => !rates.includes(k) && k !== view.slot).map((k) => ({ value: 's' + k, label: rateLabel(k) }))];
  }
  // Shows a mode ('m<k>') or a detected rate ('s<k>': the rest from its group's mode, as the RT4K takes it).
  function toSignal(v) {
    const k = +v.slice(1);
    if (v[0] === 'm') { view.mode = k; view.slot = -1; } else { view.slot = k; view.mode = modeOfRate(k); }
    render();
  }

  // Deletes the mode or rate shown from the profiles edited that have it (asked first): its settings back to
  // none, the RT4K's defaults there. Then shows the first mode still with settings.
  async function deleteSignal() {
    if (!pf || busy) return;
    const rate = view.slot >= 0, what = rate ? rateLabel(view.slot) : modeName(view.mode);
    const has = group().filter((p) => (rate ? slotsUsed : modesUsed)(map.settings, p.body).includes(rate ? view.slot : view.mode));
    if (!has.length) return;
    if (!(await window.askUser('Delete ' + what + '?', (has.length > 1 ? has.length + ' profiles lose their' : plain(has[0].name) + ' loses its') +
      ' settings for it, changed ones too: the RT4K uses its defaults there.', 'Delete', true))) return;
    for (const p of has) clearSignal(map.settings, p.body, rate ? 0 : view.mode, rate ? view.slot : -1);
    if (rate) view.slot = -1;
    else { const left = usedIn(modesUsed); view.mode = left.find((k) => k) || left[0] || 0; }
    status('Deleted ' + what + (has.length > 1 ? ' from ' + has.length + ' profiles' : '') + ': not saved yet (Undo brings it back)');
    render();
  }

  // Why a submenu doesn't apply to the profile's input ('' when it does): the RT4K shows its lines N/A.
  function menuWhy(m) {
    if (m.dep < 0) return '';
    const v = pfValue(m.dep);
    return m.when.is.includes(v) ? '' : 'Not with ' + m.when.label + ': ' + v;
  }
  let offMenu = new Map(); // each setting of a submenu that doesn't apply: why (render)

  // One setting's line, or its tile (the main menu's): its name, its value; dimmed when it doesn't apply.
  // Its tooltip: who has what (Mixed), why it doesn't apply, what it means, what it's kept per, what it asks.
  function row(i, name, tile) {
    const s = map.settings[i], d = valueOf(i), why = offMenu.get(i) || whyNot(i);
    const per = !s.each ? '' : slotOf(s) >= 0 ? 'Kept per detected rate' : s.each.by === 'mode' ? 'Kept per input mode' : 'Kept per audio input';
    const t = [d.mixed ? 'Mixed:\n' + whoHas(d.all.map(shownAs)) : '', why, s.note || (cs[i].readonly ? 'Read-only' : ''), per, s.asks ? 'The RT4K asks first: ' + s.asks : ''].filter(Boolean).join('\n');
    return '<div class="' + (tile ? 'pti' : 'per') + (changedHere(i) ? ' chg' : '') + (why ? ' na' : '') + '"' + (t ? ' title="' + esc(t) + '"' : '') + '>' +
      '<span class="' + (tile ? 'pelab' : 'pln') + (s.note || s.asks ? ' tip' : '') + '">' + esc(name || s.label) + '</span>' + control(i, d) + '</div>';
  }

  // Whether setting i is gone from the menu as the one it depends on is set (another line in its place):
  // when.hide true, wherever it doesn't apply; or the values it's gone at (elsewhere it shows N/A).
  function gone(i) {
    const s = map.settings[i], h = s.when && s.when.hide;
    if (!h || !whyNot(i)) return false;
    return h === true || h.includes(pfValue(dep[i]));
  }

  // Settings under their headings: the one above each in the menu, or what named says.
  function groups(idx, named) {
    const by = new Map();
    for (const i of idx) {
      const s = map.settings[i], h = named ? named(i) : s.heading || s.section || '';
      if (!named && gone(i)) continue; // (a search still finds it)
      if (!by.has(h)) by.set(h, []);
      by.get(h).push(i);
    }
    return [...by].map(([h, l]) => '<section class=peh>' + (h ? '<h3>' + esc(h) + '</h3>' : '') + l.map((i) => row(i, named ? label(map.settings[i]) : '')).join('') + '</section>').join('');
  }

  // The profiles open, one a line: the one shown marked, a changed one in colour; with Multi Edit a tick for
  // each (the ticked edited together).
  function renderFiles() {
    q('pem').checked = multi;
    q('pem').disabled = files.length < 2 || busy;
    q('pefl').innerHTML = files.map((p, k) => '<div class="pefr' + (p === pf ? ' on' : '') + (touched(p) ? ' chg' : '') + '">' +
      (multi ? '<input type=checkbox data-k=' + k + (ticked.has(p) ? ' checked' : '') + (ticked.has(p) && ticked.size < 2 ? ' disabled' : '') + ' aria-label="Edit ' + esc(plain(p.name)) + ' with the others">' : '') +
      '<button type=button class=pefn data-k=' + k + ' data-show title="' + esc((p.path ? '/' + p.path : 'From this computer') + (touched(p) ? '\nChanged, not saved' : '')) + '">' + esc(plain(p.name)) + '</button>' +
      '<button type=button class=pex data-k=' + k + ' data-close aria-label="Close ' + esc(plain(p.name)) + '" title="Close it">×</button></div>').join('') ||
      '<div class=pefe>None open</div>';
  }

  // The list beside the editor no taller than the window shows of its column, so all of it, its buttons at
  // its foot, stays in sight (sticky, it keeps to the window's top as the page scrolls; stacked, as tall as
  // it is).
  function fitSide() {
    const side = document.querySelector('.pefiles');
    if (!side || !side.offsetParent) return;
    if (getComputedStyle(side).position !== 'sticky') return void (side.style.maxHeight = '');
    const col = side.parentNode.getBoundingClientRect();
    side.style.maxHeight = Math.max(240, Math.min(innerHeight - 12, col.bottom) - Math.max(12, col.top)) + 'px';
  }

  function render() {
    const has = !!pf, g = group();
    q('pebody').hidden = !has;
    q('pe0').hidden = has;
    q('ped').disabled = !has || busy;
    q('pesv').disabled = !has || busy || asleep();
    q('pesv').textContent = g.length > 1 ? 'Save ' + g.length + ' to the SD card…' : 'Save to the SD card…';
    q('peu').disabled = !g.some(touched) || busy;
    q('pea').disabled = busy || asleep();
    q('peo').disabled = busy;
    renderFiles();
    fitSide(); // (its top: where the list starts, known already)
    if (!has) return;

    // what's edited: the profile shown, or how many with Multi Edit
    q('peset').textContent = g.length > 1 ? g.length + ' profiles together' + (g.some(touched) ? ' (changed)' : '') : plain(pf.name) + (touched(pf) ? ' (changed)' : '');
    q('peset').title = g.length > 1 ? plain(pf.name) + '\'s settings say what applies (the lines dimmed), and show where they differ' : pf.path ? '/' + pf.path : 'From this computer';

    // the ID of the RT4K that saved each: shown, and emptied with Clear (nothing else writes it)
    const dev = map.settings[devAt], ids = dev ? g.map((p) => hexOf(p.body, dev.bytes)) : [];
    const idOf = (id) => (/[1-9a-f]/.test(id) ? id : '(empty)'), idMixed = ids.some((id) => id !== ids[0]);
    q('pedev').hidden = !dev;
    q('pedid').textContent = idMixed ? 'Mixed' : idOf(ids[0] || '');
    q('pedid').title = idMixed ? whoHas(ids.map(idOf)) : '';
    q('pedid').classList.toggle('mix', idMixed);
    q('pedc').disabled = !ids.some((id) => /[1-9a-f]/.test(id)) || busy;
    q('pedev').classList.toggle('chg', !!dev && dev.bytes.some(([o, n]) => differs(o, n)));

    // the main menu's settings, as tiles
    const tiles = lay.tiles.map((i) => row(i, '', true));
    // the audio input whose settings show (Audio Input's, kept per port), beside the input: the profile's own first
    tiles.splice(lay.tiles.findIndex((i) => map.settings[i].label === 'Input Source') + 1, 0, '<div class=pti title="The settings kept per audio input (Audio Input) show this one\'s"><span class=pelab>Audio Input</span>' +
      '<button type=button id=peport class="pev pdd" aria-haspopup=listbox><span>' + esc(PORTS[view.port]) + '</span></button></div>');
    q('petl').innerHTML = tiles.join('');

    // the signal picked (a mode, or a rate)
    closeList();
    q('pesig').innerHTML = '<span>' + esc(view.slot >= 0 ? rateLabel(view.slot) : modeName(view.mode)) + '</span>';
    const shown = view.slot >= 0 ? usedIn(slotsUsed).includes(view.slot) : usedIn(modesUsed).includes(view.mode);
    q('pemdel').disabled = !shown || busy; // (one the profiles have no settings for: nothing to delete)
    q('pemdel').textContent = view.slot >= 0 ? 'Delete rate' : 'Delete mode';
    q('pemadd').disabled = busy;
    q('pesig').title = view.slot >= 0
      ? 'With Sample Rate Detection locked on this rate, the RT4K takes the trims, scaling and Sub-Phase from it, the rest from its input mode, ' + modeName(view.mode)
      : 'The settings kept per input mode show this one\'s';

    // a search: every setting found, under its submenu; else the tab and submenu picked
    // each submenu: why it doesn't apply, and how many of its settings changed (its tab's badge adds them up)
    const menus = lay.tabs.flatMap((t) => t.menus), whys = new Map(menus.map((m) => [m, menuWhy(m)]));
    const changes = new Map(menus.map((m) => [m, m.idx.filter(changedAnywhere).length]));
    offMenu = new Map(menus.flatMap((m) => (whys.get(m) ? m.idx.map((i) => [i, whys.get(m)]) : [])));
    q('penav').hidden = !!filter;
    if (!menus.some((m) => m.title === ui.menu)) { ui.tab = lay.tabs[0].tab; ui.menu = lay.tabs[0].menus[0].title; }
    const badge = (n) => (n ? ' <span class=peb>' + n + '</span>' : '');
    q('petabs').innerHTML = lay.tabs.map((t) => '<button data-tab="' + esc(t.tab) + '"' + (!filter && t.tab === ui.tab ? ' aria-selected=true' : '') + '>' + esc(t.tab) +
      badge(t.menus.reduce((n, m) => n + changes.get(m), 0)) + '</button>').join('');
    if (filter) {
      const where = new Map(menus.flatMap((m) => m.idx.map((i) => [i, m.item])));
      const found = map.settings.map((s, i) => i).filter((i) => !map.settings[i].hidden && (label(map.settings[i]) + ' ' + map.settings[i].path).toLowerCase().includes(filter));
      q('pepane').innerHTML = groups(found, (i) => where.get(i) || 'Main menu') || '<div class=pe0>No setting matches</div>';
      return;
    }
    const tab = lay.tabs.find((t) => t.tab === ui.tab);
    q('penav').innerHTML = tab.menus.map((m) => '<button data-menu="' + esc(m.title) + '"' + (m.title === ui.menu ? ' aria-current=true' : '') + (whys.get(m) ? ' class=na title="' + esc(whys.get(m)) + '"' : '') + '>' +
      esc(m.item) + badge(changes.get(m)) + '</button>').join('');
    const here = menus.find((m) => m.title === ui.menu), why = whys.get(here);
    q('pepane').innerHTML = (why ? '<div class=pena>' + esc(why) + ': the RT4K shows these N/A</div>' : '') + (groups(here.idx) || '<div class=pe0>No settings mapped here</div>');
  }

  async function readMap() {
    if (doc) return;
    const r = await fetch('/rt4k_settings.json');
    if (!r.ok) throw new Error('the settings map: HTTP ' + r.status);
    doc = await r.json();
  }

  // The map for the RT4K's firmware, its codecs and layout: again for profiles opened anew, kept for those
  // added (they must fit it).
  async function ready(again) {
    await readMap();
    if (map && !again) return;
    map = mapFor(doc, fw);
    if (!map) throw new Error('there is no settings map');
    cs = codecs(map.settings);
    lay = layoutOf(map);
    dep = links(map.settings);
    devAt = map.settings.findIndex((s) => s.hidden && s.label === 'Saved on device');
    splAt = map.settings.findIndex((s) => s.label === 'Samples per Line' && s.each);
  }

  // A profile's file, ready to edit (or why not).
  function prepare(name, path, data) {
    const p = parseProfile(data);
    if (!p) throw new Error(name + ' is not an RT4K profile');
    if (p.body.length !== map.size) throw new Error(name + ' has ' + p.body.length + ' bytes of settings, the map knows ' + map.size);
    return { name, path, header: p.header, body: p.body.slice(), orig: p.body.slice(), crcOk: p.crcOk };
  }

  // The mode, rate and audio input a profile shows first: its own input mode (one with settings, other than
  // 0, the one with none); no detected rate (the mode's own); the audio input it's for.
  function viewOf(p) {
    const used = modesUsed(map.settings, p.body);
    view = { mode: used.find((k) => k) || 0, slot: -1, port: p.body[PORT_AT] < PORTS.length ? p.body[PORT_AT] : 0 };
  }

  // Shows a profile of the list: alone (as it opens), or with Multi Edit ticked with the others (the
  // signal picked stays).
  function showOne(p) {
    pf = p;
    if (multi) ticked.add(p);
    else viewOf(p);
    render();
  }

  // The profile open from this path on the SD card (FAT: no case), if any.
  const byPath = (path) => files.find((x) => x.path && x.path.toLowerCase() === path.toLowerCase());

  // Into the list: the profiles not in it yet (by their path on the SD card), and those that were, as they
  // are (with their changes).
  function take(ps) {
    return ps.map((p) => {
      const was = p.path && byPath(p.path);
      if (was) return was;
      files.push(p);
      return p;
    });
  }

  const crcNote = (ps) => {
    const bad = ps.filter((p) => !p.crcOk).map((p) => plain(p.name));
    return !bad.length ? '' : (bad.length === 1 ? bad[0] + '\'s CRC doesn\'t match' : bad.length + ' have a CRC that doesn\'t match (' + bad.join(', ') + ')') +
      ': the RT4K wouldn\'t load ' + (bad.length === 1 ? 'it' : 'them') + ' as ' + (bad.length === 1 ? 'it is' : 'they are') + ' (saving writes it right).';
  };

  // A profile from this computer (or read from the SD card), into the list and shown.
  async function openData(name, path, data) {
    try {
      await ready(!files.length);
      const [p] = take([prepare(name, path, data)]);
      showOne(p);
      status(crcNote([p]) || 'Opened ' + plain(name) + (path ? ' from the SD card' : ' from this computer'), !p.crcOk);
    } catch (e) {
      status('Could not open it: ' + e.message, true);
    }
    render();
  }

  const sd = () => window.sdInternals; // the SD card through Cruller (sd.js)
  const failure = (e) => (e.message === 'Failed to fetch' ? 'Cruller did not answer' : e.message);

  // Reads these profiles from the SD card (paths from its root), one after another: the ones read, and why
  // the others weren't.
  async function readAll(paths) {
    const got = [], bad = [];
    for (const [k, path] of paths.entries()) {
      const name = path.split('/').pop();
      status('Reading ' + (paths.length > 1 ? (k + 1) + ' of ' + paths.length + ': ' : '') + plain(name) + '…');
      try { got.push(prepare(name, path, await sd().getFile(path))); } catch (e) { bad.push(plain(name) + ': ' + failure(e)); }
    }
    return { got, bad };
  }

  // Those not in the list yet, read into it (busy meanwhile): {got, bad} as readAll's, or null when there's
  // no map to read them with (said; what: open, add).
  async function readInto(paths, what) {
    try {
      await ready(!files.length);
    } catch (e) {
      status('Could not ' + what + ' them: ' + e.message, true);
      return null;
    }
    busy = true;
    render();
    const r = await readAll(paths.filter((p) => !byPath(p)));
    busy = false;
    take(r.got);
    return r;
  }

  // How reading them went: what was done, the ones that couldn't be (what: open, add), the CRCs that don't match.
  const readStatus = (done, what, { got, bad }) => status([done, bad.length ? 'Could not ' + what + ' ' + bad.join('; ') : '', crcNote(got)].filter(Boolean).join('. '),
    bad.length > 0 || got.some((p) => !p.crcOk));

  // A profile of the SD card (its Edit), into the list if it isn't there, and shown alone.
  async function openOne(path) {
    const was = byPath(path);
    if (pf && pf === was && !multi) return;
    multi = false;
    if (was) return showOne(was);
    if (asleep()) return status('The RT4K is asleep: turn it on to read its SD card, or add a file from this computer.', true);
    status('Reading ' + plain(path.split('/').pop()) + '…');
    try {
      await openData(path.split('/').pop(), path, await sd().getFile(path));
    } catch (e) {
      status('Could not read it: ' + failure(e), true);
    }
  }

  // Profiles of the SD card to edit together (its Edit them together): into the list, with Multi Edit
  // ticked (only them), the first shown. Those in the list already come as they are.
  async function openSet(paths) {
    if (!q('pebody')) build();
    if (!paths.length || busy) return;
    if (asleep()) return status('The RT4K is asleep: turn it on to read its SD card.', true);
    const r = await readInto(paths, 'open');
    if (!r) return;
    const these = paths.map(byPath).filter(Boolean);
    if (these.length) {
      multi = these.length > 1;
      ticked = new Set(these);
      pf = these[0];
      viewOf(pf);
    }
    readStatus(these.length ? 'Opened ' + (these.length === 1 ? plain(these[0].name) : these.length + ' profiles') + (multi ? ', edited together' : '') : '', 'open', r);
    render();
  }

  // Profiles from the SD card into the list, picked in a folder at a time under /profile (those in it
  // already shown ticked and greyed).
  const picker = { dir: 'profile', chosen: new Set() };
  async function pickList() {
    const d = picker.dir;
    q('pepc').innerHTML = sd().pickCrumbs(d);
    q('pepl').innerHTML = '';
    q('peps').textContent = 'Reading the folder…';
    try {
      const rows = await sd().pickRows(d, (path, e) => {
        const already = !!byPath(path);
        return '<label' + (already ? ' class=in title="Open already"' : '') + '><input type=checkbox data-path="' + esc(path) + '"' +
          (already || picker.chosen.has(path) ? ' checked' : '') + (already ? ' disabled' : '') + '>' + esc(plain(e.name)) + '</label>';
      });
      if (picker.dir !== d) return; // (another folder was opened meanwhile)
      q('pepl').innerHTML = rows;
      q('peps').textContent = '';
    } catch (e) {
      q('peps').textContent = 'Could not read the folder: ' + failure(e);
    }
  }

  function addProfiles() {
    if (busy || asleep()) return;
    const dlg = q('peadd'), ok = q('pepok');
    picker.dir = pf && pf.path ? pf.path.split('/').slice(0, -1).join('/') : 'profile';
    picker.chosen = new Set();
    const count = () => { ok.disabled = !picker.chosen.size; ok.textContent = picker.chosen.size > 1 ? 'Add ' + picker.chosen.size : 'Add'; };
    count();
    dlg.onclick = (ev) => {
      const to = ev.target.closest('[data-dir]');
      if (!to) return;
      ev.preventDefault();
      picker.dir = to.dataset.dir;
      pickList();
    };
    dlg.onchange = (ev) => {
      const c = ev.target.closest('input[data-path]');
      if (!c) return;
      if (c.checked) picker.chosen.add(c.dataset.path); else picker.chosen.delete(c.dataset.path);
      count();
    };
    dlg.onclose = async () => {
      if (dlg.returnValue !== 'yes' || !picker.chosen.size) return;
      const r = await readInto([...picker.chosen], 'add');
      if (!r) return;
      if (multi) r.got.forEach((p) => ticked.add(p));
      if (r.got.length) showOne(r.got[0]);
      readStatus(r.got.length ? 'Added ' + (r.got.length === 1 ? plain(r.got[0].name) : r.got.length + ' profiles') : '', 'add', r);
      render();
    };
    dlg.returnValue = '';
    dlg.showModal();
    pickList();
  }

  // Takes a profile off the list (its changes dropped, asked first); with one left, Multi Edit is off.
  async function closeFile(p) {
    if (!p || busy) return;
    if (touched(p) && !(await window.askUser('Close ' + plain(p.name) + '?', 'Its changes aren\'t saved: closing it drops them.', 'Close it', true))) return;
    files = files.filter((x) => x !== p);
    ticked.delete(p);
    if (files.length < 2) multi = false;
    if (pf === p) {
      pf = (multi && files.find((x) => ticked.has(x))) || files[0] || null;
      if (pf && !multi) viewOf(pf);
    }
    render();
  }

  // Downloads profiles, each its own file (a moment apart: the browser may ask once to allow several).
  function downloads(ps) {
    ps.forEach((p, k) => setTimeout(() => {
      const a = document.createElement('a');
      a.href = URL.createObjectURL(new Blob([buildProfile(p.header, p.body)], { type: 'application/octet-stream' }));
      a.download = p.name;
      a.click();
      setTimeout(() => URL.revokeObjectURL(a.href), 1000);
    }, k * 250));
  }
  const download = () => downloads(group());

  // Writes a profile to the SD card at path, with its CRC.
  async function putProfile(p, path) {
    await sd().putFile(path, buildProfile(p.header, p.body));
  }

  // Several edited together: each changed one replaces itself where it is on the SD card (asked once, every
  // file named); one from this computer is downloaded.
  async function saveAll() {
    if (busy || asleep()) return;
    const g = group(), todo = g.filter(touched), onSd = todo.filter((p) => p.path), here = todo.filter((p) => !p.path);
    if (!todo.length) return status('Nothing to save: no profile has changes.');
    const names = (ps) => first12(ps.map((p) => p.path || p.name));
    const kept = g.length - todo.length;
    if (!(await window.askUser(onSd.length ? 'Replace ' + (onSd.length === 1 ? plain(onSd[0].name) : onSd.length + ' profiles') + '?' : 'Download ' + here.length + ' profiles?',
      [onSd.length ? 'On the SD card, each where it is:\n' + names(onSd) : '', here.length ? 'From this computer, downloaded:\n' + names(here) : '',
        kept ? kept + (kept === 1 ? ' has' : ' have') + ' no changes and stay' + (kept === 1 ? 's' : '') + ' as it is.' : ''].filter(Boolean).join('\n\n'),
      onSd.length ? 'Replace' : 'Download', !!onSd.length))) return;
    busy = true;
    render();
    let done = 0;
    try {
      for (const p of onSd) {
        status('Saving ' + (onSd.length > 1 ? (done + 1) + ' of ' + onSd.length + ': ' : '') + plain(p.name) + '…');
        await putProfile(p, p.path).catch((e) => { throw new Error(plain(p.name) + ': ' + failure(e)); });
        p.orig = p.body.slice();
        p.crcOk = true;
        done++;
      }
      downloads(here);
      for (const p of here) p.orig = p.body.slice();
      status([onSd.length ? 'Saved ' + (onSd.length === 1 ? plain(onSd[0].name) : onSd.length + ' profiles') + ' to the SD card' : '', here.length ? 'downloaded ' + here.length : ''].filter(Boolean).join(', '));
    } catch (e) {
      status('Saved ' + done + ' of ' + onSd.length + '; ' + e.message + (done < onSd.length - 1 ? ' (the rest weren\'t tried)' : ''), true);
    }
    busy = false;
    render();
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
    busy = true;
    render();
    try {
      status('Saving ' + plain(name) + '…');
      await putProfile(pf, path);
      Object.assign(pf, { name, path, orig: pf.body.slice(), crcOk: true });
      let note = 'Saved ' + plain(name);
      if (load) {
        status('Loading ' + plain(name) + '…');
        const t = await sd().ask('prof load ' + (sd().profilePath(dir, name) || path), 'prof', 15000).catch(failure);
        note += t === 'prof load ok' ? ', and loaded' : ', but the RT4K did not load it (' + t + ')';
      }
      status(note, / did not /.test(note));
    } catch (e) {
      status('Saving failed: ' + failure(e), true);
    }
    busy = false;
    render();
  }

  // The page shows the editor: parts, a profile's path under /profile (#rt4k/editor/<path>) to show, or none.
  async function open(parts) {
    if (!q('pebody')) build();
    render();
    const path = (parts || []).map(decodeURIComponent).join('/');
    if (path) openOne('profile/' + path);
  }

  function onStatus(s) {
    const was = power;
    power = s.rt4k_power || power;
    fw = s.rt4k_fw || fw;
    if (q('pebody') && was !== power) render();
  }

  window.peOpen = open;
  window.peOpenSet = openSet; // sd.js: the profiles ticked, to edit together
  window.peStatus = onStatus;
  window.editorInternals = { crc16, parseProfile, buildProfile, mapFor, codecs, decode, encode, agree, clearSignal, asNumber, bytesAt, modesUsed, slotsUsed, rateName, modeName, modeOfRate, links, layoutOf }; // tests/test_editor.js
})();
