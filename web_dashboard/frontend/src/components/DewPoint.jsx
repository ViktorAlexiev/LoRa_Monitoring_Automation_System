// Dew point as its own clearly framed band: blue = informational, amber when
// the air is within 2° of it (dew / condensation is about to form).
export default function DewPoint({ readings }) {
  const dew = readings?.dew_point;
  const air = readings?.air_t;
  const near = dew != null && air != null && air - dew <= 2;
  return (
    <div className={`dew-point-row ${near ? "dew-near" : ""}`}>
      <div>
        <div className="lbl">Точка на оросяване</div>
        <div className="hint">
          {near ? "Въздухът е почти на тази температура — има опасност от роса" : "Под тази температура се образува роса"}
        </div>
      </div>
      <span className="val">{dew != null ? `${dew.toFixed(1)}°` : "—"}</span>
    </div>
  );
}
