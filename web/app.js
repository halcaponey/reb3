/* app.js -- Reb3: the web shell around the reverse-engineered Burnout 3
 * WebAssembly engine.
 *
 * Responsibilities, in order:
 *   1. a landing page whose PLAY click is the one user gesture we get, and
 *      which spends it on fullscreen + audio unlock + the disc-image flow;
 *   2. acquiring the user's own XISO -- File System Access where it exists
 *      (with the handle persisted in IndexedDB so a return visit is one
 *      click), plain <input type=file> and drag-and-drop everywhere else;
 *   3. verifying that image against the known-good retail measurements,
 *      staged so the fast checks gate the slow one, and WARNING rather than
 *      blocking on any mismatch;
 *   4. handing the File to the engine and turning its stdout back into a
 *      progress readout.
 *
 * Nothing here uploads anything.  The only fetches are this page's own files
 * and the engine bundle.
 */
'use strict';

/* ------------------------------------------------------------- the oracle --
 * Measured from the known-good image; `sha256sum` and `dd` agree with every
 * one of these.  They are the reference the page compares against. */
const REF = {
  label: 'Burnout 3: Takedown (USA) -- retail XISO',
  size: 2476867584,
  magic: 'MICROSOFT*XBOX*MEDIA',
  magicOffset: 0x10000,                 /* sector 32 of the XISO */
  sectorBytes: 2048,
  sector32Sha: '9c99e98e4f57b4e3731768736b478ec2f491d651e0b3ca596af7e5b4a3faa295',
  firstMiBSha: 'b3fa75e19bce9e7410c0846b22e8ec0b656ef8007a34c4c8542e1b4b947231c7',

  /* The executable, not the whole disc.  Hashing 2.4 GB to answer "is this
   * the right game" is the wrong trade: default.xbe is 4.3 MB, it is the
   * thing the engine actually reverse-engineers, and it pins the region and
   * the revision.  What this deliberately does NOT catch is damage in the
   * bulk data -- see THE TRADE in the mismatch copy. */
  xbeName:  'default.xbe',
  xbeSize:  4308992,
  xbeSha:   '9f497acd82adb8edfb72cc31aeeb35010b45d32b13a3f1fc6540717ff24d13f9'
};

const MiB = 1048576;

/* Sanity caps for the on-disc structures, so a garbage or hostile image makes
 * the check fail instead of making the tab allocate a gigabyte. */
const MAX_ROOT_DIR = 4 * MiB;
const MAX_XBE      = 64 * MiB;

/* Where the sibling's Emscripten output lands.  Tried in order so the page
 * works both served from the repo root (/web/index.html) and served with
 * web/ as the document root and build/web/ mounted beside it.  Resolved and
 * de-duplicated at probe time, because from /web/ the first two collapse to
 * the same URL and one 404 in the console is better than two. */
const ENGINE_CANDIDATES = [
  '../build/web/burnout3.js',
  '/build/web/burnout3.js',
  'build/web/burnout3.js'
];
const ENGINE_ARGS = ['--iso=/iso/game.xiso'];

/* ------------------------------------------------- the engine's environment
 *
 * Every diagnostic this port has is an environment variable -- B3_WEB_PROF,
 * B3_WEB_HWPROF, B3_WEB_GLCOUNT, B3_TRACK, B3_RES -- and until now the SHELL
 * had no way to set one.  The headless harness could (it injects window.B3_ENV
 * into web/smoke.html), so every knob in the port was reachable from a test
 * and unreachable from a browser, which is exactly backwards: the person who
 * needs to run B3_WEB_HWPROF is the person watching the frame rate.
 *
 * So the query string is read for them.  Anything B3_-prefixed becomes an
 * engine environment variable:
 *
 *     .../web/?B3_WEB_HWPROF=60&B3_TRACK=US_C3_V1
 *
 * ONLY B3_* NAMES, and the name is checked against a strict pattern rather
 * than passed through: this writes into the engine's ENV, and a page's query
 * string is attacker-controlled in any link somebody can be sent.  Restricting
 * it to /^B3_[A-Z0-9_]+$/ means the worst a crafted link can do is turn on a
 * diagnostic the build already ships. */
function envFromQuery() {
  const out = {};
  let n = 0;
  try {
    for (const [k, v] of new URLSearchParams(location.search)) {
      if (!/^B3_[A-Z0-9_]+$/.test(k)) continue;
      if (v.length > 128) continue;
      out[k] = v;
      n++;
    }
  } catch (e) { /* no URLSearchParams, or a malformed query: no env */ }
  return { env: out, n: n };
}

/* ------------------------------------------------------------------ state */

const S = {
  phase: 'landing',
  file: null,
  handle: null,
  checks: {
    size:      { st: 'idle', want: REF.size, got: null },
    magic:     { st: 'idle', want: REF.magic, got: null },
    sector32:  { st: 'idle', want: REF.sector32Sha, got: null },
    first1mib: { st: 'idle', want: REF.firstMiBSha, got: null },
    xbe:       { st: 'idle', want: REF.xbeSha, got: null,
                 size: null, sector: null, entries: null }
  },
  quickOk: null,
  xbeOk: null,
  cached: false,
  ms: 0,
  hasherOk: null,
  warnings: [],
  engine: { probed: false, url: null, found: null, error: null, booted: false },
  pad: undefined,          /* undefined, not null: null is "checked, none" */
  logLines: 0
};
window.__b3 = { S, REF, version: 1 };

const $ = (id) => document.getElementById(id);
const el = {};
['landing', 'play', 'fire', 'stage', 'canvas', 'frame', 'overlay', 'hud-status', 'hud-pad',
 'theme-btn', 'scale-btn', 'fs-btn', 'quit-btn', 'panel-pick', 'panel-verify',
 'panel-load', 'drop', 'file-input', 'choose-btn', 'browse-btn', 'pick-error',
 'pick-hint', 'pad-line', 'pad-mark', 'pad-text',
 'resume-row', 'resume-name', 'resume-meta', 'resume-btn',
 'forget-btn', 'v-name', 'v-meta', 'checks', 'verify-warn', 'verify-ok',
 'verify-head', 'start-btn', 'reverify-btn', 'change-btn', 'load-status',
 'load-stage', 'load-head', 'load-note', 'phases',
 'load-back-btn', 'drawer', 'drawer-hd', 'log', 'log-wrap', 'log-count',
 'log-copy', 'log-clear', 'b-coi', 'b-sab', 'b-gl', 'b-fsa', 'b-pad'
].forEach((k) => { el[k] = $(k); });

/* ------------------------------------------------------------------ utils */

const fmtBytes = (n) => n.toLocaleString('en-US') + ' B';
const fmtMB = (n) => (n / MiB).toFixed(1) + ' MiB';
const shortHash = (h) => (h ? h.slice(0, 12) + '…' + h.slice(-8) : '—');

function hex(buf) {
  const b = new Uint8Array(buf);
  let s = '';
  for (let i = 0; i < b.length; i++) s += (b[i] < 16 ? '0' : '') + b[i].toString(16);
  return s;
}

/* crypto.subtle only exists in a secure context; over plain http on a LAN
 * address it is gone, and the vendored hash covers that case.  Everything the
 * page hashes now fits in one buffer -- the biggest is default.xbe at 4.3 MB
 * -- so one-shot digests are all that is needed. */
