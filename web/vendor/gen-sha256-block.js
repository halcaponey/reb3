const out = [];
out.push("    var t1, t2, x, y;");
// load 16 words
const loads = [];
for (let i = 0; i < 16; i++) {
  const o = i * 4;
  loads.push(`    var m${i} = (p[off + ${o}] << 24) | (p[off + ${o+1}] << 16) | (p[off + ${o+2}] << 8) | p[off + ${o+3}];`);
}
out.push(loads.join('\n'));
out.push("    var a = h[0], b = h[1], c = h[2], d = h[3];");
out.push("    var e = h[4], f = h[5], g = h[6], hh = h[7];");
let r = ['a','b','c','d','e','f','g','hh'];
for (let i = 0; i < 64; i++) {
  if (i >= 16) {
    const cur = 'm' + (i % 16), p1 = 'm' + ((i + 1) % 16), p9 = 'm' + ((i + 9) % 16), p14 = 'm' + ((i + 14) % 16);
    out.push(`    x = ${p1}; y = ${p14};`);
    out.push(`    ${cur} = (${cur} + (((x >>> 7) | (x << 25)) ^ ((x >>> 18) | (x << 14)) ^ (x >>> 3)) + ${p9} + (((y >>> 17) | (y << 15)) ^ ((y >>> 19) | (y << 13)) ^ (y >>> 10))) | 0;`);
  }
  const [A,B,C,D,E,F,G,H] = r;
  out.push(`    t1 = (${H} + (((${E} >>> 6) | (${E} << 26)) ^ ((${E} >>> 11) | (${E} << 21)) ^ ((${E} >>> 25) | (${E} << 7))) + ((${E} & ${F}) ^ (~${E} & ${G})) + K${i} + m${i % 16}) | 0;`);
  out.push(`    t2 = ((((${A} >>> 2) | (${A} << 30)) ^ ((${A} >>> 13) | (${A} << 19)) ^ ((${A} >>> 22) | (${A} << 10))) + ((${A} & ${B}) ^ (${A} & ${C}) ^ (${B} & ${C}))) | 0;`);
  out.push(`    ${D} = (${D} + t1) | 0; ${H} = (t1 + t2) | 0;`);
  r = [r[7], r[0], r[1], r[2], r[3], r[4], r[5], r[6]];
}
const [A,B,C,D,E,F,G,H] = r;
out.push(`    h[0] = (h[0] + ${A}) | 0; h[1] = (h[1] + ${B}) | 0;`);
out.push(`    h[2] = (h[2] + ${C}) | 0; h[3] = (h[3] + ${D}) | 0;`);
out.push(`    h[4] = (h[4] + ${E}) | 0; h[5] = (h[5] + ${F}) | 0;`);
out.push(`    h[6] = (h[6] + ${G}) | 0; h[7] = (h[7] + ${H}) | 0;`);
console.log(out.join('\n'));
