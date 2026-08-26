/* validate_audio_ring.js -- the web audio bridge's CONSUMER, tested on its own.
 *
 *     node tools/validate_audio_ring.js
 *
 * The AudioWorkletProcessor in web/b3_web_lib.js is the one piece of the audio
 * bridge that is easy to get subtly wrong and hard to see wrong: it runs on
 * the browser's audio rendering thread, it is the half that resamples 44100
 * into whatever the context runs at, and its failure mode is a quiet artefact
 * rather than an error.  Getting a headless browser to the point of playing a
 * race just to exercise 40 lines of arithmetic is also the slowest gate in the
 * repo.
 *
 * So the processor is lifted OUT of the library file -- by the same delimiters
 * the build uses, so a change to it is a change to what is tested -- and run
 * against a SharedArrayBuffer laid out exactly as web/b3_web.c lays it out,
 * with an `AudioWorkletProcessor` base class and a `sampleRate` stubbed in.
 * That is the entire AudioWorkletGlobalScope surface it touches.
 *
 * WHAT IS ACTUALLY PROVEN, and each of these was a way to get it wrong:
 *
 *   1. it parses and registers under the name the library connects to;
 *   2. the fractional read cursor advances at mixRate/sampleRate -- 128
 *      output frames must consume 117.6 input frames at 48 kHz, not 128;
 *   3. a full-scale sine comes out at RMS 1/sqrt(2).  This is the load-bearing
 *      one: a broken interpolator, a wrong ring mask or an off-by-one index
 *      all still produce sound, and all of them move this number;
 *   4. a starved ring yields silence and COUNTS it, rather than throwing on
 *      the audio thread or reading samples that were never written.
 */
const fs = require('fs');
const path = require('path');

const LIB = path.join(__dirname, '..', 'web', 'b3_web_lib.js');
const MARK = '$b3aud_src: () => `';

/* The control block, from the enum at the top of web/b3_web.c.  If the two
 * ever disagree this test is the thing that should notice. */
const CTL = { WRITE: 0, READ: 1, UNDER: 2, SILENCE: 3, STATE: 4, RATE: 5,
              QUANTUM: 6, RMS: 7, RMSMAX: 8, RMSN: 9, BASELAT: 10, OUTLAT: 11,
              N: 12 };
const RING = 8192;
const MIXRATE = 44100;
const CTXRATE = 48000;

let fails = 0;
function ok(name, cond, detail) {
    if (!cond) fails++;
    console.log('  %s  %s%s', cond ? 'ok  ' : 'FAIL', name.padEnd(52),
                detail === undefined ? '' : detail);
}

function loadProcessor(rate) {
    const s = fs.readFileSync(LIB, 'utf8');
    const a = s.indexOf(MARK);
    if (a < 0) throw new Error('web/b3_web_lib.js no longer contains ' + MARK);
    const start = s.indexOf('`', a) + 1;
    const src = s.slice(start, s.indexOf('`', start));
    const shim =
        'class AudioWorkletProcessor { constructor(){ this.port = ' +
        '{ postMessage(){} }; } }\n' +
        'var sampleRate = ' + rate + ';\n' +
        'var registered = null;\n' +
        'function registerProcessor(n, c) { registered = [n, c]; }\n';
    return new Function(shim + src + '\nreturn registered;')();
}

function makeRing() {
    const sab = new SharedArrayBuffer((CTL.N + RING) * 4);
    return { sab, I32: new Int32Array(sab), F32: new Float32Array(sab),
             ringOff: CTL.N };
}

function run(Cls, r, rate, quanta) {
    const p = new Cls({ processorOptions: {
        memory: { buffer: r.sab }, ctl: 0, ring: r.ringOff * 4,
        frames: RING, mixRate: MIXRATE } });
    const out = [new Float32Array(128)];
    let energy = 0, n = 0;
    for (let q = 0; q < quanta; q++) {
        p.process([], [out]);
        for (const v of out[0]) { energy += v * v; n++; }
    }
    return { rms: Math.sqrt(energy / n), frames: n, proc: p, out };
}

console.log('web audio ring -- the AudioWorkletProcessor, lifted out of ' +
            'web/b3_web_lib.js\n');

let name, Cls;
try {
    [name, Cls] = loadProcessor(CTXRATE);
} catch (e) {
    console.log('  FAIL  could not load the processor: ' + e.message);
    process.exit(1);
}
ok('the processor parses and registers', typeof Cls === 'function');
ok('...under the name the library connects to', name === 'b3-ring', name);

