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
  let sleepMs = 1; // (0 in tests: no waiting)
  const sleep = (ms) => new Promise((r) => setTimeout(r, ms * sleepMs));
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

  // A menu line: its label and value ("• Top Trim:          +0" -> {label: 'Top Trim', value: '+0'}; also
  // without the colon, the value in its column: "• Enable 343.200     Off"); value null for a submenu (or
  // an action without one).
  function parseLine(text) {
    const t = String(text || '').replace(/^[^A-Za-z0-9<]+/, '').trimEnd();
    const m = /^(.*?):\s+(\S.*)$/.exec(t) || /^(.*?\S)\s{3,}(\S.*)$/.exec(t);
    return m ? { label: m[1].trim(), value: m[2].trim() } : { label: t.replace(/:$/, '').trim(), value: null };
  }

  // The section of each item whose label the menu has more than once ("Function" under Scanline and under
  // Horizontal Blur): the heading above it, a line the cursor skips. items: [{label, y}], walking down.
  function sections(rows, items) {
    const ys = new Set(items.map((it) => it.y)), seen = new Map();
    for (const it of items) seen.set(it.label, (seen.get(it.label) || 0) + 1);
    for (const it of items) {
      if (seen.get(it.label) < 2) continue;
      for (let y = it.y - 1; y >= 0; y--) {
        const t = rows[y] ? rows[y].text.trim() : '';
        if (t && !ys.has(y) && parseLine(t).value === null) { it.section = parseLine(t).label; break; }
      }
    }
    return items;
  }

  // A box asking first, nothing selected under it ("Warning! Flicker may induce epilepsy. Proceed at your
  // own risk!!", "[Cancel]  [OK]"): its text, or null. The map never says OK: back cancels it.
  function dialogOf(screen) {
    if (!screen || screen.selected || !screen.rows.some((r) => /\[[A-Za-z ]+\]/.test(r.text))) return null;
    return screen.rows.map((r) => r.text.replace(/•/g, '').trim())
      .filter((t) => t && !/^\[/.test(t) && !/^(Profile:|v\d+\.\d+)/.test(t)).join(' ');
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

  // A value as a number ("+3", "-12", "1.5x", "+0 (Y Min: 32)": what follows in brackets is the RT4K's
  // explanation; "+-100", the TW9912's negatives); NaN when it isn't one.
  const asNumber = (v) => { const m = /^([+-]?\d+(?:\.\d+)?)\s*[a-z%/]*(?:\s*\(.*\))?$/i.exec(String(v).trim().replace(/^\+-/, '-')); return m ? +m[1] : NaN; };

  // Whether two values shown are the same: numbers by their number (the brackets may hold a reading that
  // moves on its own: "1.000 (Rmax: 30)", then "(Rmax: 37)"; "-0.00" is 0.00), the rest as shown.
  const same = (a, b) => a === b || (!isNaN(asNumber(a)) && asNumber(a) === asNumber(b));

  // Two values shown, not numbers, that differ only in their digits: a reading of its own ("Locked 84%",
  // then "Locked 86%"; "Auto (4)"), not a setting another moved. (Not for walking: "1/2" and "1/3" are
  // two values.)
  const reading = (a, b) => isNaN(asNumber(a)) && String(a).replace(/\d+/g, '#') === String(b).replace(/\d+/g, '#');

  // A setting's values as numbers: its min, max and step; null when any isn't one.
  function numeric(list) {
    const n = (list || []).map(asNumber);
    if (n.length < 2 || n.some(isNaN)) return null;
    const s = [...new Set(n)].sort((a, b) => a - b);
    let step = Infinity;
    for (let i = 1; i < s.length; i++) step = Math.min(step, s[i] - s[i - 1]);
    return { min: s[0], max: s[s.length - 1], step: +step.toFixed(6) };
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

  // Each setting's own bytes, and those it shares with others (worked out from settings, a scaler's
  // factors...): a byte that changed for more than one setting is shared. A setting left without any
  // owns those it shares with just one other: one setting in two menus (the ADC's Decimation Factor, the
  // HDMI receiver's Input Pixels), or one another sets (Native Sampling, Samples per Line). What's
  // volatile (readings) is no setting's own. Sets own and shared on each.
  function split(records) {
    const count = new Map();
    for (const r of records) {
      const mine = new Set();
      for (const g of r.ranges || []) for (let i = g.off; i < g.off + g.len; i++) mine.add(i);
      for (const i of mine) count.set(i, (count.get(i) || 0) + 1);
    }
    for (const i of volatileBytes(records)) count.set(i, Infinity);
    const ranges = (bytes) => {
      const out = [];
      for (const i of bytes.sort((a, b) => a - b)) {
        const last = out[out.length - 1];
        if (last && i === last.off + last.len) last.len++;
        else out.push({ off: i, len: 1 });
      }
      return out;
    };
    for (const r of records) {
      const bytes = [];
      for (const g of r.ranges || []) for (let i = g.off; i < g.off + g.len; i++) bytes.push(i);
      const n = bytes.some((i) => count.get(i) === 1) ? 1 : 2;
      r.own = ranges(bytes.filter((i) => count.get(i) <= n));
      r.shared = ranges(bytes.filter((i) => count.get(i) > n));
    }
    return records;
  }

  const rangeText = (ranges) => ranges.map((g) => '0x' + g.off.toString(16) + (g.len > 1 ? '+' + g.len : '')).join(', ');

  // The bytes of a snapshot over ranges, as hex ("01 00").
  const bytesAt = (snap, ranges) => ranges.map((r) => Array.from(snap.subarray(r.off, r.off + r.len), hex).join(' ')).join(' | ');

  // A snapshot's bytes over ranges, by offset ({1316: 0, 1317: 1}), and back as hex over other ranges.
  function takeAt(snap, ranges) {
    const at = {};
    for (const g of ranges) for (let i = g.off; i < g.off + g.len; i++) at[i] = snap[i];
    return at;
  }
  const hexAt = (at, ranges) => ranges.map((g) => {
    const b = [];
    for (let i = g.off; i < g.off + g.len; i++) b.push(at[i] === undefined ? '??' : hex(at[i]));
    return b.join(' ');
  }).join(' | ');

  // What not to touch: lines without a value (submenus, but some are actions: "Check SD Card" looks for a
  // firmware update), and settings by label ("Output Factor" is a colour's, "Output Pixels" the HDMI
  // receiver's, "LCD Saver" saves nothing). Actions with a value (<Start>) are never pressed.
  const SKIP_MENU = /profile|diagnostic|console|status|about|check sd|update|reset|default|factory|format|calibrat|save|load|banner/i;
  const RISKY = /output(?! factor| pixels)|input source|resolution|safe ?mode|reset|default|factory|update|format|delete|\bsave\b|\bload\b/i;

  // --- the map as shipped (a JSON in src/web, mapped here, merged by tools/merge-map.js) --------------------
  // {v: 1, maps: [{firmware, ver, size, settings}]}: one map per RT4K firmware mapped, each whole. A
  // setting: {path, section (a label the menu has twice: the heading above it), label, bytes: [[offset,
  // length]], values: [[shown, hex]] (each value seen, its own bytes), min, max, step (numbers), round (a
  // list that goes round), asks (past its first or last value the RT4K warns first: what it says), capped
  // (["first"], ["last"] or both: the walk stopped before that end, so min or max is only how far it got).
  // How the run went (a setting not back as it was) stays out.

  function compact(r) {
    const c = { path: r.path.join(' › ') };
    if (r.section) c.section = r.section;
    c.label = r.label;
    if (r.own && r.own.length) {
      c.bytes = r.own.map((g) => [g.off, g.len]);
      c.values = r.values.filter((v) => v.at).map((v) => [v.value, hexAt(v.at, r.own).replace(/ /g, '')]);
    }
    if (r.min !== undefined) Object.assign(c, { min: r.min, max: r.max, step: r.step });
    if (r.round) c.round = true;
    if (r.asks) c.asks = r.asks;
    if (r.capped) c.capped = r.capped;
    return c;
  }

  const settingKey = (c) => c.path + '\n' + (c.section || '') + '\n' + c.label;

  // The settings mapped for a firmware (null: none).
  function settingsOf(doc, fw) {
    const m = (doc && doc.maps || []).find((x) => x.firmware === fw);
    return m ? m.settings : null;
  }

  // doc with settings added to firmware fw's map (those it had replaced; a new map for a new firmware).
  function keep(doc, fw, ver, size, settings) {
    doc = { v: 1, maps: (doc && doc.maps || []).map((m) => ({ ...m })) };
    let m = doc.maps.find((x) => x.firmware === fw);
    if (!m) {
      m = { firmware: fw, settings: [] };
      doc.maps.push(m);
    }
    const out = new Map(m.settings.map((c) => [settingKey(c), c]));
    for (const c of settings) out.set(settingKey(c), c);
    Object.assign(m, { ver, size, settings: [...out.values()] });
    return doc;
  }


  // --- the RT4K -----------------------------------------------------------------------------------------

  // A request, tried again twice when the network drops it (Wi-Fi hiccups): a walk takes minutes.
  // TypeError: fetch's own network failure; retry: a dev proxy that couldn't reach Cruller.
  async function retried(fn) {
    for (let i = 0; ; i++) {
      try {
        return await fn();
      } catch (e) {
        if (i >= 2 || !(e instanceof TypeError || e.retry)) throw e;
        await sleep(1000);
      }
    }
  }

  async function failed(r, what) {
    const t = (await r.text()).trim();
    const e = new Error(what + ': ' + (t || 'HTTP ' + r.status));
    e.retry = /^proxy: connect/.test(t);
    return e;
  }

  async function xfer(cmd) {
    return retried(async () => {
      const r = await fetch('/rt4k/xfer?cmd=' + cmd);
      if (!r.ok) throw await failed(r, cmd);
      return { data: new Uint8Array(await r.arrayBuffer()), ready: r.headers.get('X-Ready') || '' };
    });
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

  // Commands through /api/v1/command, up to BATCH at once; those Cruller couldn't hand the RT4K ("sent":
  // false, a 503) sent again, twice at most. Their replies, joined. (A key sent again after a dropped
  // answer may go twice: the walk checks where it is, and each setting that it came back.)
  async function send(cmds) {
    let left = cmds, replies = [];
    for (let t = 0; t < 3 && left.length; t++) {
      if (t) await sleep(500);
      const v = await retried(async () => {
        const r = await fetch('/api/v1/command', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(left.length === 1 ? { command: left[0] } : { commands: left }) });
        if (!r.ok && r.status !== 503) throw await failed(r, left[0]);
        return r.json();
      });
      const res = v.results || [];
      replies = replies.concat(res.filter((x) => x.sent).flatMap((x) => x.reply || []));
      left = left.filter((c, i) => !(res[i] && res[i].sent));
    }
    if (left.length) throw new Error(left[0] + ': the RT4K did not get it');
    return replies.join(' ');
  }

  const command = (c) => send([c]);

  // A key (none: just look again), then the menu once it settles (two reads alike); fast: one read, for
  // walking a long range (a value that seems not to have moved is read again settled, by walk()).
  async function press(key, fast) {
    if (key) {
      await command('remote ' + key);
      await sleep(150);
    }
    let prev = await look();
    if (fast) return prev;
    for (let i = 0; i < 6; i++) {
      await sleep(120);
      const now = await look();
      if (now.title === prev.title && (now.selected && now.selected.text) === (prev.selected && prev.selected.text)) return now;
      prev = now;
    }
    return prev;
  }

  const valueOf = (screen) => (screen && screen.selected ? parseLine(screen.selected.text).value : null);

  // --- the map ------------------------------------------------------------------------------------------

  const st = { running: false, stop: false, results: [], start: null, manual: null, log: [] };
  let optsOverride = null; // (tests)
  const opts = () => optsOverride || { all: q('mp-all').checked, risky: q('mp-risky').checked };

  function log(text, bad) {
    st.log.push([text, bad]);
    if (st.log.length > 400) st.log.shift();
    const box = q('mp-log');
    box.innerHTML = st.log.slice(-60).map(([t, b]) => '<div' + (b ? ' class=bad' : '') + '>' + esc(t) + '</div>').join('');
    box.scrollTop = box.scrollHeight;
  }

  // Bytes that change with any setting and don't come back (a counter, a checksum, readings: 0x57f4..
  // on 1.92.0; the scaler's factors): those left changed after two settings or more, as whole 32-bit
  // words. A setting's leftovers besides them mean it didn't.
  function volatileBytes(records) {
    const count = new Map(), out = new Set();
    for (const r of records) for (const g of r.leftover || []) for (let i = g.off; i < g.off + g.len; i++) count.set(i, (count.get(i) || 0) + 1);
    for (const [i, n] of count) if (n > 1) for (let j = i & ~3; j < (i & ~3) + 4; j++) out.add(j);
    return out;
  }

  function valuesText(r) {
    const range = r.min !== undefined ? r.min + ' to ' + r.max + ' (step ' + r.step + ')' + (r.round ? ', goes round' : '') + ': ' : r.list ? r.list.length + ' values' + (r.round ? ', round' : '') + ': ' : '';
    return range + r.values.map((v) => v.value + (v.at && r.own && r.own.length ? ' = ' + hexAt(v.at, r.own) : '')).join(' · ');
  }

  function showResults() {
    split(st.results);
    const vol = volatileBytes(st.results);
    for (const r of st.results) { // (again each time: what's volatile shows once two settings have it)
      const left = (r.leftover || []).some((g) => { for (let i = g.off; i < g.off + g.len; i++) if (!vol.has(i)) return true; return false; });
      r.note = r.backNote || (left ? 'did not come back as it was' : '');
    }
    q('mp-n').textContent = st.results.filter((r) => r.ranges && r.ranges.length).length + ' mapped, ' + st.results.length + ' seen';
    q('mp-rows').innerHTML = st.results.map((r) => '<tr' + (r.note ? ' class=bad' : '') + '><td>' + esc(r.path.join(' › ')) + '</td><td>' + esc(r.label) + '</td><td class=mono>' +
      (r.own && r.own.length ? '<b>' + rangeText(r.own) + '</b>' : '–') + (r.shared && r.shared.length ? ' <span class=small>shared ' + rangeText(r.shared) + '</span>' : '') + '</td><td class=small>' +
      esc(valuesText(r) + (r.note ? ' (' + r.note + ')' : '')) + '</td></tr>').join('');
    q('mp-dl').disabled = !st.results.length;
  }

  const BATCH = 8; // keys in one /api/v1/command, walking a long range of numbers

  // Several keys at once ("commands", up to 8), then the menu settled.
  async function pressMany(key, n) {
    for (let left = n; left > 0; left -= BATCH) await send(Array(Math.min(BATCH, left)).fill('remote ' + key));
    return press(null);
  }


  // Walks one way from the value on screen until it stops changing (its end) or comes back to from (a list
  // that goes round), limit steps at most (numbers: 16 times that). Snapshots the first snaps values, and
  // the last. Once the values are numbers a fixed step apart, it goes BATCH keys at a time, then twice as
  // many between reads up to 8 times (the keys past its end change nothing), and counts the steps from the
  // values. A box asking first is cancelled, and ends the walk (asks: its text).
  // Returns {values: [{value, snap}], steps, round, asks}.
  async function walk(way, from, limit, snaps) {
    const out = [];
    let prev = from, steps = 0, step = 0, stride = 1, asks = null;
    while (steps < (step ? limit * 16 : limit) && !st.stop) {
      if (!step && out.length >= 3) { // three steps alike from where it started: numbers, a fixed step
        const nums = [from, ...out.map((s) => s.value)].map(asNumber), d = nums[1] - nums[0];
        if (d && nums.every((n, i) => !isNaN(n) && (!i || Math.abs(n - nums[i - 1] - d) < 1e-9))) step = d;
      }
      if (step) {
        let screen = await pressMany(way, BATCH * stride);
        if ((asks = dialogOf(screen))) screen = await press('back'); // (the keys after it moved in the box)
        let v = valueOf(screen), moved = Math.round((asNumber(v) - asNumber(prev)) / step);
        if (!isNaN(moved) && moved < BATCH * stride && !asks) { // its end, or keys lost on the way: one more
          const one = valueOf(await press(way));
          const more = Math.round((asNumber(one) - asNumber(v)) / step);
          if (more > 0) {
            v = one;
            moved += more;
            stride = 0; // (back to BATCH keys below)
          }
        }
        if (isNaN(moved) || moved <= 0) break; // its end (or it went round: walked again one at a time below)
        steps += moved;
        out.push({ value: v, snap: null });
        prev = v;
        if ((stride && moved < BATCH * stride) || asks) break; // stopped short: its end
        stride = Math.min(Math.max(stride * 2, 1), 8);
        continue;
      }
      let v = valueOf(await press(way, true));
      if (same(v, prev)) v = valueOf(await press(null)); // read settled: its end, or just slow?
      if (same(v, prev)) v = valueOf(await press(way)); // or the key lost on the way: once more
      if (v === null && (asks = dialogOf(await press(null)))) await press('back');
      if (same(v, prev) || v === null) break;
      steps++;
      if (same(v, from)) return { values: out, steps, round: true };
      out.push({ value: v, snap: out.length < snaps ? await snap() : null });
      prev = v;
    }
    const last = out[out.length - 1];
    if (last && !last.snap) last.snap = await snap();
    const capped = limit > 1 && !st.stop && steps >= (step ? limit * 16 : limit); // stopped before its end
    return { values: out, steps, round: false, asks, capped };
  }

  // n steps the other way, back to where it was.
  async function stepBack(way, n) {
    if (n > 3) await pressMany(way, n);
    else for (let i = 0; i < n; i++) await press(way, true);
  }

  // The settings on the menu on screen (its bulleted lines with a value): line -> {label, value}.
  const valuesOn = (screen) => new Map(screen.rows.filter((r) => /^\s*•/.test(r.text)).map((r) => [r.y, parseLine(r.text)]).filter(([, l]) => l.value !== null));

  // The menu's line y put back to value (another setting moved it: Native Sampling sets Samples per Line,
  // and leaves it there turned off), then the cursor back on line home. There with down (the menu goes
  // round); a number one key to learn its step, then the rest at once; a list round to it. False if not.
  async function putBack(y, value, home) {
    const goTo = async (to) => {
      for (let i = 0; i < 48; i++) {
        const s = await press(null);
        if (s.selected && s.selected.y === to) return true;
        await press('down', true);
      }
      return false;
    };
    if (!(await goTo(y))) return false;
    let now = valueOf(await press(null));
    const want = asNumber(value);
    for (let i = 0; i < 64 && !same(now, value); i++) {
      const a = asNumber(now);
      if (isNaN(a) || isNaN(want)) {
        const next = valueOf(await press('right'));
        if (same(next, now)) break; // it doesn't move: a reading
        now = next;
        continue;
      }
      const way = want > a ? 'right' : 'left', one = valueOf(await press(way)), d = Math.abs(asNumber(one) - a);
      if (!d) break; // it doesn't move
      const n = Math.round(Math.abs(want - asNumber(one)) / d);
      now = n ? valueOf(await pressMany(way, n)) : one;
    }
    const ok = same(now, value);
    return (await goTo(home)) && ok;
  }

  // One setting, selected on screen: one step and back; with "all", to both its ends (or round) and back,
  // its values listed (min, max and step when they're numbers), the bytes of up to 16 and the ends. The
  // menu's other settings it moved are put back.
  async function mapSetting(path, it) {
    const rec = { path, label: it.label, values: [], ranges: [] };
    if (it.section) rec.section = it.section;
    const name = path.concat(it.section || [], it.label).join(' › ');
    st.results.push(rec);
    const all = opts().all, limit = all ? 400 : 1, snaps = all ? 16 : 1;
    const start = await press(null), shown = valuesOn(start), home = start.selected ? start.selected.y : -1;
    const before = await snap();
    const right = await walk('right', it.value, limit, snaps);
    if (!right.round) await stepBack('left', right.steps);
    let left = { values: [], steps: 0, round: false };
    if (!right.round && (all || !right.values.length)) {
      left = await walk('left', it.value, limit, snaps);
      if (!left.round) await stepBack('right', left.steps);
    }
    let back = valueOf(await press(null));
    if (!same(back, it.value) && !reading(back, it.value) && home >= 0 && !st.stop) { // keys lost on the way back
      const ok = await putBack(home, it.value, home);
      log(name + ' came back to ' + back + ': ' + (ok ? 'put back to ' : 'could not put it back to ') + it.value, !ok);
      back = valueOf(await press(null));
    }
    for (const [y, l] of valuesOn(await press(null))) {
      const was = shown.get(y);
      if (y === home || !was || was.label !== l.label || same(l.value, was.value) || reading(l.value, was.value) || RISKY.test(l.label)) continue;
      const other = st.results.find((r) => r.label === l.label && r.path.join('\n') === path.join('\n'));
      if (other && /did not change/.test(other.backNote || '')) continue; // a reading (the audio levels)
      const ok = await putBack(y, was.value, home);
      log(it.label + ' moved ' + l.label + ' to ' + l.value + ': ' + (ok ? 'put back to ' : 'could not put it back to ') + was.value, !ok);
    }
    const after = await snap();
    if (st.stop) { // half walked: left out, so a run again maps it whole
      st.results.pop();
      if (!same(back, it.value)) log(it.label + ' did not come back: ' + back + ', was ' + it.value, true);
      return showResults();
    }
    const asks = right.asks || left.asks;
    if (asks) rec.asks = asks;
    const capped = [left.capped && 'first', right.capped && 'last'].filter(Boolean);
    if (capped.length) rec.capped = capped;
    if (!right.values.length && !left.values.length) {
      rec.backNote = asks ? 'asks first, cancelled: "' + asks + '"' : 'did not change: read only?';
      log(name + ': ' + it.value + ' (' + (asks ? rec.backNote : 'did not change') + ')');
      return showResults();
    }
    const order = [...left.values.slice().reverse(), { value: it.value, snap: before }, ...right.values];
    rec.list = order.map((s) => s.value);
    rec.round = right.round || left.round;
    Object.assign(rec, numeric(rec.list) || {});
    rec.ranges = union(order.filter((s) => s.snap && s.snap !== before).flatMap((s) => diff(before, s.snap)));
    rec.values = order.filter((s) => s.snap).map((s) => ({ value: s.value, at: takeAt(s.snap, rec.ranges) }));
    rec.leftover = diff(before, after);
    if (asks) rec.backNote = 'past ' + rec.list[asks === left.asks ? 0 : rec.list.length - 1] + ' it asks first, cancelled: "' + asks + '"';
    if (!same(back, it.value)) rec.backNote = 'did not come back: ' + back + ', was ' + it.value;
    if (!all) delete rec.list;
    showResults();
    log(name + ': ' + valuesText(rec).slice(0, 160), !!rec.note);
  }

  // The menu on screen, its submenus too: its items walking down until the first comes back, then each.
  async function mapMenu(path) {
    let screen = await look();
    const title = screen.title;
    if (!screen.selected) throw new Error('no menu open on the RT4K: open the one to map (Live screen, or the remote)');
    const items = [], first = screen.selected.text;
    do {
      items.push({ ...parseLine(screen.selected.text), y: screen.selected.y });
      screen = await press('down');
    } while (screen.selected && screen.selected.text !== first && items.length < 48 && !st.stop);
    sections(screen.rows, items);
    log(title + ': ' + items.length + ' items');
    let at = 0;
    for (let i = 0; i < items.length && !st.stop; i++) {
      for (; at < i; at++) screen = await press('down');
      const box = dialogOf(screen);
      if (box) { // (a walk's cancel lost on the way)
        log(title + ': "' + box + '", cancelled', true);
        screen = await press('back');
      }
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
      if (/^<.*>$/.test(it.value)) { log(title + ' › ' + it.label + ': an action, left alone'); continue; }
      if (st.results.some((r) => r.path.join('\n') === where.join('\n') && r.label === it.label && r.section === it.section)) continue; // done before a stop
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
      const end = await snap(), vol = volatileBytes(st.results);
      const left = diff(st.start, end).filter((g) => { for (let i = g.off; i < g.off + g.len; i++) if (!vol.has(i)) return true; return false; });
      log((st.stop ? 'Stopped' : 'Done') + (left.length ? ', but ' + rangeText(left) + ' differ from the start: reload the profile you had (Profiles view)' : ': the settings are as they were'), left.length > 0);
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
        const rec = { path: [screen.title || '(by hand)'], label: sel.label, values: [{ value: '(before)', at: takeAt(st.manual, d) }, { value: sel.value || '(now)', at: takeAt(now, d) }], ranges: d };
        st.results.push(rec);
        log('By hand, ' + sel.label + ': ' + (d.length ? d.length + ' places changed' : 'nothing changed'));
        st.manual = now;
        showResults();
      }
    } catch (e) {
      log(e.message === 'Failed to fetch' ? 'Cruller did not answer' : e.message, true);
    }
  }

  // What this page mapped, as a map for the RT4K's firmware ({v: 1, maps: [...]}, one map here): to merge
  // into the one the page ships with (src/web, tools/merge-map.js), mapped here for each firmware.
  async function download() {
    try {
      const state = await (await fetch('/api/v1/state')).json();
      const fw = (state.rt4k && state.rt4k.firmware) || 'unknown';
      const f = fields(sgetReady), mapped = st.results.filter((r) => r.own && r.own.length).map(compact);
      const doc = keep(null, fw, +f.ver || null, +f.size || null, mapped);
      const a = document.createElement('a');
      a.href = URL.createObjectURL(new Blob([JSON.stringify(doc, null, 1)], { type: 'application/json' }));
      a.download = 'rt4k-settings-' + fw + '-' + new Date().toISOString().slice(0, 16).replace(/[:T]/g, '') + '.json';
      a.click();
      setTimeout(() => URL.revokeObjectURL(a.href), 1000);
      log('Downloaded ' + mapped.length + ' settings for ' + fw);
    } catch (e) {
      log(e.message === 'Failed to fetch' ? 'Cruller did not answer' : e.message, true);
    }
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
      '<label class="small"><input type="checkbox" id="mp-all"> every value: to both ends (slow)</label>' +
      '<label class="small"><input type="checkbox" id="mp-risky"> the output and the input too</label>' +
      '<span class="grow"></span><button id="mp-snap">Snapshot</button><button id="mp-cmp">Compare</button>' +
      '<button id="mp-dl" disabled>Download JSON</button></div>' +
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
  window.mapperInternals = { fields, readPlane, parseLine, sections, dialogOf, same, reading, diff, union, bytesAt, split, takeAt, hexAt, numeric, compact, keep, settingsOf, // tests/test_mapper.js
    walk, mapSetting, st, opts: (o) => { optsOverride = o; }, sleepless: () => { sleepMs = 0; } };
})();
