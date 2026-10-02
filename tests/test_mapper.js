// Tests for the settings map's logic (src/web/mapper.js): the OSD plane as text, menu lines, and where two
// snapshots of the settings differ. Run with node (tests/run.sh).

const fs = require('fs');
const path = require('path');
const vm = require('vm');

const context = { window: {}, console };
vm.createContext(context);
vm.runInContext(fs.readFileSync(path.join(__dirname, '..', 'src', 'web', 'mapper.js'), 'utf8'), context);
const m = context.window.mapperInternals;

let checks = 0, failures = 0;
function check(cond, what) {
  checks++;
  if (!cond) {
    failures++;
    console.log('  FAIL ' + what);
  }
}

// An OSD plane as the RT4K sends it: 2048 character codes, then 2048 colours (background in the top
// two bits: 1 light). rows: [text, selected].
function plane(rows, width = 40, stride = 64) {
  const d = new Uint8Array(4096);
  rows.forEach(([text, sel], y) => {
    for (let x = 0; x < width; x++) {
      const c = x < text.length ? text.charCodeAt(x) : 32;
      d[y * stride + x] = c === 0x2022 ? 0x07 : c; // a bullet: not ASCII
      d[2048 + y * stride + x] = sel ? 0x40 : 0x3f;
    }
  });
  return { data: d, ready: 'osd ready rows=' + rows.length + ' stride=' + stride + ' width=' + width + ' cells=2048 nonce=0x1' };
}

const p = plane([
  ['Scaling/Crop Setup', false],
  ['', false],
  [' • Input Crop', false],
  [' • Top Trim:          +0', true],
  [' • Bottom Trim:       +0', false],
  ['Profile: None Loaded', true],
  ['v1.92.0, Mode: No Signal', true],
]);
const r = m.readPlane(p.data, p.ready);
check(r.title === 'Scaling/Crop Setup', 'the title: ' + r.title);
check(r.selected && r.selected.y === 3, 'the selected line, the footer left out');
check(r.rows.length === 7 && r.rows[2].text === ' • Input Crop', 'rows as text, a bullet kept as one: ' + JSON.stringify(r.rows[2].text));

const line = (t) => JSON.stringify(m.parseLine(t));
check(line(' • Top Trim:          +0') === '{"label":"Top Trim","value":"+0"}', 'a setting: ' + line(' • Top Trim:          +0'));
check(line(' • Buffer Length:     Min. Lag (100.0 ms') === '{"label":"Buffer Length","value":"Min. Lag (100.0 ms"}', 'a value with spaces');
check(line(' • Vertical Only:     <Start>') === '{"label":"Vertical Only","value":"<Start>"}', 'an action');
check(line(' • Scaling/Cropping') === '{"label":"Scaling/Cropping","value":null}', 'a submenu');
check(line(' • Masking Color:') === '{"label":"Masking Color","value":null}', 'a heading with a colon');
check(line(' • HDMI• Output:      4K60') === '{"label":"HDMI• Output","value":"4K60"}', 'a label with a symbol in it');
check(line(' • Enable 343.200     Off') === '{"label":"Enable 343.200","value":"Off"}' && line(' • Progressive Detection') === '{"label":"Progressive Detection","value":null}', 'no colon, the value in its column: ' + line(' • Enable 343.200     Off'));
check(JSON.stringify(m.numeric(['3431 dots/line', '3432 dots/line'])) === '{"min":3431,"max":3432,"step":1}', 'a unit with a slash');

