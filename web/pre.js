/* web/pre.js -- THE FILESYSTEM AND ENVIRONMENT the engine boots into.
 *
 * Runs in preRun, ON THE MAIN BROWSER THREAD -- which, under PROXY_TO_PTHREAD,
 * is NOT the thread that runs main().  That split is the single most important
 * fact about this file, and it was measured rather than assumed:
 *
 *     preRun ran on : main            main() ran on : worker
 *     the worker's own FS object  : EMPTY
 *     the main thread's FS object : holds everything
 *     C fopen() from the game thread : WORKS
 *
 * i.e. Emscripten's JS filesystem lives here, on the main thread, and every
 * file syscall the game makes is proxied to it.  Two consequences shape
 * everything below:
 *
 *   1. IDBFS belongs here.  It is a main-thread filesystem and FS.syncfs is a
 *      main-thread call, so mounting the cache in preRun is exactly right.
 *
 *   2. WORKERFS CANNOT BE USED, and not merely because it asserts
 *      ENVIRONMENT_IS_WORKER.  It reads a File with FileReaderSync, which
 *      exists only on a worker -- but the read would execute HERE, on the main
 *      thread, where FileReaderSync does not exist.  The mount is impossible
 *      and would be useless if it were possible.
 *
 *      So the image does not go through the filesystem.  A dedicated helper
 *      worker owns the File and serves synchronous reads straight into the
 *      shared wasm heap; see web/b3_web.h and b3_iso_worker below.
 *
 * THE LAYOUT, otherwise deliberately the same shape as a desktop checkout so
 * that not one line of loader code has to know it is on the web:
 *
 *   /app                  MEMFS, the CWD.  Stands in for the repo root, so
 *                         every literal "build/..." path in src/ resolves the
 *                         way it does natively.
 *   /app/build/.isocache  IDBFS.  Where materialise-on-miss writes, so a
 *                         second visit does not re-extract.
 *   /iso/game.xiso        a ZERO-BYTE PLACEHOLDER.  It exists only so the
 *                         existence checks in burnout3_isodata.c succeed; the
 *                         bytes come from the bridge.
 *
 * The shell provides:  Module.b3IsoFile   a File/Blob for the Xbox image
 *                      Module.canvas      the drawing surface
 * and may set:         Module.b3CacheMode 'opfs' (default) | 'idbfs' | 'memfs'
 *                      Module.b3Env       {NAME: 'value'} engine environment
 *
 * ==================================================== THE DISC IS THE SOURCE
 * THE CACHE IS AN ACCELERATOR AND NEVER A DEPENDENCY.  Every boot -- cold,
 * warm, or with the storage layer broken or switched off -- must complete from
 * the ISO alone, because the ISO is the only thing the player actually gave
 * us.  So the whole cache path below is wrapped so that ANY failure costs one
 * log line and nothing else: the mount stays MEMFS, materialise-on-miss reads
 * the disc, and the session runs.  `b3CacheMode: 'memfs'` is that state on
 * purpose, and it is a tested gate, not a fallback nobody exercises.
 */

/* The helper worker, inlined so the build stays two artifacts (burnout3.js +
 * burnout3.wasm) and there is no extra URL for the shell to get wrong.
 *
 * It blocks in Atomics.wait on the control block and serves one read at a
 * time.  Views are rebuilt from memory.buffer on every request because
 * ALLOW_MEMORY_GROWTH can replace the buffer object underneath us. */
