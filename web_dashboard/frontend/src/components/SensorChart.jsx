const WIDTH = 260;
const PAD = 6;

function fmtTime(d) {
  return d.toLocaleString("bg-BG", { day: "2-digit", month: "2-digit", hour: "2-digit", minute: "2-digit" });
}

export default function SensorChart({ label, unit, points, color = "var(--accent)", height = 70 }) {
  const values = points.map((p) => p.value).filter((v) => v !== null && v !== undefined);

  if (values.length === 0) {
    return (
      <div>
        <div className="gauge-label">{label}</div>
        <div className="muted">Няма данни</div>
      </div>
    );
  }

  const min = Math.min(...values);
  const max = Math.max(...values);
  const range = max - min || 1;

  const xStep = points.length > 1 ? (WIDTH - PAD * 2) / (points.length - 1) : 0;
  const coords = points
    .map((p, i) => {
      if (p.value === null || p.value === undefined) return null;
      const x = PAD + i * xStep;
      const y = height - PAD - ((p.value - min) / range) * (height - PAD * 2);
      return `${x},${y}`;
    });

  // break the path at gaps (buckets with no data) instead of joining across them
  let path = "";
  let drawing = false;
  coords.forEach((c) => {
    if (!c) { drawing = false; return; }
    path += (drawing ? " L" : "M") + c;
    drawing = true;
  });

  const last = values[values.length - 1];
  const lastCoord = [...coords].reverse().find(Boolean);
  const hasTimeAxis = points.some((p) => p.t);
  const firstT = points.find((p) => p.t)?.t;
  const lastT = [...points].reverse().find((p) => p.t)?.t;

  return (
    <div>
      <div>
        <span className="gauge-label">{label}</span>
        <span className="mono">{last.toFixed(1)}{unit}</span>
      </div>
      <svg viewBox={`0 0 ${WIDTH} ${height}`} width="100%" height={height} preserveAspectRatio="none">
        <line x1={PAD} y1={height - PAD} x2={WIDTH - PAD} y2={height - PAD} stroke="var(--border)" strokeWidth="1" />
        {path && <path d={path} fill="none" stroke={color} strokeWidth="1.6" />}
        {lastCoord && (
          <circle cx={lastCoord.split(",")[0]} cy={lastCoord.split(",")[1]} r="2.4" fill={color} />
        )}
      </svg>
      {hasTimeAxis && (
        <div className="chart-time-axis">
          <span>{firstT ? fmtTime(firstT) : ""}</span>
          <span>{lastT ? fmtTime(lastT) : ""}</span>
        </div>
      )}
    </div>
  );
}
