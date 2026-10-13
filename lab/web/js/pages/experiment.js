import { api, fmt, gemsFor, h, mount, resultPill, toast } from "../ui.js";

export async function render({ match }) {
  const id = match[1];
  let timer = null;
  let compareId = "";
  const others = (await api("/api/experiments")).filter((e) => e.id !== id && e.status === "done");

  async function draw() {
    const exp = await api(`/api/experiments/${id}`);
    const cmp = compareId ? await api(`/api/experiments/${compareId}`) : null;
    mount(page(exp, cmp, others, (v) => { compareId = v; draw(); }, compareId));
    const active = ["queued", "building", "running"].includes(exp.status);
    clearTimeout(timer);
    if (active) timer = setTimeout(draw, 2000);
  }
  await draw();
  return () => clearTimeout(timer);
}

function page(exp, cmp, others, onCompare, compareId) {
  const s = exp.summary || {};
  const o = s.overall || {};
  const done = exp.games.filter((g) => ["done", "error", "cancelled"].includes(g.status)).length;
  const active = ["queued", "building", "running"].includes(exp.status);
  const actions = h("div.row",
    active ? h("button.danger", { onclick: async () => { await api(`/api/experiments/${exp.id}/cancel`, { method: "POST", body: {} }); toast("Cancelling…"); } }, "Cancel") : null,
    !active ? h("button.danger", { onclick: async () => {
      if (!confirm("Delete this experiment and its replays?")) return;
      await api(`/api/experiments/${exp.id}`, { method: "DELETE" });
      location.hash = "#/lab";
    } }, "Delete") : null);

  return h("div",
    h("div.page-head",
      h("div", h("h1", exp.name),
        h("p", `${exp.candidate} vs ${exp.opponents.join(", ")} · ${exp.maps.length} maps · ${exp.bothSides ? "both sides" : "side A only"} · commit ${exp.gitCommit || "?"} · ${fmt.ago(exp.createdAt)}`),
        exp.notes ? h("p", h("i", exp.notes)) : null),
      actions),
    exp.error ? h("div.card", h("h2.bad", "Failed"), h("pre.json", exp.error)) : null,
    h("div.card", { style: { marginBottom: "16px" } },
      h("div.row", { style: { justifyContent: "space-between" } }, h("b", `${exp.status} · ${done}/${exp.games.length} games`),
        h("span.muted", exp.startedAt ? `${Math.round(((exp.finishedAt || Date.now() / 1000) - exp.startedAt))}s` : "")),
      h("div.progress", { style: { marginTop: "8px" } }, h("div", { style: { width: `${(100 * done) / Math.max(1, exp.games.length)}%` } }))),
    h("div.tiles",
      tile(`${o.wins ?? 0}-${o.losses ?? 0}`, "wins-losses"),
      tile(fmt.pct(o.played ? o.wins / o.played : null), "win rate", o.played ? (o.wins / o.played >= 0.5 ? "good" : "bad") : ""),
      tile(fmt.signed(o.played ? o.gemDiff / o.played : null), "avg gem diff", o.gemDiff > 0 ? "good" : o.gemDiff < 0 ? "bad" : ""),
      tile(fmt.int(o.played ? o.gemsFor / o.played : null), "avg gems scored"),
      tile(fmt.int(o.played ? o.rounds / o.played : null), "avg rounds"),
      tile(o.faults ?? 0, "bot faults", o.faults ? "bad" : ""),
      tile(fmt.num(o.played ? o.deaths / o.played : null), "deaths / game"),
      tile(fmt.num(o.played ? o.killedByEnemy / o.played : null), "killed by enemy / game", o.killedByEnemy ? "warn" : ""),
      tile(fmt.int(o.played ? o.gemsLost / o.played : null), "gems lost / game")),
    h("div.grid.cols-2",
      h("div.card", h("h2", "Results by map"), matrix(exp, cmp)),
      h("div.card",
        h("div.card-head", h("h2", "Compare"),
          h("select", { onchange: (e) => onCompare(e.target.value) },
            h("option", { value: "" }, "— compare with —"),
            others.map((e) => h("option", { value: e.id, selected: e.id === compareId }, `${e.name} (${e.id})`)))),
        cmp ? compareTable(exp, cmp) : h("p.hint", "Pick an earlier experiment on the same maps to see per-map gem deltas. Δ is this experiment minus the other."),
        h("h2", { style: { marginTop: "16px" } }, "By opponent"),
        h("table", h("thead", h("tr", h("th", "opponent"), h("th.num", "W-L"), h("th.num", "avg gem diff"))),
          h("tbody", Object.entries(s.perOpponent || {}).map(([k, v]) => h("tr", h("td", k), h("td.num", `${v.wins}-${v.losses}`),
            h("td.num", fmt.signed(v.gemDiff / Math.max(1, v.played))))))))),
    h("div.card", { style: { marginTop: "16px" } }, h("h2", "Champion stats (candidate, summed over games)"), championTable(exp)),
    h("div.card", { style: { marginTop: "16px" } }, h("h2", "Games"), gamesTable(exp)));
}

function tile(v, l, cls = "") { return h("div.tile", h("div.v" + (cls ? "." + cls : ""), v), h("div.l", l)); }

