// Tests for the RT4K firmware updater's logic (src/web/fw.js): SHA-256, RetroTINK's index format,
// zip reading, which files go to the SD card. Run by tests/run.sh (node 18+).
//   RT4K_ZIP=<zip> RT4K_ZIP_SHA=<hex> node tests/test_fw.js   also checks a real firmware zip

const crypto = require('crypto');
const fs = require('fs');
const path = require('path');
const vm = require('vm');

const window = {};
const context = vm.createContext({
  window, TextDecoder, Blob, Response, DecompressionStream, Uint8Array, Uint32Array, DataView,
});
for (const file of ['sha256.js', 'fw.js']) { // as index.html loads them
  vm.runInContext(fs.readFileSync(path.join(__dirname, '..', 'src', 'web', file), 'utf8'), context);
}
const fw = window.fwInternals;

let checks = 0, failures = 0;
function check(cond, what) {
  checks++;
  if (!cond) { failures++; console.log('  FAIL ' + what); }
}
const ref = (buf) => crypto.createHash('sha256').update(buf).digest('hex');

async function main() {
  // SHA-256: known vectors, then every length around the padding boundaries against node's.
  check(fw.sha256(new Uint8Array(0)) === 'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855', 'sha256 empty');
  check(fw.sha256(Buffer.from('abc')) === 'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad', 'sha256 abc');
  for (let n = 0; n < 200; n++) {
    const b = crypto.randomBytes(n);
    check(fw.sha256(new Uint8Array(b)) === ref(b), 'sha256 length ' + n);
  }
  const big = crypto.randomBytes(1 << 20);
  check(fw.sha256(new Uint8Array(big)) === ref(big), 'sha256 1 MiB');
  const view = new Uint8Array(big.buffer, big.byteOffset + 3, 1000); // a view at an odd offset
  check(fw.sha256(view) === ref(Buffer.from(view)), 'sha256 of a subarray');

  // The index: RetroTINK's markdown layout.
  const md = [
    '# RetroTINK-4K Experimental Firmware', '', 'intro text', '',
    '## Version 1.87.3 (2026-09-25)', '',
    '### [Download](https://cdn.jsdelivr.net/gh/retrotink-llc/firmware@main/RetroTINK-4K/Experimental/rt4k_1873.zip)',
    'CRC-32: `6DCE6294`  ', 'SHA-256: `B90BE0E654322D6F41119827FF5C3ED6D1493C2565BB3484753158D406400A21`', '',
    '### Changelog:', '- Third-generation LumaCode engine', '- Replace the lumacode folder', '', '<br/>', '', '',
    '## Version 1.87.2 (2026-09-24)', '',
    '### [Download](https://cdn.jsdelivr.net/gh/retrotink-llc/firmware@main/RetroTINK-4K/Experimental/rt4k_1872.zip)',
    'SHA-256: `58e5ceef02fbf4bfd262ec7e48f5bf0154cf48ff7ba0ce557d2bc3e8f5df8559`', '',
    '### Changelog:', '- Fixes', '',
    '## Version 1.0.0 (2020-01-01)', '', 'no download link here', '',
  ].join('\n');
  const idx = fw.parseIndex(md);
  check(idx.length === 2, 'index: two usable entries (one without a link skipped), got ' + idx.length);
  check(idx[0].version === '1.87.3' && idx[0].date === '2026-09-25', 'index: version and date');
  check(idx[0].url.endsWith('/rt4k_1873.zip'), 'index: url');
  check(idx[0].sha === 'b90be0e654322d6f41119827ff5c3ed6d1493c2565bb3484753158d406400a21', 'index: sha lower-cased');
  check(idx[0].changelog.includes('LumaCode') && !idx[0].changelog.includes('1.87.2'), 'index: changelog of its own section');
  check(idx[1].changelog === '- Fixes', 'index: last changelog');
  check(fw.rawUrl(idx[0].url) === 'https://raw.githubusercontent.com/RetroTINK-LLC/firmware/main/RetroTINK-4K/Experimental/rt4k_1873.zip',
    'rawUrl maps jsDelivr to GitHub');

  // Zip: entries (folders skipped), deflated and stored data.
  const zip = new Uint8Array(fs.readFileSync(path.join(__dirname, 'fixtures', 'fw_sample.zip')));
  const entries = fw.zipEntries(zip);
  const names = entries.map((e) => e.name);
  check(names.length === 6, 'zip: 6 files, got ' + names.length);
  check(!names.some((n) => n.endsWith('/')), 'zip: no folder entries');
  const get = async (name) => Buffer.from(await fw.unzipEntry(entries.find((e) => e.name === name))).toString();
  check((await get('rt4kup.bin')) === 'updater '.repeat(100), 'zip: deflated file');
  check((await get('lumacode/NES/PVM Style D93 (FBX).lmc')) === 'luma '.repeat(50), 'zip: file in a folder, spaces in the name');
  check((await get('stored.txt')) === 'stored, not deflated', 'zip: stored file');
  let threw = false;
  try { fw.zipEntries(new Uint8Array(100)); } catch (e) { threw = true; }
  check(threw, 'zip: garbage is refused');

  // Which files go to the SD card: all but the other models' .rbf.
  const pro = names.filter((n) => fw.wanted(n, 'RT4K_Pro'));
  check(pro.includes('rt4k_1873.rbf') && !pro.includes('rt4kce_1873.rbf') && !pro.includes('rt6x_1873.rbf'), 'wanted: Pro gets rt4k_');
  check(pro.includes('rt4kup.bin') && pro.includes('lumacode/NES/PVM Style D93 (FBX).lmc'), 'wanted: updater and extras');
  const ce = names.filter((n) => fw.wanted(n, 'RT4K_CE'));
  check(ce.includes('rt4kce_1873.rbf') && !ce.includes('rt4k_1873.rbf'), 'wanted: CE gets rt4kce_');

  // A real firmware zip, when given.
  if (process.env.RT4K_ZIP) {
    const real = new Uint8Array(fs.readFileSync(process.env.RT4K_ZIP));
    if (process.env.RT4K_ZIP_SHA) check(fw.sha256(real) === process.env.RT4K_ZIP_SHA.toLowerCase(), 'real zip: SHA-256');
    const es = fw.zipEntries(real);
    for (const e of es) {
      const data = await fw.unzipEntry(e);
      check(data.length === e.size, 'real zip: ' + e.name + ' unzips to its size');
    }
    console.log('real zip: ' + es.length + ' files unzipped, ' + es.filter((e) => fw.wanted(e.name, 'RT4K_Pro')).length + ' for an RT4K Pro');
  }

  console.log(failures ? 'fw: ' + failures + ' of ' + checks + ' checks failed' : 'fw: ' + checks + ' checks ok');
  process.exit(failures ? 1 : 0);
}

main().catch((e) => { console.log('fw: ' + e.stack); process.exit(1); });
