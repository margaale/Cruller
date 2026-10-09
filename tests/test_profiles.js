// Tests for src/web/profiles.js: profile names, and each SVS input's profile. Run by tests/run.sh (node 18+).

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

check(pf.isProfile('a.rt4') && pf.isProfile('B.RT4') && pf.isProfile('c.rt6'), 'profile files');
check(!pf.isProfile('_info.txt') && !pf.isProfile('a.rt4.bak') && !pf.isProfile('rt4'), 'other files');
check(pf.plain('Maverick.rt4') === 'Maverick' && pf.plain('a.b.RT6') === 'a.b' && pf.plain('x.txt') === 'x.txt', 'names without the extension');

// Each SVS input's profile: /profile/SVS/S<n>_<anything>.rt4, the first the RT4K finds.
check(pf.slotOf('S1_SNES.rt4') === 1 && pf.slotOf('s12_PS2 480i.RT4') === 12 && pf.slotOf('S3_x.rt6') === 3, 'an input\'s files');
check(pf.slotOf('SNES.rt4') === 0 && pf.slotOf('S1-SNES.rt4') === 0 && pf.slotOf('S1_notes.txt') === 0 && pf.slotOf('SS1_x.rt4') === 0, 'other files');
check(pf.baseName('S3_PS1 480i.rt4') === 'PS1 480i' && pf.baseName('Maverick.rt4') === 'Maverick' && pf.baseName('S10_a.b.rt6') === 'a.b', 'base names');
const taken = new Set(['snes.rt4', 'snes (2).rt4']);
check(pf.freeName(taken, 'SNES', '.rt4') === 'SNES (3).rt4' && pf.freeName(taken, 'PS1', '.rt4') === 'PS1.rt4', 'free names, without case');

// The SVS folder's own files only: the RT4K loads input n's profile from nowhere else. Unassigned
// ones get X_ in place of their S<n>_ (older ones, X_ in front of it all, read the same).
check(pf.baseName('X_S1_SNES.rt4') === 'SNES' && pf.baseName('X_Spare.rt4') === 'Spare' && pf.slotOf('X_S1_SNES.rt4') === 0 && pf.slotOf('SX_SNES.rt4') === 0,
  'unassigned: X_');
const steps = (p) => p.steps.map((s) => s.op + ' ' + s.from + ' > ' + s.to).join(' ; ');
const files = ['S2_Genesis.rt4', 'S1_SNES.rt4', 'X_S4_Spare.rt4', 'Loose.rt4', 'S10_Neo.rt4']; // the card's order, as the real one
let p = pf.plan(files, 1, 'X_S4_Spare.rt4');
check(steps(p) === 'mv S1_SNES.rt4 > X_SNES.rt4 ; mv X_S4_Spare.rt4 > S1_Spare.rt4' && p.target === 'S1_Spare.rt4',
  'an unassigned one: renamed for the input, and the input\'s old one gets X_: ' + steps(p));
p = pf.plan(files, 1, 'Loose.rt4');
check(steps(p) === 'mv S1_SNES.rt4 > X_SNES.rt4 ; mv Loose.rt4 > S1_Loose.rt4' && p.target === 'S1_Loose.rt4', 'one with no prefix: the same: ' + steps(p));
p = pf.plan(files, 1, 'S2_Genesis.rt4');
check(steps(p) === 'mv S1_SNES.rt4 > X_SNES.rt4 ; cp S2_Genesis.rt4 > S1_Genesis.rt4' && p.target === 'S1_Genesis.rt4',
  'another input\'s: copied, that input keeps it: ' + steps(p));
p = pf.plan(files, 1, 'S1_SNES.rt4');
check(p.steps.length === 0 && p.target === 'S1_SNES.rt4', 'the one it has: nothing to do');
p = pf.plan(files, 1, 's1_snes.RT4');
check(p.steps.length === 0 && p.target === 'S1_SNES.rt4', 'names compare without case (FAT)');
p = pf.plan(files, 1, '');
check(steps(p) === 'mv S1_SNES.rt4 > X_SNES.rt4' && p.target === '', 'none: it gets X_');
p = pf.plan(['X_SNES.rt4'], 1, 'X_SNES.rt4');
check(steps(p) === 'mv X_SNES.rt4 > S1_SNES.rt4', 'and picked again, it\'s back as it was: ' + steps(p));
p = pf.plan(['X_S1_SNES.rt4'], 3, 'X_S1_SNES.rt4');
check(steps(p) === 'mv X_S1_SNES.rt4 > S3_SNES.rt4', 'an older X_S1_ one: the same: ' + steps(p));
p = pf.plan(files, 3, '');
check(p.steps.length === 0 && p.target === '', 'none for an input with nothing: nothing to do');
p = pf.plan(files, 3, 'S10_Neo.rt4');
check(steps(p) === 'cp S10_Neo.rt4 > S3_Neo.rt4', 'S10_ isn\'t input 1\'s, and a copy keeps the name past the S<n>_: ' + steps(p));
p = pf.plan(['S1_A.rt4', 'S1_B.rt4', 'C.rt4'], 1, 'S1_B.rt4');
check(steps(p) === 'mv S1_A.rt4 > X_A.rt4' && p.target === 'S1_B.rt4', 'two for one input: the one picked stays, alone: ' + steps(p));
p = pf.plan(['S1_SNES.rt4', 'X_SNES.rt4'], 1, '');
check(steps(p) === 'mv S1_SNES.rt4 > X_SNES (2).rt4', 'the same name unassigned twice: "(2)": ' + steps(p));
p = pf.plan(['s1_snes.RT4', 'spare.rt6'], 1, 'spare.rt6');
check(steps(p) === 'mv s1_snes.RT4 > X_snes.RT4 ; mv spare.rt6 > S1_spare.rt6', 'names and extensions kept as on the card: ' + steps(p));

console.log(failures ? `profiles.js: ${failures} of ${checks} checks failed` : `profiles.js: ${checks} checks ok`);
process.exit(failures ? 1 : 0);