function hashBackend() {
  return (self.crypto && self.crypto.subtle && self.isSecureContext)
    ? 'WebCrypto' : 'vendored';
}

async function sha256(bytes) {
  if (hashBackend() === 'WebCrypto') {
    return hex(await crypto.subtle.digest('SHA-256', bytes));
  }
  await loadScriptOnce('vendor/sha256.js');
  return self.Sha256.hex(new Uint8Array(bytes));
}

/* FIPS 180-4 known answers, run through whichever backend we actually picked.
 * If this fails, a "mismatch" below would be OUR bug, not the user's disc --
 * and the page says so rather than accusing their image. */
const HASH_VECTORS = [
  ['', 'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855'],
  ['abc', 'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad'],
  ['abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq',
   '248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1']
];

async function selfTestHasher() {
  const enc = new TextEncoder();
  const bad = [];
  try {
    for (const [msg, want] of HASH_VECTORS) {
      const got = await sha256(enc.encode(msg));
      if (got !== want) bad.push('"' + msg.slice(0, 10) + '" -> ' + got);
    }
    /* a million 'a': multi-block, and the only vector that exercises a
     * message length past the 32-bit boundary in the vendored path. */
    const got = await sha256(enc.encode('a'.repeat(1000000)));
    if (got !== 'cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0') {
      bad.push('1e6*a -> ' + got);
    }
  } catch (e) {
    bad.push('threw: ' + e.message);
  }
  S.hasherOk = bad.length === 0;
  shellLog('SHA-256 self-test (' + hashBackend() + '): ' +
           (S.hasherOk ? 'pass' : 'FAIL — ' + bad.join('; ')));
  return S.hasherOk;
}

const _scripts = new Map();
function loadScriptOnce(src) {
  if (_scripts.has(src)) return _scripts.get(src);
  const p = new Promise((res, rej) => {
    const s = document.createElement('script');
    s.src = src;
    s.onload = () => res(src);
    s.onerror = () => rej(new Error('cannot load ' + src));
    document.head.appendChild(s);
  });
  _scripts.set(src, p);
  return p;
}

function latin1(bytes) {
  let s = '';
  for (let i = 0; i < bytes.length; i++) {
    const c = bytes[i];
    s += (c >= 0x20 && c < 0x7f) ? String.fromCharCode(c) : '.';
  }
  return s;
}

/* ------------------------------------------------------------------- log */

const LOG_MAX = 4000;
const logBuf = [];
let logPending = [];
let logFlush = 0;

function log(text, cls) {
  const line = { t: text, c: cls || '' };
  logBuf.push(line);
  if (logBuf.length > LOG_MAX) logBuf.splice(0, logBuf.length - LOG_MAX);
  logPending.push(line);
  S.logLines = logBuf.length;
  if (!logFlush) logFlush = requestAnimationFrame(flushLog);
}

function flushLog() {
  logFlush = 0;
  if (!logPending.length) return;
  const atBottom = el['log-wrap'].scrollTop + el['log-wrap'].clientHeight >=
                   el['log-wrap'].scrollHeight - 24;
  const frag = document.createDocumentFragment();
  for (const l of logPending) {
    const span = document.createElement('span');
    if (l.c) span.className = l.c;
    span.textContent = l.t + '\n';
    frag.appendChild(span);
  }
  logPending = [];
  el.log.appendChild(frag);
  while (el.log.childNodes.length > LOG_MAX) el.log.removeChild(el.log.firstChild);
  el['log-count'].textContent = logBuf.length + ' line' + (logBuf.length === 1 ? '' : 's');
  if (atBottom) el['log-wrap'].scrollTop = el['log-wrap'].scrollHeight;
}

function shellLog(msg) { log('[shell] ' + msg, 'i'); }

/* Runtime failures land in the drawer, never in the browser console -- the
 * console is where the ENGINE's problems should be visible, not ours. */
window.addEventListener('error', (e) => {
  log('[shell] uncaught: ' + (e.message || e.error) + ' @ ' +
      (e.filename || '?') + ':' + (e.lineno || 0), 'e');
});
window.addEventListener('unhandledrejection', (e) => {
  log('[shell] unhandled rejection: ' + (e.reason && e.reason.message || e.reason), 'e');
});

/* ------------------------------------------------------------------- IDB */

const DB_NAME = 'b3-webshell';
let dbp = null;

function db() {
  if (dbp) return dbp;
  dbp = new Promise((res, rej) => {
    let req;
    try { req = indexedDB.open(DB_NAME, 1); }
    catch (e) { rej(e); return; }
    req.onupgradeneeded = () => {
      const d = req.result;
      if (!d.objectStoreNames.contains('kv')) d.createObjectStore('kv');
      if (!d.objectStoreNames.contains('verdicts')) d.createObjectStore('verdicts');
    };
    req.onsuccess = () => res(req.result);
    req.onerror = () => rej(req.error);
  }).catch((e) => { shellLog('IndexedDB unavailable: ' + e.message); return null; });
  return dbp;
}

async function idbGet(store, key) {
  const d = await db(); if (!d) return undefined;
  return new Promise((res) => {
    try {
      const r = d.transaction(store, 'readonly').objectStore(store).get(key);
      r.onsuccess = () => res(r.result);
      r.onerror = () => res(undefined);
    } catch (e) { res(undefined); }
  });
}

async function idbPut(store, key, val) {
  const d = await db(); if (!d) return false;
  return new Promise((res) => {
    try {
      const tx = d.transaction(store, 'readwrite');
      tx.objectStore(store).put(val, key);
      tx.oncomplete = () => res(true);
      tx.onerror = () => res(false);
      tx.onabort = () => res(false);
    } catch (e) { res(false); }
  });
}

async function idbDel(store, key) {
  const d = await db(); if (!d) return;
  try {
    const tx = d.transaction(store, 'readwrite');
    tx.objectStore(store).delete(key);
  } catch (e) { /* nothing to do */ }
}

const verdictKey = (f) => f.name + '|' + f.size + '|' + f.lastModified;

/* ---------------------------------------------------------------- theme */

function applyTheme(mode) {
  if (mode === 'system') document.documentElement.removeAttribute('data-theme');
  else document.documentElement.setAttribute('data-theme', mode);
  /* Plain words rather than a moon/sun glyph: the page ships no webfont, and
   * half the circle-glyph block is missing from a bare Linux font stack. */
  el['theme-btn'].textContent = mode === 'system' ? 'auto' : mode;
  el['theme-btn'].title = 'Theme: ' + mode + ' (click to cycle)';
}
let themeMode = 'system';
try { themeMode = localStorage.getItem('b3-theme') || 'system'; } catch (e) { /* private mode */ }
applyTheme(themeMode);
el['theme-btn'].addEventListener('click', () => {
  themeMode = themeMode === 'system' ? 'dark' : themeMode === 'dark' ? 'light' : 'system';
  try { localStorage.setItem('b3-theme', themeMode); } catch (e) { /* ignore */ }
  applyTheme(themeMode);
});

/* ----------------------------------------------------------- capabilities */

function badge(node, text, state) {
  node.textContent = text;
  if (state) node.setAttribute('data-state', state); else node.removeAttribute('data-state');
}

