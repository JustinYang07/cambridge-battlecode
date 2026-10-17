// Print a map region as ASCII using the engine WASM (walls #, holes o, altars A, bushes ", camps by type letter).
//   node lab/replay/mapdump.mjs maps/ambush.map26a [x0 y0 x1 y1]
import fs from "node:fs";
import path from "node:path";

const [mapPath, ...box] = process.argv.slice(2);
function findWasm() {
  const root = path.join(process.env.LOCALAPPDATA || "", "cambc", "assets");
  for (const dir of fs.readdirSync(root)) {
    const assets = path.join(root, dir, "visualiser", "assets");
    if (!fs.existsSync(assets)) continue;
    const f = fs.readdirSync(assets).find((n) => /^v0_0_1-.*\.wasm$/.test(n));
    if (f) return path.join(assets, f);
  }
  throw new Error("engine wasm not found");
}
let memory;
const nop = () => 0;
const wasi = { random_get: nop, clock_time_get: nop, environ_sizes_get: (a, b) => { const v = new DataView(memory.buffer); v.setUint32(a, 0, true); v.setUint32(b, 0, true); return 0; },
  environ_get: nop, args_sizes_get: nop, args_get: nop, fd_close: nop, fd_fdstat_get: nop, fd_prestat_get: () => 8, fd_prestat_dir_name: () => 8,
  fd_read: nop, fd_seek: () => 70, fd_write: (fd, iov, n, out) => { new DataView(memory.buffer).setUint32(out, 0, true); return 0; }, proc_exit: () => { throw new Error("exit"); } };
const inst = new WebAssembly.Instance(new WebAssembly.Module(fs.readFileSync(findWasm())), { wasi_snapshot_preview1: wasi, deep_wasm: { on_assert_fail_impl: () => 1 } });
const X = inst.exports; memory = X.memory; X._initialize?.();
const out = X.Engine_Malloc(32);
const dv = () => new DataView(memory.buffer);
const bytes = fs.readFileSync(mapPath);
const p = X.Engine_Malloc(bytes.length); new Uint8Array(memory.buffer, p, bytes.length).set(bytes);
if (X.Game_FromMapFileBytes(p, bytes.length, out, 0)) throw new Error("map load failed");
const game = dv().getBigUint64(out, true);
X.Game_GetBitboardMetadata(game, out);
const W = dv().getInt32(out, true), H = dv().getInt32(out + 4, true), stride = dv().getUint32(out + 16, true);
const buf = (fn) => { X[fn](game, out); return dv().getUint32(out, true); };
const walls = buf("Game_GetWallBuffer"), holes = buf("Game_GetHoleBuffer"), altars = buf("Game_GetAltarBuffer"), bushes = buf("Game_GetBushBuffer");
const test = (ptr, x, y) => (dv().getBigUint64(ptr + ((y >> 3) * stride + (x >> 3)) * 8, true) >> BigInt(((y & 7) << 3) + (x & 7))) & 1n;
const camps = new Map();
X.Game_GetNumCamps(game, out); const nc = dv().getUint32(out, true);
for (let i = 0; i < nc; i++) { X.Game_GetCampHandle(game, i, out); const h = dv().getBigUint64(out, true); X.Game_GetCamp(game, h, out);
  camps.set(`${dv().getInt32(out + 4, true)},${dv().getInt32(out + 8, true)}`, "gwEGM"[dv().getInt32(out, true)] ?? "?"); }
const [x0, y0, x1, y1] = box.length ? box.map(Number) : [0, 0, W - 1, H - 1];
console.log(`${W}x${H}; rows y=${y0}..${y1}, cols x=${x0}..${x1}`);
for (let y = y0; y <= y1; y++) {
  let row = String(y).padStart(3) + " ";
  for (let x = x0; x <= x1; x++) {
    const c = camps.get(`${x},${y}`);
    row += c ?? (test(walls, x, y) ? "#" : test(holes, x, y) ? "o" : test(altars, x, y) ? "A" : test(bushes, x, y) ? '"' : ".");
  }
  console.log(row);
}
