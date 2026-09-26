import { useEffect, useRef, useState } from "react";

const M = { top: 12, right: 14, bottom: 30, left: 46 };

// "Nice" axis ticks: 4-5 round numbers covering [lo, hi].
function niceTicks(lo, hi, count = 4, minSpan = 0) {
  // Never zoom in further than minSpan: a 0.2° wobble must not look like a
  // dramatic swing. The axis is otherwise fitted to the data (auto-zoom).
  if (hi - lo < minSpan) {
    const mid = (hi + lo) / 2;
    lo = mid - minSpan / 2;
    hi = mid + minSpan / 2;
  }
  if (hi - lo < 1e-9) { lo -= 1; hi += 1; }
  const rough = (hi - lo) / count;
  const pow = Math.pow(10, Math.floor(Math.log10(rough)));
  const f = rough / pow;
  const step = (f <= 1 ? 1 : f <= 2 ? 2 : f <= 5 ? 5 : 10) * pow;
  const start = Math.floor(lo / step) * step;
  const end = Math.ceil(hi / step) * step;
  const ticks = [];
  for (let v = start; v <= end + step / 2; v += step) ticks.push(Math.round(v / step) * step);
  ticks.step = step;
  return ticks;
}

const fmtNum = (v) => (Math.abs(v) >= 100 || Number.isInteger(v) ? v.toFixed(0) : v.toFixed(1));
// axis labels: as many decimals as the tick step needs, so no two labels look the same
const fmtTick = (v, step) => v.toFixed(step >= 1 ? 0 : Math.min(3, Math.ceil(-Math.log10(step))));
const fmtFull = (d) => d.toLocaleString("bg-BG", { day: "2-digit", month: "2-digit", hour: "2-digit", minute: "2-digit" });