(function capabilities() {
  const coi = !!self.crossOriginIsolated;
  badge(el['b-coi'], 'cross-origin isolated: ' + (coi ? 'yes' : 'NO'), coi ? 'ok' : 'warn');
  const sab = typeof SharedArrayBuffer !== 'undefined';
  badge(el['b-sab'], 'SharedArrayBuffer: ' + (sab ? 'yes' : 'no'), sab ? 'ok' : 'warn');

  let gl = false;
  try {
    const c = document.createElement('canvas');
    gl = !!(c.getContext('webgl2') || c.getContext('webgl'));
  } catch (e) { gl = false; }
  badge(el['b-gl'], 'WebGL2: ' + (gl ? 'yes' : 'no'), gl ? 'ok' : 'bad');

  const fsa = typeof window.showOpenFilePicker === 'function';
  badge(el['b-fsa'], 'file picker: ' + (fsa ? 'native' : 'fallback'), fsa ? 'ok' : null);
  el['pick-hint'].textContent = fsa
    ? 'saved for next time' : 'drag and drop or browse';

  if (!coi) {
    shellLog('not cross-origin isolated -- SharedArrayBuffer is off, so a ' +
             'pthread build will refuse to start.  Serve with COOP: same-origin ' +
             'and COEP: require-corp (tools/webserve.py does).');
  }
})();

/* -------------------------------------------------------------- gamepads */

/* The Gamepad API only lists a pad after the user presses something on it, so
 * the connect event is often the first and only evidence we get. */
let padFromEvent = null;

function padName() {
  const pads = navigator.getGamepads ? navigator.getGamepads() : [];
  for (const p of pads) if (p && p.connected) return p.id;
  return padFromEvent;
}

function refreshPad() {
  const id = padName();
  if (id === S.pad) return;
  S.pad = id;
  badge(el['b-pad'],
        id ? 'controller: ' + id.slice(0, 34) : 'controller: none — press a button',
        id ? 'ok' : null);
  el['hud-pad'].hidden = false;
  el['hud-pad'].textContent = id ? 'controller: ' + id : 'no controller — press a button';
  el['hud-pad'].title = (id ? 'controller: ' + id
                            : 'The browser only notices a controller after you ' +
                              'press a button on it.') +
    ' The keyboard always works.';

  /* The same fact, said out loud in the disc-pick step -- nothing waits on it. */
  el['pad-line'].classList.toggle('on', !!id);
  el['pad-mark'].textContent = id ? '✓' : '●';
  el['pad-text'].textContent = id
    ? 'Controller connected: ' + id
    : 'Using a controller? Press any button on it now.';
  el['pad-text'].title = id || '';
  if (id) shellLog('gamepad: ' + id + ' (button mapping is handled by the engine via SDL)');
}

window.addEventListener('gamepadconnected', (e) => {
  if (e.gamepad && e.gamepad.id) padFromEvent = e.gamepad.id;
  refreshPad();
});
window.addEventListener('gamepaddisconnected', (e) => {
  if (e.gamepad && e.gamepad.id === padFromEvent) padFromEvent = null;
  refreshPad();
});
setInterval(refreshPad, 1000);          /* the events need a button press first */
refreshPad();

/* ------------------------------------------------------- fullscreen/audio */

function fsElement() {
  return document.fullscreenElement || document.webkitFullscreenElement || null;
}

async function goFullscreen() {
  const t = el.stage;
  const fn = t.requestFullscreen || t.webkitRequestFullscreen;
  if (!fn) { shellLog('fullscreen not available in this browser'); return false; }
  try {
    await fn.call(t, { navigationUI: 'hide' });
    return true;
  } catch (e) {
    shellLog('fullscreen refused (' + (e && e.message) + ') -- running windowed');
    return false;
  }
}

function updateFsBtn() {
  const on = !!fsElement();
  el['fs-btn'].hidden = on;
}
document.addEventListener('fullscreenchange', updateFsBtn);
document.addEventListener('webkitfullscreenchange', updateFsBtn);

/* Autoplay policy: a silent buffer played inside the click gesture is what
 * actually flips an AudioContext to "running", and SDL/Emscripten will reuse
 * the same context. */
let audioCtx = null;
function unlockAudio() {
  const AC = window.AudioContext || window.webkitAudioContext;
  if (!AC) return;
  try {
    if (!audioCtx) audioCtx = new AC();
    window.__b3AudioContext = audioCtx;      /* engines look for this */
    const b = audioCtx.createBuffer(1, 1, 22050);
    const src = audioCtx.createBufferSource();
    src.buffer = b;
    src.connect(audioCtx.destination);
    src.start(0);
    if (audioCtx.state === 'suspended') audioCtx.resume();
    shellLog('audio context: ' + audioCtx.state + ' @ ' + audioCtx.sampleRate + ' Hz');
  } catch (e) {
    shellLog('audio unlock failed: ' + e.message);
  }
}

/* ------------------------------------------------------------ the fire ----
 * The landing backdrop, and nothing more: the classic cooling-diffusion fire.
 * A 160x90 byte field is seeded along its bottom row, then every cell above it
 * takes the average of the three cells below plus the one below that, minus a
 * random cooling term -- so heat rises, spreads and dies out.  The browser
 * does the upscale, so one 14k-cell pass at 30 fps is the entire cost.
 *
 * It is decoration: no library, no asset, aria-hidden, rAF (which a hidden tab
 * pauses by itself), one still frame instead of a loop under
 * prefers-reduced-motion, and stopped outright the moment PLAY is pressed so
 * it never competes with the engine for a frame. */

const FIRE_W = 160, FIRE_H = 90;
const fireHeat = new Uint8Array(FIRE_W * FIRE_H);
const fireSeed = new Uint8Array(FIRE_W);
const fireStill = !!(window.matchMedia &&
                     window.matchMedia('(prefers-reduced-motion: reduce)').matches);
let fireCtx = null, fireImg = null, firePal = null, fireRaf = 0, fireLast = 0;

/* black -> deep red -> orange -> yellow -> white, with the alpha ramping in
 * from nothing so the cold end of the field simply is not there. */
function firePalette() {
  const p = new Uint8Array(256 * 4);
  const b255 = (v) => (v < 0 ? 0 : v > 255 ? 255 : v | 0);
  for (let i = 0; i < 256; i++) {
    const t = i / 255;
    p[i * 4]     = b255(255 * (0.16 + t * 2.4));
    p[i * 4 + 1] = b255(255 * (t - 0.30) * 1.85);
    p[i * 4 + 2] = b255(255 * (t - 0.74) * 3.2);
    p[i * 4 + 3] = b255(255 * Math.min(1, t * 1.5) * 0.95);
  }
  return p;
}

function fireStep() {
  /* The source row breathes -- a bounded random walk, with everything under
   * the floor knocked to zero so the flames form tongues, not a wall. */
  for (let x = 0; x < FIRE_W; x++) {
    let v = fireSeed[x] + (Math.random() * 96 - 48);
    if (v < 0) v = 0; else if (v > 255) v = 255;
    fireSeed[x] = v;
    fireHeat[(FIRE_H - 1) * FIRE_W + x] = v < 96 ? 0 : v;
  }
  for (let y = FIRE_H - 2; y >= 0; y--) {
    const b1 = (y + 1) * FIRE_W;
    const b2 = (y + 2 < FIRE_H ? y + 2 : y + 1) * FIRE_W;
    for (let x = 0; x < FIRE_W; x++) {
      const l = x === 0 ? FIRE_W - 1 : x - 1;
      const r = x === FIRE_W - 1 ? 0 : x + 1;
      let v = (fireHeat[b1 + l] + fireHeat[b1 + x] +
               fireHeat[b1 + r] + fireHeat[b2 + x]) >> 2;
      v -= (Math.random() * 4) | 0;
      fireHeat[y * FIRE_W + x] = v > 0 ? v : 0;
    }
  }
}

