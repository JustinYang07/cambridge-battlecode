// Hash router + global job-status poller.
import { accessKey, api, h, mount, toast } from "./ui.js";
import * as dashboard from "./pages/dashboard.js";
import * as lab from "./pages/lab.js";
import * as experiment from "./pages/experiment.js";
import * as replaysPage from "./pages/replays.js";
import * as explorer from "./pages/explorer.js";
import * as mapsPage from "./pages/maps.js";
import * as platformPage from "./pages/platform.js";

const routes = [
  [/^\/$/, dashboard, "dashboard"],
  [/^\/lab$/, lab, "lab"],
  [/^\/exp\/([\w-]+)$/, experiment, "lab"],
  [/^\/replays$/, replaysPage, "replays"],
  [/^\/replay$/, explorer, "replays"],
  [/^\/maps$/, mapsPage, "maps"],
  [/^\/platform$/, platformPage, "platform"],
];

let cleanup = null;

async function route() {
  accessKey();
  const raw = location.hash.slice(1) || "/";
  const [path, query = ""] = raw.split("?");
  const params = Object.fromEntries(new URLSearchParams(query));
  if (cleanup) { try { cleanup(); } catch {} cleanup = null; }
  for (const [re, page, nav] of routes) {
    const m = path.match(re);
    if (!m) continue;
    document.querySelectorAll(".sidebar a").forEach((a) => a.classList.toggle("active", a.dataset.route === nav));
    mount(h("div.loading", "Loading…"));
    try {
      cleanup = (await page.render({ ...params, match: m })) || null;
    } catch (err) {
      mount(h("div.card", h("h2", "Something went wrong"), h("pre.json", String(err.stack || err))));
    }
    return;
  }
  mount(h("div.empty", "Page not found. ", h("a", { href: "#/" }, "Back to the dashboard")));
}

async function pollJobs() {
  const el = document.getElementById("jobstatus");
  try {
    const s = await api("/api/jobs");
    el.replaceChildren(
      s.current ? h("div", "Running ", h("a", { href: `#/exp/${s.current}` }, s.current)) : h("div", "No experiment running"),
      s.queued ? h("div", `${s.queued} queued`) : null,
    );
  } catch (err) {
    el.textContent = err.message.includes("access key") ? "Access key needed: open the shared link" : "Server offline";
  }
}

window.addEventListener("hashchange", route);
window.addEventListener("unhandledrejection", (e) => toast(String(e.reason?.message || e.reason), true));
route();
pollJobs();
setInterval(pollJobs, 4000);
