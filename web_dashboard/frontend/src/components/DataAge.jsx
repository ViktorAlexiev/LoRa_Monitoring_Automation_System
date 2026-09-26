// "Last data X minutes ago" - so a stale number can't pass for a fresh one.
export default function DataAge({ minutes }) {
  if (minutes == null) return <div className="data-age data-age-stale">Още няма данни от сензорите</div>;
  const stale = minutes >= 30;
  const text = minutes < 1 ? "току-що" : minutes < 60 ? `преди ${minutes} мин` : `преди ${Math.floor(minutes / 60)} ч`;
  return (
    <div className={`data-age ${stale ? "data-age-stale" : ""}`}>
      Последни данни {text}{stale ? " — стойностите може да са остарели" : ""}
    </div>
  );
}
