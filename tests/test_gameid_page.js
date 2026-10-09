// Tests for gameID's view (src/web/gameid.js): a console's address as typed, what Cruller would refuse,
// profiles, the games' search, a console's answer. Run by tests/run.sh (node 18+).

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
const playing = { console: 'PS2', game: 'SCUS-97481', game_name: 'God of War II' };
check(g.nowText({ playing, profile: 'PS2/GoW2.rt4', from: 'gamedb', pending: '', loaded: 'ps2/gow2.RT4' }) === 'God of War II on PS2: loaded PS2/GoW2 (from the gameDB)', 'loaded: ' + g.nowText({ playing, profile: 'PS2/GoW2.rt4', from: 'gamedb', pending: '', loaded: 'ps2/gow2.RT4' }));
check(g.nowText({ playing, profile: 'PS2/Any.rt4', from: 'other', pending: 'PS2/Any.rt4', loaded: '' }) === 'God of War II on PS2: loading PS2/Any (from its console)', 'loading');
check(g.nowText({ playing, profile: '', from: '', pending: '', loaded: '' }) === 'God of War II on PS2: no profile for it', 'a game with no profile');
check(g.nowText({ playing: null, profile: 'SVS/S4_PS2.rt4', from: 'svs', pending: '', loaded: 'SVS/S4_PS2.rt4' }) === 'Its console went off: loaded SVS/S4_PS2 (from its SVS input)', 'back to the input\'s');
check(g.nowText({ playing: null, profile: '', from: '', pending: '', loaded: '' }) === 'No game on screen' && g.nowText(null) === '', 'nothing');

console.log(failures ? `gameid.js: ${failures} of ${checks} checks failed` : `gameid.js: ${checks} checks ok`);
process.exit(failures ? 1 : 0);
