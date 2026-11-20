import Icon from "./Icon.jsx";

// "…T°" is a temperature (thermometer), "…RH" a humidity (drop).
const kindOf = (label) => (/T°/.test(label) ? "temp" : /RH/.test(label) ? "hum" : null);

export default function Gauge({ label, value, unit, warnBelow, warnAbove }) {
  const hasValue = value !== null && value !== undefined;
  const warn = hasValue && (
    (warnBelow !== undefined && warnBelow !== null && value < warnBelow) ||
    (warnAbove !== undefined && warnAbove !== null && value > warnAbove)
  );
  const kind = kindOf(label);
  return (
    <div className={`stat ${warn ? "stat-warn" : ""}`}>
      {kind && <span className={`stat-icon stat-icon-${kind}`}><Icon name={kind === "temp" ? "thermometer" : "droplet"} size={20} /></span>}
      <span className="stat-value">{hasValue ? `${value.toFixed(1)}${unit}` : "—"}</span>
      <span className="stat-label">{label}</span>
    </div>
  );
}
