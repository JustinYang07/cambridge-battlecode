import { api, fmt, h, mount } from "../ui.js";

export async function render() {
  const all = await api("/api/replays");
  const state = { source: "all", q: "" };
  const body = h("tbody");
  const count = h("span.hint");

  function draw() {
    const q = state.q.toLowerCase();
    const rows = all.filter((r) => (state.source === "all" || r.source === state.source) &&
      (!q || [r.map, r.botA, r.botB, r.experimentName, r.title, r.path].some((v) => v && String(v).toLowerCase().includes(q))));
    count.textContent = `${rows.length} of ${all.length} replays`;
    body.replaceChildren(...rows.slice(0, 400).map((r) => h("tr.click", { onclick: () => (location.hash = `#/replay?path=${encodeURIComponent(r.path)}`) },
      h("td", h("span.pill", r.source)),
      h("td", r.map || "–"),
      h("td", r.botA ? h("span", h("b", r.botA), h("span.muted", " vs "), h("b", r.botB)) : h("span.mono", r.title || r.path)),
      h("td", r.winner ? `team ${r.winner}` : "–"),
      h("td.num", r.gems ? `${r.gems[0]}–${r.gems[1]}` : "–"),
      h("td.num", r.rounds ?? "–"),
      h("td.muted", r.experimentName || ""),
      h("td.muted", fmt.ago(r.time)))));
  }

  mount(h("div",
    h("div.page-head",
      h("div", h("h1", "Replays"), h("p", "Every game the lab played, scraped platform replays, and anything in replays/. Open one to see why each champion acted as it did.")),
      h("div.row",
        h("select", { onchange: (e) => { state.source = e.target.value; draw(); } },
          ["all", "experiment", "scraped", "local"].map((s) => h("option", { value: s }, s))),
        h("input", { placeholder: "filter by map, bot, experiment…", oninput: (e) => { state.q = e.target.value; draw(); }, style: { width: "260px" } }))),
    h("div.card.tight", h("div.scroll", { style: { maxHeight: "75vh" } }, h("table",
      h("thead", h("tr", h("th", "source"), h("th", "map"), h("th", "teams (A vs B)"), h("th", "winner"), h("th.num", "gems A–B"), h("th.num", "rounds"), h("th", "experiment"), h("th", "when"))),
      body))),
    h("div", { style: { marginTop: "8px" } }, count)));
  draw();
}