// A label twice on one menu (Processing/Effects: Function, Strength): told apart by the heading above.
const fx = plane([
  ['Processing/Effects Setup', false], ['', false], [' • Scanline', false], [' • Function:          Off', true], [' • Strength:          Off', false],
  [' • Mask', false], [' • Enable:            Off', false], [' • Strength:          N/A', false], [' • Horizontal Blur', false], [' • Function:          Off', false],
]);
const fxItems = [3, 4, 6, 7, 9].map((y) => ({ ...m.parseLine(m.readPlane(fx.data, fx.ready).rows[y].text), y }));
m.sections(m.readPlane(fx.data, fx.ready).rows, fxItems);
check(JSON.stringify(fxItems.map((it) => it.section || '-')) === '["Scanline","Scanline","-","Mask","Horizontal Blur"]', 'sections of labels seen twice: ' + JSON.stringify(fxItems.map((it) => it.section || '-')));
check(JSON.stringify(m.compact({ path: ['Advanced', 'Processing/Effects Setup'], section: 'Horizontal Blur', label: 'Function' })) === '{"path":"Advanced › Processing/Effects Setup","section":"Horizontal Blur","label":"Function"}', 'a section kept');
let twice = m.keep(null, '1.92.0', 109, 22876, [{ path: 'P', section: 'Scanline', label: 'Function' }, { path: 'P', section: 'Horizontal Blur', label: 'Function' }]);
check(m.settingsOf(twice, '1.92.0').length === 2, 'both kept');

// A box asking first (Black Frame Insertion's Min. BFI Limit): nothing selected, its text read.
const BOX = [['••••••••', false], ['Warning! Flicker may induce epilepsy.', false], ['Proceed at your own risk!!', false], ['', false], ['[Cancel]      [OK]', false], ['••••••••', false]];
const box = plane(BOX);
check(m.dialogOf(m.readPlane(box.data, box.ready)) === 'Warning! Flicker may induce epilepsy. Proceed at your own risk!!', 'a box asking first: ' + m.dialogOf(m.readPlane(box.data, box.ready)));
check(m.dialogOf(r) === null, 'a menu is not one');

const a = Uint8Array.from([0, 1, 2, 3, 4, 5, 6, 7]);
const b = Uint8Array.from([0, 9, 9, 3, 4, 9, 6, 7]);
check(JSON.stringify(m.diff(a, b)) === '[{"off":1,"len":2},{"off":5,"len":1}]', 'diff: ' + JSON.stringify(m.diff(a, b)));
check(JSON.stringify(m.diff(a, b, 2)) === '[{"off":1,"len":5}]', 'diff, near ones merged');
check(m.diff(a, a).length === 0, 'no diff');
check(JSON.stringify(m.union([[{ off: 5, len: 1 }], [{ off: 1, len: 2 }, { off: 2, len: 2 }]].flat())) === '[{"off":1,"len":3},{"off":5,"len":1}]', 'union');
check(m.bytesAt(b, [{ off: 1, len: 2 }, { off: 5, len: 1 }]) === '09 09 | 09', 'bytes at ranges');
check(m.fields('sget ready size=22876 ver=110 nonce=0x12').ver === '110', 'ready fields');

// Own and shared bytes: Top Trim and Bottom Trim each have theirs, and both move the scaler's factors.
const recs = m.split([
  { label: 'Top Trim', ranges: [{ off: 0x524, len: 1 }, { off: 0xcac, len: 2 }] },
  { label: 'Bottom Trim', ranges: [{ off: 0x624, len: 1 }, { off: 0xcac, len: 2 }] },
  { label: 'Left Trim', ranges: [{ off: 0x424, len: 1 }, { off: 0xcad, len: 2 }] },
]);
const txt = (r) => JSON.stringify([r.own, r.shared]);
check(txt(recs[0]) === '[[{"off":1316,"len":1}],[{"off":3244,"len":2}]]', 'own and shared: ' + txt(recs[0]));
check(txt(recs[2]) === '[[{"off":1060,"len":1},{"off":3246,"len":1}],[{"off":3245,"len":1}]]', 'shared byte by byte: ' + txt(recs[2]));
// One setting in two menus (Decimation Factor, Input Pixels at 0xdf8): the one left without bytes of its
// own keeps that one.
const two = m.split([
  { label: 'Decimation Factor', ranges: [{ off: 0xdf8, len: 1 }, { off: 0xcac, len: 2 }] },
  { label: 'Input Pixels', ranges: [{ off: 0xdf8, len: 1 }, { off: 0x1808, len: 1 }, { off: 0xcac, len: 2 }] },
  { label: 'Top Trim', ranges: [{ off: 0x524, len: 1 }, { off: 0xcac, len: 2 }] },
]);
check(txt(two[0]) === '[[{"off":3576,"len":1}],[{"off":3244,"len":2}]]' && txt(two[1]) === '[[{"off":6152,"len":1}],[{"off":3244,"len":2},{"off":3576,"len":1}]]',
  'shared with just one other, kept by the one without its own: ' + txt(two[0]) + ' ' + txt(two[1]));
