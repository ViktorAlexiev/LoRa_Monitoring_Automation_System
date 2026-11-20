import Icon from "./Icon.jsx";

// Dew point as its own clearly framed band: blue = informational, amber when
// the air is within 2° of it (dew / condensation is about to form). When the
// zone has an external (reference) sensor the dew point comes from ITS air
// reading, and the warning compares against that outside temperature.
export default function DewPoint({ readings }) {
  const dew = readings?.dew_point;
  const external = readings?.dew_source === "external";
  const air = external ? readings?.dew_air_t : readings?.air_t;
  const near = dew != null && air != null && air - dew <= 2;
  return (
    <div className={`dew-point-row ${near ? "dew-near" : ""}`}>
      <span className="stat-icon stat-icon-hum"><Icon name="droplet" size={20} /></span>
      <div style={{ flex: 1 }}>
        <div className="lbl">Точка на оросяване</div>
        <div className="hint">
          {near ? "Въздухът е почти на тази температура — има опасност от роса" : "Под тази температура се образува роса"}
        </div>
        {external && (
          <div className="hint">
            По външния сензор: {readings.dew_air_t?.toFixed(1)}° и {readings.dew_air_h?.toFixed(0)}% влажност
          </div>
        )}
      </div>
      <span className="val">{dew != null ? `${dew.toFixed(1)}°` : "—"}</span>
    </div>
  );
}
