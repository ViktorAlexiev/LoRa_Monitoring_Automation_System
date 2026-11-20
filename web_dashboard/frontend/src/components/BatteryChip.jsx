import Icon from "./Icon.jsx";

// "78% · 3.98 V" with a battery symbol, coloured by level. compact = just the
// percent, for a tile header (the voltage is in the tooltip). has=false -> "няма"
// (nothing at all when compact).
export default function BatteryChip({ has = true, info, compact = false }) {
  if (!has) return compact ? null : <span className="muted">няма</span>;
  if (!info || info.mv == null) {
    return <span className="chip bat-chip bat-nodata" title="Още няма данни за батерията"><Icon name="battery" size={16} />{compact ? "—" : " няма данни"}</span>;
  }
  const volts = `${(info.mv / 1000).toFixed(2)} V`;
  return (
    <span className={`chip bat-chip bat-${info.level}`} title={`Батерия: ${info.percent}% · ${volts}`}>
      <Icon name="battery" size={16} /> {info.percent}%{compact ? "" : ` · ${volts}`}
    </span>
  );
}
