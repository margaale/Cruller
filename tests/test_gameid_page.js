// Tests for gameID in the Consoles view (src/web/gameid.js): a console's address as typed and shown, what
// Cruller would refuse, profiles, the games' search, a console's answer, its SVS input, what's on screen. Run
// by tests/run.sh (node 18+).

const fs = require('fs');
const path = require('path');
const vm = require('vm');

const window = {};
vm.runInNewContext(fs.readFileSync(path.join(__dirname, '..', 'src', 'web', 'gameid.js'), 'utf8'), { window, document: { getElementById: () => null } });
const g = window.gameidInternals;

let checks = 0, failures = 0;
function check(cond, what) {
  checks++;
  if (!cond) { failures++; console.log('  FAIL ' + what); }
}

// An address as typed: alone, a MemCard PRO's; http:// added; a path kept.
check(g.consoleUrl('10.10.10.88') === 'http://10.10.10.88/api/currentState', 'an IP alone: the MemCard PRO\'s path');
check(g.consoleUrl(' 10.10.10.88/ ') === 'http://10.10.10.88/api/currentState', 'trimmed, a trailing / alone');
check(g.consoleUrl('http://ps1digital.local/gameid') === 'http://ps1digital.local/gameid', 'a full one as it is');
check(g.consoleUrl('n64digital.local/gameid') === 'http://n64digital.local/gameid', 'http:// added before a path');
check(g.consoleUrl('https://10.0.1.52/api/currentState') === 'https://10.0.1.52/api/currentState', 'https kept (Cruller says why not)');
check(g.consoleUrl('') === '', 'nothing: nothing');

check(g.urlProblem('http://10.10.10.88/api/currentState') === '', 'a good one');
check(/https/.test(g.urlProblem('https://10.0.1.52/api/currentState')), 'https: not yet');
check(g.urlProblem('ftp://x') !== '' && g.urlProblem('http://') !== '' && g.urlProblem('http://a b/') !== '', 'others refused');

check(g.profileOk('PS2/God of War II.rt4') && g.profileOk('SVS/S4_PS2.RT6'), 'profiles');
check(!g.profileOk('') && !g.profileOk('/a.rt4') && !g.profileOk('a/../b.rt4') && !g.profileOk('a.txt') && !g.profileOk('a\\b.rt4'), 'not profiles');

const games = [{ id: 'SCUS-97481', name: 'God of War II', profile: 'PS2/GoW.rt4' }, { id: 'SLUS-00214', name: 'Ridge Racer', profile: 'PS1/RR.rt4' }];
check(g.filterGames(games, '').length === 2 && g.filterGames(games, '  ').length === 2, 'no search: all');
check(g.filterGames(games, 'war')[0].id === 'SCUS-97481' && g.filterGames(games, 'slus').length === 1 && g.filterGames(games, 'ps1/')[0].name === 'Ridge Racer', 'by name, ID or profile, any case');

// By console: a tag each, how many, by name; those for none when some are for one.
const tagged = [{ id: 'SCUS-97481', name: 'God of War II', profile: 'a.rt4', console: 'ps2' }, { id: 'SLUS-00214', name: 'Ridge Racer', profile: 'b.rt4', console: 'ps1' },
  { id: 'SLUS-20946', name: 'Shadow of the Colossus', profile: 'c.rt4', console: 'ps2' }, { id: 'X', name: 'Unknown', profile: 'd.rt4', console: '' }];
const names = { ps1: 'PS1', ps2: 'PS2', n64: 'N64' };
check(JSON.stringify(g.gameTags(tagged, names)) === '[{"tag":"ps1","name":"PS1","n":1},{"tag":"ps2","name":"PS2","n":2},{"tag":"-","name":"No console","n":1}]', 'tags: ' + JSON.stringify(g.gameTags(tagged, names)));
check(g.gameTags(games, names).length === 0 && g.gameTags([{ console: 'jaguar' }], names)[0].name === 'JAGUAR', 'none for none; an unknown one by its id');
check(g.filterGames(tagged, '', 'ps2').length === 2 && g.filterGames(tagged, '', '-')[0].id === 'X' && g.filterGames(tagged, 'shadow', 'ps2').length === 1 && g.filterGames(tagged, 'shadow', 'ps1').length === 0, 'by tag, and with the search');
check(g.filterGames(tagged, 'ps2').length === 2, 'the search finds a console too');