function fireDraw() {
  const d = fireImg.data;
  for (let i = 0, n = FIRE_W * FIRE_H; i < n; i++) {
    const c = fireHeat[i] << 2, o = i << 2;
    d[o] = firePal[c]; d[o + 1] = firePal[c + 1];
    d[o + 2] = firePal[c + 2]; d[o + 3] = firePal[c + 3];
  }
  fireCtx.putImageData(fireImg, 0, 0);
}

function fireInit() {
  if (fireCtx) return true;
  if (!el.fire || !el.fire.getContext) return false;
  try {
    el.fire.width = FIRE_W;
    el.fire.height = FIRE_H;
    fireCtx = el.fire.getContext('2d', { alpha: true });
    if (!fireCtx) return false;
    fireImg = fireCtx.createImageData(FIRE_W, FIRE_H);
    firePal = firePalette();
  } catch (e) {
    fireCtx = null;
    return false;
  }
  for (let i = 0; i < 70; i++) fireStep();     /* warm up: never a cold frame */
  return true;
}

function fireFrame(now) {
  fireRaf = requestAnimationFrame(fireFrame);
  if (now - fireLast < 33) return;             /* ~30 fps is more than enough */
  fireLast = now;
  fireStep();
  fireDraw();
}

function fireStart() {
  if (!fireInit()) return;
  if (fireStill) { fireDraw(); return; }       /* reduced motion: one frame */
  if (fireRaf) return;
  fireLast = 0;
  fireRaf = requestAnimationFrame(fireFrame);
}

function fireStop() {
  if (fireRaf) { cancelAnimationFrame(fireRaf); fireRaf = 0; }
}

fireStart();

/* ---------------------------------------------------------------- panels */

function showPanel(which) {
  el['panel-pick'].hidden = which !== 'pick';
  el['panel-verify'].hidden = which !== 'verify';
  el['panel-load'].hidden = which !== 'load';
  el.overlay.hidden = which === null;
}

function setStatus(txt) { el['hud-status'].textContent = txt; }

function enterStage() {
  el.stage.classList.add('on');
  el.stage.setAttribute('aria-hidden', 'false');
  el.landing.hidden = true;
  updateFsBtn();
}

function leaveStage() {
  el.stage.classList.remove('on');
  el.stage.setAttribute('aria-hidden', 'true');
  el.landing.hidden = false;
  if (fsElement() && document.exitFullscreen) document.exitFullscreen().catch(() => {});
}

/* ---------------------------------------------------------------- PLAY -- */

el.play.addEventListener('click', async () => {
  S.phase = 'starting';
  fireStop();                     /* the landing is gone; so is its backdrop */
  unlockAudio();
  enterStage();
  setStatus('waiting for a game file');
  goFullscreen();
  showPanel('pick');
  await offerStoredHandle();
});

el['quit-btn'].addEventListener('click', () => { leaveStage(); fireStart(); S.phase = 'landing'; });
el['fs-btn'].addEventListener('click', () => goFullscreen());

el['scale-btn'].addEventListener('click', () => {
  const crisp = el.canvas.classList.toggle('crisp');
  el.canvas.classList.toggle('smooth', !crisp);
  el['scale-btn'].textContent = 'scaling: ' + (crisp ? 'crisp' : 'smooth');
});

/* ------------------------------------------------------------ acquisition */

const PICK_OPTS = {
  multiple: false,
  types: [{
    description: 'Xbox ISO file',
    accept: { 'application/octet-stream': ['.iso', '.xiso', '.img'] }
  }]
};

function pickError(msg) {
  el['pick-error'].hidden = !msg;
  el['pick-error'].className = 'note bad';
  el['pick-error'].textContent = msg || '';
}

async function offerStoredHandle() {
  const rec = await idbGet('kv', 'isoHandle');
  if (!rec || !rec.handle) { el['resume-row'].hidden = true; return; }
  el['resume-name'].textContent = rec.name || 'saved game file';
  el['resume-meta'].textContent = (rec.size ? fmtBytes(rec.size) : '') +
    (rec.at ? ' · last used ' + new Date(rec.at).toLocaleDateString() : '');
  el['resume-row'].hidden = false;
  S.handle = rec.handle;

  /* Already granted (same session, or a persisted grant): go straight in. */
  try {
    if (rec.handle.queryPermission &&
        (await rec.handle.queryPermission({ mode: 'read' })) === 'granted') {
      shellLog('stored handle still granted -- opening ' + rec.name);
      useFile(await rec.handle.getFile(), rec.handle);
    }
  } catch (e) { /* needs the click below */ }
}

el['resume-btn'].addEventListener('click', async () => {
  pickError('');
  try {
    const h = S.handle;
    if (!h) return;
    if (h.requestPermission) {
      const p = await h.requestPermission({ mode: 'read' });
      if (p !== 'granted') { pickError('The browser would not open that file.'); return; }
    }
    useFile(await h.getFile(), h);
  } catch (e) {
    pickError('That file is no longer there (' + e.message + '). Pick it again.');
    el['resume-row'].hidden = true;
    idbDel('kv', 'isoHandle');
  }
});

el['forget-btn'].addEventListener('click', async () => {
  await idbDel('kv', 'isoHandle');
  S.handle = null;
  el['resume-row'].hidden = true;
  shellLog('forgot the stored disc-image handle');
});

el['choose-btn'].addEventListener('click', async () => {
  pickError('');
  if (typeof window.showOpenFilePicker !== 'function') { el['file-input'].click(); return; }
  try {
    const [h] = await window.showOpenFilePicker(PICK_OPTS);
    useFile(await h.getFile(), h);
  } catch (e) {
    if (e && e.name === 'AbortError') return;             /* user cancelled */
    shellLog('native picker failed (' + e.message + ') -- falling back');
    el['file-input'].click();
  }
});

el['browse-btn'].addEventListener('click', () => { pickError(''); el['file-input'].click(); });

el['file-input'].addEventListener('change', () => {
  const f = el['file-input'].files && el['file-input'].files[0];
  if (f) useFile(f, null);
});

/* drag-and-drop, anywhere on the page */
let dragDepth = 0;
function dragOn(on) { el.drop.classList.toggle('hot', on); }
['dragenter', 'dragover'].forEach((t) => window.addEventListener(t, (e) => {
  if (!e.dataTransfer || Array.from(e.dataTransfer.types).indexOf('Files') < 0) return;
  e.preventDefault();
  e.dataTransfer.dropEffect = 'copy';
  if (t === 'dragenter') dragDepth++;
  dragOn(true);
}));
window.addEventListener('dragleave', () => { if (--dragDepth <= 0) { dragDepth = 0; dragOn(false); } });
window.addEventListener('drop', async (e) => {
  if (!e.dataTransfer) return;
  e.preventDefault();
  dragDepth = 0; dragOn(false);
  if (S.phase === 'landing') { enterStage(); showPanel('pick'); }
  const items = e.dataTransfer.items;
  /* Chrome hands out a real FileSystemFileHandle from a drop, which is
   * persistable exactly like a picked one. */
  if (items && items[0] && items[0].getAsFileSystemHandle) {
    try {
      const h = await items[0].getAsFileSystemHandle();
      if (h && h.kind === 'file') { useFile(await h.getFile(), h); return; }
    } catch (err) { /* fall through to the plain File */ }
  }
  const f = e.dataTransfer.files && e.dataTransfer.files[0];
  if (f) useFile(f, null);
});

