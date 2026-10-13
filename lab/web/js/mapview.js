// Canvas renderer for map terrain, camps, altars and champion overlays.
import { cssVar } from "./ui.js";

const CAMP_COLORS = { wolf: "#94a3b8", elk: "#b45309", gem_cluster: "#22d3ee", gold_cluster: "#facc15", monolith: "#a855f7" };

export function campColor(type) { return CAMP_COLORS[type] || "#94a3b8"; }

// Draws the static map. `info` comes from /api/maps/<name> (or a legend-only stub).
export function drawMap(canvas, info, { cell = 6, legendOnly = false } = {}) {
  const w = info.w, hgt = info.h;
  canvas.width = w * cell;
  canvas.height = hgt * cell;
  const ctx = canvas.getContext("2d");
  ctx.fillStyle = cssVar("--panel-2");
  ctx.fillRect(0, 0, canvas.width, canvas.height);
  if (!legendOnly) {
    for (let y = 0; y < hgt; y++) {
      const walls = info.walls[y], holes = info.holes[y], bushes = info.bushes[y];
      for (let x = 0; x < w; x++) {
        let c = null;
        if (walls[x] === "1") c = "#334155";
        else if (holes[x] === "1") c = "#020617";
        else if (bushes[x] === "1") c = "#166534";
        if (c) { ctx.fillStyle = c; ctx.fillRect(x * cell, y * cell, cell, cell); }
      }
    }
    const spawn = (tiles, color) => {
      ctx.fillStyle = color;
      for (const [x, y] of tiles || []) ctx.fillRect(x * cell, y * cell, cell, cell);
    };
    spawn(info.spawnA, "#1d4ed855");
    spawn(info.spawnB, "#b91c1c55");
    for (const g of info.groups || []) {
      for (const c of g.camps) {
        ctx.fillStyle = campColor(c.type);
        const r = c.type === "monolith" ? cell * 0.62 : cell * 0.42;
        ctx.beginPath();
        ctx.arc(c.x * cell + cell / 2, c.y * cell + cell / 2, Math.max(r, 1.5), 0, Math.PI * 2);
        ctx.fill();
      }
    }
  } else {
    for (const g of info.groupLegend || []) {
      ctx.fillStyle = "#94a3b8";
      ctx.beginPath();
      ctx.arc(g.x * cell + cell / 2, g.y * cell + cell / 2, cell * 0.5, 0, Math.PI * 2);
      ctx.fill();
    }
  }
  for (const [x, y] of info.altars || []) {
    ctx.fillStyle = "#fbbf24";
    ctx.save();
    ctx.translate(x * cell + cell / 2, y * cell + cell / 2);
    ctx.rotate(Math.PI / 4);
    const s = Math.max(cell * 0.75, 3);
    ctx.fillRect(-s / 2, -s / 2, s, s);
    ctx.restore();
  }
  return ctx;
}

export function drawPoint(ctx, cell, x, y, color, radius = 0.6, stroke = null) {
  ctx.beginPath();
  ctx.arc(x * cell + cell / 2, y * cell + cell / 2, Math.max(cell * radius, 2.5), 0, Math.PI * 2);
  ctx.fillStyle = color;
  ctx.fill();
  if (stroke) { ctx.lineWidth = 2; ctx.strokeStyle = stroke; ctx.stroke(); }
}

export function drawTrail(ctx, cell, points, color) {
  if (points.length < 2) return;
  ctx.strokeStyle = color;
  ctx.lineWidth = Math.max(1.5, cell / 4);
  ctx.globalAlpha = 0.55;
  ctx.beginPath();
  points.forEach(([x, y], i) => {
    const px = x * cell + cell / 2, py = y * cell + cell / 2;
    if (i === 0) ctx.moveTo(px, py); else ctx.lineTo(px, py);
  });
  ctx.stroke();
  ctx.globalAlpha = 1;
}

export function drawTarget(ctx, cell, x, y, color) {
  const px = x * cell + cell / 2, py = y * cell + cell / 2, r = Math.max(cell * 1.1, 5);
  ctx.strokeStyle = color;
  ctx.lineWidth = 2;
  ctx.beginPath();
  ctx.arc(px, py, r, 0, Math.PI * 2);
  ctx.moveTo(px - r - 3, py); ctx.lineTo(px - r + 3, py);
  ctx.moveTo(px + r - 3, py); ctx.lineTo(px + r + 3, py);
  ctx.moveTo(px, py - r - 3); ctx.lineTo(px, py - r + 3);
  ctx.moveTo(px, py + r - 3); ctx.lineTo(px, py + r + 3);
  ctx.stroke();
}

export function cellSizeFor(info, maxWidth, maxHeight = 560) {
  return Math.max(2, Math.min(Math.floor(maxWidth / info.w), Math.floor(maxHeight / info.h), 14));
}