// A reading (0x5843, its word left changed after two settings): no setting's own.
const reading = m.split([
  { label: 'Samples per Line', ranges: [{ off: 0xcf8, len: 2 }, { off: 0x5843, len: 1 }], leftover: [{ off: 0x5840, len: 1 }] },
  { label: 'Native Sampling', ranges: [{ off: 0x23a0, len: 1 }, { off: 0xcf8, len: 2 }] },
  { label: 'Pre-ADC', ranges: [{ off: 0x1178, len: 2 }], leftover: [{ off: 0x5840, len: 1 }] },
]);
check(txt(reading[0]) === '[[{"off":3320,"len":2}],[{"off":22595,"len":1}]]', 'a reading left out of own: ' + txt(reading[0]));
check(JSON.stringify(m.numeric(['-2', '-1', '+0', '+1', '+2'])) === '{"min":-2,"max":2,"step":1}', 'numbers: min, max, step');
check(JSON.stringify(m.numeric(['31', '30', '0'])) === '{"min":0,"max":31,"step":1}', 'numbers out of order');
check(m.numeric(['Off', 'On']) === null && m.numeric(['1']) === null && m.numeric(['1', '2x', 'Auto']) === null, 'not numbers');
check(JSON.stringify(m.numeric(['0.5x', '1x', '1.5x'])) === '{"min":0.5,"max":1.5,"step":0.5}', 'with a unit');
check(JSON.stringify(m.numeric(['+-100 (Y Min: 32)', '+0 (Y Min: 32)', '+100 (Y Min: 32)'])) === '{"min":-100,"max":100,"step":100}', 'with the RT4K\'s explanation in brackets, its "+-" negatives');
const at = m.takeAt(b, [{ off: 1, len: 2 }]);
check(JSON.stringify(at) === '{"1":9,"2":9}' && m.hexAt(at, [{ off: 1, len: 2 }, { off: 6, len: 1 }]) === '09 09 | ??', 'bytes by offset, back as hex');

// The map as shipped: one per firmware, each whole; runs add up, a setting mapped again replaces it.
const top = { path: 'Scaling/Crop Setup', label: 'Top Trim', bytes: [[0x524, 2]], min: -403, max: 403, step: 1 };
const red = { path: 'Scaling/Crop Setup', label: 'Red', bytes: [[0x17d4, 1]], min: 0, max: 31, step: 1 };
let doc = m.keep(null, '1.92.0', 109, 22876, [top]);
doc = m.keep(doc, '1.92.0', 109, 22876, [red, { ...top, max: 400 }]);
check(doc.maps.length === 1 && doc.maps[0].settings.length === 2 && m.settingsOf(doc, '1.92.0')[0].max === 400, 'two runs on one firmware: one map, the later setting kept');
doc = m.keep(doc, '1.93.0', 110, 22900, [{ ...red, bytes: [[0x17e4, 1]] }]);
check(doc.maps.length === 2 && m.settingsOf(doc, '1.93.0').length === 1 && m.settingsOf(doc, '1.93.0')[0].bytes[0][0] === 0x17e4, 'a new firmware: its own map');
check(m.settingsOf(doc, '1.92.0').find((c) => c.label === 'Red').bytes[0][0] === 0x17d4 && m.settingsOf(doc, '1.80.0') === null, 'the other untouched');
const rec = { path: ['Advanced', 'Scaling/Crop Setup'], label: 'Top Trim', own: [{ off: 0x524, len: 2 }], values: [{ value: '-403', at: { 1316: 0x6d, 1317: 0xfe } }], min: -403, max: 403, step: 1 };
check(JSON.stringify(m.compact(rec)) === '{"path":"Advanced › Scaling/Crop Setup","label":"Top Trim","bytes":[[1316,2]],"values":[["-403","6dfe"]],"min":-403,"max":403,"step":1}',
  'a setting as kept: ' + JSON.stringify(m.compact(rec)));

