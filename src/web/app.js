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
window.askUser = askUser; // fw.js, sd.js

// The same, asking for a line of text: resolves it on OK, null on Cancel. select: how much of value
// starts selected (a file's name without its extension), all of it by default.
function askText(title, body, value, ok, select) {
  const input = $('ask-input');
  input.hidden = false;
  input.value = value || '';
  const answer = askUser(title, body, ok);
  input.focus();
  input.setSelectionRange(0, select === undefined ? input.value.length : select);
  // Enter here would submit with the form's first button (Cancel).
  input.onkeydown = (e) => { if (e.key === 'Enter') { e.preventDefault(); $('ask').close('yes'); } };
  return answer.then((yes) => { input.hidden = true; return yes ? input.value : null; });
}
window.askText = askText; // sd.js

// --- tabs -------------------------------------------------------------------------------------------

let tab = 'rt4k';

const IN_PORTAL = location.hostname === '192.168.4.1';

function route() {
  const [t, sub, ...rest] = (location.hash.slice(1) || (IN_PORTAL ? 'setup' : 'rt4k')).split('/');
  tab = ['rt4k', 'svs', 'cruller', 'debug', 'setup'].includes(t) ? t : 'rt4k';
  document.body.classList.toggle('setup', tab === 'setup');
  if (tab === 'setup' && !wz.started) { wz.started = true; wzGo(1); wzScan(); wzResume(); }
  document.querySelectorAll('[data-view]').forEach((e) => { e.hidden = e.dataset.view !== tab; });
  document.querySelectorAll('nav.tabs a').forEach((a) => a.toggleAttribute('aria-current', a.dataset.tab === tab));
  const view = sub === 'firmware' || sub === 'sd' ? sub : 'live';
  document.querySelectorAll('[data-subview]').forEach((e) => { e.hidden = e.dataset.subview !== view; });
  document.querySelectorAll('.sub a').forEach((a) => a.toggleAttribute('aria-current', a.dataset.sub === view));
  $('chip-rt4k').hidden = tab !== 'rt4k';
  $('chip-wifi').hidden = $('chip-ver').hidden = tab === 'rt4k';
  if (tab === 'rt4k' && view === 'firmware' && window.fwOpen) window.fwOpen();
  if (tab === 'rt4k' && view === 'sd' && window.sdOpen) window.sdOpen(rest); // rest: the folder (sd.js)
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
  if (window.sdStatus) window.sdStatus(s); // the SD card view reads the folder again once the RT4K is on
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
  const web = s.web_clients || 0, ev = s.event_clients || 0, rfc = s.rfc2217_count || 0, max = s.clients_max || 8;
  text('cl-n', web + ev + rfc);
  text('cl-max', 'of ' + max + ', shared');
  text('cl-web', web);
  text('cl-ev', ev);
  text('cl-rfc', rfc);
  $('cl-bw').style.flexGrow = web;
  $('cl-be').style.flexGrow = ev;
  $('cl-br').style.flexGrow = rfc;
  $('cl-bf').style.flexGrow = Math.max(0, max - web - ev - rfc);
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
  if (s.platform && !upd.loaded) updLoad();
  if (s.update && upd.onProgress) upd.onProgress(s.update);
  if (s.setup) wzProgress(s.setup);
  showUptime();
}

// --- SVS Bridge card ------------------------------------------------------------------------------------

const ago = (s) => (s < 5 ? 'just now' : duration(s) + ' ago');

// The switch as the bridge describes it (GET /api/v1/svs "switch"): fetched when its switch_seq changes.
const svsSw = { seq: 0, loading: 0, data: null, last: null };
const KINDS = { scart: 'SCART', component: 'Component', vga: 'VGA', svideo: 'S-Video', dterm: 'D-Terminal', bnc: 'BNC' };
const kindName = (k) => KINDS[k] || (k ? k.toUpperCase() : '');

