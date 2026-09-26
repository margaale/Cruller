// Page additions kept as a real file (served as /ui.js, embedded at build time): the RT4K's power
// state from the status Cruller pushes (rt4k_power: on / standby / starting / unknown).

(() => {
  'use strict';

  const q = (id) => document.getElementById(id);
  const tv = q('tv');
  const pwr = document.querySelector('.remote .pwr');

  // A veil over the screen while there's nothing to mirror.
  const veil = document.createElement('div');
  veil.style.cssText = 'position:absolute;display:none;align-items:center;justify-content:center;' +
    'color:#9a9a9a;font:600 1.3em system-ui,sans-serif;background:#000d;border-radius:4px;pointer-events:none';
  tv.parentNode.appendChild(veil);

  function place() {
    veil.style.left = tv.offsetLeft + 'px';
    veil.style.top = tv.offsetTop + 'px';
    veil.style.width = tv.offsetWidth + 'px';
    veil.style.height = tv.offsetHeight + 'px';
  }
  addEventListener('resize', place);

  function show(power) {
    const link = q('link');
    if (link && link.textContent.startsWith('connected')) {
      const what = { standby: 'RT4K in standby', starting: 'RT4K starting', unknown: 'RT4K not answering' }[power];
      link.textContent = 'connected' + (what ? ', ' + what : '');
    }
    veil.textContent = power === 'standby' ? 'RT4K in standby' : power === 'starting' ? 'RT4K starting…' : '';
    veil.style.display = veil.textContent ? 'flex' : 'none';
    place();
    if (pwr) {
      // The page's button handler reads these at click time.
      if (power === 'standby') {
        pwr.dataset.c = 'pwr on';
        delete pwr.dataset.confirm;
        pwr.title = 'Turn the RT4K on';
      } else {
        pwr.dataset.c = 'remote pwr';
        pwr.dataset.confirm = 'Turn the RT4K off?';
        pwr.title = 'Turn the RT4K off';
      }
    }
  }

  const st = window.st;
  window.st = (s) => {
    st(s);
    if (s && s.rt4k_power) show(s.rt4k_power);
  };

  // Tell Cruller whether this page is on screen: it stops polling the RT4K's menu for background tabs.
  let told = null; // [socket, visible] last sent
  function tellVisibility() {
    const sock = typeof ws !== 'undefined' ? ws : null; // the page's WebSocket (see conn())
    if (!sock || sock.readyState !== 1) return;
    const visible = document.visibilityState === 'visible';
    if (told && told[0] === sock && told[1] === visible) return;
    sock.send(new Uint8Array([0x10, visible ? 1 : 0]));
    told = [sock, visible];
  }
  document.addEventListener('visibilitychange', tellVisibility);
  setInterval(tellVisibility, 1000); // a reconnected socket starts out counted as visible
})();
