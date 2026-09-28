// The Cruller page (served as /app.js, embedded at build time; index.html holds the markup).
// Three tabs: RetroTINK (live screen, remote, console, firmware), Cruller (the device itself) and
// Debug (statistics). Everything live arrives over the WebSocket: status, log, and the Debug reports
// (pushed only while this page shows that tab). No polling.

'use strict';

const $ = (id) => document.getElementById(id);

// --- formatting -------------------------------------------------------------------------------------

function bytes(n) {
  if (n >= 1048576) return (n / 1048576).toFixed(2) + ' MB';
  if (n >= 1024) return (n / 1024).toFixed(1) + ' KB';
  return n + ' B';
}

function duration(s) {
  const d = Math.floor(s / 86400), h = Math.floor(s / 3600) % 24, m = Math.floor(s / 60) % 60;
  if (d) return d + ' d ' + h + ' h';
  if (h) return h + ' h ' + m + ' min';
  return m + ' min ' + (s % 60) + ' s';
}

const text = (id, t) => { const e = $(id); if (e) e.textContent = t; };

// A confirmation in the page's own style (instead of the browser's confirm()): resolves true on OK.
function askUser(title, body, ok, danger) {
  const d = $('ask');
  text('ask-title', title);
  text('ask-text', body || '');
  text('ask-yes', ok || 'OK');
  $('ask-yes').className = 'primary' + (danger ? ' danger' : '');
  d.returnValue = '';
  d.showModal();
  $('ask-no').focus();
  return new Promise((resolve) => d.addEventListener('close', () => resolve(d.returnValue === 'yes'), { once: true }));
}
window.askUser = askUser; // fw.js

// --- tabs -------------------------------------------------------------------------------------------

let tab = 'rt4k';

const IN_PORTAL = location.hostname === '192.168.4.1';

function route() {
  const [t, sub] = (location.hash.slice(1) || (IN_PORTAL ? 'setup' : 'rt4k')).split('/');
  tab = ['rt4k', 'svs', 'cruller', 'debug', 'setup'].includes(t) ? t : 'rt4k';
  document.body.classList.toggle('setup', tab === 'setup');
  if (tab === 'setup' && !wz.started) { wz.started = true; wzGo(1); wzScan(); wzResume(); }
  document.querySelectorAll('[data-view]').forEach((e) => { e.hidden = e.dataset.view !== tab; });
  document.querySelectorAll('nav.tabs a').forEach((a) => a.toggleAttribute('aria-current', a.dataset.tab === tab));
  const view = sub === 'firmware' ? 'firmware' : 'live';
  document.querySelectorAll('[data-subview]').forEach((e) => { e.hidden = e.dataset.subview !== view; });
  document.querySelectorAll('.sub a').forEach((a) => a.toggleAttribute('aria-current', a.dataset.sub === view));
  $('chip-rt4k').hidden = tab !== 'rt4k';
  $('chip-wifi').hidden = $('chip-ver').hidden = tab === 'rt4k';
  if (tab === 'rt4k' && view === 'firmware' && window.fwOpen) window.fwOpen();
  if (tab === 'rt4k' && view === 'live') fit();
  tellVisibility();
  tellDebug();
  if (tab === 'debug') { drawCharts(); if (!freeze.done) freeze(); }
}
addEventListener('hashchange', route);

// --- status (WebSocket type 5, every 5 s) -------------------------------------------------------------

let S = {}, statusAt = 0;

function st(s) {
  S = s;
  statusAt = Date.now();
  showPower(s.rt4k_power);
  if (window.fwPutProgress) window.fwPutProgress(s.put); // the firmware updater's progress bar (fw.js)
  const usb = s.rt4k_usb === 'connected';
  const power = { on: 'On', standby: 'Standby', starting: 'Starting', unknown: 'Not answering' }[s.rt4k_power] || s.rt4k_power;
  text('rt4k-chip', usb ? 'RT4K · ' + power + (fwVersion ? ' · firmware ' + fwVersion : '') : 'RT4K not connected');
  $('rt4k-dot').className = 'dot ' + (!usb ? 'bad' : s.rt4k_power === 'on' ? 'ok' : 'warn');
  text('wifi-chip', s.ssid + (s.rssi ? ' · ' + s.rssi + ' dBm' : ''));
  text('chip-ver', 'v' + s.version);

  // Cruller tab
  $('c-dot').className = 'dot ' + (usb ? 'ok' : 'bad');
  text('c-link', usb ? 'USB connected' : 'Not connected');
  text('c-usb', usb ? 'FT232R (' + s.rt4k_id + ') · ' + (s.rt4k_baud / 1e6) + ' Mbaud · RTS/CTS ' + (s.rt4k_flow ? 'on' : 'off') : '');
  text('c-drop', s.rt4k_dropped);
  $('c-drop').style.color = s.rt4k_dropped ? 'var(--warn)' : 'var(--ok)';
  text('c-tx', bytes(s.rt4k_tx));
  text('c-rx', bytes(s.rt4k_rx));
  text('w-ssid', s.ssid || '–');
  text('w-rssi', s.rssi ? s.rssi + ' dBm' : '');
  text('w-ip', s.ip || '–');
  text('w-state', s.net);
  const web = s.web_clients || 0, rfc = s.rfc2217_count || 0, max = s.clients_max || 8;
  text('cl-n', web + rfc);
  text('cl-max', 'of ' + max + ', shared');
  text('cl-web', web);
  text('cl-rfc', rfc);
  $('cl-bw').style.flexGrow = web;
  $('cl-br').style.flexGrow = rfc;
  $('cl-bf').style.flexGrow = Math.max(0, max - web - rfc);
  text('cl-ips', s.rfc2217_clients ? 'RFC 2217 from ' + s.rfc2217_clients : '');
  const heap = 192 * 1024;
  text('m-free', Math.round(s.heap_free / 1024) + ' of 192 KB');
  $('m-bar').style.width = Math.round(100 * (1 - s.heap_free / heap)) + '%';
  text('f-ver', s.version);
  text('f-part', 'partition ' + (s.boot_partition ? 'B' : 'A'));
  text('f-boot', s.boot_type);
  if (document.activeElement !== $('f-name')) $('f-name').value = s.name || '';
  text('f-host', 'Reached at ' + (s.hostname || 'cruller') + '.local' + (s.name ? '' : ' · give it a name to tell it apart'));
  showSvs(s.svs);
  if (s.setup) wzProgress(s.setup);
  showUptime();
}

