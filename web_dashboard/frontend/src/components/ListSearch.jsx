// Search box above a list of tiles: filters by name / ID as you type.
export default function ListSearch({ value, onChange, placeholder = "Търси по име или ID…" }) {
  return (
    <div className="list-search">
      <input type="search" value={value} onChange={(e) => onChange(e.target.value)} placeholder={placeholder} aria-label={placeholder} />
    </div>
  );
}

// Case-insensitive "does any of these texts contain the query" (works for Cyrillic).
export const matches = (query, ...texts) => {
  const q = query.trim().toLowerCase();
  return !q || texts.some((t) => (t || "").toString().toLowerCase().includes(q));
};