// The consoles, by their id in the SVS Bridge's list: a short name, and an icon. Each is its
// controller (or the machine itself) seen from above on a 60x40 grid: the body a tint of the text
// colour (.con .b in index.html), sticks and pads in the text colour, and the buttons in their own
// colours (PlayStation's shapes, the ABXY of Xbox, SNES and Dreamcast, GameCube's big green A,
// Neo Geo's four) so each reads at a glance, even between siblings (PS1 has no sticks, PS2 its red
// analog light, PS3 its PS button and player lights; the first Xbox its big jewel).
const HUE = { red: '#E5534B', yellow: '#F0B44C', green: '#4CC38A', blue: '#5B8DEF', pink: '#E07FBF', teal: '#3FBFA8', orange: '#F08A24', grey: '#9EA3AB' };
const I = {
  body: (d) => `<path class="b" d="${d}"/>`,
  // The cross, s its half length.
  dpad: (x, y, s = 4.6) => { const w = s * 0.36; return `<path d="M${x - w} ${y - s}h${2 * w}v${s - w}h${s - w}v${2 * w}h${w - s}v${s - w}h${-2 * w}v${w - s}h${w - s}v${-2 * w}h${s - w}z"/>`; },
  // PS1's: four separate arrows.
  arrows: (x, y) => `<path class="l" style="stroke-width:2.4;stroke-linecap:butt" d="M${x} ${y - 5}v3M${x} ${y + 2}v3M${x - 5} ${y}h3M${x + 2} ${y}h3"/>`,
  // A round pad with the cross on it (Mega Drive, Saturn).
  disc: (x, y, r = 5.2) => `<circle class="k" cx="${x}" cy="${y}" r="${r}"/><path class="l" style="stroke-width:1.1" d="M${x} ${y - r + 1.6}v${2 * r - 3.2}M${x - r + 1.6} ${y}h${2 * r - 3.2}"/>`,
  btn: (x, y, r, c) => `<circle cx="${x}" cy="${y}" r="${r}"${c ? ` fill="${c}"` : ''}/>`,
  stick: (x, y, r = 3.4) => `<circle class="k" cx="${x}" cy="${y}" r="${r}"/><circle cx="${x}" cy="${y}" r="${r * 0.42}"/>`,
  pill: (x, y, w = 3.6) => `<path class="l" style="stroke-width:1.5" d="M${x - w / 2} ${y}h${w}"/>`,
  // △ ○ × □, around (x, y).
  ps: (x, y, d = 4.6, s = 1.8) => `<g class="l" style="stroke-width:1.2">` +
    `<path stroke="${HUE.teal}" d="M${x} ${y - d - s}l${s * 1.1} ${s * 1.8}h${-s * 2.2}z"/>` +
    `<circle stroke="${HUE.red}" cx="${x + d}" cy="${y}" r="${s}"/>` +
    `<path stroke="${HUE.blue}" d="M${x - s * 0.9} ${y + d - s * 0.9}l${s * 1.8} ${s * 1.8}m0 ${-s * 1.8}l${-s * 1.8} ${s * 1.8}"/>` +
    `<rect stroke="${HUE.pink}" x="${x - d - s * 0.85}" y="${y - s * 0.85}" width="${s * 1.7}" height="${s * 1.7}"/></g>`,
  // Four buttons in a diamond: top, right, bottom, left.
  diamond: (x, y, d, r, [t, rt, b, l]) => I.btn(x, y - d, r, t) + I.btn(x + d, y, r, rt) + I.btn(x, y + d, r, b) + I.btn(x - d, y, r, l),
};
// The PlayStation pads' body, and a computer's rows of keys.
const dualshock = 'M17 8h26c6 0 10 3 11.5 9.5 2 8 3 16-1.5 18-4 1.8-7-2.5-9.5-6.5-1-1.5-2-2-3.5-2h-21c-1.5 0-2.5.5-3.5 2-2.5 4-5.5 8.3-9.5 6.5-4.5-2-3.5-10-1.5-18C7 11 11 8 17 8z';
const keyRows = (x0, x1, ys) => `<path class="l" style="stroke-width:1.9;stroke-dasharray:1.9 1.1;stroke-linecap:butt" d="${ys.map((y) => `M${x0} ${y}H${x1}`).join('')}"/>`;
const CONSOLES = {
  nes: ['NES', I.body('M5 10h50a2 2 0 0 1 2 2v17a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V12a2 2 0 0 1 2-2z') +
    `<rect class="k" x="6" y="13" width="48" height="15" rx="1"/>${I.dpad(13.5, 20.5)}` +
    `<path class="l" style="stroke-width:.9;opacity:.6" d="M22 15.5h13M22 17.5h13"/>${I.pill(25, 23.5)}${I.pill(32, 23.5)}` +
    `<rect x="37.5" y="17" width="7" height="7" rx="1" class="k"/><rect x="46" y="17" width="7" height="7" rx="1" class="k"/>${I.btn(41, 20.5, 2.6, HUE.red)}${I.btn(49.5, 20.5, 2.6, HUE.red)}`],
  snes: ['SNES', I.body('M16 9h28a11 11 0 0 1 0 22H16a11 11 0 0 1 0-22z') + `<path class="l" style="stroke-width:1.8" d="M8 9.5a12 12 0 0 1 7-2.3M45 7.2a12 12 0 0 1 7 2.3"/>` +
    `${I.dpad(15.5, 20)}<path class="l" style="stroke-width:1.7" d="M25 22.5l3-1.7M31 22.5l3-1.7"/>${I.diamond(44.5, 20, 4.4, 2.3, [HUE.blue, HUE.red, HUE.yellow, HUE.green])}`],
  n64: ['N64', I.body('M7 9.5C12 6.5 22 7 30 7s18-.5 23 2.5c3.5 2 4 6 3 10.5l-2.5 12c-.7 3-4.3 3-5 0l-2.3-9c-.4-1.5-1.3-2-2.7-2h-4.3l-2.7 13c-.6 3-7.4 3-8 0L22.8 21.5h-4.3c-1.4 0-2.3.5-2.7 2l-2.3 9c-.7 3-4.3 3-5 0L6 20c-1-4.5-.5-8.5 1-10.5z') +
    `${I.dpad(13, 15.5, 4)}${I.stick(30, 18, 3)}${I.btn(30, 11.5, 1.5, HUE.red)}${I.btn(41.5, 19, 2.4, HUE.blue)}${I.btn(38, 14.5, 2.1, HUE.green)}` +
    I.diamond(48.5, 14, 3, 1.35, [HUE.yellow, HUE.yellow, HUE.yellow, HUE.yellow])],
  gamecube: ['GameCube', I.body('M14 8c6-1 11 1.5 16 1.5S40 7 46 8c7.5 1.3 11.5 8 11 16-.4 6.5-3.5 10.5-8 10.3-4-.2-5.5-3.7-8-6.3-1-1-2-1.5-3.5-1.5h-15c-1.5 0-2.5.5-3.5 1.5-2.5 2.6-4 6.1-8 6.3-4.5.2-7.6-3.8-8-10.3-.5-8 3.5-14.7 11-16z') +
    `${I.stick(13.5, 16.5, 4)}${I.dpad(22, 23, 3.2)}${I.btn(30, 16.5, 1.2)}${I.btn(44.5, 16.5, 3.6, HUE.green)}${I.btn(39, 20.5, 1.9, HUE.red)}${I.btn(36.5, 24.3, 2.2, HUE.yellow)}` +
    `<path class="l" style="stroke-width:2.2" d="M50 12.3a6.2 6.2 0 0 1 .3 8.6M39.6 12.2a6.2 6.2 0 0 1 8.4-1.9"/>`],
  wii: ['Wii', I.body('M8 13h44a5 5 0 0 1 5 5v4a5 5 0 0 1-5 5H8a5 5 0 0 1-5-5v-4a5 5 0 0 1 5-5z') +
    `${I.btn(7, 16.5, 1.1, HUE.red)}${I.dpad(13, 20, 3.8)}<circle class="k" cx="22" cy="20" r="2.8"/>${I.btn(30, 16.3, .9)}${I.btn(30, 20, 1.3, HUE.blue)}${I.btn(30, 23.7, .9)}` +
    `${I.btn(38, 20, 1.8)}${I.btn(43.5, 20, 1.8)}${[16.5, 19, 21.5, 24].map((y) => I.btn(52.5, y, .7, HUE.blue)).join('')}`],
  sms: ['Master System', I.body('M4 11h52a1 1 0 0 1 1 1v17a1 1 0 0 1-1 1H4a1 1 0 0 1-1-1V12a1 1 0 0 1 1-1z') + `<path class="l" d="M12 11c0-3.5 2-5.5 6-5.5"/>` +
    `<rect class="k" x="7.5" y="14" width="13" height="13" rx="2"/>${I.dpad(14, 20.5, 4.4)}<rect x="34" y="17" width="7" height="7" rx="1.2"/><rect x="45" y="17" width="7" height="7" rx="1.2"/>`],
  megadrive: ['Mega Drive', I.body('M9 10.5c8-4 34-4 42 0 6.5 3.5 7 13 2 19-3.5 4-8 4-12 .5-3.5-3-7.5-4-11-4s-7.5 1-11 4c-4 3.5-8.5 3.5-12-.5-5-6-4.5-15.5 2-19z') +
    `${I.disc(15, 19, 5.5)}${I.pill(30, 15.5)}${I.btn(37, 23, 2.7)}${I.btn(43, 20.3, 2.7)}${I.btn(49, 17.6, 2.7)}`],
  saturn: ['Saturn', I.body('M11 9c9-2.5 29-2.5 38 0 7 2 8.5 12 6 18.5-2 5.5-8 6.5-12 2.5-2-2-4.5-3-6.5-3h-13c-2 0-4.5 1-6.5 3-4 4-10 3-12-2.5-2.5-6.5-1-16.5 6-18.5z') +
    `<path class="l" style="stroke-width:1.8" d="M7 10.5a10 10 0 0 1 7-3M46 7.5a10 10 0 0 1 7 3"/>${I.disc(14.5, 18.5, 5)}${I.pill(30, 19)}` +
    `${I.btn(38.5, 22.5, 2.1)}${I.btn(43.5, 21, 2.1)}${I.btn(48.5, 19.5, 2.1)}${I.btn(38.5, 16, 1.5)}${I.btn(43, 14.5, 1.5)}${I.btn(47.5, 13, 1.5)}`],
  dreamcast: ['Dreamcast', I.body('M10 7h40c5 0 7.5 4 7 9l-2 13c-.8 5-5.5 6-8.5 2.5L42 26H18l-4.5 5.5C10.5 35 5.8 34 5 29L3 16c-.5-5 2-9 7-9z') +
    `<rect class="k" x="23" y="8.5" width="14" height="10" rx="1.5"/>${I.stick(12.5, 14.5, 3.4)}${I.dpad(15, 23.5, 3.8)}` +
    `<path d="M31.3 22.3a1.6 1.6 0 1 1-1.9-1.6 3 3 0 1 1-2.3 3.7" fill="none" stroke="${HUE.orange}" stroke-width="1.2" stroke-linecap="round"/>` +
    I.diamond(46, 15.5, 3.9, 1.9, [HUE.green, HUE.blue, HUE.red, HUE.yellow])],
  ps1: ['PS1', I.body(dualshock) + `${I.arrows(14, 17)}${I.pill(26, 17, 3)}${I.pill(33.5, 17, 3)}${I.ps(46, 17)}`],
  ps2: ['PS2', I.body(dualshock) + `${I.dpad(14, 16, 4.2)}${I.ps(46, 16)}${I.stick(22, 24, 3.3)}${I.stick(38, 24, 3.3)}${I.btn(30, 21.5, .9, HUE.red)}${I.pill(26.5, 15.5, 2.6)}${I.pill(33.5, 15.5, 2.6)}`],
  ps3: ['PS3', I.body(dualshock) + `<path class="l" style="stroke:${HUE.red};stroke-width:1.3;stroke-linecap:butt" d="M25 9.5h1.8M28.1 9.5h1.8M31.2 9.5h1.8M34.3 9.5h1.8"/>` +
    `${I.dpad(14, 16, 4.2)}${I.ps(46, 16)}${I.stick(22, 24, 3.3)}${I.stick(38, 24, 3.3)}<circle class="k" cx="30" cy="20.5" r="2.1"/>${I.pill(26.5, 15, 2.6)}${I.pill(33.5, 15, 2.6)}`],
  xbox: ['Xbox', I.body('M12 5h36c6 0 10 5 10.5 12 .5 9-1 18-6 18-4 0-6-4-8-6.5H15.5c-2 2.5-4 6.5-8 6.5-5 0-6.5-9-6-18C2 10 6 5 12 5z') +
    `${I.stick(11.5, 13.5, 3.6)}${I.dpad(20.5, 23, 3.6)}${I.stick(39.5, 23, 3.4)}` +
    `<circle class="k" cx="30" cy="14" r="5.2"/><circle cx="30" cy="14" r="3.4" fill="${HUE.green}"/><path class="l" style="stroke:#1B1D21;stroke-width:1.2" d="M28.3 12.3l3.4 3.4m0-3.4l-3.4 3.4"/>` +
    `${I.diamond(48, 13.5, 3.8, 1.8, [HUE.yellow, HUE.red, HUE.green, HUE.blue])}${I.btn(52, 21.5, 1.3)}${I.btn(55, 18, 1.3, '#E6E4DE')}`],
  xbox360: ['Xbox 360', I.body('M14 8c6-1 11 1.5 16 1.5S40 7 46 8c7 1 11 7 11.5 14 .5 8-2.5 13-7 12.5-3.5-.4-5-4-8-6.5H18c-3 2.5-4.5 6.1-8 6.5-4.5.5-7.5-4.5-7-12.5C3.5 15 7.5 9 14 8z') +
    `${I.stick(14, 16, 3.4)}${I.dpad(22.5, 24, 3.4)}${I.stick(37.5, 24, 3.2)}` +
    `<circle class="k" cx="30" cy="15" r="2.9"/><circle cx="30" cy="15" r="1.6" fill="${HUE.green}"/>${I.btn(24.5, 15.5, .9)}${I.btn(35.5, 15.5, .9)}` +
    I.diamond(46, 16, 3.8, 1.9, [HUE.yellow, HUE.red, HUE.green, HUE.blue])],
  pce: ['PC Engine', I.body('M8 10h44a4 4 0 0 1 4 4v13a4 4 0 0 1-4 4H8a4 4 0 0 1-4-4V14a4 4 0 0 1 4-4z') +
    `${I.dpad(13.5, 20.5)}${I.pill(25, 24)}${I.pill(31.5, 24)}<rect class="k" x="37" y="13" width="5" height="3.2" rx=".8"/><rect class="k" x="45" y="13" width="5" height="3.2" rx=".8"/>` +
    `${I.btn(40.5, 23, 2.9)}${I.btn(48.5, 21.5, 2.9)}`],
  neogeo: ['Neo Geo', I.body('M6 12h48a3 3 0 0 1 3 3v17a3 3 0 0 1-3 3H6a3 3 0 0 1-3-3V15a3 3 0 0 1 3-3z') +
    `<circle class="k" cx="15" cy="23.5" r="7.2"/>${I.btn(15, 23.5, 4.8, HUE.red)}<circle cx="13.5" cy="22" r="1.4" fill="#fff" fill-opacity=".45"/>` +
    `${I.btn(29, 27.5, 2.9, HUE.red)}${I.btn(36, 24.5, 2.9, HUE.yellow)}${I.btn(43.5, 23, 2.9, HUE.green)}${I.btn(51, 23.5, 2.9, HUE.blue)}${I.pill(44, 16)}${I.pill(50.5, 16)}`],
  atari2600: ['Atari 2600', I.body('M17 16h26a3 3 0 0 1 3 3v14a3 3 0 0 1-3 3H17a3 3 0 0 1-3-3V19a3 3 0 0 1 3-3z') +
    `<ellipse cx="22" cy="20.5" rx="3.3" ry="2.3" fill="${HUE.red}"/><ellipse class="k" cx="32" cy="26.5" rx="5.5" ry="3.8"/>` +
    `<path class="l" style="stroke-width:3.6" d="M32 26V6.5"/>`],
  jaguar: ['Jaguar', I.body('M12 5h36c6 0 10 5 10 11.5S54.5 28 49.5 28c-3 0-5-2-7.5-2l-.8 8.5c-.2 2-1.7 3-3.7 3H22.5c-2 0-3.5-1-3.7-3L18 26c-2.5 0-4.5 2-7.5 2C5.5 28 2 22.5 2 16.5S6 5 12 5z') +
    `${I.dpad(12, 15, 4.2)}${I.btn(42, 20, 2.1)}${I.btn(47, 16.5, 2.1)}${I.btn(52, 13, 2.1)}${I.pill(25.5, 12, 3)}${I.pill(34.5, 12, 3)}` +
    [0, 1, 2].map((c) => [0, 1, 2, 3].map((r) => `<rect x="${22.2 + c * 5.8}" y="${18.2 + r * 4}" width="3.8" height="2.6" rx=".6"/>`).join('')).join('')],
  '3do': ['3DO', I.body('M13 9h34c6 0 10 4 10 10.5 0 7-4 12.5-9.5 12.5-3.5 0-5.5-2-8-4.5-1.3-1.3-2.5-2-4.5-2h-10c-2 0-3.2.7-4.5 2C18 30 16 32 12.5 32 7 32 3 26.5 3 19.5 3 13 7 9 13 9z') +
    `${I.dpad(14, 20, 4.4)}${I.btn(37, 24, 2.6)}${I.btn(42.5, 20.5, 2.6)}${I.btn(48, 17, 2.6)}${I.pill(27, 15, 3)}${I.pill(33, 15, 3)}`],
  cdi: ['CD-i', I.body('M11 9h38a7 7 0 0 1 7 7v9a7 7 0 0 1-7 7H11a7 7 0 0 1-7-7v-9a7 7 0 0 1 7-7z') +
    `<circle class="k" cx="17" cy="20.5" r="6.5"/><circle cx="17" cy="20.5" r="2.4"/>${I.btn(38, 20.5, 3, HUE.grey)}${I.btn(47, 20.5, 3, HUE.grey)}` +
    `<path class="l" style="stroke-width:.9;opacity:.6" d="M35 26.5h15"/>`],
  amiga: ['Amiga', I.body('M4 10h52a2 2 0 0 1 2 2v19a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V12a2 2 0 0 1 2-2z') +
    keyRows(6, 31, [14]) + keyRows(6, 43, [18.5, 22, 25.5]) + `<path class="l" style="stroke-width:2.2" d="M15 29.3h18"/>` + keyRows(46, 54, [18.5, 22, 25.5, 29]) +
    `${I.btn(38, 14, .9, HUE.green)}${I.btn(41.5, 14, .9, HUE.orange)}<path class="l" style="stroke-width:1.1;opacity:.7" d="M58 17v9"/>`],
  c64: ['C64', I.body('M7 10h46c1.3 0 2.2.7 2.5 2L58 30.5c.3 1.5-.6 2.5-2 2.5H4c-1.4 0-2.3-1-2-2.5L4.5 12c.3-1.3 1.2-2 2.5-2z') +
    keyRows(8, 44, [15, 19, 23, 27]) + `<path class="l" style="stroke-width:2.2" d="M17 30.2h20"/>` +
    [14.5, 19, 23.5, 28].map((y) => `<rect x="47.5" y="${y - 1.6}" width="6" height="3.2" rx=".6"/>`).join('') +
    `<path d="M9 12.4h6" stroke="${HUE.blue}" stroke-width="1.2"/><path d="M15.5 12.4h3" stroke="${HUE.red}" stroke-width="1.2"/>`],
  msx: ['MSX', I.body('M4 12h52a2 2 0 0 1 2 2v17a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V14a2 2 0 0 1 2-2z') + `<rect class="k" x="36" y="6" width="17" height="6" rx="1"/>` +
    keyRows(6, 40, [17, 21, 25]) + `<path class="l" style="stroke-width:2.2" d="M13 29h22"/>` + I.diamond(49.5, 22, 3.6, 1.6, [])],
  supergun: ['Arcade', I.body('M18 2h24l1.5 6-2 2v10l3 3v15H15.5V23l3-3V10l-2-2z') +
    `<rect x="19.5" y="3" width="21" height="4" rx=".8" fill="${HUE.orange}" fill-opacity=".85"/><rect x="20.5" y="10" width="19" height="10" rx="1" fill="#0E0F11" stroke="currentColor" stroke-width="1"/>` +
    `<path class="l" style="stroke-width:1.1" d="M15.5 23h29"/>${I.btn(22.5, 21.5, 1.7, HUE.red)}${I.btn(30.5, 22.2, 1.2, HUE.blue)}${I.btn(34.5, 22.2, 1.2, HUE.blue)}${I.btn(38.5, 22.2, 1.2, HUE.blue)}` +
    `<rect class="k" x="26" y="28" width="8" height="6" rx=".8"/>`],
  pc: ['PC', I.body('M12 4h36a2 2 0 0 1 2 2v21a2 2 0 0 1-2 2H12a2 2 0 0 1-2-2V6a2 2 0 0 1 2-2z') +
    `<rect x="13" y="7" width="34" height="18" rx="1" fill="#0E0F11" stroke="currentColor" stroke-width="1"/><path class="l" style="stroke-width:1.8" d="M30 29v4M21 35h18"/>` +
    `${I.btn(45.5, 27, .7, HUE.green)}`],
};
// A console Cruller has no drawing of (a newer bridge's list, or only a name): a plain pad.
const PAD = I.body('M16 10h28c7 0 11 5 12 11.5.8 5.5-1 10.5-5.5 10.5-3.5 0-5-3-7.5-5.5H17c-2.5 2.5-4 5.5-7.5 5.5C5 32 3.2 27 4 21.5 5 15 9 10 16 10z') +
  `${I.dpad(15.5, 19.5, 4)}${I.diamond(44.5, 19.5, 3.6, 1.7, [])}${I.pill(26.5, 18, 3)}${I.pill(33.5, 18, 3)}`;