// --- a setting walked on a simulated RT4K ------------------------------------------------------------------
// "Top Trim" from 0 to 300 at 0x524 (two bytes), a counter at 0x5810 that every change moves; the remote's
// keys through /api/v1/command, one or several; the menu and the settings as the RT4K sends them. asks: a
// change past asksPast asks first (a box until back or ok; ok goes on).

const respond = (body, headers = {}) => ({ ok: true, status: 200, headers: { get: (h) => headers[h] || null }, arrayBuffer: async () => body.buffer, json: async () => JSON.parse(body), text: async () => String(body) });
// /api/v1/command's answer: a 503 when Cruller couldn't hand the RT4K some ("sent": false).
const answered = (cmds, sent) => {
  const all = !sent || sent.every(Boolean);
  return { ...respond(JSON.stringify({ ok: all, results: cmds.map((c, i) => ({ command: c, sent: !sent || sent[i], reply: !sent || sent[i] ? ['[COM] Serial Remote: x'] : [] })) })), ok: all, status: all ? 200 : 503 };
};

function rt4k(min, max, asksPast, live, lossy) {
  const s = { v: 0, counter: 0, keys: 0, requests: 0, box: false, oks: 0, lost: 0 };
  const label = () => ' • Top Trim:          ' + (s.v >= 0 ? '+' : '') + s.v + (live ? ' (Rmax: ' + (30 + (s.reads || 0) % 8) + ')' : '');
  s.fetch = async (url, o) => {
    if (url === '/rt4k/xfer?cmd=osd') {
      s.reads = (s.reads || 0) + 1;
      const p = s.box ? plane(BOX) : plane([['Scaling/Crop Setup', false], ['', false], [label(), true]]);
      return respond(p.data, { 'X-Ready': p.ready });
    }
    if (url === '/rt4k/xfer?cmd=sget') {
      const d = new Uint8Array(22876);
      d[0x524] = s.v & 255;
      d[0x525] = s.v >> 8;
      d[0x5810] = s.counter & 255;
      return respond(d, { 'X-Ready': 'sget ready size=22876 ver=109 nonce=0x1' });
    }
    if (url === '/api/v1/command') {
      s.requests++;
      const b = JSON.parse(o.body), cmds = b.commands || [b.command], sent = cmds.map(() => true);
      for (const [i, c] of cmds.entries()) {
        s.keys++;
        if (lossy && s.keys % lossy === 0) { // (Cruller couldn't hand it the RT4K)
          s.lost++;
          sent[i] = false;
          continue;
        }
        if (s.box) {
          if (c === 'remote ok') s.oks++;
          if (c === 'remote back' || c === 'remote ok') s.box = false;
          continue;
        }
        const before = s.v;
        if (asksPast !== undefined && (c === 'remote right' ? s.v + 1 : s.v - 1) > asksPast) {
          s.box = true;
          continue;
        }
        if (c === 'remote right') s.v = Math.min(max, s.v + 1);
        if (c === 'remote left') s.v = Math.max(min, s.v - 1);
        if (s.v !== before) s.counter++;
      }
      return answered(cmds, sent);
    }
    throw new Error('no ' + url);
  };
  return s;
}

// The ADC's two lines: Native Sampling turned on sets Samples per Line (3509), and turned off leaves it.
function adc() {
  const s = { spl: 1716, ns: false, cur: 1 };
  s.fetch = async (url, o) => {
    if (url === '/rt4k/xfer?cmd=osd') {
      const p = plane([['RGB/Component ADC Setup', false], ['', false], [' • Samples per Line:  ' + s.spl + ' (1440 Act.)', s.cur === 0], [' • Native Sampling:   ' + (s.ns ? 'On' : 'Off'), s.cur === 1]]);
      return respond(p.data, { 'X-Ready': p.ready });
    }
    if (url === '/rt4k/xfer?cmd=sget') {
      const d = new Uint8Array(22876);
      d[0xcf8] = (s.spl - 1716) & 255;
      d[0xcf9] = ((s.spl - 1716) >> 8) & 255;
      d[0x23a0] = s.ns ? 1 : 0;
      return respond(d, { 'X-Ready': 'sget ready size=22876 ver=109 nonce=0x1' });
    }
    if (url === '/api/v1/command') {
      const b = JSON.parse(o.body), cmds = b.commands || [b.command];
      for (const c of cmds) {
        if (c === 'remote down' || c === 'remote up') s.cur = 1 - s.cur;
        else if (s.cur === 0 && c === 'remote right') s.spl = Math.min(4095, s.spl + 1);
        else if (s.cur === 0 && c === 'remote left') s.spl = Math.max(1024, s.spl - 1);
        else if (s.cur === 1 && (c === 'remote right' || c === 'remote left')) {
          s.ns = !s.ns;
          if (s.ns) s.spl = 3509;
        }
      }
      return answered(cmds);
    }
    throw new Error('no ' + url);
  };
  return s;
}

