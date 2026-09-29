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

  const sha256 = window.sha256; // sha256.js, loaded first

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
      if (!(await window.askUser('Install ' + version + ' now?', 'The RT4K restarts and flashes for about 40 seconds (LED pink, then blue). Don\'t power it off.', 'Install'))) {
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