/* ------------------------------------------------------------ the checks */

const CHECK_ROWS = [
  ['size',      'File size'],
  ['magic',     'XISO magic (sector 32)'],
  ['sector32',  'Sector 32 SHA-256'],
  ['first1mib', 'First 1 MiB SHA-256'],
  ['xbe',       'default.xbe SHA-256']
];

const ST_LABEL = { idle: '·', run: '◌', pass: '✓', fail: '✗', skip: '–' };

function renderChecks() {
  const tb = el.checks;
  tb.textContent = '';
  for (const [key, label] of CHECK_ROWS) {
    const c = S.checks[key];
    const tr = document.createElement('tr');
    tr.setAttribute('data-st', c.st);
    tr.id = 'row-' + key;

    const td0 = document.createElement('td');
    td0.className = 'st';
    td0.textContent = ST_LABEL[c.st] || '·';
    tr.appendChild(td0);

    const td1 = document.createElement('td');
    td1.textContent = label;
    tr.appendChild(td1);

    const td2 = document.createElement('td');
    td2.className = 'v';
    renderValue(td2, key, c);
    tr.appendChild(td2);

    tb.appendChild(tr);
  }
}

function renderValue(td, key, c) {
  const want = document.createElement('div');
  want.className = 'want';
  const isHash = key !== 'size' && key !== 'magic';
  want.textContent = 'want ' + (key === 'size' ? fmtBytes(c.want)
                                : isHash ? shortHash(c.want) : c.want);
  want.title = String(c.want);
  td.appendChild(want);

  if (c.st === 'run') { const d = document.createElement('div'); d.textContent = 'checking…'; td.appendChild(d); return; }
  if (c.st === 'idle') { const d = document.createElement('div'); d.className = 'muted'; d.textContent = 'waiting'; td.appendChild(d); return; }
  if (c.st === 'skip') { const d = document.createElement('div'); d.className = 'muted'; d.textContent = 'skipped'; td.appendChild(d); return; }

  const got = document.createElement('div');
  const ok = c.st === 'pass';
  got.className = ok ? '' : 'got-bad';
  /* A failed read reports a sentence, not a digest -- only elide real hashes,
   * or "(unreadable)" comes out as "(unreadable)…eadable)". */
  const gv = c.got === null ? '(no data)'
           : key === 'size' ? fmtBytes(c.got)
           : key === 'magic' ? '"' + c.got + '"'
           : /^[0-9a-f]{64}$/.test(c.got) ? shortHash(c.got)
           : c.got;
  got.textContent = ok ? 'got  match' : 'got  ' + gv;
  got.title = String(c.got);
  td.appendChild(got);

  if (key === 'size' && !ok && c.got !== null) {
    const d = document.createElement('div');
    d.className = 'got-bad';
    const delta = c.got - c.want;
    d.textContent = (delta > 0 ? '+' : '') + delta.toLocaleString('en-US') + ' bytes vs reference';
    td.appendChild(d);
  }
  /* Where in the image the executable was found, and whether it is the size
   * the reference says -- a wrong-sized default.xbe is a different build, and
   * saying so is more use than a bare digest mismatch. */
  if (key === 'xbe' && c.sector !== null && c.size !== null) {
    const d = document.createElement('div');
    const sizeOk = c.size === REF.xbeSize;
    d.className = sizeOk ? 'rate' : 'got-bad';
    d.textContent = fmtBytes(c.size) + ' at sector ' + c.sector +
      (sizeOk ? '' : '  (reference is ' + fmtBytes(REF.xbeSize) + ')');
    td.appendChild(d);
  }
}

function mark(key, st, got, extra) {
  const c = S.checks[key];
  c.st = st;
  if (got !== undefined) c.got = got;
  if (extra) Object.assign(c, extra);
  renderChecks();
}

function resetChecks() {
  for (const k of Object.keys(S.checks)) {
    const c = S.checks[k];
    c.st = 'idle'; c.got = null;
    if (k === 'xbe') { c.size = null; c.sector = null; c.entries = null; }
  }
  S.quickOk = null; S.xbeOk = null; S.cached = false; S.ms = 0; S.warnings = [];
  el['verify-warn'].hidden = true;
  el['verify-ok'].hidden = true;
  renderChecks();
}

/* -------------------------------------------------------------- verdicts */

/* The button never locks -- it just stops pretending. */
function updateStartLabel() {
  const anyFail = CHECK_ROWS.some(([k]) => S.checks[k].st === 'fail');
  el['start-btn'].textContent = anyFail ? 'Start anyway' : 'Start game';
  el['start-btn'].title = anyFail
    ? 'This file failed the check, but you can still try it.'
    : 'Start the game with this file';
}

function warnPanel() {
  const fails = CHECK_ROWS.filter(([k]) => S.checks[k].st === 'fail');
  updateStartLabel();
  S.warnings = fails.map(([k, label]) => ({
    key: k, label: label,
    want: String(S.checks[k].want), got: String(S.checks[k].got)
  }));

  if (!fails.length) {
    el['verify-warn'].hidden = true;
    if (S.quickOk && S.xbeOk) {
      el['verify-ok'].hidden = false;
      el['verify-ok'].textContent =
        'This is the right game file. Only the start of the file is checked, ' +
        'so damage further in could still slip through.';
    }
    return;
  }

  el['verify-ok'].hidden = true;
  const n = el['verify-warn'];
  n.hidden = false;
  n.className = 'note bad';
  n.textContent = '';

  const h = document.createElement('h3');
  h.textContent = '⚠ This is not the right game file';
  n.appendChild(h);

  const p = document.createElement('p');
  p.style.margin = '0 0 4px';
  p.textContent = 'You can play it anyway, but expect it to break. ' +
                  (fails.length === 1 ? 'One check failed:' : fails.length + ' checks failed:');
  n.appendChild(p);

  const ul = document.createElement('ul');
  for (const f of S.warnings) {
    const li = document.createElement('li');
    const b = document.createElement('b');
    b.textContent = f.label;
    li.appendChild(b);
    const w = document.createElement('span');
    w.className = 'mono';
    w.textContent = 'expected ' + (f.key === 'size' ? Number(f.want).toLocaleString('en-US') + ' B' : f.want);
    li.appendChild(w);
    const g = document.createElement('span');
    g.className = 'mono';
    g.textContent = 'got      ' + (f.key === 'size' ? Number(f.got).toLocaleString('en-US') + ' B'
                                                    : (f.got === 'null' ? '(no data)' : f.got));
    li.appendChild(g);
    ul.appendChild(li);
  }
  n.appendChild(ul);

  shellLog('VERIFICATION MISMATCH: ' + S.warnings.map((w) => w.label).join(', '));
}

/* ------------------------------------------------------ taking a disc image */