// --- SVS Bridge card ------------------------------------------------------------------------------------

const ago = (s) => (s < 5 ? 'just now' : duration(s) + ' ago');

function showSvs(v) {
  const paired = v && v.paired, known = v && v.known, live = known && v.heard_s < 150;
  // Until a bridge has reported or is paired there's nothing to show but that it's awaited.
  document.querySelectorAll('.svs-more').forEach((e) => { e.hidden = !paired && !known; });
  document.querySelectorAll('.svs-wait').forEach((e) => { e.hidden = !!(paired || known); });
  $('v-dot').className = 'dot ' + (!paired && !known ? '' : live ? 'ok' : 'warn');
  text('v-state', !paired && !known ? 'no bridge yet' : live ? 'live' : 'not heard lately');
  text('v-input', known ? v.input : '–');
  text('v-name', known ? v.name || 'input ' + v.input : 'waiting for the SVS Bridge');
  text('v-since', known ? (v.since_s < 5 ? 'switched just now' : 'on screen for ' + duration(v.since_s)) : '');
  // One tile per input, the active one lit: only once the bridge has said how many the switch has
  // (SVS models differ in inputs and outputs).
  const total = known && v.total ? v.total : 0;
  text('v-total', total ? total + ' inputs' : '');
  $('v-grid').hidden = !total;
  $('v-nogrid').hidden = !!total;
  $('v-grid').innerHTML = Array.from({ length: total }, (_, i) =>
    '<div class="' + (v.input === i + 1 ? 'on' : '') + '">' + (i + 1) + '<small>' + (v.input === i + 1 ? 'ON SCREEN' : 'S' + (i + 1)) + '</small></div>').join('');
  text('v-paired', paired || '–');
  text('v-heard', known ? ago(v.heard_s) : '–');
  $('v-hint').hidden = !!paired;
  $('v-unpair').hidden = !paired;
  const hist = known && v.history ? v.history : [];
  $('v-hist').innerHTML = hist.map(([input, s], i) => '<tr><td>Input ' + input + (i === 0 ? ' <span class="small">(now)</span>' : '') +
    '</td><td class="r">' + ago(s) + '</td></tr>').join('') || '<tr><td colspan="2" class="small">None yet</td></tr>';
}

async function unpair() {
  if (!(await askUser('Unpair the SVS Bridge?', 'Cruller forgets it; the next SVS Bridge that reports pairs instead.', 'Unpair', true))) return;
  try { await fetch('/api/svs/unpair', { method: 'POST' }); } catch (e) { /* the next status shows it */ }
}

// --- name --------------------------------------------------------------------------------------------------

// The host name Cruller derives from a name (settings.c settings_hostname): "Game room" -> cruller-game-room.
function hostFor(name) {
  const parts = (name || '').toLowerCase().split(/[^a-z0-9]+/).filter(Boolean);
  return ['cruller', ...parts].join('-');
}

async function rename() {
  const name = $('f-name').value.trim();
  if (!/^[A-Za-z0-9 _-]{1,32}$/.test(name) || !/[A-Za-z0-9]/.test(name)) { text('um', 'The name takes letters, numbers and spaces (up to 32).'); return false; }
  if (!(await askUser('Rename to ' + name + '?', 'Cruller restarts as ' + hostFor(name) + '.local. Bookmarks to the old address stop working.', 'Rename and restart'))) return false;
  const host = hostFor(name) + '.local';
  busy({ mode: 'spin', title: 'Renaming…', text: 'Saving "' + name + '".' });
  try {
    const r = await fetch('/settings', { method: 'POST', body: new URLSearchParams({ name }) });
    if (!r.ok) throw new Error((await r.text()).trim());
  } catch (e) {
    busy({ mode: 'bad', title: 'It didn\'t work', text: e.message, actions: [{ label: 'Close', onclick: busyHide }] });
    return false;
  }
  busy({ mode: 'spin', title: 'Restarting as ' + host, text: 'This page moves there as soon as it answers.' });
  await sleep(3000);
  // The new address answers (an opaque no-cors reply is enough to know it's up).
  const up = await waitFor(async () => { await fetch('http://' + host + '/status', { mode: 'no-cors', cache: 'no-store' }); return true; }, 60000);
  if (up) {
    busy({ mode: 'ok', title: 'Cruller ' + name + ' is back', text: 'Moving to ' + host + '…' });
    await sleep(1200);
    location.href = 'http://' + host + '/#cruller';
  } else {
    busy({ mode: 'bad', title: host + ' isn\'t answering yet', text: 'Some computers take a while to find new .local names. Try it again in a moment, or use the IP.', actions: [{ label: 'Open ' + host, primary: true, href: 'http://' + host + '/#cruller' }, { label: 'Close', onclick: busyHide }] });
  }
  return false;
}

// --- setup wizard (the portal) -------------------------------------------------------------------------

const wz = { started: false, ssid: '', secure: true, step: 1 };

function wzGo(n) {
  if (n === 2) {
    const other = !$('wz-ssid').hidden;
    if (other) { wz.ssid = $('wz-ssid').value.trim(); wz.secure = true; }
    if (!wz.ssid) { text('wz-err1', 'Pick a network first.'); return; }
    if (wz.secure && !$('wz-pass').value) { text('wz-err1', 'Enter the password for ' + wz.ssid + '.'); return; }
    text('wz-err1', '');
    wzName();
  }
  wz.step = n;
  document.querySelectorAll('[data-wz]').forEach((e) => { e.hidden = +e.dataset.wz !== n; });
  text('wz-step', n === 4 ? 'Done' : 'Step ' + Math.min(n, 3) + ' of 3');
  [1, 2, 3].forEach((i) => $('wz-b' + i).classList.toggle('on', i <= Math.min(n, 3)));
}

async function wzScan() {
  const list = $('wz-nets');
  list.innerHTML = '<div class="small">Looking for networks…</div>';
  try {
    const nets = await (await fetch('/wifi/scan')).json();
    list.textContent = nets.length ? '' : 'No networks found';
    for (const n of nets) {
      const b = document.createElement('button');
      b.className = 'wiz-net';
      b.innerHTML = '<span></span><span class="small"></span>';
      b.firstChild.textContent = n.ssid;
      b.lastChild.textContent = n.rssi + ' dBm' + (n.secure ? ' · 🔒' : ' · open');
      b.onclick = () => {
        list.querySelectorAll('.wiz-net').forEach((x) => x.setAttribute('aria-pressed', x === b));
        $('wz-ssid').hidden = true;
        wz.ssid = n.ssid;
        wz.secure = n.secure;
        text('wz-passlabel', n.secure ? 'Password for ' + n.ssid : n.ssid + ' is open: no password');
        $('wz-pass').disabled = !n.secure;
        if (n.secure) $('wz-pass').focus();
      };
      list.appendChild(b);
    }
  } catch (e) {
    list.textContent = 'Could not look for networks';
  }
}

