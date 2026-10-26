// Runs the browser build under node for WAEMU_RUN_MS (8 s), optionally playing WAEMU_OPEN, and
// writes the screen to a PNG - the same check tools/run.sh makes natively.
//   node tools/test_wasm.mjs [out.png] [width] [height]
import {createRequire} from 'module';
import fs from 'fs';
import path from 'path';
import zlib from 'zlib';

const require = createRequire(import.meta.url);
const root = path.resolve(path.dirname(new URL(import.meta.url).pathname.replace(/^\/([A-Z]:)/, '$1')), '..');
const out = process.argv[2] || path.join(root, 'tmp', 'wasm.png');
const W = +(process.argv[3] || 1280), H = +(process.argv[4] || 800);
process.chdir(root);

function png(file, w, h, rgba) {
  const raw = Buffer.alloc((w * 3 + 1) * h);
  for (let y = 0; y < h; y++) {
    raw[y * (w * 3 + 1)] = 0;
    for (let x = 0; x < w; x++) {
      const s = (y * w + x) * 4, d = y * (w * 3 + 1) + 1 + x * 3;
      raw[d] = rgba[s]; raw[d + 1] = rgba[s + 1]; raw[d + 2] = rgba[s + 2];
    }
  }
  const crcTable = [...Array(256)].map((_, n) => { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1; return c >>> 0; });
  const crc = (b) => { let c = ~0; for (const v of b) c = crcTable[(c ^ v) & 0xFF] ^ (c >>> 8); return (~c) >>> 0; };
  const chunk = (t, d) => { const l = Buffer.alloc(4); l.writeUInt32BE(d.length); const td = Buffer.concat([Buffer.from(t), d]); const c = Buffer.alloc(4); c.writeUInt32BE(crc(td)); return Buffer.concat([l, td, c]); };
  const ihdr = Buffer.alloc(13); ihdr.writeUInt32BE(w, 0); ihdr.writeUInt32BE(h, 4); ihdr[8] = 8; ihdr[9] = 2;
  fs.writeFileSync(file, Buffer.concat([Buffer.from([0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A]), chunk('IHDR', ihdr), chunk('IDAT', zlib.deflateSync(raw)), chunk('IEND', Buffer.alloc(0))]));
}

const createWaemu = require(path.join(root, 'waemu.js'));
const t0 = Date.now();
const Module = await createWaemu({
  locateFile: (p) => path.join(root, p),
  print: (s) => console.log(s),
  printErr: (s) => console.error(s),
  preRun: [(M) => { M.ENV.WAEMU_RUN_MS = process.env.WAEMU_RUN_MS || '8000'; if (process.env.WAEMU_OPEN) M.ENV.WAEMU_OPEN = process.env.WAEMU_OPEN; if (process.env.WAEMU_TRACE_WAVE) M.ENV.WAEMU_TRACE_WAVE = '1'; if (process.env.JWEMU_TRACE_CALLS) M.ENV.JWEMU_TRACE_CALLS = '1'; }],
});
Module.jwEvents = new Int32Array(new SharedArrayBuffer((16 + 8 * 4096) * 4));
const FS = Module.FS;
try { FS.mkdir('/persist'); } catch (e) {}
FS.chdir('/C:/Winamp');
console.log(`loaded in ${Date.now() - t0} ms`);
const t1 = Date.now();
try {
  Module._waemu_main(W, H);
} catch (e) {
  if (!(e && (e.name === 'ExitStatus' || /exit/i.test(String(e))))) throw e;
}
console.log(`ran in ${Date.now() - t1} ms`);
const f = Module.jwFrame;
if (!f) { console.error('no frame was presented'); process.exit(1); }
png(out, f.w, f.h, f.rgba);
console.log(`wrote ${out}`);