var B3_ISO_WORKER_SRC = [
'var mem = null, ctl = 0, file = null, fr = null;',
'self.onmessage = function (e) {',
'  var d = e.data;',
'  if (d.cmd !== "arm") return;',
'  mem = d.memory; ctl = d.ctl >> 2; file = d.file;',
'  fr = new FileReaderSync();',
'  var STATE = 0, OFF_LO = 1, OFF_HI = 2, LEN = 3, DST = 4, GOT = 5;',
'  var REQ = 1, DONE = 2, ERR = 3;',
'  for (;;) {',
'    var I32 = new Int32Array(mem.buffer);',
'    var cur = Atomics.load(I32, ctl + STATE);',
'    if (cur !== REQ) { Atomics.wait(I32, ctl + STATE, cur); continue; }',
'    var off = (I32[ctl + OFF_HI] >>> 0) * 4294967296 + (I32[ctl + OFF_LO] >>> 0);',
'    var len = I32[ctl + LEN] >>> 0;',
'    var dst = I32[ctl + DST] >>> 0;',
'    var st = DONE, got = 0;',
'    try {',
'      var end = Math.min(off + len, file.size);',
'      if (end > off) {',
'        var buf = fr.readAsArrayBuffer(file.slice(off, end));',
'        var src = new Uint8Array(buf);',
'        new Uint8Array(mem.buffer).set(src, dst);',
'        got = src.length;',
'      }',
'    } catch (ex) {',
'      st = ERR;',
'      console.error("[Burnout3] iso worker read failed: " + ex);',
'    }',
'    var I32b = new Int32Array(mem.buffer);',
'    I32b[ctl + GOT] = got;',
'    Atomics.store(I32b, ctl + STATE, st);',
'    Atomics.notify(I32b, ctl + STATE, 1);',
'  }',
'};'
].join('\n');

/* ======================================================== THE OPFS CACHE ====
 *
 * WHY NOT IndexedDB.  IDBFS keeps ONE IndexedDB VALUE PER FILE, and this
 * cache's files are not small: a track.obj is 16-77 MB.  A user reported
 *
 *     web: cache load failed (UnknownError: Failed to read large IndexedDB
 *          value) -- starting from a cold cache
 *
 * and Chrome means it literally -- the large values are the ones it would not
 * read back, while every small record came through.  A cache that loses
 * exactly its biggest files is worse than no cache at all, because what
 * survives is the BOOKKEEPING (the .stamps) that says the big files are
 * already done.  burnout3_isodata.c now refuses to believe that bookkeeping,
 * and this replaces the storage that produced it.
 *
 * OPFS is the origin's own private filesystem: real files, no per-value size
 * limit, and a multi-GB quota.  Measured here, headless Chromium, MAIN THREAD:
 * an 80 MB file (bigger than the largest track) writes in 420 ms and reads
 * back in 109 ms, byte-exact.
 *
 * WHY THE ASYNC API AND NOT createSyncAccessHandle.  The sync handles are
 * dedicated-worker-only, and this filesystem lives on the MAIN thread (see the
 * header above) -- so they are unreachable from here by construction.  They
 * are also unnecessary: the cache is not the live filesystem.  The live cache
 * stays MEMFS, exactly as IDBFS had it, and OPFS is only the two ends --
 * populate at boot, persist after a burst -- both of which are already
 * asynchronous moments.
 *
 * EVERY ENTRY POINT IS WRAPPED.  A browser without OPFS, a denied quota, a
 * corrupt directory, a read that throws: one line, and the session continues
 * from the disc.  That is the contract at the top of this file. */
var B3_OPFS_DIR = 'b3cache';                 /* under the origin's OPFS root */
var B3_CACHE_ROOT = '/app/build/.isocache';

/* What OPFS is known to hold: path -> "size:mtime".  Seeded by the load, kept
 * current by the persist, so a 25 MB track.obj is written once and not again
 * on every materialisation burst. */
var b3OpfsHave = Object.create(null);
var b3OpfsOn = false;

function b3OpfsPathParts(rel) {
    return rel.split('/').filter(function (s) { return s.length > 0; });
}

/* Walk to (and optionally create) the directory holding `rel`. */
async function b3OpfsDirFor(root, rel, create) {
    var parts = b3OpfsPathParts(rel);
    var d = root;
    for (var i = 0; i < parts.length - 1; i++)
        d = await d.getDirectoryHandle(parts[i], { create: !!create });
    return d;
}

/* Everything under `dir`, as relative paths. */
async function b3OpfsList(dir, prefix, outArr) {
    for await (var entry of dir.values()) {
        var rel = prefix ? prefix + '/' + entry.name : entry.name;
        if (entry.kind === 'directory') await b3OpfsList(entry, rel, outArr);
        else outArr.push({ rel: rel, handle: entry });
    }
    return outArr;
}