async function useFile(file, handle) {
  if (!file) return;
  pickError('');
  S.file = file;
  S.handle = handle || null;
  S.phase = 'verifying';
  setStatus('checking ' + file.name);

  el['v-name'].textContent = file.name;
  el['v-meta'].textContent = fmtBytes(file.size) + '  ·  ' + fmtMB(file.size) +
    (file.lastModified ? '  ·  ' + new Date(file.lastModified).toLocaleString() : '');

  resetChecks();
  showPanel('verify');
  el['start-btn'].disabled = true;
  el['verify-head'].textContent = 'checking…';

  if (handle) {
    const stored = await idbPut('kv', 'isoHandle', {
      handle: handle, name: file.name, size: file.size,
      lastModified: file.lastModified, at: Date.now()
    });
    shellLog(stored ? 'remembered this image for next time'
                    : 'could not persist the file handle (private mode?)');
  }

  shellLog('disc image: ' + file.name + ' (' + fmtBytes(file.size) + ')');
  await verify(file, false);
}

/* ------------------------------------------------------- the XISO directory
 * The volume descriptor at sector 32 names the root directory extent, and the
 * root extent is a run of dirents:
 *
 *   u16 left   child, in DWORDs from the start of the extent
 *   u16 right  child
 *   u32 start_sector
 *   u32 file_size
 *   u8  attributes
 *   u8  name_len
 *   char name[name_len]                (then padded to a 4-byte boundary)
 *
 * Retail images arrange those into a binary tree, but the records themselves
 * sit contiguously in the same buffer, so the walk below IGNORES left/right
 * and scans linearly.  That is deliberate: a linear scan cannot be led astray
 * by one bad pointer, which is exactly the situation this code exists to
 * report on.  0x00 / 0xFF in name_len is inter-sector padding -- skip to the
 * next sector rather than trying to parse it.
 *
 * Cross-checked against tools-side Python: on the known-good image both the
 * linear scan and a real tree walk return the same nine root entries. */
function walkDirents(buf) {
  const dv = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
  const out = [];
  let pos = 0;
  while (pos + 14 <= buf.length) {
    const nameLen = buf[pos + 13];
    if (nameLen === 0 || nameLen === 0xff || pos + 14 + nameLen > buf.length) {
      pos = (Math.floor(pos / REF.sectorBytes) + 1) * REF.sectorBytes;
      continue;                              /* padding -> next sector */
    }
    out.push({
      start: dv.getUint32(pos + 4, true),
      size:  dv.getUint32(pos + 8, true),
      attr:  buf[pos + 12],
      name:  latin1(buf.subarray(pos + 14, pos + 14 + nameLen))
    });
    pos = (pos + 14 + nameLen + 3) & ~3;
  }
  return out;
}

/* Returns {sector, offset, size, entries} or {err}.  `vd` is sector 32.
 * The magic having failed does NOT stop us: root_dirent_sector may still be
 * intact, and locating the executable anyway tells the user more. */
async function locateXbe(file, vd) {
  if (!vd || vd.length < 28) return { err: 'volume descriptor unreadable' };
  const dv = new DataView(vd.buffer, vd.byteOffset, vd.byteLength);
  const rootSector = dv.getUint32(20, true);
  const rootSize = dv.getUint32(24, true);
  const rootOff = rootSector * REF.sectorBytes;

  if (!rootSize || rootSize > MAX_ROOT_DIR || rootOff + rootSize > file.size) {
    return { err: 'root directory out of range (sector ' + rootSector +
                  ', ' + rootSize + ' B)' };
  }

  let root;
  try {
    root = new Uint8Array(await file.slice(rootOff, rootOff + rootSize).arrayBuffer());
  } catch (e) {
    return { err: 'root directory unreadable (' + e.message + ')' };
  }
  if (root.length < rootSize) return { err: 'root directory truncated' };

  const ents = walkDirents(root);
  const hit = ents.find((e) => e.name.toLowerCase() === REF.xbeName);
  if (!hit) {
    return { err: 'no ' + REF.xbeName + ' in image', entries: ents.length };
  }
  const off = hit.start * REF.sectorBytes;
  if (!hit.size || hit.size > MAX_XBE || off + hit.size > file.size) {
    return { err: REF.xbeName + ' extent out of range (sector ' + hit.start +
                  ', ' + hit.size + ' B)', entries: ents.length };
  }
  return { sector: hit.start, offset: off, size: hit.size, entries: ents.length };
}

/* ----------------------------------------------------------- the verify run
 * One pass, no streaming stage: four header checks plus the executable.  The
 * whole thing reads about 5.3 MB and finishes in a couple of hundred
 * milliseconds, so there is nothing to run in the background any more. */
async function verify(file, force) {
  const t0 = performance.now();

  if (!force && await applyCachedVerdict(file)) return;

  /* 1 -- size */
  mark('size', 'run');
  mark('size', file.size === REF.size ? 'pass' : 'fail', file.size);

  /* 2/3 -- sector 32: the XISO signature and its hash */
  mark('magic', 'run'); mark('sector32', 'run');
  let sec = null;
  try {
    const buf = await file.slice(REF.magicOffset, REF.magicOffset + REF.sectorBytes).arrayBuffer();
    sec = new Uint8Array(buf);
  } catch (e) {
    shellLog('cannot read sector 32: ' + e.message);
  }
  if (!sec || sec.length < REF.sectorBytes) {
    mark('magic', 'fail', sec ? '(short read, ' + (sec ? sec.length : 0) + ' B)' : '(unreadable)');
    mark('sector32', 'fail', '(unreadable)');
  } else {
    const got = latin1(sec.subarray(0, REF.magic.length));
    mark('magic', got === REF.magic ? 'pass' : 'fail', got);
    const h = await sha256(sec);
    mark('sector32', h === REF.sector32Sha ? 'pass' : 'fail', h);
  }

  /* 4 -- the first mebibyte, which covers the whole XISO header region */
  mark('first1mib', 'run');
  try {
    const buf = await file.slice(0, MiB).arrayBuffer();
    if (buf.byteLength < MiB) {
      mark('first1mib', 'fail', '(short read, ' + buf.byteLength + ' B)');
    } else {
      const h = await sha256(new Uint8Array(buf));
      mark('first1mib', h === REF.firstMiBSha ? 'pass' : 'fail', h);
    }
  } catch (e) {
    mark('first1mib', 'fail', '(unreadable: ' + e.message + ')');
  }

  S.quickOk = ['size', 'magic', 'sector32', 'first1mib']
    .every((k) => S.checks[k].st === 'pass');

  /* 5 -- the executable.  Walk the XISO directory to find default.xbe, then
   * hash its one contiguous extent. */
  mark('xbe', 'run');
  const loc = await locateXbe(file, sec);
  if (loc.err) {
    mark('xbe', 'fail', loc.err, { size: null, sector: null, entries: loc.entries || null });
    shellLog('default.xbe: ' + loc.err);
  } else {
    shellLog('default.xbe: sector ' + loc.sector + ' (offset 0x' +
             loc.offset.toString(16) + '), ' + fmtBytes(loc.size) +
             ', ' + loc.entries + ' root entries');
    try {
      const buf = await file.slice(loc.offset, loc.offset + loc.size).arrayBuffer();
      if (buf.byteLength < loc.size) {
        mark('xbe', 'fail', '(short read, ' + buf.byteLength + ' of ' + loc.size + ' B)',
             { size: loc.size, sector: loc.sector, entries: loc.entries });
      } else {
        const h = await sha256(new Uint8Array(buf));
        mark('xbe', h === REF.xbeSha ? 'pass' : 'fail', h,
             { size: loc.size, sector: loc.sector, entries: loc.entries });
      }
    } catch (e) {
      mark('xbe', 'fail', '(unreadable: ' + e.message + ')',
           { size: loc.size, sector: loc.sector, entries: loc.entries });
    }
  }
  S.xbeOk = S.checks.xbe.st === 'pass';

  S.ms = performance.now() - t0;
  S.cached = false;
  shellLog('verification: ' + (S.quickOk && S.xbeOk ? 'all pass' : 'MISMATCH') +
           ' in ' + S.ms.toFixed(0) + ' ms (' +
           fmtMB(MiB + REF.sectorBytes + (S.checks.xbe.size || 0)) + ' read)');

  await saveVerdict(file);
  finishVerify();
}

