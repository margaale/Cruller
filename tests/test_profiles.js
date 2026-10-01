// Tests for the profiles view's logic (src/web/profiles.js): "prof get"'s reply, profile names and
// folder addresses. Run by tests/run.sh (node 18+).

const fs = require('fs');
const path = require('path');
const vm = require('vm');

const window = {};
const web = path.join(__dirname, '..', 'src', 'web');
const context = vm.createContext({ window, Date, TextEncoder });
for (const f of ['sd.js', 'profiles.js']) vm.runInContext(fs.readFileSync(path.join(web, f), 'utf8'), context); // as the page loads them
const pf = window.profInternals;

let checks = 0, failures = 0;
function check(cond, what) {
  checks++;
  if (!cond) { failures++; console.log('  FAIL ' + what); }
}

// "prof get" (Cruller drops the "[COM] " prefix).
check(pf.parseLoaded('prof loaded=0') === '', 'nothing loaded');
check(pf.parseLoaded('prof loaded=1 dir=/profile file=Sony PS2/Maverick - RGBS.rt4') === 'Sony PS2/Maverick - RGBS.rt4', 'a profile in a folder, spaces kept');
check(pf.parseLoaded('prof loaded=1 dir=/profile file=SVS/S1_SNES.rt4') === 'SVS/S1_SNES.rt4', 'an SVS profile');
check(pf.parseLoaded('prof loaded=1 dir=/profile file=Top.rt4') === 'Top.rt4', 'a profile in /profile itself');
check(pf.parseLoaded('Bad Command') === null && pf.parseLoaded('prof: busy') === null, 'other replies: not known');

check(pf.isProfile('a.rt4') && pf.isProfile('B.RT4') && pf.isProfile('c.rt6'), 'profile files');
check(!pf.isProfile('_info.txt') && !pf.isProfile('a.rt4.bak') && !pf.isProfile('rt4'), 'other files');
check(pf.extFor('RT4K_Pro') === '.rt4' && pf.extFor('RT4K_CE') === '.rt4' && pf.extFor('') === '.rt4', '.rt4 for the 4Ks (and not known yet)');
check(pf.extFor('RT6X_CE') === '.rt6', '.rt6 for the 6X');
check(pf.withExt('Maverick', 'RT4K_Pro') === 'Maverick.rt4', 'the extension added');
check(pf.withExt('Maverick.RT4', 'RT4K_Pro') === 'Maverick.RT4', 'an extension kept');
check(pf.withExt('v1.2', 'RT4K_Pro') === 'v1.2.rt4', 'a dot in the name isn\'t an extension');
check(pf.plain('Maverick.rt4') === 'Maverick' && pf.plain('a.b.RT6') === 'a.b' && pf.plain('x.txt') === 'x.txt', 'names without the extension');

// Folder addresses: #rt4k/profiles/<escaped parts>, and back through sd.js (app.js splits the hash at '/').
for (const d of ['', 'SVS', 'Sony PS2', 'a%b/c#d', 'x & y/100% + more']) {
  const h = pf.hrefFor(d);
  const parts = h.slice(1).split('/').slice(2);
  check(window.sdInternals.dirFromParts(parts) === d, 'address round trip: ' + JSON.stringify(d) + ' -> ' + h);
}
check(pf.hrefFor('') === '#rt4k/profiles', 'address of /profile');
check(pf.dirOf('Sony PS2/Maverick.rt4') === 'Sony PS2' && pf.dirOf('Top.rt4') === '' && pf.dirOf('a/b/c.rt4') === 'a/b', 'a profile\'s folder');

// Each SVS input's profile: /profile/SVS/S<n>_<anything>.rt4, the first the RT4K finds.
check(pf.slotOf('S1_SNES.rt4') === 1 && pf.slotOf('s12_PS2 480i.RT4') === 12 && pf.slotOf('S3_x.rt6') === 3, 'an input\'s files');
check(pf.slotOf('SNES.rt4') === 0 && pf.slotOf('S1-SNES.rt4') === 0 && pf.slotOf('S1_notes.txt') === 0 && pf.slotOf('SS1_x.rt4') === 0, 'other files');
check(pf.baseName('S3_PS1 480i.rt4') === 'PS1 480i' && pf.baseName('Maverick.rt4') === 'Maverick' && pf.baseName('S10_a.b.rt6') === 'a.b', 'base names');
const taken = new Set(['snes.rt4', 'snes (2).rt4']);
check(pf.freeName(taken, 'SNES', '.rt4') === 'SNES (3).rt4' && pf.freeName(taken, 'PS1', '.rt4') === 'PS1.rt4', 'free names, without case');

