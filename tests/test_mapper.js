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

const a = Uint8Array.from([0, 1, 2, 3, 4, 5, 6, 7]);
const b = Uint8Array.from([0, 9, 9, 3, 4, 9, 6, 7]);
check(JSON.stringify(m.diff(a, b)) === '[{"off":1,"len":2},{"off":5,"len":1}]', 'diff: ' + JSON.stringify(m.diff(a, b)));
check(JSON.stringify(m.diff(a, b, 2)) === '[{"off":1,"len":5}]', 'diff, near ones merged');
check(m.diff(a, a).length === 0, 'no diff');
check(JSON.stringify(m.union([[{ off: 5, len: 1 }], [{ off: 1, len: 2 }, { off: 2, len: 2 }]].flat())) === '[{"off":1,"len":3},{"off":5,"len":1}]', 'union');
check(m.bytesAt(b, [{ off: 1, len: 2 }, { off: 5, len: 1 }]) === '09 09 | 09', 'bytes at ranges');
check(m.fields('sget ready size=22876 ver=110 nonce=0x12').ver === '110', 'ready fields');

console.log(failures ? `mapper.js: ${failures} of ${checks} checks failed` : `mapper.js: ${checks} checks ok`);
process.exit(failures ? 1 : 0);