/* Nothing here gates the user: the start button lights up either way, with
 * the warning attached. */
function finishVerify() {
  el['start-btn'].disabled = false;
  el['verify-head'].textContent =
    (S.quickOk && S.xbeOk ? 'looks good' : 'problems found') +
    (S.cached ? ' · cached' : ' · ' + S.ms.toFixed(0) + ' ms');
  warnPanel();
}

/* --------------------------------------------------------- verdict cache --
 * Keyed on (name, size, mtime).  Verification is only a couple of hundred
 * milliseconds now, so this is a nicety rather than the necessity it was when
 * the whole 2.4 GB got hashed -- but a returning visitor still gets an
 * instant answer, and "Re-verify" always forces a fresh read. */
const VERDICT_VER = 2;

async function applyCachedVerdict(file) {
  const v = await idbGet('verdicts', verdictKey(file));
  if (!v || v.ver !== VERDICT_VER) return false;
  for (const [k] of CHECK_ROWS) {
    const rec = v.checks[k];
    if (!rec) return false;
    mark(k, rec.st, rec.got, k === 'xbe'
      ? { size: rec.size ?? null, sector: rec.sector ?? null, entries: rec.entries ?? null }
      : null);
  }
  S.quickOk = v.quickOk;
  S.xbeOk = v.xbeOk;
  S.ms = v.ms || 0;
  S.cached = true;
  shellLog('cached verdict for ' + file.name + ': ' +
           (v.quickOk && v.xbeOk ? 'all pass' : 'MISMATCH') +
           ' (checked ' + new Date(v.at).toLocaleString() + ')');
  finishVerify();
  return true;
}

async function saveVerdict(file) {
  const checks = {};
  for (const [k] of CHECK_ROWS) {
    const c = S.checks[k];
    checks[k] = { st: c.st, got: c.got };
    if (k === 'xbe') { checks[k].size = c.size; checks[k].sector = c.sector; checks[k].entries = c.entries; }
  }
  await idbPut('verdicts', verdictKey(file), {
    ver: VERDICT_VER, checks: checks, quickOk: S.quickOk, xbeOk: S.xbeOk,
    ms: S.ms, at: Date.now()
  });
}

el['reverify-btn'].addEventListener('click', async () => {
  if (!S.file) return;
  await idbDel('verdicts', verdictKey(S.file));
  resetChecks();
  el['start-btn'].disabled = true;
  el['verify-head'].textContent = 'checking…';
  await verify(S.file, true);
});

el['change-btn'].addEventListener('click', () => {
  showPanel('pick');
  setStatus('waiting for a game file');
});

/* ---------------------------------------------------------------- engine */

function engineUrls() {
  const seen = new Set(), out = [];
  for (const rel of ENGINE_CANDIDATES) {
    let u;
    try { u = new URL(rel, document.baseURI).href; } catch (e) { continue; }
    if (!seen.has(u)) { seen.add(u); out.push(u); }
  }
  return out;
}

/* Deliberately NOT run at page load: a HEAD that 404s writes a browser-level
 * network error into the console, and the page load itself should be silent.
 * The probe happens when the user actually asks to run. */
async function probeEngine() {
  if (S.engine.probed) return S.engine;
  S.engine.probed = true;
  for (const url of engineUrls()) {
    try {
      const r = await fetch(url, { method: 'HEAD', cache: 'no-store' });
      if (r.ok) {
        S.engine.found = true; S.engine.url = url;
        shellLog('engine bundle: ' + url);
        return S.engine;
      }
    } catch (e) { S.engine.error = e.message; }
  }
  S.engine.found = false;
  shellLog('engine bundle not found (looked for ' + engineUrls().join(', ') +
           ') -- build it with the Emscripten target, then reload');
  return S.engine;
}

const PHASES = [
  ['iso',     /^\[Burnout3\] iso: source /,                    'disc mounted'],
  ['tsel',    /track select data:/,                            'track select'],
  ['circuit', /REAL circuit from Gamedata\.bgd/,               'circuit'],
  ['nav',     /retail nav:/,                                   'nav ribbon'],
  ['geom',    /REAL track geometry/,                           'track geometry'],
  ['tex',     /REAL textures/,                                 'textures'],
  ['coll',    /GAME collision world|mesh collision:/,          'collision'],
  ['cars',    /REAL car meshes/,                               'car meshes'],
  ['veh',     /vehicle roster:/,                               'vehicles'],
  ['phys',    /car physics:/,                                  'physics'],
  ['traffic', /^\[Burnout3\] TRAFFIC:|traffic data:|traffic disabled/, 'traffic'],
  ['pace',    /ai pace:/,                                      'AI pace'],
  ['font',    /font metrics:/,                                 'fonts'],
  ['hudstr',  /hud labels:/,                                   'HUD text'],
  ['audio',   /REAL audio:/,                                   'audio'],
  /* Whether sound is actually REACHING the page, which is a different
   * question from whether the banks loaded -- the mix can be perfect and
   * still go nowhere if the AudioContext never arrives.  "running SILENT"
   * deliberately does NOT light this. */
  ['sound',   /web: audio bridge up/,                          'sound']
];

const RE_STAGE = /^\[Burnout3\] iso: (\S+)\s+(\S+)\s+->\s+(\S+)\s+\(([\d.]+) s(, WITH FAILURES)?\)/;
const RE_SOURCE = /^\[Burnout3\] iso: source (.+)$/;
const RE_CACHE = /^\[Burnout3\] iso: cache\s+(\S+)/;
const RE_CTRL = /^\[Burnout3\] controller: (.+)$/;

const phaseHit = new Set();
let stageCount = 0, stageSecs = 0, revealTimer = 0;

function renderPhases() {
  const box = el.phases;
  if (box.childNodes.length !== PHASES.length) {
    box.textContent = '';
    for (const [key, , label] of PHASES) {
      const s = document.createElement('span');
      s.className = 'phase'; s.id = 'ph-' + key; s.textContent = label;
      box.appendChild(s);
    }
  }
  for (const [key] of PHASES) {
    const n = $('ph-' + key);
    if (n) n.classList.toggle('hit', phaseHit.has(key));
  }
}

