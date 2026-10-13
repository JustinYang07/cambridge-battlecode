import { api, fmt, h, mount, toast } from "../ui.js";

export async function render() {
  const status = await api("/api/status");
  const settings = status.platform;
  const selected = new Set();
  const ladderBody = h("tbody");
  const meBox = h("div.hint", "Checking sign-in…");
  const scrapeLog = h("pre.json", { style: { maxHeight: "220px" } }, "");
  const probeOut = h("pre.json", "");
  let logTimer = null;

  const cookie = h("textarea", { rows: 3, placeholder: settings.hasCookie ? "(saved; paste a new value to replace)" : "paste the Cookie request header here" });
  const token = h("input", { placeholder: settings.hasToken ? "(saved)" : "optional bearer token", style: { width: "100%" } });
  const limit = h("input", { type: "number", value: 10, min: 1, max: 500, style: { width: "80px" } });
  const scoutBody = h("tbody");
  const probePath = h("input", { value: "/api/v1/teams/me", style: { flex: 1 } });

  async function checkMe() {
    try {
      const me = await api("/api/platform/me");
      meBox.replaceChildren(me.signedIn ? h("span.good", "Signed in") : h("span.warn", "Not signed in"),
        me.registered !== undefined ? ` · registered: ${me.registered}` : "");
    } catch (e) { meBox.textContent = e.message; }
  }

  async function saveSettings() {
    try {
      await api("/api/platform/settings", { method: "POST", body: {
        cookie: cookie.value.trim() || undefined, token: token.value.trim() || undefined } });
      cookie.value = ""; token.value = "";
      toast("Saved platform settings (stored in lab/data/platform.json, git-ignored)");
      checkMe();
    } catch (e) { toast(e.message, true); }
  }

  async function scrape(allTeams) {
    if (!allTeams && !selected.size) { toast("Tick teams in the ladder first, or scrape the latest games from everyone", true); return; }
    await api("/api/platform/scrape", { method: "POST", body: { teamIds: allTeams ? [] : [...selected], limit: +limit.value } });
    toast("Scrape started");
    pollLog();
  }

  async function loadScouting() {
    const teams = await api("/api/platform/scouting").catch(() => []);
    scoutBody.replaceChildren(...(teams.length ? teams.map((t) => h("tr",
      h("td", h("b", t.name)), h("td.num", fmt.int(t.rating)), h("td.num", t.games),
      h("td.num", fmt.pct(t.wins / Math.max(1, t.games))),
      h("td.num", fmt.int(t.gems / Math.max(1, t.games))), h("td.num", fmt.int(t.oppGems / Math.max(1, t.games))),
      h("td.muted", t.picks.slice(0, 6).map(([n, c]) => `${n} ×${c}`).join(", ")),
      h("td.muted", Object.entries(t.maps).sort((a, b) => b[1] - a[1]).slice(0, 3).map(([m, c]) => `${m} ${c}`).join(", "))))
      : [h("tr", h("td", { colspan: 8 }, h("span.muted", "No scraped games yet.")))]));
  }

  async function pollLog() {
    clearTimeout(logTimer);
    const lines = await api("/api/platform/scrape-log").catch(() => []);
    scrapeLog.textContent = lines.join("\n") || "(nothing yet)";
    const finished = lines.length && /done:|error:/.test(lines[lines.length - 1]);
    if (finished) loadScouting(); else if (lines.length) logTimer = setTimeout(pollLog, 2000);
  }

  async function probe() {
    try {
      const r = await api(`/api/platform/probe?path=${encodeURIComponent(probePath.value.trim())}`);
      probeOut.textContent = `HTTP ${r.status}\n` + JSON.stringify(r.data, null, 2);
    } catch (e) { probeOut.textContent = e.message; }
  }

  mount(h("div",
    h("div.page-head", h("div", h("h1", "Platform & scraping"),
      h("p", "Live ladder from the event site, plus downloading other teams' replays into the lab (needs your signed-in session)."))),
    h("div.grid.cols-2",
      h("div.card",
        h("div.card-head", h("h2", "Ladder"), h("span.hint", "Tick teams to scrape their replays")),
        h("div.scroll", { style: { maxHeight: "640px" } }, h("table",
          h("thead", h("tr", h("th"), h("th.num", "#"), h("th", "team"), h("th.num", "rating"), h("th.num", "Δ7d"), h("th.num", "W-L-D"), h("th", "form"))),
          ladderBody))),
      h("div.grid", { style: { alignContent: "start" } },
        h("div.card",
          h("h2", "Connect your account"), meBox,
          h("p.hint", "Sign in at ", h("a", { href: status.eventUrl, target: "_blank" }, status.eventUrl),
            ", open DevTools → Network, click any request to /api/v1/…, and copy the ", h("b", "cookie"),
            " request header. It is stored only on this machine (lab/data/platform.json) and sent only to the event site."),
          h("label.field", "Cookie header", cookie),
          h("label.field", "Bearer token (optional)", token),
          h("button.primary", { onclick: saveSettings }, "Save")),
        h("div.card",
          h("h2", "Scrape replays"),
          h("div.row", "Newest ", limit, " match groups (≈5 games each) ",
            h("button", { onclick: () => scrape(false) }, "Scrape ticked teams"),
            h("button", { onclick: () => scrape(true) }, "Scrape latest from everyone")),
          h("p.hint", "Uses /api/v1/match-groups and /api/v1/matches/<id>/bundle. Replays land in lab/data/scraped/ with their statistics and appear on the Replays page. Platform bundles have no stdout, so other teams' games open in the official visualiser."),
          scrapeLog),
        h("div.card",
          h("h2", "API explorer"),
          h("p.hint", "Authenticated GET against the event API, e.g. /api/v1/match-groups?scope=all or /api/v1/teams/<id>/performance."),
          h("div.row", probePath, h("button", { onclick: probe }, "GET")),
          probeOut))),
    h("div.card", { style: { marginTop: "16px" } },
      h("div.card-head", h("h2", "Scouting (from scraped games)"), h("span.hint", "Drafts, records and gems of every team in your scraped replays")),
      h("div.scroll", h("table",
        h("thead", h("tr", h("th", "team"), h("th.num", "rating"), h("th.num", "games"), h("th.num", "win %"), h("th.num", "gems/game"),
          h("th.num", "conceded/game"), h("th", "favourite picks"), h("th", "maps seen"))),
        scoutBody)))));

  try {
    const ladder = await api("/api/platform/ladder");
    ladderBody.replaceChildren(...ladder.teams.map((t) => {
      const box = h("input", { type: "checkbox", onchange: (e) => (e.target.checked ? selected.add(t.teamId) : selected.delete(t.teamId)) });
      return h("tr",
        h("td", box), h("td.num", t.rank), h("td", h("b", t.name), t.provisional ? h("span.muted", " (prov.)") : "", h("div.hint", t.category)),
        h("td.num", fmt.int(t.rating)), h("td.num", h("span." + (t.delta7d >= 0 ? "good" : "bad"), fmt.signed(t.delta7d))),
        h("td.num", `${t.wins}-${t.losses}-${t.draws}`),
        h("td", t.form.map((f) => h("span.kind", { title: f, style: { background: f === "win" ? "var(--good)" : f === "loss" ? "var(--bad)" : "var(--muted)" } }))));
    }));
  } catch (e) {
    ladderBody.replaceChildren(h("tr", h("td", { colspan: 7 }, `Ladder unavailable: ${e.message}`)));
  }
  checkMe();
  pollLog();
  loadScouting();
  return () => clearTimeout(logTimer);
}