const svgCon = (body, cls = 'con') => `<svg class="${cls}" viewBox="0 0 60 40" fill="currentColor" aria-hidden="true">${body}</svg>`;
// An input with nothing on it: the icon's place, outlined.
const EMPTY_ICON = svgCon('<rect x="8" y="8" width="44" height="24" rx="10" fill="none" stroke="currentColor" stroke-width="1.5" stroke-dasharray="3.5 3"/>');
const consoleIcon = (id) => svgCon(CONSOLES[id] ? CONSOLES[id][1] : PAD);

async function svsLoad(seq) {
  if (svsSw.loading === seq) return;
  svsSw.loading = seq;
  try {
    const v = await (await fetch('/api/v1/svs')).json();
    svsSw.data = v.switch || null;
    svsSw.seq = v.switch_seq || 0;
  } catch (e) { /* the next status tries again */ }
  svsSw.loading = 0;
  if (svsSw.last) showSvs(svsSw.last);
}

function showSvs(v) {
  svsSw.last = v;
  const paired = v && v.paired, known = v && v.known, live = known && v.heard_s < 150;
  if (known && v.switch_seq && v.switch_seq !== svsSw.seq) svsLoad(v.switch_seq);
  const sw = known && v.switch_seq ? svsSw.data : null;
  const ins = sw ? sw.inputs : [], out = sw ? sw.output : null;
  const port = (n) => ins[n - 1] || {};
  const label = (n) => port(n).name || 'Input ' + n;
  const short = (p) => (CONSOLES[p.device] || [])[0] || p.name || '';
  // Until a bridge has reported or is paired there's nothing to show but that it's awaited.
  document.querySelectorAll('.svs-more').forEach((e) => { e.hidden = !paired && !known; });
  document.querySelectorAll('.svs-wait').forEach((e) => { e.hidden = !!(paired || known); });
  $('v-dot').className = 'dot ' + (!paired && !known ? '' : live ? 'ok' : 'warn');
  text('v-state', !paired && !known ? 'no bridge yet' : live ? 'live' : 'not heard lately');
  // Input 0: the switch has no input active.
  const on = known && v.input > 0;
  // One tile per input, the active one lit: once the bridge has said how many the switch has, or
  // described them (SVS models differ in inputs and outputs).
  const total = known ? Math.max(v.total || 0, ins.length) : 0;
  // What is on screen, and since when (the tile is lit in the grid).
  const what = on ? short(port(v.input)) || 'Input ' + v.input : '';
  $('v-since').hidden = !known;
  text('v-since', !known ? '' : (on ? what + (v.since_s < 5 ? ' just switched in' : ' on screen for ' + duration(v.since_s))
    : 'No input active' + (v.since_s < 5 ? '' : ' for ' + duration(v.since_s))) + (total ? ' · ' + total + ' inputs' : ''));
  $('v-grid').hidden = !total;
  $('v-nogrid').hidden = !!total || (!paired && !known);
  $('v-grid').innerHTML = Array.from({ length: total }, (_, i) => {
    // The console's icon and short name when the bridge says which it is, else the name given.
    // Nothing picked on it: the same tile, with an empty slot for the icon.
    const n = i + 1, p = port(n), on = v.input === n, what = short(p);
    const icon = what ? consoleIcon(p.device) : EMPTY_ICON;
    return '<div class="' + (on ? 'on' : '') + (what ? '' : ' empty') + '" title="S' + n + (p.name ? ': ' + esc(p.name) : '') +
      (p.kind ? ' · ' + esc(kindName(p.kind)) : '') + '">' + '<b>' + n + '</b>' + icon + '<span>' + (what ? esc(what) : 'Empty') + '</span>' +
      '<small>' + (on ? 'ON SCREEN' : esc(kindName(p.kind)) || 'S' + n) + '</small></div>';
  }).join('');
  $('v-noname').hidden = !total || ins.some((p) => p.name || p.device);
  // The output that goes to the RetroTINK.
  text('v-out', out ? [out.name, kindName(out.kind)].filter(Boolean).join(' · ') || '–' : '–');
  text('v-paired', paired || '–');
  text('v-heard', known ? ago(v.heard_s) : '–');
  $('v-hint').hidden = !!paired;
  $('v-unpair').hidden = !paired;
  const hist = known && v.history ? v.history : [];
  $('v-hist').innerHTML = hist.map(([input, s], i) => '<tr><td>' + (input ? esc(short(port(input)) || label(input)) +
    (short(port(input)) ? ' <span class="small">S' + input + '</span>' : '') : 'None active') +
    (i === 0 ? ' <span class="small">(now)</span>' : '') + '</td><td class="r">' + ago(s) + '</td></tr>').join('') ||
    '<tr><td colspan="2" class="small">None yet</td></tr>';
}