function wzOther() {
  $('wz-ssid').hidden = false;
  $('wz-pass').disabled = false;
  text('wz-passlabel', 'Password');
  $('wz-nets').querySelectorAll('.wiz-net').forEach((x) => x.setAttribute('aria-pressed', false));
  $('wz-ssid').focus();
}

function wzEye() { const p = $('wz-pass'); p.type = p.type === 'password' ? 'text' : 'password'; }

function wzPick(b) { $('wz-name').value = b.textContent; wzName(); }

function wzName() {
  const name = $('wz-name').value.trim();
  text('wz-host', hostFor(name) + '.local');
  text('wz-inst', 'Cruller' + (name ? ' ' + name : '') + ' in Home Assistant');
}

async function wzConnect() {
  const name = $('wz-name').value.trim();
  if (name && (!/^[A-Za-z0-9 _-]{1,32}$/.test(name) || !/[A-Za-z0-9]/.test(name))) { text('wz-err2', 'Letters, numbers and spaces only (up to 32).'); return; }
  text('wz-err2', '');
  wzGo(3);
  wzProgress({ state: 'joining', ssid: wz.ssid });
  try {
    const r = await fetch('/setup', { method: 'POST', body: new URLSearchParams({ ssid: wz.ssid, pass: wz.secure ? $('wz-pass').value : '', name }) });
    if (!r.ok) { wzProgress({ state: 'failed', ssid: wz.ssid, error: (await r.json()).error }); return; }
  } catch (e) {
    // The radio may hop to the network's channel while joining and drop the phone for a moment.
  }
  wzWatch(name);
}

// While joining: ask /setup every second too. The status push over the WebSocket may not arrive (a
// phone's captive-portal window may not keep one, or the phone drops off while the radio changes
// channel). Only in the portal, only until it's decided.
// The ring counts down the join's time (Cruller gives up after 20 s): the page never just spins.
const WZ_JOIN_S = 22; // Cruller's 20 s, and a little for the answer to come back

function wzRing(left, state) {
  const ring = $('wz-ring'), arc = $('wz-arc');
  ring.className = 'ring wz-ring' + (state === 'ok' ? ' ok' : state === 'bad' ? ' bad' : '');
  text('wz-num', state === 'ok' ? '✓' : state === 'bad' ? '!' : state === 'lost' ? '?' : left);
  arc.style.strokeDashoffset = state ? 0 : ARC * (1 - left / WZ_JOIN_S);
}

async function wzWatch(name) {
  const t0 = Date.now();
  $('wz-lost').hidden = true;
  while (wz.step === 3) {
    const left = Math.max(0, WZ_JOIN_S - Math.round((Date.now() - t0) / 1000));
    wzRing(left, '');
    await sleep(1000);
    try {
      const p = await (await fetch('/setup', { cache: 'no-store' })).json();
      if (p.state !== 'idle') wzProgress(p);
      if (p.state !== 'joining') { wzRing(0, p.state === 'ok' ? 'ok' : 'bad'); return; }
    } catch (e) { /* the phone is off the setup network for a moment */ }
    if (!left) {
      // Time's up with no answer: most likely it joined and the phone lost the setup network.
      const host = hostFor(name) + '.local';
      wzRing(0, 'lost');
      text('wz-joining', 'No answer from Cruller');
      $('wz-lost').hidden = false;
      text('wz-lost', 'It has probably joined ' + wz.ssid + ' already, and your phone dropped off the setup network. Switch back to your Wi-Fi and open ' + host + '. If Cruller_Setup is still around, try again.');
      $('wz-retry').hidden = false;
      return;
    }
  }
}

// The page reloaded (the phone reconnected to the setup network): pick up where the setup is.
async function wzResume() {
  try {
    const p = await (await fetch('/setup', { cache: 'no-store' })).json();
    if (p.state === 'idle') return;
    wz.ssid = p.ssid;
    wzGo(3);
    wzProgress(p);
    if (p.state === 'joining') wzWatch('');
    else wzRing(0, p.state === 'ok' ? 'ok' : 'bad');
  } catch (e) { /* stays on step 1 */ }
}

// Steps 3 and 4 from the setup's progress (pushed in the status while the portal is up).
function wzProgress(p) {
  if (wz.step < 3) return;
  const name = $('wz-name').value.trim();
  const states = {
    joining: ['done', p.rssi ? 'done' : 'run', 'run', 'wait'],
    ok: ['done', 'done', 'done', 'done'],
    wrong_password: ['done', 'done', 'fail', 'wait'],
    not_found: ['done', 'fail', 'wait', 'wait'],
    failed: ['done', 'done', 'fail', 'wait'],
  }[p.state] || ['done', 'run', 'wait', 'wait'];
  const labels = ['Settings saved', 'Network found', 'Checking the password', 'Getting an address'];
  const details = ['', p.rssi ? p.rssi + ' dBm' : '', '', p.ip || ''];
  $('wz-steps').innerHTML = labels.map((l, i) => '<li data-state="' + states[i] + '"><span class="si"></span><div class="sb"><div class="sr"><span>' + l +
    '</span><span class="sd small">' + details[i] + '</span></div></div></li>').join('');
  text('wz-joining', (p.state === 'joining' ? 'Joining ' : 'Trying ') + (p.ssid || wz.ssid) + '…');
  const fail = { wrong_password: ['Wrong password', 'Check it and try again: nothing is lost.'], not_found: ['Network not found', 'Is it in range, and on 2.4 GHz? Cruller can\'t use 5 GHz networks.'],
    failed: ['Could not join', p.error || 'Try again, or pick another network.'] }[p.state];
  $('wz-fail').hidden = !fail;
  $('wz-retry').hidden = !fail;
  if (fail) { text('wz-failt', fail[0]); text('wz-faild', fail[1]); }
  if (p.state === 'ok') {
    wzGo(4);
    const host = (p.hostname || hostFor(name)) + '.local';
    text('wz-done', 'Cruller' + (name ? ' ' + name : '') + ' is on your network');
    text('wz-addr', host);
    text('wz-ip', p.ip);
    text('wz-net', p.ssid);
    $('wz-open').href = 'http://' + host + '/';
    text('wz-open', 'Open ' + host);
    const end = Date.now() + (p.restart_in_s || 20) * 1000;
    clearInterval(wzProgress.t);
    wzProgress.t = setInterval(() => {
      const left = Math.max(0, Math.round((end - Date.now()) / 1000));
      text('wz-left', left ? left + ' s' : 'a moment');
      if (!left) clearInterval(wzProgress.t);
    }, 1000);
  }
}

