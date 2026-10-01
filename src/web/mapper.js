// The RT4K's settings map, read off the RT4K itself (served as /mapper.js, embedded at build time): the
// Debug tab's "Settings map". The byte map of its settings isn't public; this finds it.
//
// The RT4K sends the settings it runs on (GET /rt4k/xfer?cmd=sget: a profile without its header) and
// its menu as text (cmd=osd: a plane of character codes and colours, the selected line on a light
// background). Walking the menu with the remote's keys (POST /api/v1/command), each setting is read,
// changed one step, read again and put back: the bytes that changed are where it lives, and the values
// shown are what they mean. Actions (<Start>), profiles and info screens are left alone; the output and
// the input too unless asked. A snapshot and a compare by hand do the same for one setting.

(() => {
  'use strict';

  const q = (id) => document.getElementById(id);
  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
  const esc = (s) => String(s).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
  const hex = (b) => b.toString(16).padStart(2, '0');

  // --- pure helpers (tests/test_mapper.js) --------------------------------------------------------------

  // A ready line's fields: "osd ready rows=32 stride=64 width=40 ..." -> {rows: 32, ...}.
  function fields(ready) {
    const o = {};
    String(ready || '').split(' ').forEach((t) => { const i = t.indexOf('='); if (i > 0) o[t.slice(0, i)] = t.slice(i + 1); });
    return o;
  }

  // An OSD plane as text: its rows ({y, text, sel}: sel, a light background all across), its title (the
  // first line with text) and its selected line, the footer (the profile, the mode) left out.
  function readPlane(data, ready) {
    const f = fields(ready), rows = +f.rows || 0, w = +f.width || +f.cols || 0, s = +f.stride || w;
    const out = [];
    for (let y = 0; y < rows; y++) {
      let text = '', light = 0, any = false;
      for (let x = 0; x < w; x++) {
        const j = y * s + x, ch = data[j], co = data[2048 + j];
        text += ch >= 32 && ch < 127 ? String.fromCharCode(ch) : ch ? '•' : ' ';
        if ((co >> 6 & 3) === 1) light++;
        if (ch > 32) any = true;
      }
      out.push({ y, text: text.trimEnd(), sel: any && light === w });
    }
    const body = out.filter((r) => !/^(Profile:|v\d+\.\d+)/.test(r.text.trim()));
    const title = body.find((r) => r.text.trim());
    return { rows: out, title: title ? title.text.trim() : '', selected: body.find((r) => r.sel) || null };
  }

  // A menu line: its label and value ("• Top Trim:          +0" -> {label: 'Top Trim', value: '+0'});
  // value null for a submenu (or an action without one).
  function parseLine(text) {
    const t = String(text || '').replace(/^[^A-Za-z0-9<]+/, '').trimEnd();
    const m = /^(.*?):\s+(\S.*)$/.exec(t);
    return m ? { label: m[1].trim(), value: m[2].trim() } : { label: t.replace(/:$/, '').trim(), value: null };
  }

  // Where two snapshots differ, as ranges of bytes: [{off, len}], neighbours within gap bytes merged.
  function diff(a, b, gap = 0) {
    const out = [];
    const n = Math.min(a.length, b.length);
    for (let i = 0; i < n; i++) {
      if (a[i] === b[i]) continue;
      const last = out[out.length - 1];
      if (last && i - (last.off + last.len) <= gap) last.len = i - last.off + 1;
      else out.push({ off: i, len: 1 });
    }
    return out;
  }

  // The union of ranges, merged.
  function union(ranges) {
    const s = ranges.slice().sort((x, y) => x.off - y.off), out = [];
    for (const r of s) {
      const last = out[out.length - 1];
      if (last && r.off <= last.off + last.len) last.len = Math.max(last.len, r.off + r.len - last.off);
      else out.push({ off: r.off, len: r.len });
    }
    return out;
  }

  // The bytes of a snapshot over ranges, as hex ("01 00").
  const bytesAt = (snap, ranges) => ranges.map((r) => Array.from(snap.subarray(r.off, r.off + r.len), hex).join(' ')).join(' | ');

  // What not to touch: submenus and settings by label. Actions (a <value>) are never pressed.
  const SKIP_MENU = /profile|diagnostic|console|status|about/i;
  const RISKY = /output|input source|resolution|safe ?mode|reset|default|factory|update|format|delete|save|load/i;

  // --- the RT4K -----------------------------------------------------------------------------------------

  async function xfer(cmd) {
    const r = await fetch('/rt4k/xfer?cmd=' + cmd);
    if (!r.ok) throw new Error(cmd + ': ' + ((await r.text()).trim() || 'HTTP ' + r.status));
    return { data: new Uint8Array(await r.arrayBuffer()), ready: r.headers.get('X-Ready') || '' };
  }

  let sgetReady = '';
  async function snap() {
    const x = await xfer('sget');
    sgetReady = x.ready;
    return x.data;
  }

  async function look() {
    const x = await xfer('osd');
    return readPlane(x.data, x.ready);
  }

  async function command(c, expect) {
    const r = await fetch('/api/v1/command', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ command: c }) });
    const v = await r.json();
    if (!v.ok) throw new Error(c + ': the RT4K did not get it');
    return (v.results && v.results[0] && v.results[0].reply || []).join(' ');
  }

  // A key, then the menu once it settles (two reads alike).
  async function press(key) {
    await command('remote ' + key);
    await sleep(150);
    let prev = await look();
    for (let i = 0; i < 6; i++) {
      await sleep(120);
      const now = await look();
      if (now.title === prev.title && (now.selected && now.selected.text) === (prev.selected && prev.selected.text)) return now;
      prev = now;
    }
    return prev;
  }

  // --- the map ------------------------------------------------------------------------------------------

  const st = { running: false, stop: false, results: [], start: null, manual: null, log: [] };
  const opts = () => ({ all: q('mp-all').checked, risky: q('mp-risky').checked });

  function log(text, bad) {
    st.log.push([text, bad]);
    if (st.log.length > 400) st.log.shift();
    const box = q('mp-log');
    box.innerHTML = st.log.slice(-60).map(([t, b]) => '<div' + (b ? ' class=bad' : '') + '>' + esc(t) + '</div>').join('');
    box.scrollTop = box.scrollHeight;
  }

  function showResults() {
    q('mp-n').textContent = st.results.filter((r) => r.ranges && r.ranges.length).length + ' mapped, ' + st.results.length + ' seen';
    q('mp-rows').innerHTML = st.results.map((r) => '<tr' + (r.note ? ' class=bad' : '') + '><td>' + esc(r.path.join(' › ')) + '</td><td>' + esc(r.label) + '</td><td class=mono>' +
      (r.ranges && r.ranges.length ? r.ranges.map((g) => '0x' + g.off.toString(16) + (g.len > 1 ? '+' + g.len : '')).join(', ') : '–') + '</td><td class=small>' +
      esc(r.values.map((v) => v.value + (v.bytes ? ' = ' + v.bytes : '')).join(' · ') + (r.note ? ' (' + r.note + ')' : '')) + '</td></tr>').join('');
    q('mp-dl').disabled = !st.results.length;
  }

  // One setting, selected on screen: one step and back (or every value, up to 16, with "all").
  async function mapSetting(path, it) {
    const rec = { path, label: it.label, values: [{ value: it.value }], ranges: [] };
    st.results.push(rec);
    const before = await snap();
    let way = 'right', screen = await press(way), v = screen.selected ? parseLine(screen.selected.text).value : null;
    if (v === it.value) { way = 'left'; screen = await press(way); v = screen.selected ? parseLine(screen.selected.text).value : null; }
    if (v === it.value || v === null) {
      rec.note = 'did not change: read only?';
      log(path.join(' › ') + ' › ' + it.label + ': ' + it.value + ' (did not change)');
      return showResults();
    }
    const seen = [{ value: v, snap: await snap() }];
    let steps = 1;
    if (opts().all) {
      while (steps < 16 && !st.stop) {
        screen = await press(way);
        const nv = screen.selected ? parseLine(screen.selected.text).value : null;
        if (nv === seen[seen.length - 1].value) break; // its end
        steps++;
        if (nv === it.value) break;                     // round to where it started
        seen.push({ value: nv, snap: await snap() });
      }
    }
    // Back to where it was: round already, or as many steps the other way.
    const now = screen.selected ? parseLine(screen.selected.text).value : null;
    if (now !== it.value) for (let i = 0; i < steps; i++) screen = await press(way === 'right' ? 'left' : 'right');
    const after = await snap();
    rec.ranges = union(seen.map((s) => diff(before, s.snap)));
    rec.values[0].bytes = bytesAt(before, rec.ranges);
    seen.forEach((s) => rec.values.push({ value: s.value, bytes: bytesAt(s.snap, rec.ranges) }));
    if (diff(before, after).length) rec.note = 'did not come back as it was';
    log(path.join(' › ') + ' › ' + it.label + ': ' + rec.values.map((x) => x.value).join(', ') + ' at ' +
      (rec.ranges.map((g) => '0x' + g.off.toString(16) + (g.len > 1 ? '+' + g.len : '')).join(', ') || 'no bytes'), !!rec.note);
    showResults();
  }

  // The menu on screen, its submenus too: its items walking down until the first comes back, then each.
  async function mapMenu(path) {
    let screen = await look();
    const title = screen.title;
    if (!screen.selected) throw new Error('no menu open on the RT4K: open the one to map (Live screen, or the remote)');
    const items = [], first = screen.selected.text;
    do {
      items.push(parseLine(screen.selected.text));
      screen = await press('down');
    } while (screen.selected && screen.selected.text !== first && items.length < 48 && !st.stop);
    log(title + ': ' + items.length + ' items');
    let at = 0;
    for (let i = 0; i < items.length && !st.stop; i++) {
      for (; at < i; at++) screen = await press('down');
      const it = items[i], sel = screen.selected ? parseLine(screen.selected.text) : null;
      if (!sel || sel.label !== it.label) { log(title + ': lost the way at ' + it.label + ' (' + (sel ? sel.label : 'nothing selected') + ')', true); return; }
      const where = path.concat(title);
      if (it.value === null) {
        if (SKIP_MENU.test(it.label) || (!opts().risky && RISKY.test(it.label))) { log(title + ' › ' + it.label + ': left alone'); continue; }
        screen = await press('ok');
        if (screen.title === title) { log(title + ' › ' + it.label + ': not a submenu, left'); continue; }
        await mapMenu(where);
        screen = await press('back');
        if (screen.title !== title) { log('back from ' + it.label + ' went to "' + screen.title + '", not ' + title, true); st.stop = true; return; }
        continue;
      }
      if (/[<>]/.test(it.value)) { log(title + ' › ' + it.label + ': an action, left alone'); continue; }
      if (!opts().risky && RISKY.test(it.label)) { log(title + ' › ' + it.label + ': risky, left alone'); continue; }
      await mapSetting(where, it);
    }
  }

  async function run() {
    if (st.running) return;
    st.running = true;
    st.stop = false;
    render();
    try {
      log('Reading where it starts…');
      st.start = await snap();
      log('Settings: ' + st.start.length + ' bytes (' + sgetReady + ')');
      await mapMenu([]);
      const end = await snap(), left = diff(st.start, end);
      log(left.length ? 'Done, but ' + left.length + ' places differ from the start: reload the profile you had (Profiles view)' : (st.stop ? 'Stopped' : 'Done') + ': the settings are as they were', left.length > 0);
    } catch (e) {
      log(e.message === 'Failed to fetch' ? 'Cruller did not answer' : e.message, true);
    }
    st.running = false;
    render();
  }

  async function manual(compare) {
    try {
      if (!compare) {
        st.manual = await snap();
        log('Snapshot taken: change one setting on the RT4K, then Compare');
      } else if (st.manual) {
        const now = await snap(), screen = await look(), d = diff(st.manual, now);
        const sel = screen.selected ? parseLine(screen.selected.text) : { label: '?', value: null };
        const rec = { path: [screen.title || '(by hand)'], label: sel.label, values: [{ value: '(before)', bytes: bytesAt(st.manual, d) }, { value: sel.value || '(now)', bytes: bytesAt(now, d) }], ranges: d };
        st.results.push(rec);
        log('By hand, ' + sel.label + ': ' + (d.length ? d.length + ' places changed' : 'nothing changed'));
        st.manual = now;
        showResults();
      }
    } catch (e) {
      log(e.message === 'Failed to fetch' ? 'Cruller did not answer' : e.message, true);
    }
  }

  function download() {
    const info = window.crullerStatus || {};
    const doc = { rt4k: { firmware: info.rt4k_fw || null, model: info.rt4k_model || null }, sget: fields(sgetReady), made: new Date().toISOString(), settings: st.results };
    const a = document.createElement('a');
    a.href = URL.createObjectURL(new Blob([JSON.stringify(doc, null, 1)], { type: 'application/json' }));
    a.download = 'rt4k-settings-map-' + (doc.rt4k.firmware || 'fw') + '.json';
    a.click();
    setTimeout(() => URL.revokeObjectURL(a.href), 1000);
  }

  function render() {
    q('mp-go').disabled = st.running;
    q('mp-stop').disabled = !st.running;
    q('mp-snap').disabled = q('mp-cmp').disabled = st.running;
  }

  function build() {
    const box = q('mp');
    if (!box) return;
    box.innerHTML = '<div class="row"><h2 class="grow">Settings map</h2><span class="small" id="mp-n">nothing yet</span></div>' +
      '<div class="small">Open the RT4K\'s menu where to start (Live screen or its remote): Map walks that menu and its submenus, each setting one step and back, ' +
      'and finds the bytes it lives in. It changes the RT4K\'s settings as it goes (and the picture), and puts each back.</div>' +
      '<div class="row" style="flex-wrap:wrap;gap:6px;margin:8px 0"><button id="mp-go" class="primary">Map this menu</button><button id="mp-stop">Stop</button>' +
      '<label class="small"><input type="checkbox" id="mp-all"> every value (up to 16)</label>' +
      '<label class="small"><input type="checkbox" id="mp-risky"> the output and the input too</label>' +
      '<span class="grow"></span><button id="mp-snap">Snapshot</button><button id="mp-cmp">Compare</button><button id="mp-dl" disabled>Download JSON</button></div>' +
      '<div id="mp-log" class="small mono" style="max-height:180px;overflow:auto"></div>' +
      '<table class="tbl"><thead><tr><th>Menu</th><th>Setting</th><th>Bytes</th><th>Values</th></tr></thead><tbody id="mp-rows"></tbody></table>';
    q('mp-go').onclick = run;
    q('mp-stop').onclick = () => { st.stop = true; log('Stopping after this one…'); };
    q('mp-snap').onclick = () => manual(false);
    q('mp-cmp').onclick = () => manual(true);
    q('mp-dl').onclick = download;
    render();
  }

  if (typeof document !== 'undefined' && document.getElementById) build();
  window.mapperInternals = { fields, readPlane, parseLine, diff, union, bytesAt }; // tests/test_mapper.js
})();
