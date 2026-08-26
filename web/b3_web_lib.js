/* web/b3_web_lib.js -- the JS side of the web port's platform seam.
 *
 * This exists rather than EM_ASM blocks in the C so that src/ can stay on the
 * project's -std=c11 (EM_ASM requires -std=gnu*), and so that all of the
 * port's JavaScript lives in web/.
 */

addToLibrary({
    /* ------------------------------------------------------ the cache sync
     * Push the IDBFS cache mount out to IndexedDB.
     *
     * __proxy: 'async' -- under PROXY_TO_PTHREAD the caller is the game
     * worker, but the filesystem lives on the main browser thread.  Async
     * because nothing in the engine waits on the write: the assets are
     * already on disk as far as the running frame is concerned, and this only
     * decides whether the NEXT VISIT has to extract them again.
     *
     * A sync already in flight arms a repeat instead of queueing, so a burst
     * of small materialisations cannot pile up unbounded IndexedDB writes. */
    /* OFF THE CRITICAL PATH.  This used to write as soon as a burst finished,
     * which put a multi-hundred-megabyte cache write on the MAIN thread in the
     * middle of the load -- and the main thread is exactly what the game
     * thread's proxied read() calls are queued behind.  Measured: US_C1_V1's
     * cold TRACK GEOMETRY went 4.4 s -> 9.2 s with the write landing inside it.
     *
     * So the requests are DEBOUNCED.  Each one restarts a short timer and only
     * a quiet gap actually writes, which during a load means "once, after the
     * load".  Nothing is lost by waiting: the cache is an accelerator, and a
     * tab closed before the timer fires simply re-extracts from the disc next
     * time -- which is the guaranteed path anyway. */
    b3_web_cache_sync__proxy: 'async',
    b3_web_cache_sync: function () {
        if (!Module['b3CachePersistent']) return;
        var go = function () {
            Module['b3SyncBusy'] = 1;
            var done = function () {
                Module['b3SyncBusy'] = 0;
                if (Module['b3SyncAgain']) { Module['b3SyncAgain'] = 0; go(); }
            };
            /* OPFS (the default) owns its own writer in web/pre.js; the legacy
             * IDBFS mount still goes through syncfs.  Either way a failure is
             * ONE LINE and the session carries on from the disc. */
            if (Module['b3CachePersist']) {
                Module['b3CachePersist'](FS, out, err, done);
            } else {
                FS.syncfs(false, function (e) {
                    if (e) err('[Burnout3] web: cache sync failed: ' + e);
                    done();
                });
            }
        };
        if (Module['b3SyncBusy']) { Module['b3SyncAgain'] = 1; return; }
        if (Module['b3SyncTimer']) clearTimeout(Module['b3SyncTimer']);
        Module['b3SyncTimer'] = setTimeout(function () {
            Module['b3SyncTimer'] = 0;
            go();
        }, 2500);
    },

    /* ----------------------------------------------------- the image bridge
     * Start the helper worker that owns the File and serves synchronous reads
     * of it into the shared wasm heap.  See web/b3_web.h for why the image
     * cannot go through the filesystem at all.
     *
     * __proxy: 'sync' -- the File and the helper worker live on the main
     * browser thread, and the caller needs the size back before it can go on.
     * This runs ONCE; every read after it is pure wasm atomics between the
     * game thread and the helper, with no main thread involvement. */
    /* ------------------------------------------------------ the GL diagnosis
     * What KIND of context did the game thread actually get?  The difference
     * between a transferred OffscreenCanvas, a proxied main-thread context and
     * a plain one decides whether a frame can ever reach the screen, and none
     * of it is visible from C.  Runs on the CALLING thread (the game thread)
     * on purpose -- that is the thread whose answer matters. */
    b3_web_gl_diag: function (dst, cap) {
        var s;
        try {
            var oc = (typeof GL !== 'undefined' && GL.offscreenCanvases)
                     ? Object.keys(GL.offscreenCanvases).join(',') : '-';
            var c  = (typeof GL !== 'undefined') ? GL.currentContext : null;
            s = 'worker=' + (ENVIRONMENT_IS_WORKER ? 1 : 0) +
                ' offscreenCanvases=[' + oc + ']' +
                ' ctx=' + (c ? 'yes' : 'NO') +
                ' proxied=' + (typeof GL !== 'undefined' && GL.currentContextIsProxied ? 1 : 0) +
                ' explicitSwap=' + (c && c.attributes ? (c.attributes.explicitSwapControl ? 1 : 0) : '?') +
                ' GLctx.commit=' + (c && c.GLctx && c.GLctx.commit ? 'present' : 'ABSENT') +
                ' defaultFbo=' + (c && c.defaultFbo ? 'yes' : 'no') +
                ' canvasType=' + (c && c.GLctx && c.GLctx.canvas
                                  ? (c.GLctx.canvas.constructor
                                     ? c.GLctx.canvas.constructor.name : '?') : '?');
        } catch (e) {
            s = 'diag failed: ' + e;
        }
        stringToUTF8(s, dst, cap);
    },

    /* ================================================= WHICH GPU, REALLY ====
     * THE LINE THAT SETTLES THE FIRST QUESTION.  A player reporting
     * SwiftShader-class numbers on an RTX 3090 has, at that point, only ever
     * read the renderer string of the context the PAGE made -- and the page's
     * context is not the one this game draws through.  The engine's context
     * belongs to a worker-local OffscreenCanvas on the game thread, created by
     * b3_web_worker_ctx() below, and a browser is free to back that with
     * software while the main thread's is hardware.  Nothing printed before
     * this told anybody which of the two they had.
     *
     * So both are printed, and the WORKER's is the one that decides.  Also
     * reported, because each answers a specific question further down this
     * file:
     *
     *   readFormat/readType   gl4es' glCopyTexSubImage2D takes a SYNCHRONOUS
     *                         glReadPixels + re-upload path unless the grab
     *                         texture's format matches either RGBA/UNSIGNED_BYTE
     *                         or exactly this pair (gl4es texture_read.c:151).
     *                         This is the input to that decision.
     *   timer                 whether EXT_disjoint_timer_query* exists, i.e.
     *                         whether B3_WEB_HWPROF can report GPU time at all.
     *   ctxVersion            1 or 2.  The port asks for 2 and falls back to 1
     *                         (b3_web_gl_create_context), and the two differ in
     *                         more than the timer query: MSAA and 24-bit depth
     *                         in the aftereffects chain need 2 as well.  This
     *                         is the field that says which one was granted. */
    $b3gpu_line__deps: ['$GL'],
    $b3gpu_line: () => {
        try {
            var c = GL.currentContext;
            var g = c && c.GLctx;
            if (!g) return 'NO CONTEXT';
            var dbg = null, name = null, vend = null;
            try { dbg = g.getExtension('WEBGL_debug_renderer_info'); } catch (e) {}
            try {
                name = dbg ? g.getParameter(dbg.UNMASKED_RENDERER_WEBGL)
                           : ('masked: ' + g.getParameter(g.RENDERER));
                vend = dbg ? g.getParameter(dbg.UNMASKED_VENDOR_WEBGL) : '';
            } catch (e2) { name = 'unreadable: ' + e2; }
            var rf = 0, rt = 0;
            try {
                rf = g.getParameter(0x8B9B /*IMPLEMENTATION_COLOR_READ_FORMAT*/);
                rt = g.getParameter(0x8B9A /*IMPLEMENTATION_COLOR_READ_TYPE*/);
            } catch (e3) {}
            var tq = null;
            try {
                tq = g.getExtension('EXT_disjoint_timer_query_webgl2')
                     ? 'webgl2' : (g.getExtension('EXT_disjoint_timer_query')
                                   ? 'webgl1' : 'ABSENT');
            } catch (e4) { tq = 'ABSENT'; }
            var enam = {0x1907: 'RGB', 0x1908: 'RGBA', 0x80E0: 'BGR',
                        0x80E1: 'BGRA', 0x1401: 'UNSIGNED_BYTE',
                        0x8363: 'UNSIGNED_SHORT_5_6_5',
                        0x8033: 'UNSIGNED_SHORT_4_4_4_4',
                        0x8034: 'UNSIGNED_SHORT_5_5_5_1'};
            return name + (vend ? '  [vendor ' + vend + ']' : '') +
                   '  ctx=webgl' + (c.version || 1) +
                   ' readFormat=' + (enam[rf] || ('0x' + rf.toString(16))) +
                   '/' + (enam[rt] || ('0x' + rt.toString(16))) +
                   ' timerQuery=' + tq;
        } catch (e) {
            return 'diag failed: ' + e;
        }
    },

    /* The game thread's own context -- the one that matters. */
    b3_web_gpu_here__deps: ['$b3gpu_line'],
    b3_web_gpu_here: (dst, cap) => { stringToUTF8(b3gpu_line(), dst, cap); },

    /* The page's context, for contrast.  Deliberately a THROWAWAY canvas:
     * getContext() is one-shot per element and the visible one is spoken for
     * (see b3_web_present_probe below). */
    b3_web_gpu_main__proxy: 'sync',
    b3_web_gpu_main: (dst, cap) => {
        var s;
        try {
            var t = document.createElement('canvas');
            t.width = t.height = 4;
            var g = t.getContext('webgl') || t.getContext('experimental-webgl');
            if (!g) { s = 'NO CONTEXT'; }
            else {
                var dbg = g.getExtension('WEBGL_debug_renderer_info');
                s = dbg ? g.getParameter(dbg.UNMASKED_RENDERER_WEBGL)
                        : ('masked: ' + g.getParameter(g.RENDERER));
            }
        } catch (e) {
            s = 'probe failed: ' + e;
        }
        stringToUTF8(s, dst, cap);
    },

    /* ==================================================== THE HARDWARE PROFILE
     * B3_WEB_HWPROF=<n>.  Everything here is OFF unless that is set, and when
     * it is off not one wrapper is installed -- the context object is the one
     * the browser handed over, untouched.
     *
     * WHAT IT COUNTS AND WHY EACH ONE IS HERE.  These are not "interesting
     * statistics"; each slot is a candidate answer to "why is a 640x480 frame
     * costing 40 ms on a 3090", and between them they cover every way this
     * port can stall a GPU:
     *
     *   READPX   glReadPixels is the hardest synchronous point WebGL has: the
     *            GPU must retire everything queued, the pixels come back over
     *            the bus, and in Chrome the command buffer blocks the calling
     *            thread on the GPU process for the whole round trip.  The
     *            engine itself calls it only from capture paths -- so ONE PER
     *            FRAME HERE MEANS gl4es IS CALLING IT, which it does when
     *            glCopyTexSubImage2D cannot take its fast path.  This counter
     *            exists to catch exactly that, and it is the single most
     *            diagnostic number on the line.
     *   COPY     the postfx frame grab.  Paired with READPX it says whether
     *            the grab is a GPU-side blit or a readback in disguise.
     *   TEX/BUF  upload volume.  The animated-texture path is a suspect until
     *            measured: if it re-uploaded texels every frame TEX would be
     *            megabytes, and if it binds pre-uploaded frames it is zero.
     *   GETERR / GETPARAM   every one is a blocking round trip to the command
     *            buffer even on the direct path (measured at ~100 us in
     *            b3_web_gl_bench).  gl4es answers most glGet* from its own
     *            state; these count the ones that get through anyway.
     *   FLUSH    glFinish/glFlush from anywhere.
     *   FBO      bindFramebuffer, because gl4es issues an unconditional
     *            glGetError after EVERY ONE (its framebuffers.c:262), so this
     *            count is also a lower bound on forced round trips.
     *
     * Bytes are kept in KiB and pixels in kilopixels so an int32 cannot wrap
     * over a long session. */
    $b3hw: { on: 0, ctl: 0, g: null, ext: null, ver: 1, q: [], cur: null,
             disjoint: 0, pn: {}, started: 0, landed: 0 },

    /* The GL enums worth naming in the getParameter report.  Anything not
     * here prints as hex, which is still enough to grep gl4es for. */
    $b3hw_pname: {
        0x0B44: 'CULL_FACE', 0x0B45: 'CULL_FACE_MODE', 0x0BA6: 'MODELVIEW_MATRIX',
        0x0BA7: 'PROJECTION_MATRIX', 0x0BA2: 'VIEWPORT', 0x0C10: 'SCISSOR_BOX',
        0x0C11: 'SCISSOR_TEST', 0x0BE2: 'BLEND', 0x0B71: 'DEPTH_TEST',
        0x0B90: 'STENCIL_TEST', 0x0C22: 'COLOR_CLEAR_VALUE',
        0x0B73: 'DEPTH_CLEAR_VALUE', 0x0B72: 'DEPTH_WRITEMASK',
        0x0B74: 'DEPTH_FUNC', 0x0C23: 'COLOR_WRITEMASK',
        0x0B21: 'LINE_WIDTH', 0x84E0: 'ACTIVE_TEXTURE',
        0x8069: 'TEXTURE_BINDING_2D', 0x8514: 'TEXTURE_BINDING_CUBE_MAP',
        0x8894: 'ARRAY_BUFFER_BINDING', 0x8895: 'ELEMENT_ARRAY_BUFFER_BINDING',
        0x8CA6: 'FRAMEBUFFER_BINDING', 0x8CA7: 'RENDERBUFFER_BINDING',
        0x8B8D: 'CURRENT_PROGRAM', 0x0D33: 'MAX_TEXTURE_SIZE',
        0x0D57: 'SAMPLES', 0x80A9: 'SAMPLES_ARB', 0x0D50: 'DEPTH_BITS',
        0x8B9B: 'IMPLEMENTATION_COLOR_READ_FORMAT',
        0x8B9A: 'IMPLEMENTATION_COLOR_READ_TYPE',
        0x8FBB: 'GPU_DISJOINT_EXT', 0x8038: 'POLYGON_OFFSET_FACTOR',
        0x2A00: 'POLYGON_OFFSET_UNITS', 0x8005: 'BLEND_COLOR',
        0x0D32: 'MAX_VIEWPORT_DIMS', 0x846D: 'ALIASED_POINT_SIZE_RANGE',
        0x846E: 'ALIASED_LINE_WIDTH_RANGE'
    },

    /* The same growth-safe Int32 view the present control block uses. */
    $b3hw_i32__deps: ['$b3gl_i32'],
    $b3hw_i32: () => b3gl_i32(),

    /* Bytes a WebGL pixel/buffer argument actually carries.  Emscripten hands
     * these in as typed-array views onto the wasm heap, so byteLength is the
     * truth; an ImageBitmap/HTMLImage source has none and is estimated from
     * its own dimensions. */
    $b3hw_bytes: (v) => {
        if (v == null) return 0;
        /* bufferData(target, SIZE, usage) -- an allocation, not an upload.
         * It is counted as a CALL and deliberately not as BYTES: the whole
         * point of the byte column is how much data crosses to the GPU per
         * frame, and an orphaning bufferData(NULL) crosses none. */
        if (typeof v === 'number') return 0;
        if (typeof v.byteLength === 'number') return v.byteLength;
        if (typeof v.width === 'number' && typeof v.height === 'number')
            return v.width * v.height * 4;
        return 0;
    },

    $b3hw_hook__deps: ['$GL', '$b3hw', '$b3hw_i32', '$b3hw_bytes'],
    $b3hw_hook: (ctl) => {
        var c = GL.currentContext;
        if (!c || !c.GLctx) return 0;
        var g = c.GLctx;
        if (g.b3HwHooked) return 2;
        g.b3HwHooked = 1;
        b3hw.on = 1; b3hw.ctl = ctl; b3hw.g = g; b3hw.ver = c.version || 1;
        var b = ctl >> 2;

        /* name -> [count slot, size slot, how to size it from the arguments] */
        var TEX = 0, TEXKB = 1, COPY = 2, COPYKPX = 3, BUF = 4, BUFKB = 5,
            RDPX = 6, RDPXKPX = 7, GETERR = 8, GETPARAM = 9, FLUSH = 10,
            FBO = 11;
        var wrap = function (name, cslot, sslot, size) {
            var fn = g[name];
            if (typeof fn !== 'function') return;
            g[name] = function () {
                var I = b3hw_i32();
                Atomics.add(I, b + cslot, 1);
                if (sslot >= 0) {
                    var n = 0;
                    try { n = size(arguments); } catch (e) { n = 0; }
                    if (n > 0) Atomics.add(I, b + sslot, n);
                }
                return fn.apply(g, arguments);
            };
        };
        var last = (a) => b3hw_bytes(a[a.length - 1]);
        var kb   = (a) => Math.round(last(a) / 1024);

        wrap('texImage2D',    TEX, TEXKB, kb);
        wrap('texSubImage2D', TEX, TEXKB, kb);
        wrap('compressedTexImage2D',    TEX, TEXKB, kb);
        wrap('compressedTexSubImage2D', TEX, TEXKB, kb);
        /* The two copies carry their extent at different argument positions:
         *   copyTexImage2D   (target, level, format, x, y, w, h, border)
         *   copyTexSubImage2D(target, level, xoff, yoff, x, y, w, h)
         * so w,h are [5],[6] and [6],[7] respectively. */
        wrap('copyTexImage2D',    COPY, COPYKPX,
             (a) => Math.round((a[5] * a[6]) / 1024));
        wrap('copyTexSubImage2D', COPY, COPYKPX,
             (a) => Math.round((a[6] * a[7]) / 1024));
        wrap('bufferData',    BUF, BUFKB, (a) => Math.round(b3hw_bytes(a[1]) / 1024));
        wrap('bufferSubData', BUF, BUFKB, (a) => Math.round(b3hw_bytes(a[2]) / 1024));
        wrap('readPixels',    RDPX, RDPXKPX,
             (a) => Math.round((a[2] * a[3]) / 1024));
        wrap('getError',      GETERR,   -1, null);
        wrap('finish',        FLUSH,    -1, null);
        wrap('flush',         FLUSH,    -1, null);
        wrap('bindFramebuffer', FBO,    -1, null);

        /* getParameter is wrapped by hand rather than through wrap(), because
         * the COUNT alone is not actionable.  Every one of these is a blocking
         * round trip to the command buffer, and gl4es answers most glGet* from
         * its own CPU state -- so the ones that get through are a specific,
         * findable list of enums, and knowing WHICH is the difference between
         * "36 round trips a frame" and a patch.  The tally is plain JS on
         * whichever thread the context is on; nothing outside the dump reads
         * it. */
        (function () {
            var fn = g.getParameter;
            if (typeof fn !== 'function') return;
            g.getParameter = function (pname) {
                var I = b3hw_i32();
                Atomics.add(I, b + GETPARAM, 1);
                b3hw.pn[pname] = (b3hw.pn[pname] || 0) + 1;
                return fn.call(g, pname);
            };
        })();

        /* ---- the GPU's own clock, where the browser offers one.
         * EXT_disjoint_timer_query_webgl2 needs a WebGL 2 context and this
         * port asks for WebGL 1, so ABSENT is the expected answer and the
         * profile line says "gpu n/a" rather than pretending. */
        try {
            b3hw.ext = g.getExtension('EXT_disjoint_timer_query_webgl2');
            if (b3hw.ext) b3hw.ver = 2;
            else { b3hw.ext = g.getExtension('EXT_disjoint_timer_query');
                   if (b3hw.ext) b3hw.ver = 1; }
        } catch (e) { b3hw.ext = null; }
        return 1;
    },

    /* Bracket one frame of GL work.  Called from the present, which is the one
     * point in the loop that happens exactly once a frame: end the query that
     * has been open since the last present, start the next.  Results are
     * collected lazily -- a timer query is not available for several frames,
     * and waiting for one would be the very stall this is trying to find. */
    $b3hw_gpu_tick__deps: ['$b3hw', '$b3hw_i32'],
    $b3hw_gpu_tick: () => {
        var e = b3hw.ext, g = b3hw.g;
        if (!e || !g) return;
        var GPU_US = 14, GPU_N = 15, b = b3hw.ctl >> 2;
        try {
            if (b3hw.cur) {
                if (b3hw.ver === 2) g.endQuery(e.TIME_ELAPSED_EXT);
                else                e.endQueryEXT(e.TIME_ELAPSED_EXT);
                b3hw.q.push(b3hw.cur);
                b3hw.cur = null;
            }
            /* harvest whatever has landed.  The disjoint flag is read ONCE per
             * tick, not once per pending query: it is itself a getParameter,
             * i.e. a blocking round trip, and a probe that inflates the very
             * counter it reports is worse than no probe. */
            var I = b3hw_i32(), keep = [];
            var dis = g.getParameter(e.GPU_DISJOINT_EXT);
            if (dis) b3hw.disjoint++;
            for (var i = 0; i < b3hw.q.length; i++) {
                var q = b3hw.q[i], ok, ns;
                if (b3hw.ver === 2) {
                    ok = g.getQueryParameter(q, g.QUERY_RESULT_AVAILABLE);
                } else {
                    ok = e.getQueryObjectEXT(q, e.QUERY_RESULT_AVAILABLE_EXT);
                }
                if (!ok) { keep.push(q); continue; }
                ns = (b3hw.ver === 2) ? g.getQueryParameter(q, g.QUERY_RESULT)
                                      : e.getQueryObjectEXT(q, e.QUERY_RESULT_EXT);
                if (!dis && ns > 0) {
                    Atomics.add(I, b + GPU_US, Math.round(ns / 1000));
                    Atomics.add(I, b + GPU_N, 1);
                    b3hw.landed = 1;
                }
                if (b3hw.ver === 2) g.deleteQuery(q); else e.deleteQueryEXT(q);
            }
            /* A query that never lands must not accumulate: Chrome EXPOSES
             * EXT_disjoint_timer_query on a WebGL 1 context and then never
             * services TIME_ELAPSED, so without this bound the pending list
             * would grow for the life of the run.  Past the bound the probe
             * retires itself and the report says why. */
            b3hw.q = keep.length > 8 ? keep.slice(keep.length - 8) : keep;
            if (b3hw.started > 240 && !b3hw.landed) { b3hw.ext = null; return; }
            /* open the next frame's */
            var nq = (b3hw.ver === 2) ? g.createQuery() : e.createQueryEXT();
            if (b3hw.ver === 2) g.beginQuery(e.TIME_ELAPSED_EXT, nq);
            else                e.beginQueryEXT(e.TIME_ELAPSED_EXT, nq);
            b3hw.cur = nq;
            b3hw.started++;
        } catch (ex) {
            b3hw.ext = null;          /* one failure retires the whole probe */
        }
    },

    /* WHICH glGet* got through, and how the GPU clock is doing.  Printed by
     * b3_web_hw_tick() on the same denominator as the rest of the profile. */
    $b3hw_dump__deps: ['$b3hw', '$b3hw_pname'],
    $b3hw_dump: (frames) => {
        var ks = Object.keys(b3hw.pn).filter((k) => b3hw.pn[k] > 0);
        if (ks.length) {
            ks.sort((a, b) => b3hw.pn[b] - b3hw.pn[a]);
            out('[Burnout3] [hwsync] getParameter by enum, per frame (each one '
                + 'is a blocking round trip): ' +
                ks.slice(0, 12).map((k) =>
                    (b3hw_pname[k] || ('0x' + (+k).toString(16))) + ' ' +
                    (b3hw.pn[k] / (frames || 1)).toFixed(2)).join(', ') +
                (ks.length > 12 ? ', +' + (ks.length - 12) + ' more' : ''));
            for (var i = 0; i < ks.length; i++) b3hw.pn[ks[i]] = 0;
        }
        /* MEASURED 2026-08-25, and the reason this line no longer blames the
         * context version: moving the port to WebGL 2 got the RIGHT extension
         * (EXT_disjoint_timer_query_webgl2, so ver 2 above and the core
         * beginQuery/endQuery/getQueryParameter path) and Chrome STILL never
         * makes a TIME_ELAPSED result available -- 241 queries, no result, on
         * both SwiftShader and ANGLE/Vulkan on an RTX 3090, headless.  So the
         * timer query is not a WebGL 1 limitation this port has now escaped;
         * it is unavailable here either way, and B3_WEB_HWPROF_SYNC's timed
         * glFinish stays the only GPU-time number this harness can report.
         * Do not build a measurement on the query without re-checking this. */
        if (b3hw.started && !b3hw.landed)
            out('[Burnout3] [hwsync] EXT_disjoint_timer_query' +
                (b3hw.ver === 2 ? '_webgl2' : '') + ' is EXPOSED on this ' +
                'WebGL ' + b3hw.ver + ' context but returned no result in ' +
                b3hw.started + ' queries -- Chrome does not service ' +
                'TIME_ELAPSED here. Use B3_WEB_HWPROF_SYNC=1 for a timed ' +
                'glFinish instead.');
    },

    b3_web_hwprof_arm__deps: ['$b3hw_hook'],
    b3_web_hwprof_arm: (ctl) => b3hw_hook(ctl),

    b3_web_hwprof_arm_main__proxy: 'sync',
    b3_web_hwprof_arm_main__deps: ['$b3hw_hook'],
    b3_web_hwprof_arm_main: (ctl) => b3hw_hook(ctl),

    b3_web_hwprof_dump__deps: ['$b3hw_dump'],
    b3_web_hwprof_dump: (frames) => b3hw_dump(frames),

    b3_web_hwprof_dump_main__proxy: 'sync',
    b3_web_hwprof_dump_main__deps: ['$b3hw_dump'],
    b3_web_hwprof_dump_main: (frames) => b3hw_dump(frames),

    /* ===================================================== THE DIRECT CONTEXT
     * See web/b3_web.c for WHY this exists.  The short version: a WebGL
     * context created on the main thread and used from the game worker makes
     * EVERY gl* call a cross-thread dispatch, and a great many of them are
     * SYNCHRONOUS round trips (Emscripten's system/lib/gl/webgl1.c dispatches
     * glDrawElements, glVertexAttribPointer, glGetError, glTexParameteri and
     * 64 others through emscripten_sync_run_in_main_runtime_thread).  A
     * renderer drawing through the GL 2.1 compatibility surface makes tens of
     * thousands of calls a frame, so the port is bound by the thread boundary,
     * not by the GPU.
     *
     * The fix is to give the game thread a context IT OWNS.  Emscripten
     * decides direct-vs-proxied per call, at run time, from a TLS flag that
     * webgl1.c sets in emscripten_webgl_make_context_current(): if the
     * handle's owning thread is this thread, the call goes straight to JS.
     * GL.registerContext() stamps the owning thread as whoever calls it -- so
     * creating the context HERE, on the game worker, flips every one of those
     * calls to the direct path with no change to gl4es or to the engine.
     *
     * The canvas is a WORKER-LOCAL `new OffscreenCanvas`, not the transferred
     * DOM one, because a transferred canvas presents only when the worker
     * returns to its event loop and this game's frame loop never does.  A
     * worker-local one is never presented at all -- it is snapshotted with
     * transferToImageBitmap() and the bitmap is handed to the main thread.
     * One postMessage a frame instead of tens of thousands of dispatches. */
    $b3gl: { oc: null, ctl: 0, view: null, buf: null, on: 0 },

    /* The shared control block, four int32s.  Written from BOTH threads, so
     * every access is atomic and the view is rebuilt when memory growth
     * replaces the backing SharedArrayBuffer. */
    $b3gl_i32__deps: ['$b3gl'],
    $b3gl_i32: () => {
        if (b3gl.buf !== wasmMemory.buffer) {
            b3gl.buf = wasmMemory.buffer;
            b3gl.view = new Int32Array(b3gl.buf);
        }
        return b3gl.view;
    },

    /* ================================================ THE VIEWPORT, PUSHED
     *
     * WHY THIS EXISTS.  b3_web_defend_pin() has to know how big the canvas'
     * CSS box is, and it used to ask -- every 30 presents, from the game
     * worker, through emscripten_get_element_css_size() and
     * emscripten_get_device_pixel_ratio().  BOTH of those are
     * `__proxy: 'sync'` (emsdk src/lib/libhtml5.js), so twice a second the
     * GAME LOOP BLOCKED until the main thread got round to answering.
     *
     * That is why a main-thread burst shows up as a frame-rate dip the main
     * thread never touched: a flood of proxied console.log (FULL SLAM lines,
     * crash-trace chatter), an OPFS cache write, a GC -- the loop is parked
     * behind whichever of them is in the queue, and g_real_fps, measured on
     * the worker over a 30-frame window, absorbs all of it.  A user reported
     * exactly this shape: real 19.7 fps for ~3 windows, full recovery.
     *
     * SO THE READ IS INVERTED.  The main thread OWNS the measurement and
     * writes it into shared memory whenever the browser says it changed --
     * ResizeObserver for the element's box, a re-armed matchMedia for
     * devicePixelRatio (there is no dpr event; the resolution media query
     * re-arm is the standard trick), and window resize as a backstop.  The
     * worker then reads four int32s out of its own heap and never blocks.
     *
     * A SEQLOCK, because three numbers are written and read without a mutex:
     * the writer bumps `seq` to odd, stores, bumps it to even; the reader
     * retries while it is odd or changed underneath.  Contention is nil
     * (writes happen on a resize, reads twice a second) so a spin is free.
     *
     * Returns 1 if the watcher is installed and the slot now holds a real
     * measurement.  0 means the caller keeps the old sync-proxied path -- a
     * browser without ResizeObserver still works, just as slowly as before. */
    b3_web_viewport_watch__proxy: 'sync',
    b3_web_viewport_watch__deps: ['$b3gl_i32'],
    b3_web_viewport_watch: function (slot) {
        try {
            var el = Module['canvas'] || document.getElementById('canvas');
            if (!el || typeof ResizeObserver === 'undefined') return 0;
            var base = slot >> 2;
            var write = function () {
                var qw, qh, qd;
                /* MEASURE FIRST, OUTSIDE THE SEQLOCK.  Everything that can
                 * reasonably throw -- a detached element, a layout the browser
                 * refuses -- happens here, where throwing costs nothing but a
                 * skipped update. */
                try {
                    var r = el.getBoundingClientRect();
                    /* Fixed point, because the slot is integers and the
                     * fractions are the whole point -- a box is 1920.4 CSS px,
                     * not 1920, and that is what the rounding upstream trips
                     * over. Q10 for the box (4 M fits an int32 at 3840 px),
                     * Q20 for the ratio. */
                    qw = Math.round(r.width * 1024);
                    qh = Math.round(r.height * 1024);
                    qd = Math.round((window.devicePixelRatio || 1) * 1048576);
                } catch (e) {
                    return;                  /* keep the last good measurement */
                }
                try {
                    /* b3gl_i32() every time, NOT a captured view: this build
                     * is ALLOW_MEMORY_GROWTH, and growth replaces the backing
                     * SharedArrayBuffer and detaches any view made before it. */
                    var i32 = b3gl_i32();
                    Atomics.add(i32, base, 1);            /* -> odd */
                    i32[base + 1] = qw;
                    i32[base + 2] = qh;
                    i32[base + 3] = qd;
                    Atomics.add(i32, base, 1);            /* -> even */
                } catch (e) {
                    /* Growth on another thread can detach the view BETWEEN the
                     * two adds.  Leaving the counter ODD would wedge every
                     * future read -- the reader would spin its retries out and
                     * give up for the rest of the session, and the viewport
                     * would silently stop following.  Put it back even. */
                    try {
                        var v = b3gl_i32();
                        if (Atomics.load(v, base) & 1) Atomics.add(v, base, 1);
                    } catch (e2) { /* nothing further is possible */ }
                }
            };
            write();
            new ResizeObserver(write).observe(el);
            window.addEventListener('resize', write);
            /* dpr has no event of its own.  A resolution media query fires
             * once when the ratio leaves the queried value, so it is re-armed
             * against the NEW ratio each time. */
            var armDpr = function () {
                try {
                    var mq = window.matchMedia(
                        '(resolution: ' + (window.devicePixelRatio || 1) +
                        'dppx)');
                    mq.addEventListener('change', function () {
                        write();
                        armDpr();
                    }, { once: true });
                } catch (e) { /* older engine: the resize backstop covers it */ }
            };
            armDpr();
            return 1;
        } catch (e) {
            return 0;
        }
    },

    /* Can the visible canvas take a bitmaprenderer context?  Asked BEFORE the
     * worker context is created, and deliberately asked of a THROWAWAY canvas:
     * getContext() is one-shot per element, so probing the real one would
     * spend it and leave the proxied fallback unable to make a WebGL context
     * on it. */
    b3_web_present_probe__proxy: 'sync',
    b3_web_present_probe: function () {
        try {
            var t = document.createElement('canvas');
            if (!t.getContext('bitmaprenderer')) return 0;
            return (Module['canvas'] || document.getElementById('canvas')) ? 1 : 0;
        } catch (e) {
            return 0;
        }
    },

    /* Main thread: adopt the visible canvas as a bitmaprenderer sink and
     * listen for the game worker's frames.
     *
     * The listener is added to the pthread's Worker with addEventListener, so
     * it runs ALONGSIDE Emscripten's own worker.onmessage rather than
     * replacing it.  A message with no `cmd` field falls through Emscripten's
     * handler untouched (src/lib/libpthread.js: `default: if (cmd) err(...)`),
     * which is why the frame messages are keyed on `b3` instead. */
    b3_web_present_init__proxy: 'sync',
    b3_web_present_init__deps: ['$b3gl', '$b3gl_i32', '$PThread'],
    b3_web_present_init: function (tid, ctl, w, h) {
        var worker = PThread.pthreads[tid];
        if (!worker) {
            err('[Burnout3] web: no worker for thread ' + tid +
                ' -- cannot present');
            return 0;
        }
        var cv = Module['canvas'] || document.getElementById('canvas');
        var bctx = cv && cv.getContext('bitmaprenderer');
        if (!bctx) return 0;
        if (cv.width !== w || cv.height !== h) { cv.width = w; cv.height = h; }

        var latest = null, raf = 0, wd = 0;

        /* THE CONSUMER.  transferFromImageBitmap() TAKES OWNERSHIP of the
         * bitmap and closes it, so the only bitmap this side must close by
         * hand is one it drops. */
        var draw = function () {
            if (raf) { cancelAnimationFrame(raf); raf = 0; }
            if (wd) { clearTimeout(wd); wd = 0; }
            if (!latest) return;
            var b = latest; latest = null;
            bctx.transferFromImageBitmap(b);
            Atomics.sub(b3gl_i32(), (ctl >> 2) + 0, 1);
        };

        worker.addEventListener('message', function (e) {
            var d = e.data;
            if (!d || d.b3 !== 'f') return;
            if (latest) {
                /* The page did not paint since the last frame arrived: this
                 * one supersedes it.  Drop the STALE one, never the new one --
                 * a dropped frame must never be the newest state. */
                latest.close();
                latest = null;
                Atomics.sub(b3gl_i32(), (ctl >> 2) + 0, 1);
                Atomics.add(b3gl_i32(), (ctl >> 2) + 3, 1);
            }
            latest = d.b;
            /* rAF is the pacing source; the timeout is a WATCHDOG for the case
             * where the page is not being composited at all (a background tab,
             * some headless configurations) and rAF simply never fires. */
            if (!raf) raf = requestAnimationFrame(draw);
            if (!wd) wd = setTimeout(draw, 250);
        });
        return 1;
    },

    /* Game worker: make the OffscreenCanvas and a context this thread OWNS.
     * Returns the Emscripten context handle, or 0 to fall back to proxying.
     *
     * `major` is the WebGL version b3_web.c wants (2 by default).  The walk is
     * 2-then-1 and EACH ATTEMPT GETS ITS OWN OffscreenCanvas: a canvas whose
     * getContext() has succeeded is spent, and while a getContext() that
     * returned null is documented not to settle the context mode, an
     * OffscreenCanvas costs nothing to re-make and this is not the place to
     * lean on that.  The version that came out is read back off the context
     * object by b3_web_ctx_version(), never assumed from `major`. */
    b3_web_worker_ctx__deps: ['$b3gl', '$GL'],
    b3_web_worker_ctx: function (w, h, ctl, depth, stencil, antialias, major) {
        if (typeof OffscreenCanvas === 'undefined') return 0;
        var attempt = function (ver) {
            var oc = new OffscreenCanvas(w, h);
            /* The same attributes b3_web.c asks for, minus the two that only
             * meant anything under proxying: no offscreen back buffer (this
             * context draws straight into the OffscreenCanvas's own default
             * framebuffer, which is what transferToImageBitmap snapshots) and
             * no proxying. */
            var handle = GL.createContext(oc, {
                alpha: false,
                depth: !!depth,
                stencil: !!stencil,
                antialias: !!antialias,
                premultipliedAlpha: false,
                preserveDrawingBuffer: false,
                failIfMajorPerformanceCaveat: false,
                powerPreference: 'high-performance',
                majorVersion: ver,
                minorVersion: 0,
                enableExtensionsByDefault: 1,
                explicitSwapControl: 0,
                proxyContextToMainThread: 0,
                renderViaOffscreenBackBuffer: 0
            });
            if (!handle) return 0;
            b3gl.oc = oc;
            b3gl.ctl = ctl;
            return handle;
        };
        try {
            var h2 = (major >= 2) ? attempt(2) : 0;
            if (h2) return h2;
            if (major >= 2)
                err('[Burnout3] web: no worker-local WebGL 2 context -- '
                    + 'falling back to WebGL 1');
            return attempt(1);
        } catch (e) {
            err('[Burnout3] web: worker-local WebGL context failed: ' + e);
            return 0;
        }
    },

    /* WHICH WEBGL THE CALLING THREAD'S CONTEXT IS.  1 or 2, or 0 if there is
     * none.  Emscripten stamps `version` on the context record when it creates
     * it; the instanceof is the belt-and-braces read of the object itself, for
     * a context this port did not create through GL.createContext(). */
    b3_web_ctx_version__deps: ['$GL'],
    b3_web_ctx_version: () => {
        try {
            var c = GL.currentContext;
            if (!c || !c.GLctx) return 0;
            if (c.version) return c.version;
            return (typeof WebGL2RenderingContext !== 'undefined'
                    && c.GLctx instanceof WebGL2RenderingContext) ? 2 : 1;
        } catch (e) {
            return 0;
        }
    },

    /* Game worker: one frame across the boundary.
     *
     * BACKPRESSURE.  The worker is free-running and postMessage never blocks,
     * so without a cap a worker ahead of the compositor would allocate a GPU
     * bitmap per frame and hand the main thread a backlog to close.  The
     * in-flight count is capped at two (one being painted, one queued); over
     * that the frame is dropped HERE, before the bitmap is ever allocated,
     * which is strictly cheaper than allocating it for the main thread to
     * throw away. */
    b3_web_present_frame__deps: ['$b3gl', '$b3gl_i32', '$b3hw', '$b3hw_gpu_tick'],
    b3_web_present_frame: function () {
        var oc = b3gl.oc;
        if (!oc) return;
        /* One frame of GL work has just been issued: close its GPU timer and
         * open the next one.  Costs nothing at all when B3_WEB_HWPROF is off,
         * because b3hw.ext is null and the tick returns on its first line. */
        if (b3hw.on) b3hw_gpu_tick();
        var I32 = b3gl_i32(), idx = b3gl.ctl >> 2;
        if (Atomics.load(I32, idx + 0) >= 2) {
            Atomics.add(I32, idx + 2, 1);
            return;
        }
        /* THE TWO HALVES ARE TIMED SEPARATELY and that is the point: a slow
         * transferToImageBitmap is the browser materialising a copy of the
         * drawing buffer, a slow postMessage is the transfer/clone path, and
         * they are different bugs.  Timed only when armed -- two
         * performance.now() calls a frame is nothing, but "nothing" is not
         * "zero", and the default path stays exactly what it was. */
        var t0 = b3hw.on ? performance.now() : 0;
        var b = oc.transferToImageBitmap();
        var t1 = b3hw.on ? performance.now() : 0;
        Atomics.add(I32, idx + 0, 1);
        Atomics.add(I32, idx + 1, 1);
        postMessage({ b3: 'f', b: b }, [b]);
        if (b3hw.on) {
            var HB = b3hw.ctl >> 2;
            Atomics.add(I32, HB + 12, Math.round((t1 - t0) * 1000));
            Atomics.add(I32, HB + 13,
                        Math.round((performance.now() - t1) * 1000));
        }
    },

    /* ================================================= THE GL CALL COUNTER
     * "The port is call-bound" was an inference from a frame-time sweep.  This
     * turns it into a number: every method on the live WebGLRenderingContext
     * is wrapped, and each call bumps a bucket in the shared wasm heap.
     *
     * It wraps the CONTEXT rather than anything in C or in gl4es on purpose --
     * that is the exact place where a call stops being the port's business and
     * becomes the browser's, so the count is of REAL WebGL calls, and the same
     * probe measures the proxied build and the direct one without changing.
     *
     * THE SYNC SET is not decoration.  Emscripten dispatches 68 of its 140 GL
     * entry points through emscripten_sync_run_in_main_runtime_thread (see
     * system/lib/gl/webgl1.c) -- glDrawElements and glVertexAttribPointer
     * among them, which is to say the two calls gl4es makes on EVERY batch.
     * Under proxying each of those is a blocking round trip, so counting them
     * separately is the difference between "many calls" and "many stalls".
     *
     * B3_WEB_GLCOUNT=<n> prints one line every n presents.  Off by default:
     * the wrappers cost a JS call per GL call. */
    $b3glc_sync: {
        bindAttribLocation: 1, checkFramebufferStatus: 1, compressedTexImage2D: 1,
        compressedTexSubImage2D: 1, createProgram: 1, createShader: 1,
        deleteBuffers: 1, deleteFramebuffers: 1, deleteRenderbuffers: 1,
        deleteTextures: 1, drawElements: 1, finish: 1, flush: 1,
        createBuffer: 1, createFramebuffer: 1, createRenderbuffer: 1,
        createTexture: 1, deleteBuffer: 1, deleteFramebuffer: 1,
        deleteRenderbuffer: 1, deleteTexture: 1,
        getActiveAttrib: 1, getActiveUniform: 1, getAttachedShaders: 1,
        getAttribLocation: 1, getBufferParameter: 1, getError: 1,
        getFramebufferAttachmentParameter: 1, getParameter: 1,
        getProgramInfoLog: 1, getProgramParameter: 1,
        getRenderbufferParameter: 1, getShaderInfoLog: 1, getShaderParameter: 1,
        getShaderPrecisionFormat: 1, getShaderSource: 1, getUniform: 1,
        getUniformLocation: 1, getVertexAttrib: 1, getVertexAttribOffset: 1,
        isBuffer: 1, isEnabled: 1, isFramebuffer: 1, isProgram: 1,
        isRenderbuffer: 1, isShader: 1, isTexture: 1, readPixels: 1,
        shaderSource: 1, texParameterf: 1, texParameteri: 1,
        vertexAttribPointer: 1
    },

    /* name -> bucket.  Order matters: the first match wins. */
    $b3glc_bucket: (n) => {
        if (n === 'drawArrays' || n === 'drawElements') return 2;
        if (n.startsWith('vertexAttrib') || n.startsWith('bufferData') ||
            n.startsWith('bufferSubData') || n === 'bindBuffer' ||
            n === 'enableVertexAttribArray' || n === 'disableVertexAttribArray')
            return 3;
        if (n.startsWith('tex') || n === 'bindTexture' ||
            n === 'activeTexture' || n === 'generateMipmap' ||
            n === 'pixelStorei' || n === 'compressedTexImage2D' ||
            n === 'compressedTexSubImage2D') return 4;
        if (n.startsWith('uniform')) return 5;
        if (n.startsWith('get') || n.startsWith('is') || n === 'readPixels' ||
            n === 'finish' || n === 'checkFramebufferStatus') return 8;
        if (n.startsWith('bindFramebuffer') || n.startsWith('framebuffer') ||
            n.startsWith('bindRenderbuffer') || n.startsWith('renderbuffer'))
            return 7;
        if (n === 'useProgram' || n.startsWith('shader') ||
            n.startsWith('attachShader') || n === 'linkProgram' ||
            n === 'compileShader' || n === 'bindAttribLocation' ||
            n.startsWith('create') || n.startsWith('delete')) return 6;
        if (n === 'enable' || n === 'disable' || n.startsWith('blend') ||
            n.startsWith('depth') || n.startsWith('stencil') ||
            n.startsWith('cull') || n === 'frontFace' || n === 'scissor' ||
            n === 'viewport' || n.startsWith('color') || n.startsWith('clear') ||
            n === 'lineWidth' || n === 'polygonOffset' || n === 'flush' ||
            n.startsWith('sample') || n === 'hint') return 9;
        return 10;
    },

    /* The per-name tally lives in plain JS on whichever thread the context is
     * on, because nothing outside the dump ever reads it.  It is what turns
     * "26 000 calls a frame" into "and here is which ones", which is the only
     * form of the number a draw-path rewrite can act on. */
    $b3glc_names: {},

    $b3glc_hook__deps: ['$GL', '$b3gl_i32', '$b3glc_sync', '$b3glc_bucket',
                        '$b3glc_names'],
    $b3glc_hook: (ctl) => {
        var c = GL.currentContext;
        if (!c || !c.GLctx) return 0;
        var g = c.GLctx;
        if (g.b3Counted) return 2;
        g.b3Counted = 1;
        var base = ctl >> 2, n = 0;
        for (var k in g) {
            if (typeof g[k] !== 'function') continue;
            (function (name, fn) {
                var slot = base + b3glc_bucket(name);
                var sync = b3glc_sync[name] ? base + 1 : 0;
                b3glc_names[name] = 0;
                g[name] = function () {
                    var I32 = b3gl_i32();
                    Atomics.add(I32, base, 1);
                    Atomics.add(I32, slot, 1);
                    if (sync) Atomics.add(I32, sync, 1);
                    b3glc_names[name]++;
                    return fn.apply(g, arguments);
                };
            })(k, g[k]);
            n++;
        }
        return n ? 1 : 0;
    },

    $b3glc_dump__deps: ['$b3glc_names', '$b3glc_sync'],
    $b3glc_dump: (frames) => {
        var ks = Object.keys(b3glc_names).filter((k) => b3glc_names[k] > 0);
        ks.sort((a, b) => b3glc_names[b] - b3glc_names[a]);
        var parts = ks.slice(0, 24).map((k) =>
            k + (b3glc_sync[k] ? '*' : '') + ' ' +
            (b3glc_names[k] / frames).toFixed(1));
        out('[Burnout3] [glcount] per frame by name (* = a SYNC dispatch when ' +
            'proxied): ' + parts.join(', '));
        for (var i = 0; i < ks.length; i++) b3glc_names[ks[i]] = 0;
    },

    /* Two entry points each, because where the context object LIVES is the
     * whole question: under proxying it is on the main thread, and with a
     * worker-local context it is here. */
    b3_web_glcount_here__deps: ['$b3glc_hook'],
    b3_web_glcount_here: (ctl) => b3glc_hook(ctl),

    b3_web_glcount_main__proxy: 'sync',
    b3_web_glcount_main__deps: ['$b3glc_hook'],
    b3_web_glcount_main: (ctl) => b3glc_hook(ctl),

    b3_web_glcount_dump_here__deps: ['$b3glc_dump'],
    b3_web_glcount_dump_here: (frames) => b3glc_dump(frames),

    b3_web_glcount_dump_main__proxy: 'sync',
    b3_web_glcount_dump_main__deps: ['$b3glc_dump'],
    b3_web_glcount_dump_main: (frames) => b3glc_dump(frames),

    /* ================================================ THE texParameter AUDIT
     * B3_WEB_TEXAUDIT=<n>: wrap texParameteri/texParameterf and report every
     * call made with NO TEXTURE BOUND to the target.
     *
     * WHY IT IS A REAL BUG AND NOT A BROWSER QUIRK.  Setting a wrap/filter
     * mode with nothing bound is the D3D address-mode idiom -- the mode is a
     * sampler render state there, not a property of the texture -- and it is
     * silently harmless on desktop GL, where name 0 is a real default texture
     * object that absorbs the write.  WebGL has no default texture object, so
     * the same call is INVALID_OPERATION and the parameter is simply lost:
     * whatever gets bound next draws with whatever mode it happened to carry.
     *
     * The audit reports the WASM STACK, not just a count, because the whole
     * problem is finding the call sites: `new Error().stack` inside the
     * wrapper unwinds through the wasm frames, and this build keeps its name
     * section, so the offender comes out as a C function name.  Off by
     * default -- the wrapper costs a getParameter (a real round trip to the
     * command buffer) on every texParameter call. */
    $b3tex: { n: 0, bad: 0, max: 0, seen: {} },

    $b3tex_hook__deps: ['$GL', '$b3tex'],
    $b3tex_hook: (max) => {
        var c = GL.currentContext;
        if (!c || !c.GLctx) return 0;
        var g = c.GLctx;
        if (g.b3TexAudited) return 2;
        g.b3TexAudited = 1;
        b3tex.max = max;
        ['texParameteri', 'texParameterf'].forEach(function (name) {
            var fn = g[name];
            if (typeof fn !== 'function') return;
            g[name] = function (target, pname, param) {
                b3tex.n++;
                var bound = null;
                try {
                    bound = g.getParameter(
                        target === g.TEXTURE_CUBE_MAP
                            ? g.TEXTURE_BINDING_CUBE_MAP : g.TEXTURE_BINDING_2D);
                } catch (e) { /* fall through: report it as unbound */ }
                if (!bound) {
                    b3tex.bad++;
                    var st;
                    try { st = new Error().stack || '?'; } catch (e2) { st = '?'; }
                    /* Drop the wrapper's own frame and keep the next few: the
                     * first wasm name in there is the engine function. */
                    st = st.split('\n').slice(2, 7)
                           .map((s) => s.trim()).join(' <- ');
                    b3tex.seen[st] = (b3tex.seen[st] || 0) + 1;
                }
                return fn.call(g, target, pname, param);
            };
        });
        return 1;
    },

    $b3tex_dump__deps: ['$b3tex'],
    $b3tex_dump: (frames) => {
        var ks = Object.keys(b3tex.seen);
        ks.sort((a, b) => b3tex.seen[b] - b3tex.seen[a]);
        out('[Burnout3] [texaudit] ' + b3tex.bad + ' of ' + b3tex.n +
            ' texParameter calls had NO TEXTURE BOUND (' +
            (b3tex.bad / (frames || 1)).toFixed(2) + '/frame), ' +
            ks.length + ' distinct site(s)');
        for (var i = 0; i < ks.length && i < b3tex.max; i++)
            out('[Burnout3] [texaudit]   x' + b3tex.seen[ks[i]] + '  ' + ks[i]);
        b3tex.n = 0; b3tex.bad = 0; b3tex.seen = {};
    },

    b3_web_texaudit_here__deps: ['$b3tex_hook'],
    b3_web_texaudit_here: (max) => b3tex_hook(max),

    b3_web_texaudit_main__proxy: 'sync',
    b3_web_texaudit_main__deps: ['$b3tex_hook'],
    b3_web_texaudit_main: (max) => b3tex_hook(max),

    b3_web_texaudit_dump_here__deps: ['$b3tex_dump'],
    b3_web_texaudit_dump_here: (frames) => b3tex_dump(frames),

    b3_web_texaudit_dump_main__proxy: 'sync',
    b3_web_texaudit_dump_main__deps: ['$b3tex_dump'],
    b3_web_texaudit_dump_main: (frames) => b3tex_dump(frames),

    b3_web_iso_bridge_start__proxy: 'sync',
    b3_web_iso_bridge_start: function (ctlPtr) {
        var file = Module['b3IsoFile'];
        if (!file) return 0;
        var w = Module['b3IsoWorker'];
        if (!w) {
            err('[Burnout3] web: image bridge worker was never created');
            return 0;
        }
        /* wasmMemory.buffer is the SharedArrayBuffer the game thread and the
         * helper both address; ctlPtr is a byte offset into it. */
        w.postMessage({ cmd: 'arm', memory: wasmMemory, ctl: ctlPtr, file: file });
        return file.size;
    },

    /* ===================================================== THE AUDIO BRIDGE
     * The page-side half.  See web/b3_web.h for the whole design and
     * web/b3_web.c for the ring's layout, which THIS FILE MUST AGREE WITH --
     * the slot order below is the C enum, and changing one end without the
     * other misreports silently rather than failing.
     *
     * THE PROCESSOR SOURCE is inlined here for the same reason the image
     * bridge's worker is inlined in web/pre.js: the build stays two artifacts
     * (burnout3.js + burnout3.wasm) and there is no third URL for a shell to
     * get wrong.  addModule() wants a URL, so it goes through a blob:.
     *
     * WHAT THE PROCESSOR MAY NOT DO, and it is not a style preference: it runs
     * on the AUDIO RENDERING THREAD, which has a hard real-time budget of one
     * quantum and on which Atomics.wait is forbidden outright.  So it never
     * waits, never allocates in process(), and answers a short ring with
     * silence -- counting the fact, because a silent underrun that nobody
     * counts is indistinguishable from a mix that had nothing to play.
     *
     * THE RESAMPLER is a fractional read cursor, `step` input frames per
     * output frame, linearly interpolated between the two ring samples it
     * falls between.  step is 44100/48000 = 0.91875 on every machine this has
     * been measured on, and exactly 1 on a 44.1 kHz context, in which case the
     * interpolation is a no-op by construction rather than by a special case.
     *
     * The cursor is kept as an integer frame counter plus a fraction, NOT as
     * one big float: at 48 kHz a float64 cursor would still be exact for
     * days, but the integer half is what gets published to READ, and READ has
     * to be the same monotonic int32 the C side differences. */
    /* A FUNCTION, not a bare string: a `$name` library value is emitted into
     * the glue as `var name = <the value>`, so a string constant would land
     * there as CODE rather than as text.  (It did, and acorn refused the
     * build.)  Returning it from a thunk keeps it a string. */
    $b3aud_src: () => `
      const CTL_WRITE = 0, CTL_READ = 1, CTL_UNDER = 2, CTL_SILENCE = 3,
            CTL_STATE = 4, CTL_RATE = 5, CTL_QUANTUM = 6,
            CTL_RMS = 7, CTL_RMSMAX = 8, CTL_RMSN = 9;

      class B3Ring extends AudioWorkletProcessor {
        constructor (opts) {
          super();
          const o = opts.processorOptions;
          this.mem   = o.memory;
          this.ctlB  = o.ctl;
          this.ringB = o.ring;
          this.n     = o.frames;          /* ring capacity, a power of two */
          this.mask  = o.frames - 1;
          this.step  = o.mixRate / sampleRate;
          this.pos   = 0;                 /* frames consumed, monotonic */
          this.frac  = 0;
          this.sum   = 0;                 /* running sum of squares */
          this.cnt   = 0;
          this.started = false;
          this.views();
        }

        /* ALLOW_MEMORY_GROWTH can hand out a new SharedArrayBuffer object for
         * the same underlying memory, so the views are rebuilt whenever the
         * buffer identity changes -- an identity compare per quantum, which is
         * nothing, against reading a stale view for ever, which is silence.
         * (The image bridge's helper rebuilds unconditionally per request for
         * exactly this reason; here the check is cheaper than the rebuild.) */
        views () {
          const b = this.mem.buffer;
          if (this.buf === b) return;
          this.buf = b;
          this.I32 = new Int32Array(b);
          this.F32 = new Float32Array(b);
          this.ctl  = this.ctlB  >> 2;
          this.ring = this.ringB >> 2;
        }

        process (inputs, outputs) {
          this.views();
          const out = outputs[0][0];
          const q = out.length;
          const I32 = this.I32, F32 = this.F32;
          const ctl = this.ctl, ring = this.ring, mask = this.mask;

          if (!this.started) {
            this.started = true;
            Atomics.store(I32, ctl + CTL_RATE, sampleRate | 0);
            Atomics.store(I32, ctl + CTL_QUANTUM, q | 0);
            Atomics.store(I32, ctl + CTL_STATE, 1);
          }

          /* HOW MANY INPUT FRAMES THIS QUANTUM NEEDS.  The +2 is the
           * interpolator's reach: the last output frame reads pos+1. */
          const w = Atomics.load(I32, ctl + CTL_WRITE) | 0;
          let   p = this.pos | 0;
          const avail = (w - p) | 0;
          const need = Math.ceil(q * this.step) + 2;

          let i = 0;
          if (avail >= need) {
            let frac = this.frac;
            for (; i < q; i++) {
              const i0 = ring + ((p) & mask);
              const i1 = ring + ((p + 1) & mask);
              const s = F32[i0] + (F32[i1] - F32[i0]) * frac;
              out[i] = s;
              this.sum += s * s;
              frac += this.step;
              const adv = frac | 0;        /* step < 2, so this is 0 or 1 */
              p += adv;
              frac -= adv;
            }
            this.frac = frac;
            this.pos = p | 0;
            Atomics.store(I32, ctl + CTL_READ, this.pos);
          } else {
            /* UNDERRUN.  Silence, counted -- never a wait, and never a
             * partial read that would leave the cursor mid-quantum. */
            Atomics.add(I32, ctl + CTL_UNDER, 1);
            Atomics.add(I32, ctl + CTL_SILENCE, q);
            for (; i < q; i++) out[i] = 0;
          }

          /* RMS over ~0.25 s windows.  Scaled to int32 because the whole
           * control block is int32 and the C side reads it with the same
           * atomics as every other slot. */
          this.cnt += q;
          if (this.cnt >= 12000) {
            const rms = Math.sqrt(this.sum / this.cnt);
            const v = Math.min(32767, Math.round(rms * 32767)) | 0;
            Atomics.store(I32, ctl + CTL_RMS, v);
            if (v > Atomics.load(I32, ctl + CTL_RMSMAX))
              Atomics.store(I32, ctl + CTL_RMSMAX, v);
            Atomics.add(I32, ctl + CTL_RMSN, 1);
            this.sum = 0;
            this.cnt = 0;
          }
          return true;
        }
      }
      registerProcessor('b3-ring', B3Ring);
    `,

    /* __proxy: 'sync' -- the AudioContext lives on the main browser thread and
     * the caller needs the sample rate back before it can print, or decide
     * whether to start the pump at all.  Only the RATE is synchronous: loading
     * the worklet module is a promise, and nothing waits on it, because the
     * ring is filled either way and the worklet drains it whenever it is
     * ready.  Runs ONCE; every sample after it is pure shared memory between
     * the pump thread and the audio thread, with no main thread involvement at
     * all -- which is the entire point, since the main thread is where this
     * port's frames already queue. */
    b3_web_audio_start__proxy: 'sync',
    b3_web_audio_start__deps: ['$b3aud_src'],
    b3_web_audio_start: function (ctlPtr, ringPtr, frames, mixRate) {
        var ctx = Module['audioContext'] ||
                  (typeof window !== 'undefined' && window['__b3AudioContext']);
        if (!ctx) return 0;
        if (Module['b3AudioNode']) return ctx.sampleRate;   /* idempotent */
        if (!ctx.audioWorklet) {
            err('[Burnout3] web: no AudioWorklet on this context (a secure ' +
                'context is required) -- running silent');
            return 0;
        }
        /* THE BROWSER'S SHARE OF THE LATENCY, written straight into the
         * control block so the C side can print the whole budget in one line
         * rather than half of it.  Slots 10 and 11 are BASELAT/OUTLAT in the
         * enum at the top of web/b3_web.c -- the two ends must agree.
         * baseLatency is available immediately; outputLatency is 0 until the
         * context is actually running, so it is refreshed below and again on
         * every state change. */
        var ctlI = ctlPtr >> 2;
        var lat = function () {
            try {
                var I = new Int32Array(wasmMemory.buffer);
                I[ctlI + 10] = Math.round((ctx.baseLatency || 0) * 1e6);
                I[ctlI + 11] = Math.round((ctx.outputLatency || 0) * 1e6);
            } catch (e) {}
        };
        lat();
        ctx.addEventListener('statechange', lat);

        var url = URL.createObjectURL(
            new Blob([b3aud_src()], { type: 'text/javascript' }));
        ctx.audioWorklet.addModule(url).then(function () {
            URL.revokeObjectURL(url);
            var node = new AudioWorkletNode(ctx, 'b3-ring', {
                numberOfInputs: 0,
                numberOfOutputs: 1,
                outputChannelCount: [1],
                processorOptions: {
                    /* The SharedArrayBuffer both ends address.  The Memory
                     * object rather than its .buffer, so the processor can
                     * follow ALLOW_MEMORY_GROWTH. */
                    memory: wasmMemory,
                    ctl: ctlPtr, ring: ringPtr,
                    frames: frames, mixRate: mixRate
                }
            });
            node.connect(ctx.destination);
            Module['b3AudioNode'] = node;
            lat();
            /* A context that was unlocked by a gesture is already running;
             * one that was not stays suspended and the ring simply sits at
             * its target until something resumes it.  Nothing leaks either
             * way -- the pump stops producing at the target. */
            if (ctx.state === 'suspended') ctx.resume();
        }).catch(function (e) {
            URL.revokeObjectURL(url);
            err('[Burnout3] web: AudioWorklet would not load (' + e +
                ') -- running silent');
        });
        return ctx.sampleRate;
    }
});
