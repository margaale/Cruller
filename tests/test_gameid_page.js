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

// What's on screen, as On screen says it.
const sv = { known: true, input: 4, inputs, files: { 1: 'S1_PS1.rt4', 2: 'S2_Genesis.rt4', 4: 'S4_PS2.rt4' } };
const ps2 = { name: 'PS2', url: 'http://10.10.10.88/api/currentState', other: '', svs_input: 0, enabled: true };
const playing = { console: 'PS2', game: 'SCUS-97481', game_name: 'God of War II' };
const seen = { name: 'PS2', on: true, game: 'SCUS-97481', game_name: 'God of War II', kind: 'ps2', on_screen: true };
const mine = [{ id: 'SCUS-97481', name: 'God of War II', profile: 'PS2/GoW2.rt4' }];
const state = (o) => Object.assign({ consoles: [seen], svs_input: 4, playing, profile: '', from: '', pending: '', loaded: '', note: '', note_age_s: 0 }, o);
let r = g.onScreen(state({ profile: 'PS2/GoW2.rt4', from: 'gamedb', loaded: 'ps2/gow2.RT4', note: 'loaded PS2/GoW2.rt4', note_age_s: 120 }), sv, [ps2], mine, 'on');
check(r.title === 'God of War II' && r.sub === 'PS2 on input 4 · SCUS-97481' && r.profile === 'PS2/GoW2.rt4' && r.badge === 'Loaded 2 min ago' && r.tone === 'ok' &&
  /loads its own S4_PS2 first/.test(r.why) && !r.add && !r.dim, 'a game in your games, loaded: ' + JSON.stringify(r));
r = g.onScreen(state({ profile: 'PS2/GoW2.rt4', from: 'gamedb', pending: 'PS2/GoW2.rt4' }), sv, [ps2], mine, 'standby');
check(r.badge === 'Waiting for the RT4K' && r.tone === 'wait', 'waiting for the RT4K');
r = g.onScreen(state({ profile: 'PS2/GoW2.rt4', from: 'gamedb', pending: 'PS2/GoW2.rt4' }), sv, [ps2], mine, 'on');
check(r.badge === 'Loading…', 'loading');
r = g.onScreen(state({ profile: 'PS2/GoW2.rt4', from: 'gamedb', note: 'could not load PS2/GoW2.rt4 (prof err)' }), sv, [ps2], mine, 'on');
check(r.badge === 'Could not load' && r.tone === 'bad' && r.why === 'could not load PS2/GoW2.rt4 (prof err)', 'could not load');
r = g.onScreen(state({}), sv, [ps2], [], 'on');
check(r.profile === 'SVS/S4_PS2.rt4' && r.badge === 'The input\'s own' && r.add && r.add.id === 'SCUS-97481' && r.add.name === 'God of War II' &&
  /the input's own stays/.test(r.why), 'not in your games: the input\'s own, and added in a click: ' + JSON.stringify(r));
r = g.onScreen(state({ profile: 'PS2/Any.rt4', from: 'other' }), sv, [ps2], [], 'on');
check(r.profile === 'PS2/Any.rt4' && /its console's profile/.test(r.why) && r.add, 'not in your games: its gameID\'s for those');
r = g.onScreen(state({ playing: null, svs_input: 0 }), sv, [ps2], mine, 'on');
check(r.title === 'No input active' && r.dim && r.profile === '' && !r.addFor, 'no input active');
r = g.onScreen(state({ playing: null, svs_input: 1, consoles: [seen] }), sv, [ps2], mine, 'on');
check(r.title === 'PS1' && r.sub === 'Input 1 · nothing on it tells its game' && r.addFor === 1 && r.profile === 'SVS/S1_PS1.rt4' && r.badge === 'The input\'s own', 'an input without a gameID: ' + JSON.stringify(r));
r = g.onScreen(state({ playing: null, consoles: [Object.assign({}, seen, { game: '', game_name: '', on_screen: false })] }), sv, [ps2], mine, 'on');
check(r.title === 'PS2' && r.sub === 'Input 4 · no game it can tell' && !r.addFor, 'its console on, no game');
r = g.onScreen(state({ playing: null, consoles: [{ name: 'PS2', on: false, game: '', game_name: '', kind: 'ps2', on_screen: false }], profile: 'SVS/S4_PS2.rt4', from: 'svs', loaded: 'SVS/S4_PS2.rt4' }), sv, [ps2], mine, 'on');
check(r.sub === 'Input 4 · its gameID doesn\'t answer' && r.profile === 'SVS/S4_PS2.rt4' && r.tone === 'ok' && /went off/.test(r.why), 'its console went off: the input\'s own again');
r = g.onScreen(state({}), { known: false, input: 0, inputs: [], files: {} }, [ps2], [], 'on');
check(r.sub === 'PS2 · SCUS-97481' && r.profile === '' && /RT4K keeps/.test(r.why), 'no SVS: a game not in your games');
r = g.onScreen(state({ playing: null }), { known: false, input: 0, inputs: [], files: {} }, [ps2], [], 'on');
check(r.title === 'No game on screen' && r.sub === 'Asking its console which game it runs', 'no SVS, no game');
r = g.onScreen(null, null, [], [], '');
check(r.title === 'No game on screen' && r.sub === 'No consoles yet', 'nothing known yet');

console.log(failures ? `gameid.js: ${failures} of ${checks} checks failed` : `gameid.js: ${checks} checks ok`);
process.exit(failures ? 1 : 0);
