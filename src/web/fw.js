// RT4K firmware updater for the Cruller page (served as /fw.js, embedded at build time).
//
// The browser does the heavy lifting: it reads RetroTINK's firmware index on GitHub, downloads the
// chosen zip, checks it against the SHA-256 in the index, unzips it and hashes every file. Cruller
// then writes each file to the RT4K's SD card (POST /rt4k/put, RTL1 put) and runs the RT4K's own
// installer (fwup check / fwup go) once the user confirms.

(() => {
  'use strict';

  const RAW = 'https://raw.githubusercontent.com/RetroTINK-LLC/firmware/main/';
  const CHANNELS = [['Experimental', '4k-experimental.md'], ['Release', '4k.md']];
  const RBF_PREFIXES = ['rt4k_', 'rt4kce_', 'rt6x_']; // one .rbf per model in each zip

  const q = (id) => document.getElementById(id);
  let installed = null; // {version, model}
  let busy = false;
  const channels = {}; // name -> [{version, date, url, sha, changelog}]

  // --- SHA-256 (crypto.subtle needs a secure context; the page is plain http) ---------------------

  const K = new Uint32Array([
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
  ]);

  function sha256(bytes) {
    const h = new Uint32Array([0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19]);
    const w = new Uint32Array(64);
    const n = bytes.length;
    const total = ((n + 9 + 63) >> 6) << 6; // message + 0x80 + 64-bit length, padded to 64
    const tail = new Uint8Array(total - (n & ~63));
    tail.set(bytes.subarray(n & ~63));
    tail[n & 63] = 0x80;
    const bits = n * 8;
    const tv = new DataView(tail.buffer);
    tv.setUint32(tail.length - 8, Math.floor(bits / 0x100000000));
    tv.setUint32(tail.length - 4, bits >>> 0);
    const body = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    for (let off = 0; off < total; off += 64) {
      const inBody = off + 64 <= (n & ~63);
      for (let i = 0; i < 16; i++) {
        w[i] = inBody ? body.getUint32(off + i * 4) : tv.getUint32(off - (n & ~63) + i * 4);
      }
      for (let i = 16; i < 64; i++) {
        const a = w[i - 15], b = w[i - 2];
        const s0 = ((a >>> 7) | (a << 25)) ^ ((a >>> 18) | (a << 14)) ^ (a >>> 3);
        const s1 = ((b >>> 17) | (b << 15)) ^ ((b >>> 19) | (b << 13)) ^ (b >>> 10);
        w[i] = (w[i - 16] + s0 + w[i - 7] + s1) | 0;
      }
      let [a, b, c, d, e, f, g, hh] = h;
      for (let i = 0; i < 64; i++) {
        const S1 = ((e >>> 6) | (e << 26)) ^ ((e >>> 11) | (e << 21)) ^ ((e >>> 25) | (e << 7));
        const t1 = (hh + S1 + ((e & f) ^ (~e & g)) + K[i] + w[i]) | 0;
        const S0 = ((a >>> 2) | (a << 30)) ^ ((a >>> 13) | (a << 19)) ^ ((a >>> 22) | (a << 10));
        const t2 = (S0 + ((a & b) ^ (a & c) ^ (b & c))) | 0;
        hh = g; g = f; f = e; e = (d + t1) | 0; d = c; c = b; b = a; a = (t1 + t2) | 0;
      }
      h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    return Array.from(h, (x) => x.toString(16).padStart(8, '0')).join('');
  }

  // --- the firmware index (RetroTINK's markdown pages) ----------------------------------------------

  function parseIndex(md) {
    const out = [];
    const re = /^## Version (\S+) \((\d{4}-\d\d-\d\d)\)\s*$/gm;
    const heads = [...md.matchAll(re)];
    heads.forEach((m, i) => {
      const section = md.slice(m.index, i + 1 < heads.length ? heads[i + 1].index : md.length);
      const url = (section.match(/\((https:[^)\s]+\.zip)\)/) || [])[1];
      const sha = (section.match(/SHA-256:\s*`([0-9a-fA-F]{64})`/) || [])[1];
      if (!url || !sha) return;
      const log = section.split(/### Changelog:?/)[1] || '';
      const changelog = log.split(/<br\/?>|\n## /)[0].trim();
      out.push({ version: m[1], date: m[2], url, sha: sha.toLowerCase(), changelog });
    });
    return out;
  }

  function rawUrl(url) {
    // cdn.jsdelivr.net/gh/retrotink-llc/firmware@main/<path> -> raw.githubusercontent.com/.../main/<path>
    const m = url.match(/firmware@main\/(.+)$/i);
    return m ? RAW + m[1] : url;
  }

  // --- zip ------------------------------------------------------------------------------------------

  function zipEntries(buf) {
    const v = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
    let eocd = -1;
    for (let i = buf.length - 22; i >= Math.max(0, buf.length - 65557); i--) {
      if (v.getUint32(i, true) === 0x06054b50) { eocd = i; break; }
    }
    if (eocd < 0) throw new Error('not a zip file');
    const count = v.getUint16(eocd + 10, true);
    let p = v.getUint32(eocd + 16, true);
    const dec = new TextDecoder();
    const entries = [];
    for (let i = 0; i < count; i++) {
      if (v.getUint32(p, true) !== 0x02014b50) throw new Error('damaged zip directory');
      const method = v.getUint16(p + 10, true);
      const csize = v.getUint32(p + 20, true);
      const size = v.getUint32(p + 24, true);
      const nlen = v.getUint16(p + 28, true), xlen = v.getUint16(p + 30, true), clen = v.getUint16(p + 32, true);
      const local = v.getUint32(p + 42, true);
      const name = dec.decode(buf.subarray(p + 46, p + 46 + nlen));
      p += 46 + nlen + xlen + clen;
      if (name.endsWith('/')) continue; // folders: created from the file paths
      const data = local + 30 + v.getUint16(local + 26, true) + v.getUint16(local + 28, true);
      entries.push({ name, method, size, raw: buf.subarray(data, data + csize) });
    }
    return entries;
  }

  async function unzipEntry(e) {
    if (e.method === 0) return e.raw;
    if (e.method !== 8) throw new Error(e.name + ': unsupported compression ' + e.method);
    const stream = new Blob([e.raw]).stream().pipeThrough(new DecompressionStream('deflate-raw'));
    const out = new Uint8Array(await new Response(stream).arrayBuffer());
    if (out.length !== e.size) throw new Error(e.name + ': unzipped to ' + out.length + ' bytes, expected ' + e.size);
    return out;
  }

  // Everything in the zip, like extracting it to the SD card, except the other models' .rbf files.
  function wanted(name, model) {
    const base = name.split('/').pop().toLowerCase();
    const prefix = RBF_PREFIXES.find((p) => base.startsWith(p) && base.endsWith('.rbf'));
    if (!prefix) return true;
    const mine = /ce/i.test(model) ? 'rt4kce_' : 'rt4k_';
    return prefix === mine;
  }

  // --- Cruller ----------------------------------------------------------------------------------------

  async function ask(cmd, expect, timeout) {
    const r = await fetch('/rt4k/ask?expect=' + encodeURIComponent(expect) + (timeout ? '&timeout=' + timeout : ''),
      { method: 'POST', body: cmd });
    const t = (await r.text()).trim();
    if (!r.ok) throw new Error(cmd + ': ' + t);
    return t;
  }

  function putFile(path, data, sha) {
    return new Promise((resolve, reject) => {
      const x = new XMLHttpRequest();
      x.open('POST', '/rt4k/put?path=' + encodeURIComponent(path) + '&sha=' + sha);
      x.onload = () => (x.status === 200 ? resolve(x.responseText.trim()) : reject(new Error(path + ': ' + x.responseText.trim())));
      x.onerror = () => reject(new Error(path + ': connection to Cruller lost'));
      x.send(data);
    });
  }

  async function download(url, onProgress) {
    let r;
    try {
      r = await fetch(url);
      if (!r.ok) throw new Error('HTTP ' + r.status);
    } catch (e) {
      r = await fetch(rawUrl(url)); // jsDelivr failing: straight from GitHub
      if (!r.ok) throw new Error('download failed: HTTP ' + r.status);
    }
    const total = +r.headers.get('Content-Length') || 0;
    const reader = r.body.getReader();
    const parts = [];
    let got = 0;
    for (;;) {
      const { done, value } = await reader.read();
      if (done) break;
      parts.push(value);
      got += value.length;
      onProgress(got, total);
    }
    const buf = new Uint8Array(got);
    let o = 0;
    for (const p of parts) { buf.set(p, o); o += p.length; }
    return buf;
  }

  // --- UI -------------------------------------------------------------------------------------------

  const mb = (n) => (n / 1048576).toFixed(1) + ' MB';

  function status(text) {
    q('fws').textContent = text;
  }

  // The install steps (download, check, write, install), each waiting / running / done / failed, with
  // a detail line and, while it runs, a bar. max 0: size unknown (an indeterminate bar).
  const STEPS = [['dl', 'Download the zip'], ['chk', 'Check it against the SHA-256 in RetroTINK\'s index'],
    ['wr', 'Write the files to the RT4K\'s SD card'], ['in', 'Install: the RT4K restarts for about 40 s']];
  let current = null;

  function step(id, state, detail, value, max) {
    const li = q('fws-' + id);
    li.dataset.state = state;
    li.querySelector('.sd').textContent = detail || '';
    const p = li.querySelector('progress');
    p.hidden = state !== 'run' || max === undefined;
    if (!p.hidden) {
      if (max) { p.max = max; p.value = value; } else p.removeAttribute('value');
    }
    current = state === 'run' ? id : current;
  }

  // The page's status handler passes each status's "put" (bytes Cruller handed to the RT4K): the
  // browser's own upload progress only counts what it buffered, all of it within a moment.
  let onPut = null;
  window.fwPutProgress = (put) => { if (onPut && put) onPut(put); };

  function selected() {
    const list = channels[q('fwc').value] || [];
    return list[+q('fwv').value];
  }

  function showVersions() {
    const list = channels[q('fwc').value] || [];
    q('fwv').innerHTML = '';
    list.forEach((f, i) => {
      const o = document.createElement('option');
      o.value = i;
      o.textContent = f.version + ' (' + f.date + ')' + (installed && installed.version === f.version ? ' - installed' : '');
      q('fwv').appendChild(o);
    });
    showChangelog();
  }

  function showChangelog() {
    const f = selected();
    q('fwl').textContent = f ? f.changelog : '';
    q('fwi').disabled = busy || !f || !installed;
  }

  async function install() {
    const f = selected();
    if (!f || busy) return;
    busy = true;
    q('fwi').disabled = true;
    q('fwsteps').hidden = false;
    STEPS.forEach(([id]) => step(id, 'wait'));
    status('');
    try {
      step('dl', 'run', 'starting…', 0, 0);
      const zip = await download(f.url, (got, total) => step('dl', 'run', mb(got) + (total ? ' of ' + mb(total) : ''), got, total));
      step('dl', 'done', mb(zip.length));
      step('chk', 'run', 'hashing…');
      await new Promise((r) => setTimeout(r, 20));
      const zsha = sha256(zip);
      if (zsha !== f.sha) throw new Error('the download does not match the SHA-256 in RetroTINK\'s index');
      step('chk', 'done', zsha.slice(0, 12) + '… matches');
      const entries = zipEntries(zip).filter((e) => wanted(e.name, installed.model));
      if (!entries.some((e) => e.name.toLowerCase() === 'rt4kup.bin')) throw new Error('no rt4kup.bin in the zip');
      // rt4kup.bin last: it names the .rbf to install, so it must not arrive before the .rbf does.
      entries.sort((a, b) => (a.name.toLowerCase() === 'rt4kup.bin') - (b.name.toLowerCase() === 'rt4kup.bin'));
      const total = entries.reduce((s, e) => s + e.size, 0);
      const dirs = new Set();
      for (const e of entries) {
        const parts = e.name.split('/');
        for (let i = 1; i < parts.length; i++) dirs.add(parts.slice(0, i).join('/'));
      }
      step('wr', 'run', 'preparing…', 0, total);
      for (const d of [...dirs].sort((a, b) => a.length - b.length)) {
        step('wr', 'run', 'creating ' + d + '/', 0, total);
        const r = await ask('mkdir ' + d, 'mkdir');
        if (!/ok|EXIST/.test(r)) throw new Error('mkdir ' + d + ': ' + r);
      }
      let done = 0;
      const t0 = Date.now();
      for (const [i, e] of entries.entries()) {
        const data = await unzipEntry(e);
        const sha = sha256(data);
        const show = (sent) => {
          const now = done + Math.min(sent, e.size);
          const rate = now / Math.max(1, (Date.now() - t0) / 1000);
          step('wr', 'run', e.name + ' (' + (i + 1) + ' of ' + entries.length + ') · ' + mb(now) + ' of ' + mb(total) +
            (rate > 1024 ? ' · ' + Math.round(rate / 1024) + ' KB/s' : ''), now, total);
        };
        show(0);
        onPut = (put) => { if (put.path === e.name) show(put.sent); };
        await putFile(e.name, data, sha);
        onPut = null;
        done += e.size;
      }
      step('wr', 'done', entries.length + ' files · ' + mb(total) + ' in ' + Math.round((Date.now() - t0) / 1000) + ' s');
      step('in', 'run', 'the RT4K checks the files…');
      const check = await ask('fwup check', 'fwup', 15000);
      const version = (check.match(/version=(\S+)/) || [])[1];
      const token = (check.match(/token=([0-9A-Fa-f]+)/) || [])[1];
      if (!check.startsWith('fwup ok') || !token) throw new Error('the RT4K refused the update: ' + check);
      step('in', 'run', 'ready to install ' + version);
      if (!confirm('Install RT4K firmware ' + version + ' now?\n\nThe RT4K restarts and flashes for about 40 seconds (LED pink, then blue). Do not power it off.')) {
        step('in', 'wait', 'not installed');
        status('The files are on the SD card; the RT4K menu (OSD/Firmware > Check SD Card) can install them later.');
        return;
      }
      const go = await ask('fwup go ' + token, 'fwup', 10000);
      if (!/flashing/.test(go)) throw new Error('the RT4K did not start the install: ' + go);
      step('in', 'done', 'flashing ' + version + ': the RT4K restarts by itself (about 40 s)');
    } catch (e) {
      onPut = null;
      if (current) step(current, 'fail', e.message);
      else status('Failed: ' + e.message);
    } finally {
      busy = false;
      current = null;
      showChangelog();
    }
  }

  async function open() {
    // The page shows and hides the box (its Firmware view); this fills it the first time.
    const box = q('fw');
    if (box.dataset.ready) return;
    box.dataset.ready = 1;
    box.innerHTML =
      '<div class=panel style="max-width:880px">' +
      '<div><div style="font:700 26px var(--head)">RetroTINK firmware</div>' +
      '<div id=fwh class=small>Asking the RT4K for its version...</div></div>' +
      '<div class=row style="flex-wrap:wrap"><select id=fwc style="flex:0 0 160px"></select><select id=fwv style="flex:1 1 240px"></select>' +
      '<button id=fwi class=primary disabled>Download and install</button></div>' +
      '<h2>What\'s new</h2><pre id=fwl style="height:220px"></pre>' +
      '<ol id=fwsteps class=steps hidden>' + STEPS.map(([id, label]) => '<li id=fws-' + id + ' data-state=wait><span class=si></span>' +
        '<div class=sb><div class=sr><span>' + label + '</span><span class="sd small"></span></div><progress hidden></progress></div></li>').join('') +
      '</ol><div id=fws class=small></div>' +
      '<div class=small>From RetroTINK\'s firmware repository, checked against its SHA-256. Keep this page open, and don\'t power the RT4K off while it installs.</div></div>';
    q('fwc').onchange = showVersions;
    q('fwv').onchange = showChangelog;
    q('fwi').onclick = install;
    try {
      const ver = await ask('ver', 'FW Version');
      const model = await ask('model', 'model=');
      installed = { version: (ver.match(/FW Version:\s*(\S+)/) || [])[1], model: model.split(' ').slice(1).join(' ') };
      q('fwh').textContent = 'Installed: ' + installed.version + ' on ' + installed.model.replace(/_/g, ' ');
    } catch (e) {
      q('fwh').textContent = 'The RT4K did not answer (' + e.message + '). Is it on?';
    }
    status('Reading RetroTINK\'s firmware index...');
    for (const [name, file] of CHANNELS) {
      try {
        const r = await fetch(RAW + file);
        channels[name] = parseIndex(await r.text());
        const o = document.createElement('option');
        o.value = name;
        o.textContent = name;
        q('fwc').appendChild(o);
      } catch (e) {
        status('Could not read ' + file + ': ' + e.message);
      }
    }
    status('');
    showVersions();
  }

  window.fwOpen = open;
  window.fwInternals = { sha256, parseIndex, rawUrl, zipEntries, unzipEntry, wanted }; // for tests/test_fw.js
})();