async function unpair() {
  if (!(await askUser('Unpair the SVS Bridge?', 'Cruller forgets it; the next SVS Bridge that reports pairs instead.', 'Unpair', true))) return;
  try { await fetch('/api/v1/svs/unpair', { method: 'POST' }); } catch (e) { /* the next status shows it */ }
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

// --- Cruller firmware updates ----------------------------------------------------------------------------
//
// The page lists the releases (GitHub's API allows cross-origin reads) and picks this platform's image
// among each one's assets; Cruller downloads it itself (POST /update/fetch, src/core/ota_fetch.h),
// since GitHub doesn't let pages read a release's assets. The status carries the download's progress.

const FW_RELEASES = 'https://api.github.com/repos/margaale/Cruller/releases?per_page=100'; // alphas add up
const FW_ASSET = { rp2: '-pico2_w-cruller.uf2', esp32: '-esp32s3_devkitc1_n16r8-cruller.bin' }; // each platform's image among a release's assets
const upd = { loaded: false, list: [], onProgress: null };

// "0.3.10" vs "0.3.9": -1, 0, 1. A pre-release comes before its release, and pre-releases compare by N,
// CI's run number, whatever their label: 0.3.3-pr.40 < 0.3.3-alpha.41 < 0.3.3.
function cmpVersion(a, b) {
  const [ca, pa] = a.split('-'), [cb, pb] = b.split('-');
  const x = ca.split('.').map(Number), y = cb.split('.').map(Number);
  for (let i = 0; i < 3; i++) if ((x[i] || 0) !== (y[i] || 0)) return (x[i] || 0) < (y[i] || 0) ? -1 : 1;
  if (!pa || !pb) return pa ? -1 : pb ? 1 : 0;
  const na = Number(pa.split('.').pop()), nb = Number(pb.split('.').pop());
  return na === nb ? 0 : na < nb ? -1 : 1;
}

async function updLoad() {
  upd.loaded = true;
  $('u-refresh').classList.add('spin');
  $('u-refresh').disabled = true;
  text('u-state', 'Checking GitHub…');
  // Alphas (develop builds) are listed only to a board already running one: a release (master) sees releases.
  const onAlpha = S.version.includes('-');
  try {
    const rels = await (await fetch(FW_RELEASES, { cache: 'no-store' })).json();
    const suffix = FW_ASSET[S.platform];
    upd.list = rels.filter((r) => !r.draft && (onAlpha || !r.prerelease)).map((r) => {
      const a = suffix && r.assets.find((x) => x.name.endsWith(suffix));
      return a && { version: r.tag_name.replace(/^v/, ''), alpha: r.prerelease, date: (r.published_at || '').slice(0, 10), notes: r.body || '',
        file: { url: a.browser_download_url, name: a.name, size: a.size, sha256: (a.digest || '').replace(/^sha256:/, '') } };
    }).filter(Boolean);
  } catch (e) {
    text('u-state', 'Could not read the releases from GitHub');
    return;
  } finally {
    $('u-refresh').classList.remove('spin');
    $('u-refresh').disabled = false;
  }
  const sel = $('u-ver');
  sel.innerHTML = '';
  upd.list.forEach((r, i) => {
    const c = cmpVersion(r.version, S.version);
    const o = document.createElement('option');
    o.value = i;
    o.textContent = r.version + (r.date ? ' (' + r.date + ')' : '') + (r.alpha ? ' · alpha' : '') + (c === 0 ? ' · installed' : c < 0 ? ' · older' : '');
    sel.appendChild(o);
  });
  // The newest version above the installed one is picked, or else the installed one.
  const newer = upd.list.findIndex((r) => cmpVersion(r.version, S.version) > 0);
  const installed = upd.list.findIndex((r) => cmpVersion(r.version, S.version) === 0);
  if (newer >= 0 || installed >= 0) sel.value = newer >= 0 ? newer : installed;
  text('u-state', !upd.list.length ? 'No releases for this board yet' : newer >= 0 ? upd.list[newer].version + ' available' : 'Up to date (' + S.version + ')');
  updShow();
}

function updShow() {
  const r = upd.list[+$('u-ver').value];
  text('u-notes', r ? r.notes || '(no notes)' : '');
  // The Pico's boot ROM starts the newer of its two slots: an older version wouldn't boot.
  $('u-go').disabled = !r || cmpVersion(r.version, S.version) <= 0;
  text('u-go', r && cmpVersion(r.version, S.version) <= 0 ? (cmpVersion(r.version, S.version) ? 'Older than installed' : 'Installed') : 'Download and install');
}

async function updInstall() {
  const r = upd.list[+$('u-ver').value], f = r && r.file;
  if (!f || !(await askUser('Install Cruller ' + r.version + '?', 'Cruller downloads it from GitHub, checks it, and restarts into it (about 30 s).', 'Install'))) return;
  // When Cruller can't: the file by hand from GitHub, then "Install from a file…".
  const failed = (title, why) => busy({ mode: 'bad', title, text: why + ' You can download ' + f.name + ' yourself and install it with "Install from a file…".',
    actions: [{ label: 'Download ' + r.version, primary: true, href: f.url }, { label: 'Install from a file…', onclick: () => { busyHide(); $('u-file').click(); } },
      { label: 'Close', onclick: busyHide }] });
  if (!f.sha256) {
    failed('GitHub lists no SHA-256 for it', 'Cruller installs only a download it can check.');
    return;
  }
  const mb = (n) => (n / 1048576).toFixed(2);
  const connecting = () => busy({ mode: 'progress', frac: 0, title: 'Installing ' + r.version, text: 'Cruller is connecting to GitHub…' });
  connecting();
  try {
    const resp = await fetch('/update/fetch?url=' + encodeURIComponent(f.url) + '&sha=' + f.sha256 + '&size=' + f.size, { method: 'POST' });
    if (!resp.ok) {
      failed('Cruller didn\'t start the download', (await resp.text()).trim() + '.');
      return;
    }
  } catch (e) {
    failed('Cruller didn\'t answer', 'The connection to Cruller dropped.');
    return;
  }
  const why = (m) => (m.split(' ')[0].includes('.') ? m : m.charAt(0).toUpperCase() + m.slice(1)) + '.'; // "github.com answered 404" stays
  upd.onProgress = (u) => {
    if (u.from !== 'github') return;
    if (u.failed) {
      upd.onProgress = null;
      failed('The download failed', why(u.failed));
    } else if (u.done || (u.size && u.got === u.size)) {
      upd.onProgress = null;
      updRestart(r.version, (m) => failed('The download failed', why(m)));
    } else if (!u.got) {
      connecting();
    } else {
      busy({ mode: 'progress', frac: u.got / u.size, title: 'Installing ' + r.version,
        text: 'Cruller downloads it from GitHub and writes it: ' + mb(u.got) + ' of ' + mb(u.size) + ' MB' });
    }
  };
}

// Once an image is written, Cruller restarts into it: the page reloads when it's back. A download that
// didn't check out shows up in the status instead (Cruller keeps running): onFailed(why).
async function updRestart(label, onFailed) {
  busy({ mode: 'spin', title: 'Restarting into ' + label + '…', text: 'Cruller checks the new firmware and keeps it once it runs.' });
  await sleep(4000);
  let version = '', failure = '';
  const back = await waitFor(async () => {
    const s = await (await fetch('/status', { cache: 'no-store' })).json();
    version = s.version;
    failure = (onFailed && s.update && s.update.failed) || '';
    return failure || s.uptime_s < 120;
  }, 90000);
  if (failure) {
    onFailed(failure);
  } else if (back) {
    busy({ mode: 'ok', title: 'Cruller ' + version + ' is running', text: 'Reloading the page…' });
    await sleep(1500);
    location.reload();
  } else {
    busy({ mode: 'bad', title: 'Taking longer than expected', text: 'Cruller hasn\'t answered for a minute and a half. If it doesn\'t come back, it returns to the previous firmware by itself.', actions: [{ label: 'Reload', primary: true, onclick: () => location.reload() }] });
  }
}

async function updFile(file) {
  $('u-file').value = '';
  if (!file) return;
  if (!(await askUser('Install ' + file.name + '?', 'Cruller writes it and restarts into it (about 20 s). Only images built for this board work.', 'Install'))) return;
  updSend(new Uint8Array(await file.arrayBuffer()), file.name);
}

// Sends an image to /update; the bar follows what Cruller has received (pushed in the status).
function updSend(data, label) {
  const show = (got) => busy({ mode: 'progress', frac: got / data.length, title: 'Installing ' + label, text: 'Sending it to Cruller: ' + (got / 1048576).toFixed(2) + ' of ' + (data.length / 1048576).toFixed(2) + ' MB' });
  show(0);
  upd.onProgress = (u) => { if (!u.from) show(u.got); };
  const x = new XMLHttpRequest();
  x.open('POST', '/update');
  x.onload = () => {
    upd.onProgress = null;
    if (x.status !== 200) {
      busy({ mode: 'bad', title: 'The update failed', text: x.responseText.trim(), actions: [{ label: 'Close', onclick: busyHide }] });
      return;
    }
    updRestart(label);
  };
  x.onerror = () => {
    upd.onProgress = null;
    busy({ mode: 'bad', title: 'The connection to Cruller dropped', text: 'The update didn\'t finish; Cruller keeps its current firmware.', actions: [{ label: 'Close', onclick: busyHide }] });
  };
  x.send(data);
}

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
  if (o.mode === 'progress') { // a share done: the arc fills, the percentage in the middle
    arc.style.strokeDashoffset = ARC * (1 - Math.min(1, Math.max(0, o.frac || 0)));
    text('busy-num', Math.round(100 * (o.frac || 0)) + '%');
  }
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
const sensorSamples = []; // [average mV, lowest mV, °C × 10], 100 ms apart: the last 2 minutes
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
const OWNERS = ['page', 'power check', 'Cruller', 'HTTP API', 'SD card'];
const owner = (n) => OWNERS[n] || 'RFC 2217 #' + (n - 4);

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

  // The board's supply and temperature (the Pico 2 W; null on a board without them)
  const sn = r.sensors;
  $('p-sens').hidden = !sn;
  if (sn) {
    sensorSamples.push(...sn.samples);
    if (sensorSamples.length > 1200) sensorSamples.splice(0, sensorSamples.length - 1200);
    text('lg-sup', 'supply ' + (sn.supply_mv / 1000).toFixed(2) + ' V');
    text('lg-temp', 'chip ' + (sn.temperature_dc / 10).toFixed(1) + ' °C');
    text('sn-note', 'lowest since start ' + (sn.supply_min_mv / 1000).toFixed(2) + ' V · 5 V on USB: ' +
      (sn.usb_power > 0 ? 'yes' : sn.usb_power === 0 ? 'no' : '?'));
  }

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

// A line chart: series [{data, color, width, right}], y from 0 to a round maximum (or opts.range:
// [low, high]), labels on the left. Series with `right` use a second axis, labelled on the right
// (opts.right: {unit, range}).
function chart(id, series, unit, opts = {}) {
  const c = $(id), r = c.getBoundingClientRect(), d = devicePixelRatio || 1;
  if (!r.width) return;
  c.width = Math.round(r.width * d);
  c.height = Math.round(r.height * d);
  const g = c.getContext('2d');
  g.scale(d, d);
  const W = r.width, H = r.height, L = 40, T = 12, B = 20, R = opts.right ? 44 : 8;
  const axis = (list, range) => {
    if (range) return range;
    const max = Math.max(1, ...list.flatMap((s) => s.data));
    const step = Math.pow(10, Math.floor(Math.log10(max)));
    return [0, Math.ceil(max / step) * step];
  };
  const left = axis(series.filter((s) => !s.right), opts.range);
  const right = opts.right && axis(series.filter((s) => s.right), opts.right.range);
  const label = (v) => (Math.abs(v) >= 10 ? Math.round(v) : v.toFixed(1));
  g.font = '10px ' + getComputedStyle(document.body).getPropertyValue('--mono');
  g.fillStyle = '#6B6F76';
  g.strokeStyle = '#23262B';
  for (let i = 0; i <= 3; i++) {
    const y = T + (H - T - B) * i / 3;
    g.beginPath(); g.moveTo(L, y); g.lineTo(W - R, y); g.stroke();
    g.fillText(label(left[0] + (left[1] - left[0]) * (3 - i) / 3), 6, y + 3);
    if (right) g.fillText(label(right[0] + (right[1] - right[0]) * (3 - i) / 3), W - R + 6, y + 3);
  }
  g.fillText(unit, 6, H - 5);
  if (right) g.fillText(opts.right.unit, W - R + 6, H - 5);
  for (const s of series) {
    if (s.data.length < 2) continue;
    const [lo, hi] = s.right ? right : left;
    g.strokeStyle = s.color;
    g.lineWidth = s.width || 1.5;
    g.lineJoin = 'round';
    g.beginPath();
    s.data.forEach((v, i) => {
      const x = L + (W - L - R) * i / (s.data.length - 1), y = T + (H - T - B) * (1 - (v - lo) / (hi - lo));
      if (i) g.lineTo(x, y); else g.moveTo(x, y);
    });
    g.stroke();
  }
}

// The supply (left, V) and the chip's temperature (right, °C): the last 2 minutes, 10 points a second.
function drawSensors() {
  if ($('p-sens').hidden || sensorSamples.length < 2) return;
  const avgV = sensorSamples.map((s) => s[0] / 1000), lowV = sensorSamples.map((s) => s[1] / 1000);
  const temp = sensorSamples.map((s) => s[2] / 10);
  // 4.0-5.5 V (USB's 5 V less the input diode sits near 4.7-4.9), wider if the supply leaves it.
  const vLo = Math.min(4, Math.floor(Math.min(...lowV) * 2) / 2), vHi = Math.max(5.5, Math.ceil(Math.max(...avgV) * 2) / 2);
  const tLo = Math.floor(Math.min(...temp) / 5) * 5 - 5, tHi = Math.ceil(Math.max(...temp) / 5) * 5 + 5;
  chart('ch-sens', [
    { data: lowV, color: '#2E7D5B', width: 1 },
    { data: avgV, color: '#4CC38A' },
    { data: temp, color: '#E8618C', right: true },
  ], 'V', { range: [vLo, vHi], right: { unit: '°C', range: [tLo, tHi] } });
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
  drawSensors();
}

// --- start ---------------------------------------------------------------------------------------------

route();
conn();
