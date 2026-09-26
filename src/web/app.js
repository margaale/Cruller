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

// --- tabs -------------------------------------------------------------------------------------------

let tab = 'rt4k';

function route() {
  const [t, sub] = (location.hash.slice(1) || (location.hostname === '192.168.4.1' ? 'cruller' : 'rt4k')).split('/');
  tab = ['rt4k', 'cruller', 'debug'].includes(t) ? t : 'rt4k';
  document.querySelectorAll('[data-view]').forEach((e) => { e.hidden = e.dataset.view !== tab; });
  document.querySelectorAll('nav.tabs a').forEach((a) => a.toggleAttribute('aria-current', a.dataset.tab === tab));
  const view = sub === 'firmware' ? 'firmware' : 'live';
  document.querySelectorAll('[data-subview]').forEach((e) => { e.hidden = e.dataset.subview !== view; });
  document.querySelectorAll('.sub a').forEach((a) => a.toggleAttribute('aria-current', a.dataset.sub === view));
  $('chip-rt4k').hidden = tab !== 'rt4k';
  $('chip-wifi').hidden = $('chip-ver').hidden = tab === 'rt4k';
  if (tab === 'rt4k' && view === 'firmware' && window.fwOpen) window.fwOpen();
  if (tab === 'rt4k' && view === 'live') fit();
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
  showUptime();
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
function out(t) { append('rx', t); append('d-rx', t); }

// --- the WebSocket: terminal text, OSD planes, font, log, status (see ws.h) ------------------------------

let ws, font = null;
const planes = [null, null], BG = [[5, 7, 12], [233, 237, 243], [32, 192, 32], [208, 32, 32]];

function send(t) {
  if (ws && ws.readyState === 1) { ws.send(t); return true; }
  return false;
}

// Stats overlay: screen updates per second (menu / messages), key-to-screen time, draw time.
const ST = { n: [0, 0], lat: 0, key: 0, draw: 0 };
setInterval(() => {
  if (!$('stats').hidden) {
    $('stats').textContent = 'menu ' + ST.n[0] + '/s  msgs ' + ST.n[1] + '/s\nkey->screen ' + (ST.lat ? ST.lat + ' ms' : '-') +
      '\ndraw ' + ST.draw.toFixed(1) + ' ms';
  }
  ST.n = [0, 0];
}, 1000);

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
      ST.n[u[1] - 1]++;
      if (u[1] === 1 && ST.key) { ST.lat = Math.round(performance.now() - ST.key); ST.key = 0; keySeen(ST.lat); }
      planes[u[1] - 1] = d.length ? { r: new TextDecoder().decode(u.subarray(3, 3 + n)), d: d.slice() } : null;
      draw();
    }
  };
}

// Tell Cruller whether this page is on screen: it stops polling the RT4K's menu for background tabs.
let told = null; // [socket, visible] last sent
function tellVisibility() {
  if (!ws || ws.readyState !== 1) return;
  const visible = document.visibilityState === 'visible';
  if (told && told[0] === ws && told[1] === visible) return;
  ws.send(new Uint8Array([0x10, visible ? 1 : 0]));
  told = [ws, visible];
}
document.addEventListener('visibilitychange', tellVisibility);
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

function draw() {
  const t0 = performance.now();
  draw1();
  ST.draw = performance.now() - t0;
}

function draw1() {
  const t = $('tv'), g = t.getContext('2d'), W = t.width, H = t.height;
  g.fillStyle = '#000';
  g.fillRect(0, 0, W, H);
  if (!font) return;
  const mk = planes[0] ? kv(planes[0].r) : {}, ph = (mk.rows || 32) * 16, sc = H * (2048 / 2160) / ph;
  if (planes[0]) {
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
  b.onclick = () => {
    if (b.dataset.confirm && !confirm(b.dataset.confirm)) return;
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

$('tv').ondblclick = () => $('tv').requestFullscreen();
addEventListener('resize', () => { fit(); if (tab === 'debug') drawCharts(); });
document.addEventListener('fullscreenchange', () => setTimeout(fit, 50));

// --- Cruller firmware upload ---------------------------------------------------------------------------

function upd() {
  const f = $('uf2').files[0], m = $('um'), p = $('pg');
  if (!f) { m.textContent = 'Choose a .uf2 file'; return; }
  const x = new XMLHttpRequest();
  x.open('POST', '/update');
  p.hidden = false;
  x.upload.onprogress = (e) => { if (e.lengthComputable) { p.max = e.total; p.value = e.loaded; } };
  x.onload = () => { m.textContent = x.responseText; };
  x.onerror = () => { m.textContent = 'Upload failed'; };
  x.send(f);
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
  const showing = tab === 'debug';
  if (toldDebug && toldDebug[0] === ws && toldDebug[1] === showing) return;
  ws.send(new Uint8Array([0x11, showing ? 1 : 0]));
  toldDebug = [ws, showing];
}

function onDebug(kind, t) {
  if (kind === 1) {
    const s = JSON.parse(t);
    samples.push({ t: Date.now(), tx: s.rt4k_tx, rx: s.rt4k_rx });
    if (samples.length > 61) samples.shift();
    st(s);
  } else if (kind === 2) {
    text('d-tasks', t);
  } else if (kind === 3) {
    text('d-console', t);
    replyTimes.length = 0;
    for (const m of t.matchAll(/reply\s+(\d+) ms/g)) replyTimes.push(+m[1]);
  } else if (kind === 4) {
    text('d-memory', t);
    drawCharts(); // the last of each round
  }
}

async function getText(path) {
  const r = await fetch(path, { cache: 'no-store' });
  return r.text();
}

// The task list: on request only (scanning the stacks pauses both cores for a few milliseconds).
async function stacks() {
  text('d-stacks', '…');
  try {
    const t = await getText('/debug/tasks?stacks');
    text('d-stacks', t.split('\n').filter((l) => /stack free/.test(l)).join('\n'));
  } catch (e) {
    text('d-stacks', 'Could not read them: ' + e.message);
  }
}

async function freeze() {
  freeze.done = true;
  try { text('d-freeze', await getText('/debug/freeze')); } catch (e) { text('d-freeze', 'Could not read it: ' + e.message); }
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
  const data = { status: S, console: $('d-console').textContent, tasks: $('d-tasks').textContent, memory: $('d-memory').textContent, keyTimes };
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