function showUptime() {
  if (S.uptime_s !== undefined) text('f-up', duration(S.uptime_s + Math.floor((Date.now() - statusAt) / 1000)));
}
setInterval(showUptime, 1000);

// The RT4K's own version, for the header chip: asked once through the console (see fw.js's ask()).
let fwVersion = '';
async function askVersion() {
  if (askVersion.busy) return;
  askVersion.busy = true;
  setTimeout(() => { askVersion.busy = false; }, 30000); // at most every 30 s while it doesn't answer
  try {
    const r = await fetch('/rt4k/ask?expect=FW%20Version', { method: 'POST', body: 'ver' });
    const m = (await r.text()).match(/FW Version:\s*(\S+)/);
    if (m) { fwVersion = m[1]; st(S); }
  } catch (e) { /* off or busy: the chip just goes without it */ }
}

// --- power: veil over the screen, and what the power key does -----------------------------------------

function showPower(power) {
  const link = $('link');
  if (link.textContent.startsWith('connected')) {
    const what = { standby: 'RT4K in standby', starting: 'RT4K starting', unknown: 'RT4K not answering' }[power];
    link.textContent = 'connected' + (what ? ', ' + what : '');
  }
  const veil = $('veil');
  veil.textContent = power === 'standby' ? 'RT4K in standby' : power === 'starting' ? 'RT4K starting…' : '';
  veil.style.display = veil.textContent ? 'flex' : 'none';
  const pwr = document.querySelector('.remote .pwr');
  if (power === 'standby') {
    pwr.dataset.c = 'pwr on';
    delete pwr.dataset.confirm;
    pwr.title = 'Turn the RT4K on';
  } else {
    pwr.dataset.c = 'remote pwr';
    pwr.dataset.confirm = 'Turn the RT4K off?';
    pwr.title = 'Turn the RT4K off';
  }
  if (power === 'on' && !fwVersion) askVersion();
}

// --- log and console text -----------------------------------------------------------------------------

function append(id, t) {
  const e = $(id);
  const stick = e.scrollTop + e.clientHeight >= e.scrollHeight - 4;
  e.textContent += t;
  if (e.textContent.length > 30000) e.textContent = e.textContent.slice(-20000);
  if (stick || e.offsetParent === null) e.scrollTop = e.scrollHeight;
}

function lg(t) { append('lg', t); }
function out(t) { append('rx', t); }

// --- the WebSocket: terminal text, OSD planes, font, log, status (see ws.h) ------------------------------

let ws, font = null;
const planes = [null, null], BG = [[5, 7, 12], [233, 237, 243], [32, 192, 32], [208, 32, 32]];

function send(t) {
  if (ws && ws.readyState === 1) { ws.send(t); return true; }
  return false;
}

// Key-to-screen as this page sees it: from a key sent to the next menu change received.
const ST = { lat: 0, key: 0 };

// Key-to-screen times seen by this page (for the Cruller tab and the Debug chart).
const keyTimes = [];
function keySeen(ms) {
  keyTimes.push(ms);
  if (keyTimes.length > 50) keyTimes.shift();
  const avg = Math.round(keyTimes.reduce((a, b) => a + b, 0) / keyTimes.length);
  text('c-lat', avg + ' ms');
  text('c-lat2', 'avg of last ' + keyTimes.length + ' · max ' + Math.max(...keyTimes));
  text('lg-key', 'key → screen ' + avg + ' ms');
  if (tab === 'debug') drawCharts();
}

function blink() {
  ST.key = performance.now();
  const l = $('led');
  l.classList.add('on');
  clearTimeout(blink.t);
  blink.t = setTimeout(() => l.classList.remove('on'), 150);
}

function conn() {
  ws = new WebSocket('ws://' + location.host + '/ws');
  ws.binaryType = 'arraybuffer';
  ws.onopen = () => { $('link').textContent = 'connected'; tellVisibility(); tellDebug(); };
  ws.onclose = () => { $('link').textContent = 'reconnecting'; setTimeout(conn, 2000); };
  ws.onmessage = (e) => {
    const u = new Uint8Array(e.data);
    if (u[0] === 1) out(new TextDecoder('latin1').decode(u.subarray(1)));
    else if (u[0] === 3) { font = u.slice(1); draw(); }
    else if (u[0] === 4) lg(new TextDecoder('latin1').decode(u.subarray(1)));
    else if (u[0] === 5) st(JSON.parse(new TextDecoder().decode(u.subarray(1))));
    else if (u[0] === 6) onDebug(u[1], new TextDecoder().decode(u.subarray(2)));
    else if (u[0] === 2) {
      const n = u[2], d = u.subarray(3 + n);
      if (u[1] === 1 && ST.key) { ST.lat = Math.round(performance.now() - ST.key); ST.key = 0; keySeen(ST.lat); }
      planes[u[1] - 1] = d.length ? { r: new TextDecoder().decode(u.subarray(3, 3 + n)), d: d.slice() } : null;
      draw();
    }
  };
}

// Tell Cruller whether this page shows the live screen: it polls the RT4K's OSD (~25 KB/s) only while
// some page does, so a background tab, or one on Cruller, Debug or Firmware, counts as hidden.
let told = null; // [socket, visible] last sent
function tellVisibility() {
  if (!ws || ws.readyState !== 1) return;
  const visible = document.visibilityState === 'visible' && tab === 'rt4k' && !$('tv').closest('[data-subview]').hidden;
  if (told && told[0] === ws && told[1] === visible) return;
  ws.send(new Uint8Array([0x10, visible ? 1 : 0]));
  told = [ws, visible];
}
document.addEventListener('visibilitychange', () => { tellVisibility(); tellDebug(); });
setInterval(tellVisibility, 1000); // a reconnected socket starts out counted as visible

// --- the TV screen -------------------------------------------------------------------------------------