// Line chart with numbered axes and a hover / touch read-out: move the mouse
// or drag a finger over the chart to see the exact value and time.
export default function SensorChart({ label, unit, points, color = "var(--accent)", height = 220, bandMin, bandMax, minSpan = 0 }) {
  const wrap = useRef(null);
  const [width, setWidth] = useState(320);
  const [hover, setHover] = useState(null); // index into pts

  useEffect(() => {
    if (!wrap.current) return undefined;
    const ro = new ResizeObserver(([e]) => setWidth(Math.max(240, Math.round(e.contentRect.width))));
    ro.observe(wrap.current);
    return () => ro.disconnect();
  }, []);

  const pts = points
    .filter((p) => p.value !== null && p.value !== undefined && p.t)
    .map((p) => ({ v: p.value, t: p.t instanceof Date ? p.t : new Date(p.t) }));

  if (pts.length === 0) {
    return (
      <div ref={wrap}>
        <div className="gauge-label">{label}</div>
        <div className="muted">Няма данни</div>
      </div>
    );
  }

  const vals = pts.map((p) => p.v);
  const extra = [bandMin, bandMax].filter((x) => x != null);
  const ticks = niceTicks(Math.min(...vals, ...extra), Math.max(...vals, ...extra), 4, minSpan);
  const yLo = ticks[0];
  const yHi = ticks[ticks.length - 1];
  const t0 = pts[0].t.getTime();
  const t1 = pts[pts.length - 1].t.getTime();
  const tSpan = t1 - t0 || 1;

  const iw = width - M.left - M.right;
  const ih = height - M.top - M.bottom;
  const x = (t) => M.left + ((t - t0) / tSpan) * iw;
  const y = (v) => M.top + (1 - (v - yLo) / (yHi - yLo)) * ih;

  const line = pts.map((p, i) => `${i ? "L" : "M"}${x(p.t.getTime()).toFixed(1)},${y(p.v).toFixed(1)}`).join(" ");
  const area = `${line} L${x(t1).toFixed(1)},${y(yLo)} L${x(t0).toFixed(1)},${y(yLo)} Z`;

  const sameDay = new Date(t0).toDateString() === new Date(t1).toDateString();
  const xLabel = (ms) => {
    const d = new Date(ms);
    if (tSpan > 3 * 86400000) return d.toLocaleDateString("bg-BG", { day: "2-digit", month: "2-digit" });
    const hm = d.toLocaleTimeString("bg-BG", { hour: "2-digit", minute: "2-digit" });
    return sameDay ? hm : `${d.toLocaleDateString("bg-BG", { day: "2-digit", month: "2-digit" })} ${hm}`;
  };
  const nx = width < 420 ? 3 : 5;
  const xTicks = Array.from({ length: nx }, (_, i) => t0 + (tSpan * i) / (nx - 1));

  function onMove(e) {
    const rect = e.currentTarget.getBoundingClientRect();
    const px = ((e.clientX - rect.left) / rect.width) * width;
    let best = 0;
    let bestD = Infinity;
    pts.forEach((p, i) => {
      const d = Math.abs(x(p.t.getTime()) - px);
      if (d < bestD) { bestD = d; best = i; }
    });
    setHover(best);
  }

  const last = pts[pts.length - 1];
  const hp = hover != null ? pts[hover] : null;
  const tipW = 132;
  const tipX = hp ? Math.min(Math.max(x(hp.t.getTime()) - tipW / 2, M.left), width - M.right - tipW) : 0;

  return (
    <div ref={wrap} className="chart">
      <div className="chart-head">
        <span className="gauge-label">{label}</span>
        <span className="chart-now" style={{ color }}>{fmtNum(last.v)}{unit}</span>
      </div>
      <svg
        viewBox={`0 0 ${width} ${height}`} width="100%" height={height} className="chart-svg"
        onPointerMove={onMove} onPointerDown={onMove} onPointerLeave={() => setHover(null)}
        role="img" aria-label={`${label}: последна стойност ${fmtNum(last.v)}${unit}`}
      >
        {ticks.map((tv) => (
          <g key={tv}>
            <line x1={M.left} x2={width - M.right} y1={y(tv)} y2={y(tv)} className="chart-grid" />
            <text x={M.left - 8} y={y(tv) + 4} textAnchor="end" className="chart-tick">{fmtTick(tv, ticks.step)}{unit}</text>
          </g>
        ))}
        {bandMin != null && <line x1={M.left} x2={width - M.right} y1={y(bandMin)} y2={y(bandMin)} className="chart-limit" />}
        {bandMax != null && <line x1={M.left} x2={width - M.right} y1={y(bandMax)} y2={y(bandMax)} className="chart-limit" />}
        {xTicks.map((tv, i) => (
          <text key={tv} x={x(tv)} y={height - 8} className="chart-tick"
                textAnchor={i === 0 ? "start" : i === nx - 1 ? "end" : "middle"}>{xLabel(tv)}</text>
        ))}
        <path d={area} fill={color} opacity="0.12" />
        <path d={line} fill="none" stroke={color} strokeWidth="2.5" strokeLinejoin="round" strokeLinecap="round" />
        {!hp && <circle cx={x(last.t.getTime())} cy={y(last.v)} r="4.5" fill={color} stroke="var(--surface)" strokeWidth="2" />}
        {hp && (
          <g pointerEvents="none">
            <line x1={x(hp.t.getTime())} x2={x(hp.t.getTime())} y1={M.top} y2={M.top + ih} className="chart-cross" />
            <circle cx={x(hp.t.getTime())} cy={y(hp.v)} r="5.5" fill={color} stroke="var(--surface)" strokeWidth="2.5" />
            <g transform={`translate(${tipX},${M.top + 2})`}>
              <rect width={tipW} height="44" rx="8" className="chart-tip" />
              <text x="10" y="19" className="chart-tip-val">{fmtNum(hp.v)}{unit}</text>
              <text x="10" y="36" className="chart-tip-time">{fmtFull(hp.t)}</text>
            </g>
          </g>
        )}
      </svg>
      {(bandMin != null || bandMax != null) && <div className="chart-legend">Пунктирът показва зададените граници</div>}
    </div>
  );
}
