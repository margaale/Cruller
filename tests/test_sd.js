// Tests for the SD card view's logic (src/web/sd.js): GET /rt4k/ls's format, sorting, folder
// addresses. Run by tests/run.sh (node 18+).

const fs = require('fs');
const path = require('path');
const vm = require('vm');

const window = {};
vm.runInNewContext(fs.readFileSync(path.join(__dirname, '..', 'src', 'web', 'sd.js'), 'utf8'), { window, Date });
const sd = window.sdInternals;

let checks = 0, failures = 0;
function check(cond, what) {
  checks++;
  if (!cond) { failures++; console.log('  FAIL ' + what); }
}

// A listing as Cruller sends it (from the RT4K's "ent t=... sz=... mt=... nm=..." lines).
const body = 'D\t0\t1762343402\tSony PS2\n' +
  'F\t45810\t1762343402\t_Profile Instructions.html\n' +
  'D\t0\t1762343404\tprofile custom instructions & Info\n' +
  'F\t4580256\t1577836800\trt4k_1821.rbf\n' +
  'F\t12\t1577836800\tDisc 10.rt4\n' +
  'F\t12\t1577836801\tdisc 2.rt4\n' +
  'F\t1\t1577836800\t  spaced  \n' +
  'garbage line\n\n';
const list = sd.parseList(body);
check(list.length === 7, 'parseList: 7 entries, got ' + list.length);
check(list[0].dir && list[0].name === 'Sony PS2' && list[0].mtime === 1762343402, 'parseList: folder');
check(!list[3].dir && list[3].size === 4580256, 'parseList: file size');
check(list[6].name === '  spaced  ', 'parseList: spaces kept');
check(sd.parseList('').length === 0, 'parseList: empty folder');

const names = (l) => l.map((e) => e.name).join('|');
const byName = sd.sortEntries(list, 'name', false);
check(byName[0].dir && byName[1].dir, 'sort: folders first');
check(names(byName).startsWith('profile custom instructions & Info|Sony PS2|'), 'sort: folders by name, case aside: ' + names(byName));
check(names(byName).indexOf('disc 2.rt4') < names(byName).indexOf('Disc 10.rt4'), 'sort: numeric ("2" before "10")');
const bySize = sd.sortEntries(list, 'size', true);
check(bySize[2].name === 'rt4k_1821.rbf', 'sort: biggest file first after the folders');
check(bySize[0].dir && bySize[1].dir, 'sort: folders stay first when descending');
check(bySize[0].name === 'profile custom instructions & Info', 'sort: ties (the folders\' size) stay A to Z when descending');
check(names(bySize).endsWith('disc 2.rt4|Disc 10.rt4|  spaced  '), 'sort: equal sizes by name A to Z: ' + names(bySize));
check(sd.sortEntries(list, 'name', true)[0].name === 'Sony PS2', 'sort: name descending, folders first');

// Folder addresses: #rt4k/sd/<escaped parts>, and back (app.js splits the hash at '/').
for (const d of ['', 'profile', 'profile/Sony PS2', 'a%b/c#d/e?f', 'x & y/100% + more']) {
  const h = sd.hrefFor(d);
  const parts = h.slice(1).split('/').slice(2);
  check(sd.dirFromParts(parts) === d, 'address round trip: ' + JSON.stringify(d) + ' -> ' + h);
}
check(sd.hrefFor('') === '#rt4k/sd', 'address of the root');
check(sd.getUrl('profile/Nintendo SNES + SFC/a&b.rt4') === '/rt4k/get?path=profile%2FNintendo%20SNES%20%2B%20SFC%2Fa%26b.rt4',
  'download address: the whole path escaped (+ and & too)');
check(sd.dirFromParts(['bad%zz']) === 'bad%zz', 'a broken escape is kept as is');

check(sd.size(0) === '0 B' && sd.size(1536) === '1.5 KB' && sd.size(45810) === '45 KB' && sd.size(4580256) === '4.4 MB', 'sizes');
check(sd.when(1577836800) === '2020-01-01 00:00', 'dates as stored on the card');

console.log(failures ? `sd.js: ${failures} of ${checks} checks failed` : `sd.js: ${checks} checks ok`);
process.exit(failures ? 1 : 0);