/* MEMFS mkdir -p for the cache mirror. */
function b3MkdirP(FS, abs) {
    var parts = b3OpfsPathParts(abs), cur = '';
    for (var i = 0; i < parts.length; i++) {
        cur += '/' + parts[i];
        try { FS.mkdir(cur); } catch (e) { /* exists */ }
    }
}

/* POPULATE: OPFS -> MEMFS.  Returns a short summary string for the log.
 *
 * Two things make this fast enough to sit in front of main(), and both are
 * lifted from what IDBFS does:
 *   * canOwn -- FS.writeFile normally COPIES into MEMFS, which for a ~950 MB
 *     cache is a second 950 MB of allocation and memcpy.  The buffer here is
 *     freshly decoded per file and never touched again, so MEMFS can take it.
 *   * a read window -- 4 800 sequential await round trips spend most of their
 *     time idle.  Reading in batches pipelines them. */
var B3_OPFS_BATCH = 24;

async function b3OpfsLoad(FS) {
    var root = await navigator.storage.getDirectory();
    var dir  = await root.getDirectoryHandle(B3_OPFS_DIR, { create: true });
    var all  = await b3OpfsList(dir, '', []);
    var n = 0, bytes = 0, bad = 0;

    for (var i = 0; i < all.length; i += B3_OPFS_BATCH) {
        var slice = all.slice(i, i + B3_OPFS_BATCH);
        var bufs = await Promise.all(slice.map(async function (it) {
            try {
                var f = await it.handle.getFile();
                return new Uint8Array(await f.arrayBuffer());
            } catch (e) {
                /* ONE bad file is not a bad cache.  Skip it and carry on --
                 * the resolver re-extracts whatever is missing, which is
                 * exactly what the IndexedDB failure should have done. */
                return null;
            }
        }));
        for (var k = 0; k < slice.length; k++) {
            var buf = bufs[k];
            if (!buf) { bad++; continue; }
            var abs = B3_CACHE_ROOT + '/' + slice[k].rel;
            b3MkdirP(FS, abs.substring(0, abs.lastIndexOf('/')));
            FS.writeFile(abs, buf, { canOwn: true });
            b3OpfsHave[slice[k].rel] =
                buf.length + ':' + FS.stat(abs).mtime.getTime();
            n++; bytes += buf.length;
        }
    }
    return n + ' files, ' + (bytes / 1048576).toFixed(1) + ' MiB' +
           (bad ? ', ' + bad + ' unreadable and skipped' : '');
}

/* Every regular file under the cache mount, as paths relative to it.
 *
 * LSTAT, NOT STAT, AND THAT IS LOAD-BEARING.  <cache>/.root/build is a SYMLINK
 * TO <cache> -- burnout3_isodata.c makes it so the car and art stages can write
 * the literal "build/cars/..." they emit.  stat() follows it, so a walk built
 * on stat() descends .root/build/.root/build/... forever and re-persists the
 * whole cache through every level of the cycle.  lstat() sees the symlink for
 * what it is, and symlinks are skipped: nothing under .root is a real file. */
function b3MemfsWalk(FS, abs, prefix, outArr) {
    var names;
    try { names = FS.readdir(abs); } catch (e) { return outArr; }
    for (var i = 0; i < names.length; i++) {
        var name = names[i];
        if (name === '.' || name === '..') continue;
        var sub = abs + '/' + name, rel = prefix ? prefix + '/' + name : name;
        var st;
        try { st = FS.lstat(sub); } catch (e) { continue; }
        if (FS.isLink(st.mode)) continue;
        if (FS.isDir(st.mode)) b3MemfsWalk(FS, sub, rel, outArr);
        else outArr.push({ rel: rel, abs: sub,
                           tag: st.size + ':' + st.mtime.getTime() });
    }
    return outArr;
}

/* PERSIST: MEMFS -> OPFS, only what changed.  Deletions matter too -- the
 * resolver now DROPS a stale .stamps entry, and a persistence layer that kept
 * resurrecting it would re-extract the same track on every single boot. */