function onEngineLine(text, kind) {
  log(text, kind === 'err' ? 'e' : '');

  const src = text.match(RE_SOURCE);
  if (src) {
    el['load-status'].textContent = 'game file loaded: ' + src[1];
    setStatus('mounted ' + src[1].split('/').pop());
  }
  const cache = text.match(RE_CACHE);
  if (cache) el['load-head'].textContent = 'cache ' + cache[1];

  const st = text.match(RE_STAGE);
  if (st) {
    stageCount++;
    stageSecs += parseFloat(st[4]) || 0;
    el['load-stage'].hidden = false;
    el['load-stage'].textContent =
      'materialising ' + st[2] + ' → ' + st[3] + '  (' + st[4] + ' s' +
      (st[5] ? ', WITH FAILURES' : '') + ')';
    el['load-status'].textContent =
      'extracting from the disc — ' + stageCount + ' stage' +
      (stageCount === 1 ? '' : 's') + ', ' + stageSecs.toFixed(1) + ' s so far';
    if (st[5]) noteLoad('warn', 'A materialisation stage reported failures: ' + st[2]);
  }

  const ctrl = text.match(RE_CTRL);
  if (ctrl) {
    el['hud-pad'].hidden = false;
    el['hud-pad'].textContent = 'controller: ' + ctrl[1];
  }

  for (const [key, re] of PHASES) {
    if (!phaseHit.has(key) && re.test(text)) {
      phaseHit.add(key);
      renderPhases();
      if (key !== 'iso') el['load-status'].textContent = 'loading — ' + text.replace(/^\[Burnout3\] /, '');
    }
  }

  if (/FATAL/.test(text)) noteLoad('bad', text.replace(/^\[Burnout3\] /, ''));

  if (revealTimer) clearTimeout(revealTimer);
  if (S.engine.booted) revealTimer = setTimeout(revealGame, 1400);
}

function noteLoad(kind, msg) {
  const n = el['load-note'];
  n.hidden = false;
  n.className = 'note ' + kind;
  n.textContent = msg;
}

function revealGame() {
  if (!S.engine.booted) return;
  showPanel(null);
  S.phase = 'running';
  setStatus('running');
  el.canvas.focus();
}

el['load-back-btn'].addEventListener('click', () => {
  if (S.engine.booted) revealGame();
  else showPanel('verify');
});

el['start-btn'].addEventListener('click', async () => {
  const file = S.file;
  if (!file) return;
  showPanel('load');
  phaseHit.clear(); renderPhases();
  stageCount = 0; stageSecs = 0;
  el['load-note'].hidden = true;
  el['load-stage'].hidden = true;
  S.phase = 'booting';
  setStatus('starting the game');

  if (!S.quickOk || S.xbeOk === false) {
    noteLoad('warn', 'This file failed the check, so the game may not work.');
  }

  const eng = await probeEngine();
  if (!eng.found) {
    el['load-status'].textContent = 'the game is not installed here';
    noteLoad('warn',
      'The game itself is not on this server yet, so there is nothing to run. ' +
      'Everything up to this point works without it. Looked for: ' +
      engineUrls().join(', '));
    setStatus('the game is not installed here');
    S.phase = 'no-engine';
    return;
  }

  el['load-status'].textContent = 'loading ' + eng.url.split('/').pop() + '…';
  try {
    await loadScriptOnce(eng.url);
  } catch (e) {
    el['load-status'].textContent = 'engine bundle failed to load';
    noteLoad('bad', e.message);
    S.phase = 'no-engine';
    return;
  }

  if (typeof window.createB3Module !== 'function') {
    el['load-status'].textContent = 'engine bundle loaded but exports nothing';
    noteLoad('bad', 'burnout3.js loaded but window.createB3Module is missing. ' +
                    'The build needs -sMODULARIZE=1 -sEXPORT_NAME=createB3Module.');
    S.phase = 'no-engine';
    return;
  }

  el.canvas.classList.add('live');
  el['load-status'].textContent = 'engine starting…';

  const q = envFromQuery();
  if (q.n) {
    shellLog('engine environment from the query string: ' +
             Object.keys(q.env).map((k) => k + '=' + q.env[k]).join(' '));
  }

  const cfg = {
    canvas: el.canvas,
    b3IsoFile: file,                       /* the contract: raw File, no copy */
    b3Env: q.env,                          /* web/pre.js applies these to ENV */
    arguments: ENGINE_ARGS.slice(),
    print: (t) => onEngineLine(String(t), 'out'),
    printErr: (t) => onEngineLine(String(t), 'err'),
    onAbort: (why) => {
      noteLoad('bad', 'engine aborted: ' + why);
      setStatus('aborted');
      showPanel('load');
    },
    setStatus: (t) => { if (t) el['load-head'].textContent = String(t); }
  };
  if (audioCtx) cfg.audioContext = audioCtx;

  try {
    const mod = await window.createB3Module(cfg);
    S.engine.booted = true;
    window.__b3.module = mod;
    shellLog('engine runtime up (' + ENGINE_ARGS.join(' ') + ')');
    revealTimer = setTimeout(revealGame, 1400);
  } catch (e) {
    S.engine.error = String(e && e.message || e);
    el['load-status'].textContent = 'engine failed to start';
    noteLoad('bad', S.engine.error);
    setStatus('engine failed');
  }
});

/* Arrow keys and space scroll the page by default; once the engine owns the
 * canvas they belong to the game. */
window.addEventListener('keydown', (e) => {
  if (S.phase !== 'running') return;
  if ([' ', 'ArrowUp', 'ArrowDown', 'ArrowLeft', 'ArrowRight', 'Tab'].indexOf(e.key) >= 0) {
    e.preventDefault();
  }
}, { passive: false });

/* Clicking the letterbox while the game runs gives the canvas focus back. */
el.frame.addEventListener('pointerdown', () => { if (S.phase === 'running') el.canvas.focus(); });

/* ---------------------------------------------------------------- drawer */

function toggleDrawer() {
  const closed = el.drawer.classList.toggle('closed');
  el['drawer-hd'].setAttribute('aria-expanded', String(!closed));
  if (!closed) el['log-wrap'].scrollTop = el['log-wrap'].scrollHeight;
}
el['drawer-hd'].addEventListener('click', (e) => {
  if (e.target.tagName === 'BUTTON') return;
  toggleDrawer();
});
el['drawer-hd'].addEventListener('keydown', (e) => {
  if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); toggleDrawer(); }
});
el['log-copy'].addEventListener('click', async () => {
  const txt = logBuf.map((l) => l.t).join('\n');
  try { await navigator.clipboard.writeText(txt); el['log-copy'].textContent = 'copied'; }
  catch (e) { el['log-copy'].textContent = 'blocked'; }
  setTimeout(() => { el['log-copy'].textContent = 'copy'; }, 1200);
});
el['log-clear'].addEventListener('click', () => {
  logBuf.length = 0; logPending = []; el.log.textContent = '';
  el['log-count'].textContent = '0 lines';
});

/* ------------------------------------------------------------------ boot */

renderChecks();
renderPhases();
shellLog('Reb3 web shell ready — reference image: ' + REF.label +
         ', ' + fmtBytes(REF.size));
shellLog('verification reads the XISO header + ' + REF.xbeName + ' (' +
         fmtBytes(REF.xbeSize) + '), not the whole 2.4 GB; nothing is uploaded');

/* `ready` goes up only once the hasher has been proved against the FIPS
 * vectors -- automation (and the harness) waits on it rather than on
 * window.__b3, which exists from the top of the file. */
selfTestHasher()
  .catch((e) => { shellLog('self-test threw: ' + e.message); })
  .then(() => { window.__b3.ready = true; });