function kv(r) {
  const o = {};
  r.split(' ').forEach((t) => { const i = t.indexOf('='); if (i > 0) o[t.slice(0, i)] = +t.slice(i + 1); });
  return o;
}

// 16:9, black. The main plane's rows fill its height, anchored left; the secondary plane (messages)
// goes top right at the same scale. Background mode 0 is transparent. Geometry as the RT4K draws it at
// 4K (measured on the TV): the main plane's 512-pixel grid is 2048 of the 2160 lines (x4), 3% from the
// left, low on the screen; the secondary plane is at twice that scale near the top-right corner. The
// canvas has as many pixels as it shows (device pixels).
function fit() {
  const t = $('tv'), r = t.getBoundingClientRect(), d = devicePixelRatio || 1;
  if (!r.width) return;
  t.width = Math.max(1, Math.round(r.width * d));
  t.height = Math.max(1, Math.round(r.height * d));
  draw();
}

function render(p) {
  const k = kv(p.r), rows = k.rows || 0, w = k.width || k.cols || 0, s = k.stride || w, d = p.d;
  const c = document.createElement('canvas');
  c.width = w * 8;
  c.height = rows * 16;
  if (!rows || !w) return c;
  const g = c.getContext('2d'), im = g.createImageData(c.width, c.height), px = im.data;
  for (let y = 0; y < rows; y++) {
    for (let x = 0; x < w; x++) {
      const j = y * s + x, ch = d[j], co = d[2048 + j], m = co >> 6 & 3;
      const fg = [(co >> 4 & 3) * 85, (co >> 2 & 3) * 85, (co & 3) * 85], bg = BG[m];
      for (let gy = 0; gy < 16; gy++) {
        const bits = font[gy * 256 + ch];
        for (let gx = 0; gx < 8; gx++) {
          const on = bits >> gx & 1, q = ((y * 16 + gy) * c.width + x * 8 + gx) * 4, v = on ? fg : bg;
          px[q] = v[0]; px[q + 1] = v[1]; px[q + 2] = v[2]; px[q + 3] = on || m ? 255 : 0;
        }
      }
    }
  }
  g.putImageData(im, 0, 0);
  return c;
}

// Scales an area of an OSD bitmap by s, same factor both ways: whole numbers straight (sharp pixels),
// fractions via the next whole number and a smooth shrink, so the font's strokes stay even.
function blit(g, c, sx, sy, sw, sh, dx, dy, s) {
  dx = Math.round(dx);
  dy = Math.round(dy);
  g.imageSmoothingEnabled = false;
  if (Math.abs(s - Math.round(s)) < 0.01) {
    s = Math.round(s);
    g.drawImage(c, sx, sy, sw, sh, dx, dy, sw * s, sh * s);
    return;
  }
  const k = Math.ceil(s), u = document.createElement('canvas');
  u.width = sw * k;
  u.height = sh * k;
  const ug = u.getContext('2d');
  ug.imageSmoothingEnabled = false;
  ug.drawImage(c, sx, sy, sw, sh, 0, 0, sw * k, sh * k);
  g.imageSmoothingEnabled = true;
  g.imageSmoothingQuality = 'high';
  g.drawImage(u, dx, dy, Math.round(sw * s), Math.round(sh * s));
}

// With no menu open, the RT4K leaves a copy of the message plane (e.g. "HDMI? / No Signal") in the
// main plane, which the TV doesn't show: same characters and colours in every cell.
function sameAsMessages(p0, p1) {
  const a = kv(p0.r), b = kv(p1.r), sa = a.stride || a.width || 0, sb = b.stride || b.cols || 0;
  for (let y = 0; y < (a.rows || 0); y++) {
    for (let x = 0; x < (a.width || a.cols || 0); x++) {
      const i = y * sa + x, inB = y < (b.rows || 0) && x < (b.cols || b.width || 0), j = y * sb + x;
      const ca = p0.d[i] > 32 || p0.d[2048 + i] & 192 ? p0.d[i] | p0.d[2048 + i] << 8 : 0;
      const cb = inB && (p1.d[j] > 32 || p1.d[2048 + j] & 192) ? p1.d[j] | p1.d[2048 + j] << 8 : 0;
      if (ca !== cb) return false;
    }
  }
  return true;
}

function draw() {
  const t = $('tv'), g = t.getContext('2d'), W = t.width, H = t.height;
  g.fillStyle = '#000';
  g.fillRect(0, 0, W, H);
  if (!font) return;
  const mk = planes[0] ? kv(planes[0].r) : {}, ph = (mk.rows || 32) * 16, sc = H * (2048 / 2160) / ph;
  if (planes[0] && !(planes[1] && sameAsMessages(planes[0], planes[1]))) {
    const c = render(planes[0]);
    blit(g, c, 0, 0, c.width, c.height, W * 0.031, H * 0.974 - c.height * sc, sc);
  }
  // Secondary plane: only its content (it's left-aligned inside a 32-column box).
  if (planes[1]) {
    const p = planes[1], k = kv(p.r), rows = k.rows || 0, w = k.width || k.cols || 0, stp = k.stride || w;
    let x1 = -1, y1 = -1;
    for (let y = 0; y < rows; y++) {
      for (let x = 0; x < w; x++) {
        const j = y * stp + x;
        if (p.d[j] > 32 || p.d[2048 + j] & 192) { if (x > x1) x1 = x; if (y > y1) y1 = y; }
      }
    }
    if (x1 >= 0) {
      const c = render(p), sw = (x1 + 1) * 8, sh = (y1 + 1) * 16, s2 = sc * 2;
      blit(g, c, 0, 0, sw, sh, W * 0.955 - sw * s2, H * 0.012, s2);
    }
  }
}

// --- remote, keyboard, console input -------------------------------------------------------------------

document.querySelectorAll('[data-c]').forEach((b) => {
  b.onclick = async () => {
    if (b.dataset.confirm && !(await askUser(b.dataset.confirm, 'The RT4K goes to standby; the power key turns it back on.', 'Turn off', true))) return;
    if (send(b.dataset.c)) blink();
  };
});

const keys = { ArrowUp: 'up', ArrowDown: 'down', ArrowLeft: 'left', ArrowRight: 'right', Enter: 'ok', Escape: 'back', Backspace: 'back', Tab: 'menu' };
document.onkeydown = (e) => {
  if (tab !== 'rt4k' || e.target.tagName === 'INPUT' || !keys[e.key]) return;
  e.preventDefault();
  if (send('remote ' + keys[e.key])) blink();
};