async function b3OpfsPersist(FS) {
    var root = await navigator.storage.getDirectory();
    var dir  = await root.getDirectoryHandle(B3_OPFS_DIR, { create: true });
    var live = b3MemfsWalk(FS, B3_CACHE_ROOT, '', []);
    var seen = Object.create(null);
    var wrote = 0, bytes = 0, gone = 0;

    for (var i = 0; i < live.length; i++) {
        var it = live[i];
        seen[it.rel] = 1;
        if (b3OpfsHave[it.rel] === it.tag) continue;
        var data = FS.readFile(it.abs);
        var d = await b3OpfsDirFor(dir, it.rel, true);
        var fh = await d.getFileHandle(it.rel.split('/').pop(), { create: true });
        var w = await fh.createWritable();
        await w.write(data);
        await w.close();
        b3OpfsHave[it.rel] = it.tag;
        wrote++; bytes += data.length;
    }
    for (var rel in b3OpfsHave) {
        if (seen[rel]) continue;
        try {
            var dd = await b3OpfsDirFor(dir, rel, false);
            await dd.removeEntry(rel.split('/').pop());
        } catch (e) { /* already gone */ }
        delete b3OpfsHave[rel];
        gone++;
    }
    return { wrote: wrote, bytes: bytes, gone: gone };
}

/* Drop a legacy IDBFS store, once.  The directive is to drop it CLEANLY
 * rather than half-load it -- and a half-loaded IDBFS cache is precisely the
 * bug this replaces. */
async function b3OpfsDropLegacyIdb(out) {
    try {
        if (!indexedDB.databases) return;
        var dbs = await indexedDB.databases();
        for (var i = 0; i < dbs.length; i++) {
            if (dbs[i].name !== B3_CACHE_ROOT) continue;
            await new Promise(function (res) {
                var q = indexedDB.deleteDatabase(B3_CACHE_ROOT);
                q.onsuccess = q.onerror = q.onblocked = function () { res(1); };
            });
            out('[Burnout3] web: dropped the old IndexedDB cache (superseded ' +
                'by OPFS, which has no per-value size limit)');
        }
    } catch (e) { /* nothing to drop, or no permission to look */ }
}

/* Mount-time entry point.  Holds main() until the populate lands, exactly as
 * the IDBFS path did -- and lets go on every path, including the failing
 * ones, because a cache that cannot load must not stop the game. */
function b3OpfsInstall(FS, out, err) {
    if (typeof navigator === 'undefined' || !navigator.storage ||
        !navigator.storage.getDirectory) {
        Module['b3CachePersistent'] = false;
        out('[Burnout3] web: no OPFS here -- the cache is MEMFS for this ' +
            'session and everything comes from the disc');
        return;
    }
    Module['b3CachePersistent'] = true;
    Module['b3CachePersist'] = b3CachePersistOpfs;   /* only this mode has it */
    b3OpfsOn = true;
    addRunDependency('b3-cache-load');
    (async function () {
        try {
            await b3OpfsDropLegacyIdb(out);
            var summary = await b3OpfsLoad(FS);
            out('[Burnout3] web: cache loaded from OPFS (' + summary + ')');
        } catch (e) {
            b3OpfsOn = false;
            Module['b3CachePersistent'] = false;
            err('[Burnout3] web: OPFS cache unavailable (' + e +
                ') -- running from the disc, nothing will persist');
        }
        removeRunDependency('b3-cache-load');
    })();
}

/* Called from b3_web_cache_sync (web/b3_web_lib.js) after a materialisation
 * burst, and installed ONLY by the OPFS mode above.  Never throws into the
 * engine: a write that fails takes persistence down, not the session. */
function b3CachePersistOpfs(FS, out, err, done) {
    if (!b3OpfsOn) { done(); return; }
    b3OpfsPersist(FS).then(function (r) {
        if (r.wrote || r.gone)
            out('[Burnout3] web: cache -> OPFS (' + r.wrote + ' files, ' +
                (r.bytes / 1048576).toFixed(1) + ' MiB' +
                (r.gone ? ', ' + r.gone + ' removed' : '') + ')');
        done();
    }, function (e) {
        b3OpfsOn = false;
        Module['b3CachePersistent'] = false;
        err('[Burnout3] web: cache write failed (' + e +
            ') -- this session runs from the disc and will not persist');
        done();
    });
};

