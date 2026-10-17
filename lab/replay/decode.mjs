// Decode a CAMBC replay (.repl26a) into per-round JSON, using the official engine WASM that ships
// with the cambc visualiser (it replays the game exactly). No browser needed.
//
//   node lab/replay/decode.mjs <replay.repl26a> <out.json>
//
// Output: { rounds: [ { round, gems:[a,b], champions:[{team,type,x,y,hp,maxHp,alive,gold,gems,kills,items}],
//                       wards:[a,b], captures:[a,b] } ... ], outcome, draft }
// Layouts come from the visualiser's snapshot parser (PanelChrome-*.js: function hr / xr); the engine
// checks the ABI version and snapshot size, so a changed engine fails loudly instead of misreading.
import fs from "node:fs";
import path from "node:path";

const [replayPath, outPath] = process.argv.slice(2);
if (!replayPath || !outPath) {
  console.error("usage: node decode.mjs <replay.repl26a> <out.json>");
  process.exit(2);
}

function findWasm() {
  const root = path.join(process.env.LOCALAPPDATA || "", "cambc", "assets");
  for (const dir of fs.readdirSync(root)) {
    const assets = path.join(root, dir, "visualiser", "assets");
    if (!fs.existsSync(assets)) continue;
    const f = fs.readdirSync(assets).find((n) => /^v0_0_1-.*\.wasm$/.test(n));
    if (f) return path.join(assets, f);
  }
  throw new Error("engine wasm not found under %LOCALAPPDATA%/cambc/assets (run `cambc view` once)");
}

// Minimal WASI shim (as the visualiser's): stdout/stderr only, no files.
let memory;
const wasi = {
  random_get(p, n) { const b = new Uint8Array(memory.buffer, p, n); for (let i = 0; i < n; i++) b[i] = (Math.random() * 256) | 0; return 0; },
  clock_time_get(id, _prec, out) { new DataView(memory.buffer).setBigUint64(out, BigInt(Date.now()) * 1000000n, true); return 0; },
  environ_sizes_get(a, b) { const v = new DataView(memory.buffer); v.setUint32(a, 0, true); v.setUint32(b, 0, true); return 0; },
  environ_get() { return 0; },
  args_sizes_get(a, b) { const v = new DataView(memory.buffer); v.setUint32(a, 0, true); v.setUint32(b, 0, true); return 0; },
  args_get() { return 0; },
  fd_close(fd) { return fd <= 2 ? 0 : 8; },
  fd_fdstat_get(fd, p) { if (fd > 2) return 8; const v = new DataView(memory.buffer); v.setUint8(p, 2); v.setUint16(p + 2, 0, true); v.setBigUint64(p + 8, 0n, true); v.setBigUint64(p + 16, 0n, true); return 0; },
  fd_prestat_get() { return 8; },
  fd_prestat_dir_name() { return 8; },
  fd_read(fd, _iov, _n, out) { if (fd !== 0) return 8; new DataView(memory.buffer).setUint32(out, 0, true); return 0; },
  fd_seek(fd) { return fd <= 2 ? 70 : 8; },
  fd_write(fd, iov, n, out) {
    const v = new DataView(memory.buffer); let total = 0;
    for (let i = 0; i < n; i++) total += v.getUint32(iov + i * 8 + 4, true);
    v.setUint32(out, total, true); return 0;
  },
  proc_exit(code) { throw new Error(`engine exited ${code}`); },
};

const module = new WebAssembly.Module(fs.readFileSync(findWasm()));
const imports = { wasi_snapshot_preview1: wasi, deep_wasm: { on_assert_fail_impl() { return 1; } } };
const instance = new WebAssembly.Instance(module, imports);
const X = instance.exports;
memory = X.memory;
X._initialize?.();