// mapper.js on a simulated RT4K, every value walked.
function on(sim) {
  const ids = {};
  const ctx = {
    window: {}, console, setTimeout, Blob: class {}, URL: {},
    fetch: (u, o) => sim.fetch(u, o),
    document: { getElementById: (id) => ids[id] || (ids[id] = { textContent: '', innerHTML: '', disabled: false, checked: false, scrollTop: 0, scrollHeight: 0 }) },
  };
  vm.createContext(ctx);
  vm.runInContext(fs.readFileSync(path.join(__dirname, '..', 'src', 'web', 'mapper.js'), 'utf8'), ctx);
  const w = ctx.window.mapperInternals;
  w.sleepless();
  w.opts({ all: true, risky: false });
  return w;
}

async function walked() {
  const sim = rt4k(0, 300);
  const w = on(sim);
  await w.mapSetting(['Scaling/Crop Setup'], { label: 'Top Trim', value: '+0' });
  const rec = w.st.results[0];
  check(sim.v === 0, 'walked to both ends and back to +0: ' + sim.v);
  check(rec && rec.min === 0 && rec.max === 300 && rec.step === 1, 'its range: ' + JSON.stringify(rec && [rec.min, rec.max, rec.step]));
  // (With one setting, the counter can't be told from its own bytes: that takes two.)
  check(rec && JSON.stringify(rec.ranges) === '[{"off":1316,"len":2},{"off":22544,"len":1}]', 'its bytes: ' + JSON.stringify(rec && rec.ranges));
  check(sim.requests < 120, 'eight keys at a time once the step is known: ' + sim.requests + ' requests for ' + sim.keys + ' keys');
  check(!rec.backNote, 'came back: ' + rec.backNote);
}

// A long range (the ADC's Samples per Line, ~1716 of thousands): past the 400 steps a list gets, and more
// keys between reads as it goes.
async function long() {
  const sim = rt4k(0, 1000);
  const w = on(sim);
  await w.mapSetting(['RGB/Component ADC Setup'], { label: 'Top Trim', value: '+0' });
  const rec = w.st.results[0];
  check(sim.v === 0 && rec.max === 1000 && !rec.backNote, 'walked to 1000 and back: ' + JSON.stringify([sim.v, rec.max, rec.backNote]));
  check(sim.reads < 120, 'up to 64 keys between reads: ' + sim.reads + ' reads');
  // Past 16 times 400 steps (the colour matrix's ±0.001 coefficients): its ends left open.
  const far = rt4k(-10000, 10000), wf = on(far);
  await wf.mapSetting(['Color Correction Setup'], { label: 'Top Trim', value: '+0' });
  const rf = wf.st.results[0];
  check(far.v === 0 && JSON.stringify(rf.capped) === '["first","last"]' && wf.compact({ ...rf, own: [{ off: 0x524, len: 2 }] }).capped.length === 2, 'capped both ways: ' + JSON.stringify([far.v, rf.capped, rf.min, rf.max]));
}

// Keys Cruller couldn't hand the RT4K: one in 13, answered "sent": false. Sent again.
async function lossy() {
  const sim = rt4k(-200, 200, undefined, false, 13), w = on(sim);
  await w.mapSetting(['Color Correction Setup'], { label: 'Top Trim', value: '+0' });
  const rec = w.st.results[0];
  check(sim.lost > 10 && sim.v === 0 && rec.min === -200 && rec.max === 200, 'its ends and back with keys lost: ' + JSON.stringify([sim.lost, sim.v, rec.min, rec.max, rec.backNote]));
}