const steps = (p) => p.steps.map((s) => s.op + ' ' + (s.path || s.from + ' > ' + s.to)).join(' ; ');
const svs = { dir: 'SVS', files: ['S1_SNES.rt4', 'S2_Genesis.rt4', 'Spare.rt4', 'S10_Neo.rt4'] };
let p = pf.plan(svs, 1, 'Sony PS2/GT4.rt4');
check(steps(p) === 'mv SVS/S1_SNES.rt4 > SVS/SNES.rt4 ; cp Sony PS2/GT4.rt4 > SVS/S1_GT4.rt4' && p.target === 'SVS/S1_GT4.rt4',
  'from another folder: the old one loses its S1_, the new one is copied in: ' + steps(p));
p = pf.plan(svs, 1, 'SVS/Spare.rt4');
check(steps(p) === 'mv SVS/S1_SNES.rt4 > SVS/SNES.rt4 ; mv SVS/Spare.rt4 > SVS/S1_Spare.rt4' && p.target === 'SVS/S1_Spare.rt4',
  'one in the folder without an input: renamed, not copied: ' + steps(p));
p = pf.plan(svs, 1, 'SVS/S2_Genesis.rt4');
check(steps(p) === 'mv SVS/S1_SNES.rt4 > SVS/SNES.rt4 ; cp SVS/S2_Genesis.rt4 > SVS/S1_Genesis.rt4' && p.target === 'SVS/S1_Genesis.rt4',
  'another input\'s: copied, that input keeps it: ' + steps(p));
p = pf.plan(svs, 1, 'SVS/S1_SNES.rt4');
check(p.steps.length === 0 && p.target === 'SVS/S1_SNES.rt4', 'the one it has: nothing to do');
p = pf.plan(svs, 1, '');
check(steps(p) === 'mv SVS/S1_SNES.rt4 > SVS/SNES.rt4' && p.target === '', 'none: it only loses its S1_');
p = pf.plan(svs, 3, '');
check(p.steps.length === 0, 'none for an input with nothing: nothing to do');
p = pf.plan(svs, 3, 'SVS/S10_Neo.rt4');
check(steps(p) === 'cp SVS/S10_Neo.rt4 > SVS/S3_Neo.rt4', 'S10_ isn\'t input 1\'s, and copies keep their name past the S<n>_: ' + steps(p));
p = pf.plan({ dir: 'SVS', files: ['S1_A.rt4', 'S1_B.rt4', 'C.rt4'] }, 1, 'SVS/S1_B.rt4');
check(steps(p) === 'mv SVS/S1_A.rt4 > SVS/A.rt4' && p.target === 'SVS/S1_B.rt4', 'two for one input: the one picked stays, alone: ' + steps(p));
p = pf.plan({ dir: 'SVS', files: ['S1_SNES.rt4', 'SNES.rt4'] }, 1, 'SVS/SNES.rt4');
check(steps(p) === 'mv SVS/S1_SNES.rt4 > SVS/SNES (2).rt4 ; mv SVS/SNES.rt4 > SVS/S1_SNES.rt4', 'a name taken: the old one gets "(2)": ' + steps(p));
p = pf.plan({ dir: '', files: [] }, 2, 'Sega/Saturn.rt6');
check(steps(p) === 'mkdir SVS ; cp Sega/Saturn.rt6 > SVS/S2_Saturn.rt6' && p.target === 'SVS/S2_Saturn.rt6', 'no SVS folder yet: made first: ' + steps(p));
p = pf.plan({ dir: 'svs', files: ['s1_snes.RT4', 'spare.rt4'] }, 1, 'svs/spare.rt4');
check(steps(p) === 'mv svs/s1_snes.RT4 > svs/snes.RT4 ; mv svs/spare.rt4 > svs/S1_spare.rt4', 'the folder and files as named on the card, without case: ' + steps(p));
p = pf.plan(svs, 2, 'SVS/old/Genesis.rt4');
check(steps(p) === 'mv SVS/S2_Genesis.rt4 > SVS/Genesis.rt4 ; cp SVS/old/Genesis.rt4 > SVS/S2_Genesis.rt4', 'a folder inside SVS is another folder: ' + steps(p));

console.log(failures ? `profiles.js: ${failures} of ${checks} checks failed` : `profiles.js: ${checks} checks ok`);
process.exit(failures ? 1 : 0);