function cmd(id) {
  const i = $(id);
  if (i.value) { send(i.value); out('> ' + i.value + '\n'); i.value = ''; }
  return false;
}

// The remote keeps its look and is scaled to the screen panel's height (the grid row's height; the
// remote itself is out of flow). Narrow layouts stack it instead (CSS), unscaled.
function fitRemote() {
  const r = $('remote'), wrap = r.parentNode;
  if (getComputedStyle(r).position !== 'absolute') { r.style.transform = ''; return; }
  const h = wrap.getBoundingClientRect().height, natural = r.offsetHeight;
  if (!h || !natural) return;
  const s = h / natural;
  r.style.transform = 'scale(' + s + ')';
  const w = Math.round(r.offsetWidth * s) + 'px';
  if (wrap.style.width !== w) wrap.style.width = w; // the screen narrows a little, the row follows
}
new ResizeObserver(fitRemote).observe($('tv').closest('.panel'));

$('tv').ondblclick = () => $('tv').requestFullscreen();
addEventListener('resize', () => { fit(); if (tab === 'debug') drawCharts(); });
document.addEventListener('fullscreenchange', () => setTimeout(fit, 50));

// --- restart and factory reset -------------------------------------------------------------------------

// A full-page overlay while Cruller restarts: a ring that spins (waiting), counts down (seconds), or
// shows how it ended (ok / bad), with a title, a line of text and optional buttons.
const ARC = 276.5; // the ring's circumference (r = 44)

function busy(o) {
  const ring = $('busy-ring'), arc = $('busy-arc');
  $('busy').hidden = false;
  text('busy-title', o.title || '');
  text('busy-text', o.text || '');
  ring.className = 'ring' + (o.mode === 'spin' ? ' spin' : o.mode === 'ok' ? ' ok' : o.mode === 'bad' ? ' bad' : '');
  text('busy-num', '');
  text('busy-icon', o.mode === 'ok' ? '✓' : o.mode === 'bad' ? '!' : o.icon || '');
  clearInterval(busy.t);
  arc.style.transition = 'none';
  arc.style.strokeDashoffset = o.mode === 'count' ? 0 : '';
  const acts = $('busy-actions');
  acts.textContent = '';
  for (const a of o.actions || []) {
    const b = a.href ? document.createElement('a') : document.createElement('button');
    b.textContent = a.label;
    b.className = (a.href ? 'btn ' : '') + (a.primary ? 'primary' : '');
    if (a.href) b.href = a.href;
    if (a.onclick) b.onclick = a.onclick;
    acts.appendChild(b);
  }
  if (o.mode !== 'count') return Promise.resolve();
  // Countdown: the number goes down each second while the arc empties.
  return new Promise((resolve) => {
    let left = o.seconds;
    text('busy-num', left);
    requestAnimationFrame(() => { arc.style.transition = ''; arc.style.strokeDashoffset = ARC / o.seconds; });
    busy.t = setInterval(() => {
      left--;
      text('busy-num', Math.max(0, left));
      arc.style.strokeDashoffset = ARC * Math.min(1, (o.seconds - left + 1) / o.seconds);
      if (left <= 0) { clearInterval(busy.t); resolve(); }
    }, 1000);
  });
}

function busyHide() { clearInterval(busy.t); $('busy').hidden = true; }

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// Waits until check() says yes (tried every 1.5 s), up to timeout_ms. Only while Cruller restarts.
async function waitFor(check, timeoutMs) {
  for (const t0 = Date.now(); Date.now() - t0 < timeoutMs;) {
    try { if (await check()) return true; } catch (e) { /* not back yet */ }
    await sleep(1500);
  }
  return false;
}

// Cruller answers again, freshly started (not the old one still shutting down).
const backUp = async () => {
  const r = await fetch('/status', { cache: 'no-store' });
  return r.ok && (await r.json()).uptime_s < 120;
};

async function restart(forget) {
  const ok = forget
    ? await askUser('Factory reset?', 'Cruller forgets its Wi-Fi network, its name and its SVS Bridge, and restarts into the setup portal: join the "Cruller_Setup" network to set it up again.', 'Erase and restart', true)
    : await askUser('Restart Cruller?', 'The page reconnects by itself in a few seconds.', 'Restart');
  if (!ok) return;
  busy({ mode: 'spin', title: forget ? 'Erasing settings…' : 'Restarting Cruller…', text: forget ? 'Wi-Fi, name and SVS Bridge.' : 'Asking it to restart.' });
  try {
    const r = await fetch(forget ? '/factory-reset' : '/restart', { method: 'POST' });
    if (!r.ok) throw new Error((await r.text()).trim());
  } catch (e) {
    busy({ mode: 'bad', title: 'It didn\'t work', text: e.message, actions: [{ label: 'Close', onclick: busyHide }] });
    return;
  }
  if (forget) {
    // Cruller leaves this network: nothing to wait for here, just what to do next.
    await busy({ mode: 'count', seconds: 12, title: 'Restarting into setup…', text: 'Cruller forgot this network. Its setup network appears in a few seconds.' });
    busy({ mode: 'ok', title: 'Cruller is ready to set up', text: 'On your phone or computer, join the Wi-Fi network\n"Cruller_Setup".\n\nThe setup opens by itself (or browse to 192.168.4.1).' });
    return;
  }
  busy({ mode: 'spin', title: 'Restarting Cruller…', text: 'Back in about 10 seconds.' });
  await sleep(3000);
  if (await waitFor(backUp, 60000)) {
    busy({ mode: 'ok', title: 'Cruller is back', text: '' });
    await sleep(1500);
    busyHide();
  } else {
    busy({ mode: 'bad', title: 'Taking longer than expected', text: 'Cruller hasn\'t answered for a minute. Check that it has power, then reload.', actions: [{ label: 'Reload', primary: true, onclick: () => location.reload() }] });
  }
}

// --- Wi-Fi: nearby networks (GET /wifi/scan); tapping one fills in the name --------------------------------

async function scan() {
  const find = $('find'), list = $('nets');
  find.disabled = true;
  find.textContent = 'Looking for networks…';
  try {
    const nets = await (await fetch('/wifi/scan')).json();
    list.textContent = nets.length ? '' : 'No networks found';
    for (const n of nets) {
      const b = document.createElement('button');
      b.type = 'button';
      b.style.justifyContent = 'space-between';
      b.innerHTML = '<span></span><span class="small"></span>';
      b.firstChild.textContent = n.ssid;
      b.lastChild.textContent = n.rssi + ' dBm' + (n.secure ? '' : ' · open');
      b.onclick = () => { document.querySelector('input[name=ssid]').value = n.ssid; document.querySelector('input[name=pass]').focus(); };
      list.appendChild(b);
    }
  } catch (e) {
    list.textContent = 'Could not look for networks';
  }
  find.disabled = false;
  find.textContent = 'Find networks again';
}
$('find').onclick = scan;