const outBuf = X.Engine_Malloc(32);
const dv = () => new DataView(memory.buffer);
function cstr(p) { const b = new Uint8Array(memory.buffer); let e = p; while (b[e]) e++; return Buffer.from(b.subarray(p, e)).toString("utf8"); }
function check(code, what) { if (code !== 0) throw new Error(`${what} failed: engine error ${code}: ${cstr(X.GetLastErrorMsg())}`); }

// Load the replay.
const bytes = fs.readFileSync(replayPath);
const ptr = X.Engine_Malloc(bytes.length);
new Uint8Array(memory.buffer, ptr, bytes.length).set(bytes);
check(X.Game_FromReplayFileBytes(ptr, bytes.length, outBuf), "Game_FromReplayFileBytes");
X.Engine_Free(ptr);
const game = dv().getBigUint64(outBuf, true);

// Static map facts: camp groups (rewards, respawn, camp types and tiles) and team A's spawn area.
function mapInfo() {
  const v = () => dv();
  check(X.Game_GetNumCampGroups(game, outBuf), "GetNumCampGroups");
  const ng = v().getUint32(outBuf, true);
  const groups = [];
  for (let g = 0; g < ng; g++) {
    check(X.Game_GetCampGroupHandle(game, g, outBuf), "GetCampGroupHandle");
    const gh = v().getBigUint64(outBuf, true);
    check(X.Game_GetCampGroup(game, gh, outBuf), "GetCampGroup");
    const info = { gems: v().getUint32(outBuf, true), gold: v().getUint32(outBuf + 4, true), cooldown: v().getUint32(outBuf + 8, true) };
    const n = v().getUint32(outBuf + 12, true);
    info.camps = [];
    for (let k = 0; k < n; k++) {
      check(X.Game_GetCampGroupCamp(game, gh, k, outBuf), "GetCampGroupCamp");
      const h = v().getBigUint64(outBuf, true);
      check(X.Game_GetCamp(game, h, outBuf), "GetCamp");
      info.camps.push({ type: v().getInt32(outBuf, true), x: v().getInt32(outBuf + 4, true), y: v().getInt32(outBuf + 8, true) });
    }
    groups.push(info);
  }
  check(X.Game_GetTeamASpawnArea(game, outBuf), "GetTeamASpawnArea");
  const spawnA = { minX: v().getInt32(outBuf, true), minY: v().getInt32(outBuf + 4, true), maxX: v().getInt32(outBuf + 8, true), maxY: v().getInt32(outBuf + 12, true) };
  check(X.Game_GetBitboardMetadata(game, outBuf), "GetBitboardMetadata");
  const width = v().getInt32(outBuf, true), height = v().getInt32(outBuf + 4, true);
  check(X.Game_GetSymmetry(game, outBuf), "GetSymmetry");
  const symmetry = v().getInt32(outBuf, true);  // 0 horizontal (x mirrored), 1 vertical (y), 2 rotational 180
  // The map defines one half; the game interleaves each defined group with its mirror image
  // (group 2k = defined group k, 2k+1 = its reflection). Checked: every capture event in a test
  // replay has a capturing champion on the group this gives.
  const mirror = (c) => ({ ...c, x: symmetry === 1 ? c.x : width - 1 - c.x, y: symmetry === 0 ? c.y : height - 1 - c.y });
  const all = groups.flatMap((g) => [g, { ...g, camps: g.camps.map(mirror) }]);
  return { width, height, symmetry, groups: all, spawnA };
}
const map = mapInfo();

check(X.Game_Visualiser_Timeline_GetMain(game, outBuf), "Timeline_GetMain");
const timeline = dv().getBigUint64(outBuf, true);
const args = X.Engine_Malloc(32);
check(X.Game_Visualiser_Timeline_Info(game, timeline, args, args + 8, args + 16), "Timeline_Info");
const size = dv().getUint32(args, true);

const SNAP = 40152;
if (X.Game_Visualiser_TimelineSnapshotSize() !== SNAP) throw new Error("unsupported timeline snapshot layout");
const snap = X.Engine_Malloc(SNAP);