check(JSON.stringify(g.readGame('{"currentMode":"PS2","gameName":"God of War II","gameID":"SCUS-97481"}')) === '{"id":"SCUS-97481","name":"God of War II"}', 'a MemCard PRO2\'s answer');
check(JSON.stringify(g.readGame('3E5055B6-2E92DA52-N-45\n')) === '{"id":"3E5055B6-2E92DA52-N-45","name":""}', 'an N64Digital\'s, as text');
check(g.readGame('{"currentMode":"PS2"}').id === '', 'JSON without a game: none');

// What Cruller knows, as the view says it.
check(g.liveText(null, false) === 'Not asked' && g.liveText(null, true) === '…' && g.liveText({ on: false }, true) === 'Off', 'a console not asked, not known yet, off');
check(g.liveText({ on: true, game: '' }, true) === 'No game' && g.liveText({ on: true, game: 'SCUS-97481', game_name: 'God of War II' }, true) === 'God of War II' &&
  g.liveText({ on: true, game: 'SLUS-00214', game_name: '' }, true) === 'SLUS-00214', 'a console on: its game, by name when it says one');
// A console's address as shown: a MemCard PRO's by its host.
check(g.shortUrl('http://10.10.10.88/api/currentState') === '10.10.10.88' && g.shortUrl('http://ps1digital.local/gameid') === 'ps1digital.local/gameid', 'addresses shown');
check(g.consoleUrl(g.shortUrl('http://10.10.10.88/api/currentState')) === 'http://10.10.10.88/api/currentState' &&
  g.consoleUrl(g.shortUrl('http://ps1digital.local/gameid')) === 'http://ps1digital.local/gameid', 'and typed back as they were');

// The SVS input a console is on: the one set, or on Auto the one of its kind when just one is.
const inputs = [{ name: 'PS1', device: 'ps1' }, { name: 'Mega Drive', device: 'megadrive' }, { name: '', device: '' }, { name: 'PS2', device: 'ps2' }];
check(g.inputOf({ svs_input: 2 }, 'ps2', inputs) === 2 && g.inputOf({ svs_input: 9 }, 'ps2', inputs) === 0, 'set: that one (one the switch has)');
check(g.inputOf({ svs_input: 0 }, 'ps2', inputs) === 4 && g.inputOf({ svs_input: 0 }, '', inputs) === 0 && g.inputOf({ svs_input: 0 }, 'n64', inputs) === 0, 'Auto: by its kind');
check(g.inputOf({ svs_input: 0 }, 'ps1', inputs.concat([{ name: 'PS1', device: 'ps1' }])) === 0 && g.inputOf({ svs_input: 0 }, 'ps2', []) === 0, 'Auto: two of its kind, or no switch: none');

// The profile for what's on screen, on the card on screen.
const sv = { known: true, files: { 1: 'S1_PS1.rt4', 2: 'S2_Genesis.rt4', 4: 'S4_PS2.rt4' } };
const ps2 = { name: 'PS2', on: true, game: 'SCUS-97481', game_name: 'God of War II', kind: 'ps2', on_screen: true };
const ps1d = { name: 'PS1', on: true, game: 'SCUS-94163', game_name: 'Final Fantasy VII', kind: 'ps1', on_screen: false };
const state = (o) => Object.assign({ consoles: [ps2, ps1d], svs_input: 4, rt4k_input: '', on_svs: null, profile: '', from: '', pending: '', loaded: '', note: '', note_age_s: 0 }, o);
let r = g.loadedNow(state({ profile: 'PS2/GoW2.rt4', from: 'gamedb', loaded: 'ps2/gow2.RT4', note: 'loaded PS2/GoW2.rt4', note_age_s: 120 }), sv, 'on');
check(r.k === 0 && !r.n && r.profile === 'PS2/GoW2.rt4' && r.badge === 'Loaded · 2 min ago' && r.tone === 'ok' && r.why === 'From your games' &&
  r.more === 'Input 4 loads its own S4_PS2 first, this one 3 s after', 'a game in your games, loaded: ' + JSON.stringify(r));