/* ---- 1. a full-scale sine through the 44100 -> 48000 resampler ---- */
{
    const r = makeRing();
    for (let i = 0; i < RING; i++)
        r.F32[r.ringOff + i] = Math.sin(i * 2 * Math.PI * 441 / MIXRATE);
    Atomics.store(r.I32, CTL.WRITE, 4096);          /* published, not yet read */

    const quanta = 30;
    const got = run(Cls, r, CTXRATE, quanta);
    const read = Atomics.load(r.I32, CTL.READ);
    const want = quanta * 128 * MIXRATE / CTXRATE;

    ok('the read cursor steps at mixRate/sampleRate',
       Math.abs(read - want) <= 1,
       read + ' frames consumed for ' + quanta * 128 + ' out (want ~' +
       Math.round(want) + ')');
    ok('a full-scale sine comes back at RMS 1/sqrt(2)',
       Math.abs(got.rms - Math.SQRT1_2) < 0.02, got.rms.toFixed(5));
    ok('nothing underran while the ring was full',
       Atomics.load(r.I32, CTL.UNDER) === 0);
    ok('the context rate is published to the C side',
       Atomics.load(r.I32, CTL.RATE) === CTXRATE);
    ok('...and the render quantum, measured not assumed',
       Atomics.load(r.I32, CTL.QUANTUM) === 128);
    ok('...and STATE says the worklet is live',
       Atomics.load(r.I32, CTL.STATE) === 1);

    /* ---- 2. starve it: silence, counted, and no throw ---- */
    const under0 = Atomics.load(r.I32, CTL.UNDER);
    let threw = null, loud = 0;
    try {
        for (let q = 0; q < 40; q++) {
            got.proc.process([], [got.out]);
            for (const v of got.out[0]) if (v !== 0) loud++;
        }
    } catch (e) { threw = e; }
    ok('a starved ring does not throw on the audio thread', !threw,
       threw ? threw.message : '');
    ok('...it counts the underruns', Atomics.load(r.I32, CTL.UNDER) > under0,
       under0 + ' -> ' + Atomics.load(r.I32, CTL.UNDER));
    ok('...and the silence frames', Atomics.load(r.I32, CTL.SILENCE) > 0,
       Atomics.load(r.I32, CTL.SILENCE) + ' frames');
}

/* ---- 3. a 44.1 kHz context: the resampler must be a pass-through ---- */
{
    const [, C441] = loadProcessor(MIXRATE);
    const r = makeRing();
    for (let i = 0; i < RING; i++)
        r.F32[r.ringOff + i] = (i % 2) ? 0.5 : -0.5;   /* Nyquist square */
    Atomics.store(r.I32, CTL.WRITE, 4096);
    const got = run(C441, r, MIXRATE, 20);
    ok('a 44100 context reads the ring 1:1',
       Atomics.load(r.I32, CTL.READ) === 20 * 128,
       Atomics.load(r.I32, CTL.READ) + ' frames for ' + 20 * 128 + ' out');
    /* At step exactly 1 the interpolation weight is 0 every frame, so the
     * alternating +-0.5 must survive untouched -- an interpolator that
     * smeared would come back at less than 0.5. */
    ok('...and does not smear the samples on the way',
       Math.abs(got.rms - 0.5) < 1e-6, got.rms.toFixed(6));
}

/* ---- 4. the ring index must wrap, including past a negative cursor ---- */
{
    const r = makeRing();
    for (let i = 0; i < RING; i++) r.F32[r.ringOff + i] = 0.25;
    /* THE CURSOR STARTS 300 FRAMES SHORT OF THE INT32 WRAP, with a full
     * 4096-frame lead published -- so WRITE is already negative while READ is
     * not, and the run below walks READ across the boundary too.  The pump's
     * counters do this after 13.5 hours of audio.  Two things have to survive
     * it: the (w - p) difference, which is only right in int32 arithmetic, and
     * the masked index, which must stay inside the ring while the cursor is
     * negative.  The quanta are sized to consume 3527 of the 4096 available,
     * so an underrun here would be a real one and not the test running dry. */
    const pos0 = 2147483647 - 300;
    const p = new Cls({ processorOptions: {
        memory: { buffer: r.sab }, ctl: 0, ring: r.ringOff * 4,
        frames: RING, mixRate: MIXRATE } });
    p.pos = pos0;
    Atomics.store(r.I32, CTL.WRITE, (pos0 + 4096) | 0);   /* wraps negative */
    const out = [new Float32Array(128)];
    let bad = 0;
    for (let q = 0; q < 30; q++) {
        p.process([], [out]);
        for (const v of out[0]) if (v !== 0.25) bad++;
    }
    ok('the ring index survives the int32 wrap', bad === 0,
       bad ? bad + ' samples outside the ring' : 'READ now ' +
       Atomics.load(r.I32, CTL.READ) + ' (wrapped)');
    ok('...and (write - read) stays right across it',
       Atomics.load(r.I32, CTL.UNDER) === 0 && Atomics.load(r.I32, CTL.READ) < 0);
}

console.log('\n%s', fails ? fails + ' FAILED' : 'all checks passed');
process.exit(fails ? 1 : 0);
