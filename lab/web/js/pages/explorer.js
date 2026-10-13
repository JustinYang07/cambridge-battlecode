// Decision explorer: replay a game through our bots' eyes. For every champion and round it
// shows what the bot chose, the alternatives it considered, and the points behind each score.
import { api, cssVar, fmt, h, kindColor, kindSwatch, mount, onKey } from "../ui.js";
import { cellSizeFor, drawMap, drawPoint, drawTarget, drawTrail } from "../mapview.js";

const REGION_COLORS = ["#f97316", "#22c55e", "#3b82f6", "#e879f9", "#facc15", "#14b8a6", "#ef4444", "#a3a3a3"];
const SLOT_COLORS = ["#38bdf8", "#f472b6", "#a3e635", "#fb923c", "#c084fc", "#facc15", "#2dd4bf", "#f87171"];
const ACTIONS = [
  [/^m(-?\d+),(-?\d+)$/, "move to ($1,$2)"], [/^d(-?\d+),(-?\d+)$/, "dash to ($1,$2)"],
  [/^ae(-?\d+),(-?\d+)$/, "attack enemy at ($1,$2)"], [/^ac(-?\d+),(-?\d+)$/, "attack camp at ($1,$2)"],
  [/^aoe(-?\d+),(-?\d+)$/, "area spell at ($1,$2)"], [/^st(-?\d+),(-?\d+)$/, "stun enemy at ($1,$2)"],
  [/^se(-?\d+),(-?\d+)$/, "spell enemy at ($1,$2)"], [/^sc(-?\d+),(-?\d+)$/, "spell camp at ($1,$2)"],
  [/^h(-?\d+),(-?\d+)$/, "heal ally at ($1,$2)"], [/^t(\d+)$/, "tribute $1 gems"], [/^b(\d+)(\w+)$/, "buy $1 $2 shard(s)"],
];
const SIMPLE = { r: "attack reset", sh: "shield", hold: "hold for party", cap: "hold capture", rest: "rest on spawn",
  rc: "recall", "ch:rc": "channelling recall", "ch:t": "channelling tribute", "ch:tp": "channelling teleport" };

export function describeActions(act) {
  if (!act) return "no action";
  return act.split(";").filter(Boolean).map((a) => {
    if (SIMPLE[a]) return SIMPLE[a];
    for (const [re, text] of ACTIONS) if (re.test(a)) return a.replace(re, text);
    return a;
  }).join(" · ");
}