function matrix(exp, cmp) {
  const cols = [];
  for (const opp of exp.opponents) for (const side of exp.bothSides ? ["A", "B"] : ["A"]) cols.push([opp, side]);
  const grid = h("div.matrix", { style: { gridTemplateColumns: `110px repeat(${cols.length}, minmax(80px, 1fr))` } });
  grid.append(h("div"));
  for (const [opp, side] of cols) grid.append(h("div.muted", { style: { fontSize: "11.5px", textAlign: "center" } }, `${opp} · as ${side}`));
  for (const map of exp.maps) {
    grid.append(h("div", { style: { alignSelf: "center" } }, map));
    for (const [opp, side] of cols) {
      const g = exp.games.find((x) => x.map === map && x.opponent === opp && x.candidateSide === side);
      const gems = g && gemsFor(g);
      const win = g?.result?.winner === side;
      const prev = cmp?.games.find((x) => x.map === map && x.opponent === opp && x.candidateSide === side);
      const prevGems = prev && gemsFor(prev);
      const delta = gems && prevGems ? (gems[0] - gems[1]) - (prevGems[0] - prevGems[1]) : null;
      const cell = h("div.cell" + (!gems ? ".pending" : win ? ".win" : ".loss"), {
        title: g?.result?.error || "",
        style: { cursor: g?.result?.replay ? "pointer" : "default" },
        onclick: () => g?.result?.replay && (location.hash = `#/replay?path=${encodeURIComponent(g.result.replay)}`),
      }, gems ? `${gems[0]}–${gems[1]}` : g?.status || "–",
        delta !== null ? h("div", { class: delta >= 0 ? "good" : "bad", style: { fontSize: "11px" } }, `Δ ${fmt.signed(delta)}`) : null);
      grid.append(cell);
    }
  }
  return h("div", grid, h("p.hint", "Cells show candidate gems – opponent gems. Click a cell to open its replay in the decision explorer."));
}

function compareTable(exp, cmp) {
  const rows = exp.maps.map((map) => {
    const diff = (e) => {
      const gs = e.games.filter((g) => g.map === map && g.result?.teams);
      if (!gs.length) return null;
      return gs.reduce((s, g) => { const x = gemsFor(g); return s + x[0] - x[1]; }, 0) / gs.length;
    };
    const a = diff(exp), b = diff(cmp);
    return h("tr", h("td", map), h("td.num", fmt.signed(b)), h("td.num", fmt.signed(a)),
      h("td.num", a !== null && b !== null ? h("span." + (a - b >= 0 ? "good" : "bad"), fmt.signed(a - b)) : "–"));
  });
  const oa = exp.summary?.overall, ob = cmp.summary?.overall;
  return h("table", h("thead", h("tr", h("th", "map"), h("th.num", cmp.name.slice(0, 18)), h("th.num", "this"), h("th.num", "Δ"))),
    h("tbody", rows, h("tr", h("td", h("b", "overall W-L")), h("td.num", ob ? `${ob.wins}-${ob.losses}` : "–"), h("td.num", oa ? `${oa.wins}-${oa.losses}` : "–"), h("td"))));
}

function championTable(exp) {
  const agg = {};
  for (const g of exp.games) {
    const r = g.result;
    if (!r?.teams) continue;
    const mine = r.teams[g.candidateSide === "A" ? 0 : 1];
    for (const c of mine.champions) {
      const a = (agg[c.name] ||= { games: 0, submittedGems: 0, collectedGems: 0, kills: 0, deaths: 0, earnedGold: 0, monsterDamageDone: 0, monsterDamageTaken: 0, championDamageDone: 0 });
      a.games += 1;
      for (const k of Object.keys(a)) if (k !== "games") a[k] += c[k] || 0;
    }
  }
  const names = Object.keys(agg).sort((a, b) => agg[b].submittedGems / agg[b].games - agg[a].submittedGems / agg[a].games);
  if (!names.length) return h("div.empty", "No finished games yet.");
  const per = (a, k) => fmt.int(a[k] / a.games);
  return h("div.scroll", h("table",
    h("thead", h("tr", h("th", "champion"), h("th.num", "games"), h("th.num", "gems tributed/g"), h("th.num", "gems picked up/g"),
      h("th.num", "deaths/g"), h("th.num", "kills/g"), h("th.num", "gold/g"), h("th.num", "camp dmg dealt/g"), h("th.num", "camp dmg taken/g"))),
    h("tbody", names.map((n) => { const a = agg[n]; return h("tr", h("td", h("b", n)), h("td.num", a.games), h("td.num", per(a, "submittedGems")),
      h("td.num", per(a, "collectedGems")), h("td.num", fmt.num(a.deaths / a.games)), h("td.num", fmt.num(a.kills / a.games)),
      h("td.num", per(a, "earnedGold")), h("td.num", per(a, "monsterDamageDone")), h("td.num", per(a, "monsterDamageTaken"))); }))));
}

function gamesTable(exp) {
  return h("div.scroll", h("table",
    h("thead", h("tr", h("th", "map"), h("th", "opponent"), h("th", "side"), h("th", "result"), h("th.num", "gems"), h("th.num", "round"),
      h("th", "our draft"), h("th.num", "time"), h("th", ""))),
    h("tbody", exp.games.map((g) => {
      const r = g.result || {};
      const gems = gemsFor(g);
      const mine = r.teams ? r.teams[g.candidateSide === "A" ? 0 : 1] : null;
      return h("tr",
        h("td", g.map), h("td", g.opponent), h("td", g.candidateSide), h("td", resultPill(g)),
        h("td.num", gems ? `${gems[0]}–${gems[1]}` : "–"), h("td.num", r.rounds ?? "–"),
        h("td.muted", mine ? mine.draft.join(", ") : r.error ? h("span.bad", r.error.slice(0, 80)) : ""),
        h("td.num", r.seconds ? `${r.seconds}s` : ""),
        h("td", r.replay ? h("a", { href: `#/replay?path=${encodeURIComponent(r.replay)}` }, "inspect →") : ""));
    }))));
}
