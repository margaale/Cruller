// Merges settings maps downloaded from the Debug tab's Settings map (rt4k-settings-<fw>-<time>.json) into
// the one the page ships with, src/web/rt4k_settings.json: each run's settings into its firmware's map
// (one per RT4K firmware, each whole). Run from the repo's root:
//   node tools/merge-map.js ~/Downloads/rt4k-settings-1.92.0-*.json

const fs = require('fs');
const path = require('path');
const vm = require('vm');

const OUT = path.join(__dirname, '..', 'src', 'web', 'rt4k_settings.json');

// mapper.js's keep(), as the page runs it.
const ctx = { window: {}, console };
vm.createContext(ctx);
vm.runInContext(fs.readFileSync(path.join(__dirname, '..', 'src', 'web', 'mapper.js'), 'utf8'), ctx);
const { keep, settingsOf } = ctx.window.mapperInternals;

const files = process.argv.slice(2);
if (!files.length) {
  console.error('usage: node tools/merge-map.js <downloaded map>...');
  process.exit(2);
}
let doc = fs.existsSync(OUT) ? JSON.parse(fs.readFileSync(OUT, 'utf8')) : null;
for (const f of files) {
  const run = JSON.parse(fs.readFileSync(f, 'utf8'));
  for (const m of run.maps || []) {
    doc = keep(doc, m.firmware, m.ver, m.size, settingsOf(run, m.firmware));
    console.log(path.basename(f) + ': ' + settingsOf(run, m.firmware).length + ' settings for ' + m.firmware);
  }
}
fs.writeFileSync(OUT, JSON.stringify(doc, null, 1) + '\n');
for (const m of doc.maps) console.log(m.firmware + ': ' + m.settings.length + ' settings');