// --- Debug ---------------------------------------------------------------------------------------------

// Cruller pushes the reports over the WebSocket (type 6) every 2 s while this page shows Debug.
const samples = []; // {t, tx, rx}: from the status in each report, for the byte-rate charts
const replyTimes = [];
let toldDebug = null; // [socket, showing] last sent

function tellDebug() {
  if (!ws || ws.readyState !== 1) return;
  const showing = tab === 'debug' && document.visibilityState === 'visible';
  if (toldDebug && toldDebug[0] === ws && toldDebug[1] === showing) return;
  ws.send(new Uint8Array([0x11, showing ? 1 : 0]));
  toldDebug = [ws, showing];
}

let report = null; // the last Debug report

const esc = (s) => String(s).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
const avg = (a) => (a.length ? Math.round(a.reduce((x, y) => x + y, 0) / a.length) : 0);
const rows = (id, html) => { $(id).innerHTML = html; };
const barCell = (used, size) => {
  const p = size ? Math.min(100, Math.round(100 * used / size)) : 0;
  return '<td><div class="bar"><div style="width:' + p + '%;background:' + (p > 85 ? 'var(--warn)' : '#C9CCD1') + '"></div></div></td>';
};
const OWNERS = ['page', 'power check', 'Cruller', 'HTTP API'];
const owner = (n) => OWNERS[n] || 'RFC 2217 #' + (n - 3);

function onDebug(kind, t) {
  if (kind === 6) { // every line from the RT4K: "owner\ttext"
    const now = new Date().toTimeString().slice(0, 8);
    const lines = t.split('\n').map((l) => {
      const i = l.indexOf('\t'), o = +l.slice(0, i);
      // binary leftovers of a transfer (a frame's tail before its closing line) as dots
      const txt = l.slice(i + 1).replace(/[^\x20-\x7e]+/g, '·').replace(/^.*?\[COM\] /, '').trim();
      return txt ? now + '  ' + (o < 0 ? 'everyone' : owner(o)).padEnd(12) + txt : '';
    }).filter(Boolean);
    if (lines.length) append('d-rx', lines.join('\n') + '\n');
    return;
  }
  if (kind !== 5) return;
  const r = report = JSON.parse(t);
  const s = r.status;
  samples.push({ t: Date.now(), tx: s.rt4k_tx, rx: s.rt4k_rx });
  if (samples.length > 61) samples.shift();
  st(s);

  // Serial link
  const se = r.serial;
  $('s-dot').className = 'dot ' + (se.usb ? 'ok' : 'bad');
  text('s-usb', se.usb ? 'connected' : 'not connected');
  text('s-over', se.overruns);
  $('s-over').style.color = se.overruns ? 'var(--warn)' : 'var(--ok)';
  text('s-err', se.line_errors);
  $('s-err').style.color = se.line_errors ? 'var(--warn)' : 'var(--ok)';
  text('s-speed', 'FT232R · ' + se.baud / 1e6 + ' Mbaud');
  text('s-rx', bytes(se.rx) + ' · ' + se.packets.toLocaleString() + ' packets');
  text('s-tx', bytes(se.tx));
  text('s-cmds', se.commands + ' · ' + se.commands_dropped);
  text('s-flow', se.flow ? 'on' : 'off');
  text('s-ctsoff', se.cts_off_packets + ' packets');
  text('s-wait', se.link_wait_max_ms + ' ms');
  for (const k of ['cts', 'dsr', 'dcd', 'ri']) $('s-' + k).className = 'pill' + (se[k] ? ' on' : '');

  // Command queue: oldest first from Cruller; newest first here
  const cmds = r.console.slice().reverse();
  const answered = cmds.filter((c) => c.reply_ms >= 0);
  replyTimes.length = 0;
  answered.forEach((c) => replyTimes.push(c.reply_ms));
  text('q-reply', answered.length ? avg(replyTimes) + ' ms' : '–');
  text('q-reply2', answered.length ? 'avg of ' + answered.length + ' · max ' + Math.max(...replyTimes) + ' ms' : 'no replies yet');
  const wins = cmds.map((c) => c.window_ms);
  text('q-win', wins.length ? avg(wins) + ' ms' : '–');
  text('q-win2', wins.length ? 'avg · max ' + Math.max(...wins) + ' ms' : 'until the reply is complete');
  rows('q-rows', cmds.slice(0, 8).map((c) => '<tr><td>' + esc(c.cmd) + '</td><td>' + owner(c.owner) + '</td><td class="r' +
    (c.reply_ms < 0 ? ' warn">none' : '">' + c.reply_ms + ' ms') + '</td><td class="r">' + c.window_ms + ' ms</td></tr>').join('') ||
    '<tr><td colspan="4" class="small">No commands yet</td></tr>');

  // Memory and network
  const m = r.memory, h = m.heap;
  text('n-heap', Math.round(h.free / 1024) + ' of ' + Math.round(h.size / 1024) + ' KB · lowest ' + Math.round(h.lowest / 1024));
  $('n-heapbar').style.width = Math.round(100 * (1 - h.free / h.size)) + '%';
  const pools = m.pools.filter((p) => !/netbufs|UDP/.test(p.name));
  pools.push({ name: 'network heap (KB)', used: +(m.lwip_heap.used / 1024).toFixed(1), peak: +(m.lwip_heap.peak / 1024).toFixed(1),
    size: Math.round(m.lwip_heap.size / 1024), failed: m.lwip_heap.failed });
  rows('n-rows', pools.map((p) => '<tr><td>' + esc(p.name.replace(' (NETCONN)', '')) + ' <span class="small">of ' + p.size + '</span></td><td class="r">' +
    p.used + '</td><td class="r">' + p.peak + '</td><td class="r ' + (p.failed ? 'warn' : 'good') + '">' + p.failed + '</td>' +
    barCell(p.used, p.size) + '</tr>').join(''));
  text('n-clients', m.clients.used + ' of ' + m.clients.max + ' · ' + m.clients.web + ' pages, ' + m.clients.rfc2217 + ' RFC 2217');

  // Screen mirror
  const mi = r.mirror;
  text('r-key', mi.key.count ? mi.key.avg + ' ms' : '–');
  text('r-key2', mi.key.count ? mi.key.count + ' keys · max ' + mi.key.max + ' ms' : 'press a key on the remote');
  text('r-pages', mi.visible_pages);
  text('r-state', mi.state);
  rows('r-rows', mi.planes.map((p) => {
    const errs = p.no_link + p.timeout + p.device + p.protocol;
    return '<tr><td>' + (p.name === 'osd' ? 'Menu' : 'Messages') + '</td><td class="r">' + p.frames + '</td><td class="r">' + p.poll_last_ms +
      ' ms</td><td class="r">' + p.poll_max_ms + ' ms</td><td class="r ' + (errs ? 'warn' : 'good') + '" title="no link ' + p.no_link +
      ', timeout ' + p.timeout + ', refused ' + p.device + ', bad frame ' + p.protocol + '">' + errs + '</td></tr>';
  }).join(''));

  drawCharts();
}