// A reading in brackets that moves on its own (the ADC's gains: "1.000 (Rmax: 30)"): not a step.
async function live() {
  check(m.same('1.000 (Rmax: 30)', '1.000 (Rmax: 37)') && m.same('-0.00', '0.00') && !m.same('Auto (PAL)', 'Auto (NTSC)') && !m.same('1.004', '1.000'), 'the same value shown');
  const sim = rt4k(-50, 50, undefined, true);
  const w = on(sim);
  await w.mapSetting(['RGB/Component ADC Setup'], { label: 'Top Trim', value: '+0 (Rmax: 30)' });
  const rec = w.st.results[0];
  check(sim.v === 0 && rec.min === -50 && rec.max === 50 && !rec.backNote, 'a moving reading walked by its number: ' + JSON.stringify([sim.v, rec.min, rec.max, rec.backNote]));
}

// A setting that asks first: right away (none of it walked), and past +2 (up to it walked).
async function asked() {
  let sim = rt4k(0, 300, 0), w = on(sim);
  await w.mapSetting(['Black Frame Insertion Setup'], { label: 'Top Trim', value: '+0' });
  let rec = w.st.results[0];
  check(!sim.box && sim.oks === 0 && sim.v === 0, 'the box cancelled, never OK, the setting as it was: ' + JSON.stringify([sim.box, sim.oks, sim.v]));
  check(rec && /^asks first, cancelled: "Warning! Flicker/.test(rec.backNote), 'noted: ' + (rec && rec.backNote));
  sim = rt4k(0, 300, 2);
  w = on(sim);
  await w.mapSetting(['Black Frame Insertion Setup'], { label: 'Top Trim', value: '+0' });
  rec = w.st.results[0];
  check(!sim.box && sim.oks === 0 && sim.v === 0 && rec.min === 0 && rec.max === 2, 'walked up to the box, cancelled, back: ' + JSON.stringify([sim.box, sim.oks, sim.v, rec.min, rec.max]));
  check(/^past \+2 it asks first/.test(rec.backNote), 'noted where: ' + rec.backNote);
  const kept = w.compact({ ...rec, own: [{ off: 0x524, len: 2 }], note: 'did not come back as it was' });
  check(/^Warning! Flicker/.test(kept.asks) && kept.note === undefined, 'kept: what it asks, not how the run went: ' + JSON.stringify(kept));
  sim = rt4k(0, 300, 20); // eight keys at a time by then: the box comes up in the middle of them
  w = on(sim);
  await w.mapSetting(['Black Frame Insertion Setup'], { label: 'Top Trim', value: '+0' });
  rec = w.st.results[0];
  check(!sim.box && sim.oks === 0 && sim.v === 0 && rec.max === 20 && /^past \+20 it asks/.test(rec.backNote), 'the box in a batch: ' + JSON.stringify([sim.box, sim.oks, sim.v, rec.max, rec.backNote]));
}

// A setting that moves another on its menu: the other put back, the cursor back on it.
async function movesAnother() {
  const sim = adc(), w = on(sim);
  await w.mapSetting(['RGB/Component ADC Setup'], { label: 'Native Sampling', value: 'Off' });
  check(!sim.ns && sim.spl === 1716 && sim.cur === 1, 'Samples per Line put back, the cursor on Native Sampling: ' + JSON.stringify(sim));
  check(w.st.log.some((l) => /Native Sampling moved Samples per Line to 3509 .*: put back to 1716/.test(l[0])), 'said so: ' + JSON.stringify(w.st.log.map((l) => l[0])));
}

walked().then(asked).then(long).then(live).then(movesAnother).then(lossy).then(() => {
  console.log(failures ? `mapper.js: ${failures} of ${checks} checks failed` : `mapper.js: ${checks} checks ok`);
  process.exit(failures ? 1 : 0);
}, (e) => {
  console.log('  FAIL the walk threw: ' + e.stack);
  process.exit(1);
});
