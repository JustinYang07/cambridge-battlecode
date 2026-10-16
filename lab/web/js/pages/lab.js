import { api, h, mount, toast } from "../ui.js";
import { expRow } from "./dashboard.js";

const LAST_AB_KEY = "cambc-lab-last-ab";

export async function render() {
  const [status, experiments] = await Promise.all([api("/api/status"), api("/api/experiments")]);
  const bots = status.bots.filter((b) => b.kind !== "tool");
  const ours = bots.filter((b) => b.kind === "bot");
  // Start from the last A/B: what this browser last ran, else the newest experiment (any bots that
  // no longer exist are dropped). Never default to the starter bot.
  const names = new Set(bots.map((b) => b.name));
  let last = null;
  try { last = JSON.parse(localStorage.getItem(LAST_AB_KEY) || "null"); } catch { last = null; }
  if (!last && experiments.length) last = { candidate: experiments[0].candidate, opponents: experiments[0].opponents };
  const lastOpponents = (last?.opponents || []).filter((n) => names.has(n));
  const state = {
    candidate: names.has(last?.candidate) ? last.candidate : ours[0]?.name || bots[0]?.name,
    opponents: new Set(lastOpponents),
    maps: new Set(status.maps),
    bothSides: true,
  };

  const err = h("div.form-err");
  const candidateSel = h("select", { onchange: (e) => (state.candidate = e.target.value) },
    bots.map((b) => h("option", { value: b.name, selected: b.name === state.candidate }, b.name)));
  const oppChips = h("div.chips", bots.map((b) => chip(b.name, state.opponents)));
  const mapChips = h("div.chips", status.maps.map((m) => chip(m, state.maps)));
  const nameInput = h("input", { placeholder: "e.g. bank threshold 60 → 80", style: { width: "100%" } });
  const notesInput = h("textarea", { rows: 2, placeholder: "What changed and what you expect" });
  const runBtn = h("button.primary", { onclick: submit }, "Run experiment");

  async function submit() {
    err.textContent = "";
    runBtn.disabled = true;
    try {
      const exp = await api("/api/experiments", { method: "POST", body: {
        candidate: state.candidate, opponents: [...state.opponents], maps: [...state.maps],
        bothSides: state.bothSides, name: nameInput.value.trim(), notes: notesInput.value.trim() } });
      try { localStorage.setItem(LAST_AB_KEY, JSON.stringify({ candidate: state.candidate, opponents: [...state.opponents] })); } catch { /* storage off: the newest experiment is the fallback */ }
      toast(`Queued ${exp.games.length} games`);
      location.hash = `#/exp/${exp.id}`;
    } catch (e) {
      err.textContent = e.message;
    } finally {
      runBtn.disabled = false;
    }
  }

  // Snapshot tool
  const snapBot = h("select", ours.map((b) => h("option", { value: b.name }, b.name)));
  const snapLabel = h("input", { placeholder: "label, e.g. v3" });
  const snapRef = h("input", { placeholder: "optional git ref, e.g. HEAD~1" });
  const snapErr = h("div.form-err");
  const snapBtn = h("button", { onclick: async () => {
    snapErr.textContent = "";
    snapBtn.disabled = true;
    try {
      const r = await api("/api/bots/snapshot", { method: "POST", body: { bot: snapBot.value, label: snapLabel.value.trim(), gitRef: snapRef.value.trim() } });
      toast(`Created ${r.name}`);
      render();
    } catch (e) { snapErr.textContent = e.message; } finally { snapBtn.disabled = false; }
  } }, "Create snapshot");

  const games = () => state.opponents.size * state.maps.size * (state.bothSides ? 2 : 1);
  const estimate = h("span.hint");
  const refresh = () => { estimate.textContent = `${games()} games · deterministic, so one game per (map, side, opponent)`; };
  refresh();
  document.addEventListener("click", refresh, { once: false });

  mount(h("div",
    h("div.page-head", h("div", h("h1", "A/B Lab"), h("p", "Play a candidate against opponents across maps, from both sides."))),
    h("div.grid.cols-side",
      h("div.grid", { style: { alignContent: "start" } },
        h("div.card",
          h("h2", "New experiment"),
          h("label.field", "Candidate bot", candidateSel),
          h("label.field", "Opponents", oppChips),
          h("label.field", h("span.row", "Maps ", h("a", { href: "#", onclick: (e) => { e.preventDefault(); toggleAll(state.maps, status.maps, mapChips); refresh(); } }, "all / none")), mapChips),
          h("label.field", h("span.row", h("input", { type: "checkbox", checked: true, onchange: (e) => { state.bothSides = e.target.checked; refresh(); } }), " Play both sides (A and B)")),
          h("label.field", "Name", nameInput),
          h("label.field", "Notes", notesInput),
          h("div.row", runBtn, estimate), err),
        h("div.card",
          h("h2", "Snapshot a bot"),
          h("p.hint", "Freezes a bot's source as bots/snap-<bot>-<label> so you can A/B your next edit against it. With a git ref, the snapshot comes from that commit."),
          h("label.field", "Bot", snapBot),
          h("label.field", "Label", snapLabel),
          h("label.field", "Git ref", snapRef),
          snapBtn, snapErr)),
      h("div.card",
        h("div.card-head", h("h2", "Experiments"), h("span.hint", "Games are deterministic: ±2-3 wins between near-identical bots is noise; watch average gem difference too.")),
        experiments.length
          ? h("div.scroll", { style: { maxHeight: "none" } }, h("table",
              h("thead", h("tr", h("th", "experiment"), h("th", "status"), h("th.num", "W-L"), h("th.num", "avg gem diff"))),
              h("tbody", experiments.map(expRow))))
          : h("div.empty", "No experiments yet.")))));
  return () => document.removeEventListener("click", refresh);
}

function chip(name, set) {
  const el = h("span.chip" + (set.has(name) ? ".on" : ""), { onclick: () => {
    if (set.has(name)) set.delete(name); else set.add(name);
    el.classList.toggle("on", set.has(name));
  } }, name);
  return el;
}

function toggleAll(set, all, container) {
  const fill = set.size !== all.length;
  set.clear();
  if (fill) all.forEach((m) => set.add(m));
  container.querySelectorAll(".chip").forEach((c) => c.classList.toggle("on", fill));
}