async function getText(path) {
  const r = await fetch(path, { cache: 'no-store' });
  return r.text();
}

// The task list: on request only (scanning the stacks pauses both cores for a few milliseconds).
async function stacks() {
  rows('t-rows', '<tr><td colspan="4" class="small">Reading…</td></tr>');
  try {
    const t = await getText('/debug/tasks?stacks');
    const list = [...t.matchAll(/^(\S+)\s+(\S+)\s+prio (\d+) stack free (\d+)$/gm)].map((m) => ({ name: m[1], state: m[2], prio: +m[3], free: +m[4] }));
    list.sort((a, b) => a.free - b.free);
    rows('t-rows', list.map((k) => '<tr><td>' + esc(k.name) + '</td><td>' + k.state + '</td><td class="r">' + k.prio + '</td><td class="r ' +
      (k.free < 128 ? 'warn' : '') + '">' + k.free * 4 + ' B</td></tr>').join('')); // high-water mark, in 4-byte words
  } catch (e) {
    rows('t-rows', '<tr><td colspan="4" class="small">Could not read them: ' + esc(e.message) + '</td></tr>');
  }
}

// The freeze recorder: the newest sample of each core.
async function freeze() {
  freeze.done = true;
  try {
    const t = await getText('/debug/freeze');
    const last = {};
    for (const m of t.matchAll(/^core (\d)\s+(-?\d+) ms (\S+)\s+pc=\S+ lr=\S+ wdt left (\d+) fed (\d+) ms ago/gm)) last[m[1]] = m;
    rows('z-rows', Object.values(last).map((m) => '<tr><td>' + m[1] + '</td><td>' + esc(m[3]) + '</td><td class="r">' + (m[4] / 1000).toFixed(1) +
      ' s</td><td class="r">' + m[5] + ' ms ago</td></tr>').join('') || '<tr><td colspan="4" class="small">No samples</td></tr>');
  } catch (e) {
    rows('z-rows', '<tr><td colspan="4" class="small">Could not read it: ' + esc(e.message) + '</td></tr>');
  }
}

async function tool(path) {
  text('d-tool', '…');
  try {
    const r = await fetch(path, { method: 'POST' });
    text('d-tool', await r.text());
  } catch (e) {
    text('d-tool', e.message);
  }
}

function copyJson() {
  const data = { report: report || { status: S }, keyTimesSeenByThisPage: keyTimes };
  navigator.clipboard.writeText(JSON.stringify(data, null, 2)).then(() => text('d-state', 'Copied'), () => text('d-state', 'Could not copy'));
}

// A line chart: series [{data, color}], y from 0 to a round maximum, labels on the left.
function chart(id, series, unit) {
  const c = $(id), r = c.getBoundingClientRect(), d = devicePixelRatio || 1;
  if (!r.width) return;
  c.width = Math.round(r.width * d);
  c.height = Math.round(r.height * d);
  const g = c.getContext('2d');
  g.scale(d, d);
  const W = r.width, H = r.height, L = 40, T = 12, B = 20, R = 8;
  let max = Math.max(1, ...series.flatMap((s) => s.data));
  const step = Math.pow(10, Math.floor(Math.log10(max)));
  max = Math.ceil(max / step) * step;
  g.font = '10px ' + getComputedStyle(document.body).getPropertyValue('--mono');
  g.fillStyle = '#6B6F76';
  g.strokeStyle = '#23262B';
  for (let i = 0; i <= 3; i++) {
    const y = T + (H - T - B) * i / 3;
    g.beginPath(); g.moveTo(L, y); g.lineTo(W - R, y); g.stroke();
    const v = max * (3 - i) / 3;
    g.fillText(v >= 10 ? Math.round(v) : v.toFixed(1), 6, y + 3);
  }
  g.fillText(unit, 6, H - 5);
  for (const s of series) {
    if (s.data.length < 2) continue;
    g.strokeStyle = s.color;
    g.lineWidth = 1.5;
    g.lineJoin = 'round';
    g.beginPath();
    s.data.forEach((v, i) => {
      const x = L + (W - L - R) * i / (s.data.length - 1), y = T + (H - T - B) * (1 - v / max);
      if (i) g.lineTo(x, y); else g.moveTo(x, y);
    });
    g.stroke();
  }
}

function rates(key) {
  const out = [];
  for (let i = 1; i < samples.length; i++) {
    const dt = (samples[i].t - samples[i - 1].t) / 1000;
    out.push(Math.max(0, samples[i][key] - samples[i - 1][key]) / 1024 / dt);
  }
  return out;
}

function drawCharts() {
  const tx = rates('tx'), rx = rates('rx');
  const last = (a) => (a.length ? a[a.length - 1] : 0);
  text('lg-tx', 'sent ' + last(tx).toFixed(2) + ' KB/s');
  text('lg-rx', 'received ' + last(rx).toFixed(1) + ' KB/s');
  if (replyTimes.length) text('lg-rep', 'reply ' + Math.round(replyTimes.reduce((a, b) => a + b, 0) / replyTimes.length) + ' ms');
  chart('ch-tx', [{ data: tx, color: '#4CC38A' }], 'KB/s');
  chart('ch-rx', [{ data: rx, color: '#4CC38A' }], 'KB/s');
  chart('ch-lat', [{ data: keyTimes, color: '#E8618C' }, { data: replyTimes.slice().reverse(), color: '#C9CCD1' }], 'ms');
}

// --- start ---------------------------------------------------------------------------------------------

route();
conn();