export async function render({ path }) {
  if (!path) { mount(h("div.empty", "No replay selected. ", h("a", { href: "#/replays" }, "Pick one"))); return; }
  const data = await api(`/api/replay?path=${encodeURIComponent(path)}`);
  const game = data.game;
  const mapInfo = game?.map ? await api(`/api/maps/${game.map}`).catch(() => null) : null;
  const traced = ["a", "b"].filter((s) => data.teams[s].turns.length);
  const state = {
    tab: traced.length ? "decisions" : "visualiser",
    side: game ? (game.candidateSide === "B" && traced.includes("b") ? "b" : traced[0]) : traced[0],
    slot: 0, round: 1,
  };

  const root = h("div");
  mount(root);
  let stopKeys = () => {};

  function teamName(side) {
    if (game) return side === "a" ? game.botA : game.botB;
    return `team ${side.toUpperCase()}`;
  }

  function draw() {
    stopKeys();
    const header = h("div.page-head",
      h("div",
        h("h1", game ? `${game.botA} vs ${game.botB}` : data.file),
        h("p", game ? [`${game.map || "unknown map"} · `, game.result?.winner ? `team ${game.result.winner} won` : "no result",
          game.result?.teams ? ` · ${game.result.teams[0].gems}–${game.result.teams[1].gems} gems · round ${game.result.rounds}` : "",
          game.experiment ? [" · ", h("a", { href: `#/exp/${game.experiment}` }, game.experimentName || game.experiment)] : " · scraped from the platform"] : data.path),
        game?.result?.teams ? h("p.hint", game.result.teams.map((t, i) => `${i ? "B" : "A"}: ${t.draft.join(", ")} (${t.kills} kills, ${t.deaths} deaths)`).join("   ·   ")) : null),
      h("div.row",
        traced.length > 1 ? h("div.chips", traced.map((s) => h("span.chip" + (s === state.side ? ".on" : ""),
          { onclick: () => { state.side = s; state.slot = 0; draw(); } }, `${s.toUpperCase()}: ${teamName(s)}`))) : null,
        h("a.btn", { href: `/files/${encodeURI(data.path)}`, download: "" }, "Download")));
    const tabs = h("div.tabs", [["decisions", "Decisions"], ["visualiser", "Official visualiser"], ["draft", "Draft"], ["logs", "Raw logs"]]
      .map(([k, label]) => h("button" + (state.tab === k ? ".on" : ""), { onclick: () => { state.tab = k; draw(); } }, label)));
    let body;
    if (state.tab === "decisions") body = traced.length ? decisions() : h("div.empty", "Neither team printed basicBot decision traces. Use the official visualiser or raw logs.");
    else if (state.tab === "visualiser") body = h("iframe.vis", { src: `/visualiser/index.html?replay=${encodeURIComponent(`/files/${data.path}`)}` });
    else if (state.tab === "draft") body = draft();
    else body = logs();
    root.replaceChildren(header, tabs, body);
  }

  // ------------------------------------------------------------------ decisions tab
  function decisions() {
    const team = data.teams[state.side];
    const slots = [...new Set(team.turns.map((t) => t.u))].sort((a, b) => a - b);
    if (!slots.includes(state.slot)) state.slot = slots[0];
    const bySlot = Object.fromEntries(slots.map((u) => [u, team.turns.filter((t) => t.u === u)]));
    const detailsBySlot = Object.fromEntries(slots.map((u) => [u, team.details.filter((d) => d.u === u)]));
    const maxRound = Math.max(...team.turns.map((t) => t.r));
    const firstRound = Math.min(...team.turns.map((t) => t.r));
    state.round = Math.min(Math.max(state.round, firstRound), maxRound);

    const timeline = h("canvas", { height: slots.length * 26 + 26 });
    const mapCanvas = h("canvas");
    const champList = h("div");
    const why = h("div");
    const roundLabel = h("b.mono");
    const slider = h("input", { type: "range", min: 1, max: maxRound, value: state.round, style: { flex: 1 },
      oninput: (e) => setRound(+e.target.value) });

    const unitName = (u) => team.units?.[u] || `slot ${u}`;
    const turnAt = (u, r) => {
      const list = bySlot[u];
      let lo = 0, hi = list.length - 1, best = null;
      while (lo <= hi) { const mid = (lo + hi) >> 1; if (list[mid].r <= r) { best = list[mid]; lo = mid + 1; } else hi = mid - 1; }
      return best;
    };
    const detailAt = (u, r) => {
      let best = null;
      for (const d of detailsBySlot[u]) { if (d.r <= r) best = d; else break; }
      return best;
    };
    const targetPoint = (code) => {
      if (!code) return null;
      let m = code.match(/^g(\d+)$/);
      if (m && team.groups[m[1]]) return [team.groups[m[1]].x, team.groups[m[1]].y];
      m = code.match(/^a(\d+)$/);
      if (m && team.altars[+m[1]]) return team.altars[+m[1]];
      m = code.match(/^e\((-?\d+),(-?\d+)\)$/);
      if (m) return [+m[1], +m[2]];
      return null;
    };

    // Map frame: real terrain when the lab knows the map, otherwise a legend-only sketch.
    const info = mapInfo || legendInfo(team);
    function drawMapFrame() {
      const width = Math.max(260, mapCanvas.parentElement?.clientWidth || 400);
      const cell = cellSizeFor(info, width, 460);
      const ctx = drawMap(mapCanvas, info, { cell, legendOnly: !mapInfo });
      // Team plan: ring each camp group in its region's colour.
      if (team.plan) {
        team.plan.groupRegion.forEach((r, gid) => {
          const g = team.groups[gid];
          if (r < 0 || !g) return;
          ctx.strokeStyle = REGION_COLORS[r % REGION_COLORS.length];
          ctx.lineWidth = 2;
          ctx.beginPath();
          ctx.arc(g.x * cell + cell / 2, g.y * cell + cell / 2, Math.max(cell * 1.6, 6), 0, Math.PI * 2);
          ctx.stroke();
        });
      }
      const sel = turnAt(state.slot, state.round);
      const trail = bySlot[state.slot].filter((t) => t.r <= state.round && t.r > state.round - 40 && t.x >= 0).map((t) => [t.x, t.y]);
      drawTrail(ctx, cell, trail, SLOT_COLORS[state.slot % 8]);
      const tp = sel && targetPoint(sel.t);
      if (tp) drawTarget(ctx, cell, tp[0], tp[1], kindColor(sel.k));
      for (const u of slots) {
        const t = turnAt(u, state.round);
        if (!t || t.x < 0 || t.k === "dead") continue;
        drawPoint(ctx, cell, t.x, t.y, SLOT_COLORS[u % 8], u === state.slot ? 0.9 : 0.6, u === state.slot ? cssVar("--text") : null);
      }
    }

    function drawTimeline() {
      const width = Math.max(400, timeline.parentElement?.clientWidth || 900);
      const dpr = window.devicePixelRatio || 1;
      const height = slots.length * 26 + 26;
      timeline.width = width * dpr; timeline.height = height * dpr;
      timeline.style.height = height + "px";
      const ctx = timeline.getContext("2d");
      ctx.scale(dpr, dpr);
      ctx.fillStyle = cssVar("--panel"); ctx.fillRect(0, 0, width, height);
      const left = 90, plot = width - left - 8;
      const xOf = (r) => left + ((r - 1) / Math.max(1, maxRound - 1)) * plot;
      ctx.font = "11px system-ui"; ctx.textBaseline = "middle";
      slots.forEach((u, i) => {
        const y = 4 + i * 26;
        ctx.fillStyle = u === state.slot ? cssVar("--text") : cssVar("--muted");
        ctx.fillText(unitName(u).slice(0, 12), 6, y + 10);
        const list = bySlot[u];
        for (let k = 0; k < list.length; k++) {
          const t = list[k], next = list[k + 1];
          const x0 = xOf(t.r), x1 = next ? xOf(next.r) : xOf(t.r + 1);
          ctx.fillStyle = kindColor(t.k);
          ctx.fillRect(x0, y, Math.max(1, x1 - x0), 20);
        }
        // Health line inside the lane.
        ctx.strokeStyle = "#ffffffaa"; ctx.lineWidth = 1; ctx.beginPath();
        let started = false;
        for (const t of list) {
          if (t.k === "dead") { started = false; continue; }
          const d = detailAt(u, t.r);
          const maxHp = d?.mhp || 1200;
          const yy = y + 20 - Math.min(1, t.hp / maxHp) * 18;
          if (!started) { ctx.moveTo(xOf(t.r), yy); started = true; } else ctx.lineTo(xOf(t.r), yy);
        }
        ctx.stroke();
        let lost = 0;
        for (const t of list) { if (t.lost > lost) { ctx.fillStyle = cssVar("--bad"); ctx.fillRect(xOf(t.r) - 1, y - 2, 2, 4); } lost = t.lost; }
      });
      const ay = height - 14;
      ctx.fillStyle = cssVar("--faint");
      for (let r = 0; r <= maxRound; r += maxRound > 1000 ? 200 : 100) { ctx.fillText(String(r), xOf(Math.max(r, 1)) - 8, ay); }
      ctx.strokeStyle = cssVar("--accent"); ctx.lineWidth = 2; ctx.beginPath();
      ctx.moveTo(xOf(state.round), 0); ctx.lineTo(xOf(state.round), height - 22); ctx.stroke();
      timeline.onclick = timeline.onmousemove = (e) => {
        if (e.type === "mousemove" && e.buttons !== 1) return;
        const rect = timeline.getBoundingClientRect();
        const r = Math.round(1 + ((e.clientX - rect.left - left) / plot) * (maxRound - 1));
        const lane = Math.floor((e.clientY - rect.top - 4) / 26);
        if (e.type === "click" && slots[lane] !== undefined) state.slot = slots[lane];
        setRound(Math.min(maxRound, Math.max(1, r)));
      };
    }

    function drawChamps() {
      champList.replaceChildren(h("table", h("tbody", slots.map((u) => {
        const t = turnAt(u, state.round);
        return h("tr.click", { onclick: () => { state.slot = u; refresh(); } },
          h("td", h("span.kind", { style: { background: SLOT_COLORS[u % 8], borderRadius: "50%" } }), u === state.slot ? h("b", unitName(u)) : unitName(u)),
          h("td", team.plan && team.plan.slotRegion[u] !== undefined
            ? h("span.pill", { style: { color: REGION_COLORS[team.plan.slotRegion[u] % REGION_COLORS.length] } }, `region ${team.plan.slotRegion[u]}`) : ""),
          h("td", t ? [kindSwatch(t.k), t.k] : "–"),
          h("td.muted", t?.label || ""),
          h("td.num", t ? fmt.int(t.hp) : "–"),
          h("td.num", t ? `${t.gm} gems` : ""));
      }))));
    }

    function drawWhy() {
      const t = turnAt(state.slot, state.round);
      const d = detailAt(state.slot, state.round);
      if (!t) { why.replaceChildren(h("div.empty", "No trace for this champion yet.")); return; }
      const opts = d?.opts || [];
      const maxScore = Math.max(1, ...opts.map((o) => o.s));
      why.replaceChildren(
        h("div.card-head", h("h2", `${unitName(state.slot)} · round ${t.r}`),
          h("div.row", h("button", { onclick: () => jumpChange(-1), title: "previous decision change ([)" }, "◀ change"),
            h("button", { onclick: () => jumpChange(1), title: "next decision change (])" }, "change ▶"))),
        h("dl.kv",
          h("dt", "doing"), h("dd", kindSwatch(t.k), `${t.k} ${t.label || ""}`),
          h("dt", "actions"), h("dd", describeActions(t.act)),
          h("dt", "position"), h("dd", t.x >= 0 ? `(${t.x}, ${t.y})` : "–"),
          h("dt", "health"), h("dd", d ? `${fmt.int(t.hp)} / ${fmt.int(d.mhp)}` : fmt.int(t.hp)),
          h("dt", "bag · gold"), h("dd", `${t.gm} gems · ${t.au} gold`),
          h("dt", "fuel used"), h("dd", t.fuel ? `${fmt.int(t.fuel)} / ~97k` : "–", t.lost ? h("span.bad", ` · ${t.lost} turns lost so far`) : "")),
        h("h3", { style: { marginTop: "16px" } }, d ? `Options considered (scored on round ${d.r}${d.r !== t.r ? `, ${t.r - d.r} rounds ago` : ""})` : "No detailed decision recorded yet"),
        d?.note ? h("p.hint", d.note) : null,
        ...opts.map((o, i) => optionCard(o, i === 0, maxScore)));
    }

    function optionCard(o, chosen, maxScore) {
      const plus = o.f.filter((f) => f[2] === "+"), time = o.f.filter((f) => f[2] === "t"), mul = o.f.filter((f) => f[2] === "*");
      const reward = plus.reduce((s, f) => s + f[1], 0), turns = time.reduce((s, f) => s + f[1], 0);
      const factor = mul.reduce((s, f) => s * f[1], 1);
      return h("div.opt" + (chosen ? ".chosen" : ""),
        h("div.top", h("div", kindSwatch(o.k), h("b", o.k), " ", h("span.muted", o.label), chosen ? h("span.pill.win", { style: { marginLeft: "8px" } }, "chosen") : null),
          h("span.score", fmt.num(o.s))),
        h("div.bar", h("div", { style: { width: `${Math.max(2, (100 * o.s) / maxScore)}%`, background: kindColor(o.k) } })),
        h("div.factors",
          plus.map((f) => h("span.factor.plus", { title: "reward (gem-equivalents)" }, `+${fmt.num(f[1])} ${f[0]}`)),
          time.map((f) => h("span.factor.time", { title: "turns this will take" }, `${fmt.num(f[1])}t ${f[0]}`)),
          mul.map((f) => h("span.factor.mul" + (f[1] < 1 ? ".down" : ""), { title: "multiplier" }, `×${fmt.num(f[1], 2)} ${f[0]}`))),
        h("div.formula", `${fmt.num(reward)} reward ÷ ${fmt.num(Math.max(turns, 1))} turns × 100 × ${fmt.num(factor, 2)} = ${fmt.num(o.s)} pts per 100 turns`),
        o.n ? h("div.hint", o.n) : null);
    }

    function jumpChange(dir) {
      const list = bySlot[state.slot];
      const cur = turnAt(state.slot, state.round);
      if (!cur) return;
      const idx = list.indexOf(cur);
      for (let i = idx + dir; i >= 0 && i < list.length; i += dir) {
        if (list[i].k !== cur.k || list[i].t !== cur.t) { setRound(list[i].r); return; }
      }
    }

    function setRound(r) {
      state.round = r;
      slider.value = r;
      refresh();
    }

    function refresh() {
      roundLabel.textContent = `round ${state.round} / ${maxRound}`;
      drawTimeline();
      drawMapFrame();
      drawChamps();
      drawWhy();
    }

    stopKeys = onKey((e) => {
      if (e.key === "ArrowRight") setRound(Math.min(maxRound, state.round + (e.shiftKey ? 10 : 1)));
      else if (e.key === "ArrowLeft") setRound(Math.max(1, state.round - (e.shiftKey ? 10 : 1)));
      else if (e.key === "ArrowDown" || e.key === "ArrowUp") {
        const i = slots.indexOf(state.slot) + (e.key === "ArrowDown" ? 1 : -1);
        state.slot = slots[(i + slots.length) % slots.length];
        refresh();
      } else if (e.key === "]") jumpChange(1);
      else if (e.key === "[") jumpChange(-1);
      else return;
      e.preventDefault();
    });

    const summary = h("div.tiles", slots.map((u) => {
      const list = bySlot[u];
      const share = (k) => list.filter((t) => t.k === k).length / Math.max(1, list.length);
      const deaths = list.filter((t, i) => t.k === "dead" && i > 0 && list[i - 1].k !== "dead").length;
      return h("div.tile",
        h("div", h("span.kind", { style: { background: SLOT_COLORS[u % 8], borderRadius: "50%" } }), h("b", unitName(u))),
        h("div.l", `farm ${fmt.pct(share("farm") + share("capture"))} · bank ${fmt.pct(share("bank"))} · heal ${fmt.pct(share("heal"))} · dead ${fmt.pct(share("dead"))}`),
        h("div.l", `${deaths} deaths · ${list[list.length - 1]?.lost || 0} lost turns`));
    }));

    const wrap = h("div",
      summary,
      h("div.card", { style: { marginBottom: "16px" } },
        h("div.card-head", h("h2", "Timeline"), h("div.row", roundLabel,
          h("span.hint", h("span.kbd", "←"), h("span.kbd", "→"), " round · ", h("span.kbd", "shift"), " ×10 · ", h("span.kbd", "↑"), h("span.kbd", "↓"), " champion · ", h("span.kbd", "["), h("span.kbd", "]"), " decision changes"))),
        h("div.legend", ["farm", "capture", "bank", "heal", "flee", "fight", "wait", "channel", "dead"].map((k) => h("span", kindSwatch(k), k)),
          h("span", "white line = health · red tick = lost turn")),
        h("div.timeline-wrap", timeline),
        h("div.row", { style: { marginTop: "8px" } }, slider)),
      h("div.explorer",
        h("div.grid", { style: { alignContent: "start" } },
          h("div.card", h("h2", mapInfo ? `Map · ${game.map}` : "Map (legend only)"), mapCanvas,
            h("p.hint", "Dots are your champions at this round; the line is the selected champion's last 40 rounds; the crosshair is its current target. Coloured rings show the team plan's regions (each champion's home region is in the list below).")),
          h("div.card", h("h2", "Champions"), champList)),
        h("div.card", why)));
    requestAnimationFrame(refresh);
    return wrap;
  }

  function draft() {
    return h("div.grid.cols-2", ["a", "b"].map((s) => h("div.card", h("h2", `Team ${s.toUpperCase()}: ${teamName(s)}`),
      data.teams[s].drafts.length ? data.teams[s].drafts.map((d) => {
        const max = Math.max(...d.opts.map((o) => o.s));
        return h("div", { style: { marginBottom: "14px" } }, h("b", `Pick ${d.pick + 1}: ${d.chosen}`),
          d.opts.slice(0, 6).map((o) => h("div.row", { style: { gap: "10px" } },
            h("span", { style: { width: "110px" } }, o.k),
            h("div.bar", { style: { flex: 1, margin: 0 } }, h("div", { style: { width: `${(100 * o.s) / max}%`, background: o.k === d.chosen ? cssVar("--accent") : cssVar("--line-2") } })),
            h("span.mono", fmt.num(o.s)))));
      }) : h("div.empty", "No draft trace (not a basicBot, or traces disabled)."))));
  }

  function logs() {
    return h("div.grid.cols-2", ["a", "b"].map((s) => h("div.card", h("h2", `Team ${s.toUpperCase()}: ${teamName(s)}`),
      h("pre.json", data.teams[s].other.slice(0, 1500).map((o) => o.text).join("\n") || "(no non-trace output)"))));
  }

  draw();
  return () => stopKeys();
}

function legendInfo(team) {
  const pts = [...Object.values(team.groups).map((g) => [g.x, g.y]), ...team.altars, ...team.turns.filter((t) => t.x >= 0).map((t) => [t.x, t.y])];
  const w = Math.max(10, ...pts.map((p) => p[0] + 2)), hh = Math.max(10, ...pts.map((p) => p[1] + 2));
  return { w, h: hh, altars: team.altars, groupLegend: Object.values(team.groups) };
}
