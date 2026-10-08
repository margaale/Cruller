// Tests for the profile editor's logic (src/web/editor.js): a profile's CRC, and every setting the map
// has written and read back. Run with node (tests/run.sh).

const fs = require('fs');
const path = require('path');
const vm = require('vm');

const context = { window: {}, console, document: { getElementById: () => null } };
vm.createContext(context);
vm.runInContext(fs.readFileSync(path.join(__dirname, '..', 'src', 'web', 'editor.js'), 'utf8'), context);
const e = context.window.editorInternals;
const doc = JSON.parse(fs.readFileSync(path.join(__dirname, '..', 'src', 'web', 'rt4k_settings.json'), 'utf8'));

let checks = 0, failures = 0;
function check(cond, what) {
  checks++;
  if (!cond) {
    failures++;
    console.log('  FAIL ' + what);
  }
}

// CRC-16/XMODEM's check value, and a profile built and read back.
check(e.crc16(Buffer.from('123456789')) === 0x31c3, 'CRC-16/XMODEM of "123456789": ' + e.crc16(Buffer.from('123456789')).toString(16));
const header = new Uint8Array(128);
header.set(Buffer.from('RT4K Profile'));
const body = new Uint8Array(22876);
body[0x524] = 10;
const file = e.buildProfile(header, body);
const back = e.parseProfile(file);
check(file.length === 23004 && back && back.crcOk && back.body[0x524] === 10, 'a profile built: 128 + 22876 bytes, its CRC right');
file[128 + 0x524] = 11;
check(!e.parseProfile(file).crcOk, 'a body changed after: the CRC says so');
check(e.parseProfile(new Uint8Array(200)) === null, 'not a profile');

// The map for a firmware: its own, else the newest.
const two = { maps: [{ firmware: '1.92.0' }, { firmware: '1.100.0' }] };
check(e.mapFor(two, '1.92.0').firmware === '1.92.0' && e.mapFor(two, '1.93.0').firmware === '1.100.0', 'the map for a firmware');

// Every setting of the shipped map: each value it saw written and read back; numbers at both ends.
for (const m of doc.maps) {
  const codecs = e.codecs(m.settings);
  let numbers = 0, lists = 0, raw = 0;
  m.settings.forEach((s, i) => {
    const c = codecs[i], name = m.firmware + ' ' + s.path + ' › ' + (s.section ? s.section + ' › ' : '') + s.label;
    if (c.type === 'raw') { if (!s.readonly) raw++; return; } // (readonly: shown, not edited: the device ID)
    const b = new Uint8Array(22876);
    if (c.type === 'list') {
      lists++;
      for (const [shown] of s.values) {
        check(e.encode(s, c, b, shown) && e.decode(s, c, b).value === shown, name + ': ' + shown);
      }
      return;
    }
    numbers++;
    for (const v of [c.min, c.max]) {
      e.encode(s, c, b, v);
      const got = e.decode(s, c, b).value;
      check(Math.abs(got - v) < c.step / 2, name + ': ' + v + ' read back as ' + got);
    }
    for (const [shown, hex] of s.values || []) {
      const v = e.asNumber(shown);
      if (isNaN(v) || /\?/.test(hex)) continue;
      e.encode(s, c, b, v);
      const got = e.decode(s, c, b);
      if (c.fit.kind === 'f32') check(Math.abs(got.value - v) < c.step / 2, name + ': ' + shown + ' read back as ' + got.value); // (the RT4K's own drift a little)
      else check(got.hex === hex, name + ': ' + shown + ' written as ' + got.hex + ', the RT4K wrote ' + hex);
    }
  });
  check(raw === 0, m.firmware + ': every setting editable (' + numbers + ' numbers, ' + lists + ' lists, ' + raw + ' not)');
}

console.log(failures ? `editor.js: ${failures} of ${checks} checks failed` : `editor.js: ${checks} checks ok`);
process.exit(failures ? 1 : 0);