function read() {
  check(X.Game_Visualiser_ReadTimelineState(game, snap), "ReadTimelineState");
  const v = dv(), t = snap;
  const u32 = (o) => v.getUint32(t + o, true), i32 = (o) => v.getInt32(t + o, true), f32 = (o) => v.getFloat32(t + o, true);
  if (u32(0) !== 1 || u32(4) !== SNAP) throw new Error("unsupported game snapshot ABI");
  const header = {
    round: u32(8), phase: u32(24), outcome: u32(28),
    gems: [Number(v.getBigUint64(t + 32, true)), Number(v.getBigUint64(t + 40, true))],
    awaitingInput: !!u32(20), inputPosition: u32(56),
    drafts: [0, 1].map((side) => [0, 1, 2, 3, 4].map((k) => i32(120 + 12 + side * 20 + k * 4))),
  };
  const table = (countOff, dataOff, stride, cap, fn) => {
    const n = u32(countOff); if (n > cap) throw new Error("bad snapshot count");
    const out = []; for (let k = 0; k < n; k++) out.push(fn(dataOff + k * stride)); return out;
  };
  const champions = table(172, 196, 252, 16, (n) => ({
    index: u32(n + 4), team: u32(n + 8), type: u32(n + 12),
    x: i32(n + 16), y: i32(n + 20), hp: Math.round(f32(n + 24)), maxHp: Math.round(f32(n + 28)),
    alive: !!u32(n + 32), gold: u32(n + 36), gems: u32(n + 40), kills: u32(n + 44),
    items: Array.from({ length: 42 }, (_, k) => u32(n + 48 + k * 4)),
  }));
  const groups = table(180, 18228, 8, 500, (e) => ({ reward: !!u32(e), captureTeam: u32(e + 4) }));
  const wards = table(184, 22228, 8, 128, (e) => ({ team: u32(e), disabled: !!u32(e + 4) }));
  return { header, champions, groups, wards };
}

check(X.Game_Visualiser_Timeline_Seek(game, timeline, 0, 0), "Timeline_Seek");
const rounds = [];
let prev = null, draft = null, last;
for (let steps = 0; steps < 5_000_000; steps++) {
  const s = read();
  last = s;
  if (!draft && s.header.drafts[0].some((d) => d >= 0)) draft = s.header.drafts;
  if (prev && s.header.round !== prev.header.round) rounds.push(compact(prev));
  prev = s;
  if (s.header.outcome || (s.header.awaitingInput && s.header.inputPosition >= size)) break;
  check(X.Game_Visualiser_Timeline_ReplayNext(game, 1), "ReplayNext");
}
if (last) rounds.push(compact(last));

function compact(s) {
  return {
    round: s.header.round,
    gems: s.header.gems,
    champions: s.champions.map((c) => ({ i: c.index, team: c.team, type: c.type, x: c.x, y: c.y, hp: c.hp, maxHp: c.maxHp,
      alive: c.alive, gold: c.gold, gems: c.gems, kills: c.kills, items: c.items.reduce((a, b) => a + b, 0),
      inv: c.items.map((n, k) => (n ? [k, n] : null)).filter(Boolean) })),
    // Camp groups (index = group id): which team holds a pending capture reward / last captured it.
    groups: s.groups.map((g) => (g.reward ? 1 : 0) * 4 + (g.captureTeam & 3)),
    wards: [0, 1].map((tm) => s.wards.filter((w) => w.team === tm && !w.disabled).length),
    captures: [0, 1].map((tm) => s.groups.filter((g) => g.captureTeam === tm).length),
  };
}

fs.writeFileSync(outPath, JSON.stringify({ version: 3, map, outcome: last?.header.outcome, draft: last?.header.drafts ?? draft, rounds }));
console.log(`decoded ${rounds.length} rounds, outcome ${last?.header.outcome}, gems ${last?.header.gems}`);
