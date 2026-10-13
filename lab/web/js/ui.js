// Shared helpers: API client, DOM builder, formatting, colours.

const KEY_STORE = "cambc-lab-key";

export function accessKey() {
  const m = location.hash.match(/[#&?]key=([^&]+)/);
  if (m) {
    try { localStorage.setItem(KEY_STORE, m[1]); } catch {}
    document.cookie = `lab_key=${m[1]}; path=/; SameSite=Strict`;
    history.replaceState(null, "", location.pathname + "#/");
    return m[1];
  }
  try {
    const k = localStorage.getItem(KEY_STORE);
    if (k) document.cookie = `lab_key=${k}; path=/; SameSite=Strict`;
    return k;
  } catch { return null; }
}

export async function api(path, { method = "GET", body } = {}) {
  const headers = { "Content-Type": "application/json" };
  const key = accessKey();
  if (key) headers["X-Lab-Key"] = key;
  const res = await fetch(path, { method, headers, body: body === undefined ? undefined : JSON.stringify(body) });
  const data = await res.json().catch(() => ({ error: `HTTP ${res.status}` }));
  if (!res.ok) throw new Error(data.error || `HTTP ${res.status}`);
  return data;
}

// h("div.card#id", {onclick}, child, "text", [children])
export function h(spec, attrs, ...children) {
  if (attrs === null || typeof attrs !== "object" || attrs instanceof Node || Array.isArray(attrs)) {
    children.unshift(attrs);
    attrs = {};
  }
  const [, tag = "div", rest = ""] = spec.match(/^([a-z0-9]*)(.*)$/i);
  const el = document.createElement(tag || "div");
  for (const part of rest.match(/[.#][^.#]+/g) || []) {
    if (part[0] === ".") el.classList.add(part.slice(1)); else el.id = part.slice(1);
  }
  for (const [k, v] of Object.entries(attrs)) {
    if (v === undefined || v === null || v === false) continue;
    if (k.startsWith("on")) el.addEventListener(k.slice(2), v);
    else if (k === "style" && typeof v === "object") Object.assign(el.style, v);
    else if (k === "html") el.innerHTML = v;
    else if (v === true) el.setAttribute(k, "");
    else el.setAttribute(k, v);
  }
  for (const c of children.flat(Infinity)) {
    if (c === null || c === undefined || c === false) continue;
    el.append(c instanceof Node ? c : document.createTextNode(String(c)));
  }
  return el;
}

export function mount(el) {
  const view = document.getElementById("view");
  view.replaceChildren(el);
  return view;
}

export function toast(message, isError = false) {
  const t = h("div" + (isError ? ".err" : ""), message);
  document.getElementById("toast").append(t);
  setTimeout(() => t.remove(), isError ? 7000 : 3500);
}

export const KIND_COLORS = {
  farm: "--k-farm", capture: "--k-capture", bank: "--k-bank", heal: "--k-heal", flee: "--k-flee",
  fight: "--k-fight", wait: "--k-wait", dead: "--k-dead", channel: "--k-channel",
};
export function cssVar(name) { return getComputedStyle(document.documentElement).getPropertyValue(name).trim(); }
export function kindColor(kind) { return cssVar(KIND_COLORS[kind] || "--k-wait"); }
export function kindSwatch(kind) { return h("span.kind", { style: { background: kindColor(kind) } }); }

export const fmt = {
  int: (v) => (v === null || v === undefined ? "–" : Math.round(v).toLocaleString()),
  signed: (v) => (v === null || v === undefined ? "–" : (v > 0 ? "+" : "") + Math.round(v).toLocaleString()),
  pct: (v) => (v === null || v === undefined || Number.isNaN(v) ? "–" : `${Math.round(v * 100)}%`),
  num: (v, d = 1) => (v === null || v === undefined ? "–" : Number(v).toFixed(d).replace(/\.0+$/, "")),
  ago(ts) {
    if (!ts) return "–";
    const s = Date.now() / 1000 - ts;
    if (s < 60) return "just now";
    if (s < 3600) return `${Math.floor(s / 60)}m ago`;
    if (s < 86400) return `${Math.floor(s / 3600)}h ago`;
    return new Date(ts * 1000).toLocaleDateString();
  },
};

export function resultPill(game) {
  const r = game.result;
  if (!r) return h("span.pill" + (game.status === "running" ? ".run" : ""), game.status);
  if (r.error) return h("span.pill.loss", "error");
  const win = r.winner === game.candidateSide;
  return h("span.pill." + (win ? "win" : "loss"), win ? "WIN" : "loss");
}

export function gemsFor(game) {
  const r = game.result;
  if (!r || !r.teams) return null;
  const mine = game.candidateSide === "A" ? 0 : 1;
  return [r.teams[mine].gems, r.teams[1 - mine].gems];
}

export function onKey(handler) {
  const fn = (e) => { if (!["INPUT", "TEXTAREA", "SELECT"].includes(e.target.tagName)) handler(e); };
  document.addEventListener("keydown", fn);
  return () => document.removeEventListener("keydown", fn);
}
