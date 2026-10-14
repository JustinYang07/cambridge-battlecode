// Decode a .repl26a replay with the official engine (the visualiser's WebAssembly build) and print
// one JSON object: per-round champion states, camp group states and every action, for any team.
//
//   node lab/tools/decode_replay.mjs <bundle.repl26a | replay.repl26a> [--wasm path] [--every N]
//
// The bundle is a ZIP holding replay.repl26a (platform bundles hold only that); the inner file is
// what the engine reads. Layouts mirror the visualiser's snapshot readers (timeline ABI v1).
import fs from "node:fs";
import path from "node:path";
import os from "node:os";
import zlib from "node:zlib";

const args = process.argv.slice(2);
const file = args.find((a) => !a.startsWith("--"));
const every = Number(args[args.indexOf("--every") + 1]) || 1;
let wasmPath = args.includes("--wasm") ? args[args.indexOf("--wasm") + 1] : null;
if (!wasmPath) {
  const root = path.join(os.homedir(), "Library/Caches/cambc/assets");
  const found = [];
  for (const d of fs.existsSync(root) ? fs.readdirSync(root) : []) {
    const dir = path.join(root, d, "visualiser/assets");
    if (!fs.existsSync(dir)) continue;
    for (const f of fs.readdirSync(dir)) if (/^v\d+_\d+_\d+-.*\.wasm$/.test(f)) found.push(path.join(dir, f));
  }
  wasmPath = found.sort((a, b) => fs.statSync(b).mtimeMs - fs.statSync(a).mtimeMs)[0];
}
if (!file || !wasmPath) { console.error("usage: decode_replay.mjs <replay> (engine wasm not found?)"); process.exit(2); }

// --- the replay bytes: unwrap the bundle ZIP if needed (stored or deflated entry)
function innerReplay(buf) {
  if (buf.readUInt32LE(0) !== 0x04034b50) return buf;
  let off = 0;
  while (off + 30 <= buf.length && buf.readUInt32LE(off) === 0x04034b50) {
    const method = buf.readUInt16LE(off + 8), csize = buf.readUInt32LE(off + 18);
    const nlen = buf.readUInt16LE(off + 26), xlen = buf.readUInt16LE(off + 28);
    const name = buf.toString("utf8", off + 30, off + 30 + nlen);
    const start = off + 30 + nlen + xlen, data = buf.subarray(start, start + csize);
    if (name === "replay.repl26a") return method === 0 ? data : zlib.inflateRawSync(data);
    off = start + csize;
  }
  throw new Error("no replay.repl26a in bundle");
}
const replay = innerReplay(fs.readFileSync(file));

// --- engine
let memory;
const wasi = {
  random_get: (p, n) => { new Uint8Array(memory.buffer, p, n).fill(0); return 0; },
  clock_time_get: (id, prec, out) => { new DataView(memory.buffer).setBigUint64(out, 0n, true); return 0; },
  environ_sizes_get: (a, b) => { const v = new DataView(memory.buffer); v.setUint32(a, 0, true); v.setUint32(b, 0, true); return 0; },
  environ_get: () => 0,
  args_sizes_get: (a, b) => { const v = new DataView(memory.buffer); v.setUint32(a, 0, true); v.setUint32(b, 0, true); return 0; },
  args_get: () => 0, fd_close: () => 0, fd_fdstat_get: () => 0, fd_prestat_get: () => 8, fd_prestat_dir_name: () => 8,
  fd_read: () => 8, fd_seek: () => 70,
  fd_write: (fd, iovs, n, out) => { const v = new DataView(memory.buffer); let c = 0; for (let i = 0; i < n; i++) c += v.getUint32(iovs + i * 8 + 4, true); v.setUint32(out, c, true); return 0; },
  proc_exit: (c) => { throw new Error(`engine exit ${c}`); },
};
const { instance } = await WebAssembly.instantiate(fs.readFileSync(wasmPath), {
  wasi_snapshot_preview1: wasi,
  deep_wasm: { on_assert_fail_impl: () => { throw new Error("engine assertion failed"); } },
});
const E = instance.exports;
memory = E.memory;
E._initialize?.();
const view = () => new DataView(memory.buffer);
const cstr = (p) => { const b = new Uint8Array(memory.buffer); let e = p; while (b[e]) e++; return new TextDecoder().decode(b.subarray(p, e)); };
const check = (rc, what) => { if (rc !== 0) throw new Error(`${what}: engine error ${rc} ${cstr(E.GetLastErrorMsg())}`); };