r = g.loadedNow(state({ profile: 'SVS/S4_PS2.rt4', from: 'gamedb', loaded: 'SVS/S4_PS2.rt4', note: 'SVS/S4_PS2.rt4 loaded already', note_age_s: 3 }), sv, 'on');
check(r.badge === 'Loaded · just now' && r.why === 'From your games: the input\'s own' && !r.more, 'its profile the input\'s own: ' + JSON.stringify(r));
r = g.loadedNow(state({ profile: 'PS2/GoW2.rt4', from: 'gamedb', pending: 'PS2/GoW2.rt4' }), sv, 'standby');
check(r.badge === 'Waits for the RT4K' && r.tone === 'wait', 'waiting for the RT4K');
r = g.loadedNow(state({ profile: 'PS2/GoW2.rt4', from: 'gamedb', pending: 'PS2/GoW2.rt4' }), sv, 'on');
check(r.badge === 'Loading…' && r.tone === 'wait', 'loading');
r = g.loadedNow(state({ profile: 'PS2/GoW2.rt4', from: 'gamedb', note: 'could not load PS2/GoW2.rt4 (prof err)' }), sv, 'on');
check(r.badge === 'Could not load' && r.tone === 'bad' && r.more === 'could not load PS2/GoW2.rt4 (prof err)', 'could not load');
r = g.loadedNow(state({ profile: 'PS2/Any.rt4', from: 'other' }), sv, 'on');
check(r.k === 0 && r.badge === 'Its profile' && r.tone === '' && /gameID's, for games not in your games/.test(r.why), 'its gameID\'s for those, not loaded yet');
check(g.loadedNow(state({}), sv, 'on') === null, 'a game not in your games: nothing (the input\'s own stays)');
r = g.loadedNow(state({ consoles: [Object.assign({}, ps2, { on: false, game: '', on_screen: false }), ps1d], profile: 'SVS/S4_PS2.rt4', from: 'svs', loaded: 'SVS/S4_PS2.rt4' }), sv, 'on');
check(r.k === -1 && r.n === 4 && r.tone === 'ok' && /went off/.test(r.why), 'its console went off: on its input\'s card');
check(g.loadedNow(state({ consoles: [Object.assign({}, ps2, { on_screen: false }), ps1d], svs_input: 0 }), sv, 'on') === null && g.loadedNow(null, sv, 'on') === null, 'nothing on screen, or nothing known');

// A console not on the SVS (-1): on no input; its card the one on screen while the RT4K shows another input.
const ps1c = { name: 'PS1', url: 'http://ps1digital.local/gameid', other: '', svs_input: -1, enabled: true };
check(g.inputOf(ps1c, 'ps1', inputs) === 0, 'not on the SVS: on no input, though one has its kind');
r = g.loadedNow(state({ consoles: [Object.assign({}, ps2, { on_screen: false }), Object.assign({}, ps1d, { on_screen: true })], rt4k_input: 'HDMI', on_svs: false,
  profile: 'PS1/FF7.rt4', from: 'gamedb', loaded: 'PS1/FF7.rt4' }), sv, 'on');
check(r.k === 1 && r.why === 'From your games' && !r.more && r.tone === 'ok', 'played on another input: its card, no word of the SVS input\'s: ' + JSON.stringify(r));
r = g.loadedNow(state({ consoles: [Object.assign({}, ps2, { on_screen: false }), ps1d], rt4k_input: 'HDMI', on_svs: false }), sv, 'on');
check(r === null, 'another input, nothing on it');

console.log(failures ? `gameid.js: ${failures} of ${checks} checks failed` : `gameid.js: ${checks} checks ok`);
process.exit(failures ? 1 : 0);