Module['preRun'] = Module['preRun'] || [];

Module['preRun'].push(function () {
    var iso = Module['b3IsoFile'];

    /* Module.b3Env -- applied to the engine's environment.  Done here because
     * ENV is only in scope inside the module's own code; the shell cannot
     * reach it from the page.  Every B3_* knob the desktop build reads from
     * the environment works through this. */
    var envIn = Module['b3Env'];
    if (envIn) {
        for (var k in envIn) {
            if (Object.prototype.hasOwnProperty.call(envIn, k)) ENV[k] = envIn[k];
        }
    }

    /* ------------------------------------------------- the canvas's name
     * The context comes from an OffscreenCanvas transferred to the game
     * thread, and Emscripten resolves WHICH canvas to transfer from the
     * selector baked in at link time (-sOFFSCREENCANVASES_TO_PTHREAD='#canvas')
     * -- NOT from Module.canvas.  A shell whose canvas carries a different id
     * would hand the engine a context-less thread and fail deep inside gl4es.
     *
     * So adopt the shell's canvas instead of demanding it be named: this runs
     * on the main thread in preRun, before run() spawns the game pthread and
     * therefore before the transfer is resolved. */
    var cv = Module['canvas'];
    if (cv && cv.id !== 'canvas' && !document.getElementById('canvas')) {
        out('[Burnout3] web: adopting the shell canvas (id "' +
            (cv.id || '') + '" -> "canvas") for the OffscreenCanvas transfer');
        cv.id = 'canvas';
    }

    /* ---------------------------------------------------------- the CWD */
    FS.mkdir('/app');
    FS.mkdir('/app/build');
    FS.chdir('/app');

    /* -------------------------------------------------------- the image */
    FS.mkdir('/iso');
    FS.writeFile('/iso/game.xiso', new Uint8Array(0));
    if (iso) {
        Module['b3IsoWorker'] = new Worker(
            URL.createObjectURL(new Blob([B3_ISO_WORKER_SRC],
                                         { type: 'text/javascript' })));
        out('[Burnout3] web: iso ' + (iso.size >>> 20) +
            ' MiB, served by the image bridge (no copy, read on demand)');
    } else {
        err('[Burnout3] web: no Module.b3IsoFile -- the engine will find no disc');
    }

    /* -------------------------------------------------------- the cache */
    var mode = Module['b3CacheMode'] || 'opfs';
    FS.mkdir('/app/build/.isocache');
    if (mode === 'opfs') {
        b3OpfsInstall(FS, out, err);
    } else if (mode === 'idbfs') {
        FS.mount(IDBFS, {}, '/app/build/.isocache');
        Module['b3CachePersistent'] = true;
        /* IDBFS starts EMPTY: the previous visit's contents only appear after
         * an explicit populate-direction syncfs.  Hold main() until it lands,
         * otherwise the engine sees a cold cache and re-extracts everything
         * it already had. */
        addRunDependency('b3-cache-load');
        FS.syncfs(true, function (e) {
            if (e) {
                err('[Burnout3] web: cache load failed (' + e +
                    ') -- starting from a cold cache');
            } else {
                out('[Burnout3] web: cache loaded from IndexedDB');
            }
            removeRunDependency('b3-cache-load');
        });
    } else {
        /* MEMFS fallback: nothing persists across visits, and every
         * materialised asset stays resident in the wasm heap for the whole
         * session on top of what the game itself holds.  A boot plus one
         * track is the order of 100-200 MiB of extracted assets, so this
         * roughly doubles the memory the tab needs.  Use it only where
         * IndexedDB is unavailable (private windows, some embeddings). */
        Module['b3CachePersistent'] = false;
        out('[Burnout3] web: cache is MEMFS -- nothing will persist, and the ' +
            'extracted assets stay in the heap for the session');
    }
});