const out = E.Engine_Malloc(64), args8 = E.Engine_Malloc(64);
const src = E.Engine_Malloc(replay.length);
new Uint8Array(memory.buffer, src, replay.length).set(replay);
check(E.Game_FromReplayFileBytes(src, replay.length, out), "load replay");
const game = view().getBigUint64(out, true);
E.Engine_Free(src);

check(E.Game_Visualiser_Timeline_GetMain(game, out), "main timeline");
const timeline = view().getBigUint64(out, true);
check(E.Game_Visualiser_Timeline_Info(game, timeline, args8, args8 + 8, args8 + 16), "timeline info");
const size = view().getUint32(args8, true);
if (E.Game_Visualiser_TimelineSnapshotSize() !== 40152) throw new Error("unsupported timeline snapshot layout");
const snap = E.Engine_Malloc(40152);

const STEP = "Phase Respawn Capture Tick Move Attack Cast Input EndTurn SetPriority _ Recall SubmitGems Shop PlaceWard Consumable CancelChannel Sandbox GemsSubmitted Draft AltarLock TeleportChannel TeleportComplete MovementBlocked Resign Sell TurnRollover".split(" ");
const CHAMPS = "Ryth VesperVale Vendetta Cassi Issac Stellar Prism Marcella Willow Ren Varg Tavi HeartOfRuin".split(" ");

function read() {
  check(E.Game_Visualiser_ReadTimelineState(game, snap), "read state");
  const v = view(), u = (o) => v.getUint32(snap + o, true), i = (o) => v.getInt32(snap + o, true), f = (o) => v.getFloat32(snap + o, true);
  if (u(0) !== 1 || u(4) !== 40152) throw new Error("unsupported snapshot ABI");
  const kind = u(68);
  const head = {
    round: u(8), team: u(16), awaitingInput: !!u(20), phase: u(24), outcome: u(28), inputPosition: u(56),
    gems: [Number(v.getBigUint64(snap + 32, true)), Number(v.getBigUint64(snap + 40, true))],
    step: { kind: STEP[kind] ?? kind, actor: { kind: u(72), index: u(76) }, from: [i(80), i(84)], to: [i(88), i(92)],
            slot: u(108) !== 4294967295 ? u(108) : undefined },
  };
  const champs = [];
  for (let k = 0, n = u(172); k < n; k++) {
    const o = 196 + k * 252;
    champs.push({ idx: u(o + 4), team: u(o + 8), type: CHAMPS[u(o + 12)] ?? u(o + 12), x: i(o + 16), y: i(o + 20),
                  hp: Math.round(f(o + 24)), maxHp: Math.round(f(o + 28)), alive: !!u(o + 32), gold: u(o + 36),
                  gems: u(o + 40), kills: u(o + 44) });
  }
  const groups = [];
  for (let k = 0, n = u(180); k < n; k++) groups.push({ reward: !!u(18228 + k * 8), captureTeam: u(18228 + k * 8 + 4) });
  return { head, champs, groups };
}

check(E.Game_Visualiser_Timeline_Seek(game, timeline, 0, 0), "seek");
const rounds = [], actions = [];
let lastRound = -1;
// `size` counts recorded inputs, not engine steps: step until the game ends or would need an input
// beyond the recording (the visualiser's indexer does the same).
for (let guard = 0; guard < 5e6; guard++) {
  const s = read();
  const st = s.head.step;
  if (["Move", "Attack", "Cast", "Recall", "SubmitGems", "Shop", "Consumable", "AltarLock", "Capture", "GemsSubmitted"].includes(st.kind))
    actions.push([s.head.round, st.kind, st.actor.kind, st.actor.index, st.from, st.to, st.slot ?? -1]);
  if (s.head.round !== lastRound && s.head.round % every === 0) {
    rounds.push({ r: s.head.round, gems: s.head.gems, champs: s.champs.map((c) => [c.idx, c.team, c.type, c.x, c.y, c.hp, c.maxHp, c.alive ? 1 : 0, c.gold, c.gems, c.kills]),
                  groups: s.groups.map((g) => (g.reward ? 1 : 0) + 2 * g.captureTeam) });
    lastRound = s.head.round;
  }
  if (s.head.outcome || (s.head.awaitingInput && s.head.inputPosition >= size)) break;
  check(E.Game_Visualiser_Timeline_ReplayNext(game, 1), "next");
}
const last = read();
process.stdout.write(JSON.stringify({ file: path.basename(file), size, finalRound: last.head.round, outcome: last.head.outcome,
  finalGems: last.head.gems, champFields: ["idx", "team", "type", "x", "y", "hp", "maxHp", "alive", "gold", "gems", "kills"],
  actionFields: ["round", "kind", "actorKind", "actorIndex", "from", "to", "slot"], rounds, actions }));
