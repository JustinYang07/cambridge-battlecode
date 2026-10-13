import { api, h, mount } from "../ui.js";
import { campColor, cellSizeFor, drawMap } from "../mapview.js";

export async function render({ name }) {
  const summaries = await api("/api/maps");
  const detail = h("div");
  const cards = summaries.map((s) => {
    const canvas = h("canvas");
    const card = h("div.card.mapcard", { onclick: () => (location.hash = `#/maps?name=${s.name}`) },
      canvas,
      h("div.row", { style: { justifyContent: "space-between", marginTop: "8px" } }, h("b", s.name), h("span.muted", `${s.w}×${s.h} · ${s.teamSize} picks`)),
      h("div.hint", `${s.groups} camp groups · ${s.altars} altars · ${s.gemsPerCycle} gems per full clear`),
      h("div.hint", Object.entries(s.camps).map(([k, v]) => `${v} ${k.replace("_cluster", "")}`).join(" · ")));
    api(`/api/maps/${s.name}`).then((info) => drawMap(canvas, info, { cell: cellSizeFor(info, 220, 220) }));
    return card;
  });
  mount(h("div",
    h("div.page-head", h("div", h("h1", "Maps"), h("p", "The published map pool, extracted with the tool-mapdump helper bot."))),
    detail,
    h("div.mapcards", cards)));
  if (name) showDetail(name, detail);
}

async function showDetail(name, container) {
  const info = await api(`/api/maps/${name}`);
  const canvas = h("canvas");
  const groups = info.groups.map((g, i) => h("tr",
    h("td.num", i), h("td", g.camps.map((c) => h("span", { style: { color: campColor(c.type), marginRight: "6px" } }, c.type.replace("_cluster", "")))),
    h("td.mono", `(${g.camps[0].x},${g.camps[0].y})`), h("td.num", g.gems), h("td.num", g.gold), h("td.num", g.respawn),
    h("td.num", g.camps.reduce((s, c) => s + (c.hp || 0), 0)), h("td.num", g.camps.reduce((s, c) => s + (c.str || 0), 0))));
  container.replaceChildren(h("div.grid.cols-2", { style: { marginBottom: "18px" } },
    h("div.card", h("div.card-head", h("h2", `${name} · ${info.w}×${info.h} · ${info.symmetry}`), h("a", { href: "#/maps" }, "close")), canvas,
      h("div.legend", [["wall", "#334155"], ["hole", "#020617"], ["bush", "#166534"], ["altar", "#fbbf24"], ["wolf", campColor("wolf")],
        ["elk", campColor("elk")], ["gem", campColor("gem_cluster")], ["gold", campColor("gold_cluster")], ["monolith", campColor("monolith")]]
        .map(([l, c]) => h("span", h("span.kind", { style: { background: c } }), l)))),
    h("div.card", h("h2", "Camp groups"), h("div.scroll", h("table",
      h("thead", h("tr", h("th.num", "id"), h("th", "members"), h("th", "at"), h("th.num", "gems"), h("th.num", "gold"), h("th.num", "respawn"), h("th.num", "hp"), h("th.num", "str"))),
      h("tbody", groups))),
      h("p.hint", `Constants: ${Object.entries(info.constants || {}).map(([k, v]) => `${k} ${v}`).join(" · ")}`))));
  drawMap(canvas, info, { cell: cellSizeFor(info, canvas.parentElement.clientWidth - 32, 560) });
}
