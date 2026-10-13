import { api, fmt, h, mount } from "../ui.js";
import { cellSizeFor, drawMap } from "../mapview.js";

export async function render() {
  const [status, experiments] = await Promise.all([api("/api/status"), api("/api/experiments")]);
  const latest = experiments.find((e) => e.status === "done") || experiments[0];
  const ladderCard = h("div.card", h("div.card-head", h("h2", "Ladder (live)"), h("a", { href: "#/platform" }, "Full ladder →")),
    h("div.muted", "Loading…"));

  const myBots = status.bots.filter((b) => b.kind === "bot" || b.kind === "snapshot");
  const view = h("div",
    h("div.page-head",
      h("div", h("h1", "Dashboard"), h("p", "Backtest bots, inspect decisions and scout the ladder.")),
      h("div.row", h("a.btn", { href: "#/lab" }, "New A/B test"), h("a.btn", { href: "#/replays" }, "Open a replay"))),
    h("div.tiles",
      tile(status.bots.filter((b) => b.kind === "bot").length, "bots"),
      tile(status.bots.filter((b) => b.kind === "snapshot").length, "snapshots"),
      tile(status.maps.length, "maps"),
      tile(experiments.length, "experiments"),
      tile(status.visualiser ? "yes" : "no", "official visualiser"),
      tile(status.cpu, "CPU cores")),
    h("div.grid.cols-2",
      latestCard(latest),
      ladderCard,
      h("div.card",
        h("div.card-head", h("h2", "Bots"), h("a", { href: "#/lab" }, "Snapshot a bot →")),
        h("table", h("thead", h("tr", h("th", "name"), h("th", "kind"), h("th", "files"), h("th", "edited"))),
          h("tbody", myBots.map((b) => h("tr",
            h("td", h("b", b.name), b.dirty ? h("span.pill", { style: { marginLeft: "6px" } }, "uncommitted") : null),
            h("td.muted", b.kind), h("td.num", b.files), h("td.muted", fmt.ago(b.modified))))))),
      h("div.card", h("div.card-head", h("h2", "Recent experiments"), h("a", { href: "#/lab" }, "All →")),
        experiments.length ? h("table", h("tbody", experiments.slice(0, 8).map((e) => expRow(e)))) : h("div.empty", "No experiments yet."))),
  );
  mount(view);

  api("/api/platform/ladder").then((ladder) => {
    ladderCard.replaceChild(h("table",
      h("thead", h("tr", h("th", "#"), h("th", "team"), h("th.num", "rating"), h("th.num", "W-L"))),
      h("tbody", ladder.teams.slice(0, 10).map((t) => h("tr",
        h("td.num", t.rank), h("td", t.name), h("td.num", fmt.int(t.rating)), h("td.num", `${t.wins}-${t.losses}`))))),
    ladderCard.lastChild);
  }).catch((err) => { ladderCard.lastChild.textContent = `Ladder unavailable: ${err.message}`; });
}

function tile(v, l) { return h("div.tile", h("div.v", v), h("div.l", l)); }

function latestCard(e) {
  if (!e) return h("div.card", h("h2", "Latest experiment"), h("div.empty", "Run your first A/B test from the A/B Lab."));
  const o = e.summary?.overall || {};
  return h("div.card",
    h("div.card-head", h("h2", "Latest experiment"), h("a", { href: `#/exp/${e.id}` }, "Open →")),
    h("div", h("b", e.name), h("div.muted", `${e.id} · ${fmt.ago(e.createdAt)} · commit ${e.gitCommit || "?"}`)),
    h("div.tiles", { style: { marginTop: "12px", marginBottom: 0 } },
      tile(`${o.wins ?? 0}-${o.losses ?? 0}`, "wins-losses"),
      tile(fmt.pct(o.played ? o.wins / o.played : null), "win rate"),
      tile(fmt.signed(o.played ? o.gemDiff / o.played : null), "avg gem diff")));
}

export function expRow(e) {
  const o = e.summary?.overall || {};
  return h("tr.click", { onclick: () => (location.hash = `#/exp/${e.id}`) },
    h("td", h("b", e.name), h("div.muted", { style: { fontSize: "12px" } }, `${fmt.ago(e.createdAt)} · ${e.maps?.length ?? "?"} maps`)),
    h("td", h("span.pill" + (e.status === "running" || e.status === "building" ? ".run" : ""), e.status)),
    h("td.num", o.played ? `${o.wins}-${o.losses}` : "–"),
    h("td.num", h("span" + (o.gemDiff > 0 ? ".good" : o.gemDiff < 0 ? ".bad" : ""), o.played ? fmt.signed(o.gemDiff / o.played) : "–")));
}

export function mapThumb(info, size = 200) {
  const c = h("canvas");
  drawMap(c, info, { cell: cellSizeFor(info, size, size) });
  return c;
}
