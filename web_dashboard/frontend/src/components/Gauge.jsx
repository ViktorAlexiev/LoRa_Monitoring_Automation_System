export default function Gauge({ label, value, unit, warnBelow, warnAbove }) {
  const hasValue = value !== null && value !== undefined;
  const warn = hasValue && (
    (warnBelow !== undefined && warnBelow !== null && value < warnBelow) ||
    (warnAbove !== undefined && warnAbove !== null && value > warnAbove)
  );
  return (
    <div className={`stat ${warn ? "stat-warn" : ""}`}>
      <span className="stat-value">{hasValue ? `${value.toFixed(1)}${unit}` : "—"}</span>
      <span className="stat-label">{label}</span>
    </div>
  );
}
