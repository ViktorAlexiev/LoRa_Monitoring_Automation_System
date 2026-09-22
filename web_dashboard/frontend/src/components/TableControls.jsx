import { useRef } from "react";

let seq = 0;

export function SearchBox({ value, onChange, options, placeholder }) {
  const idRef = useRef(null);
  if (idRef.current === null) {
    seq += 1;
    idRef.current = `search-list-${seq}`;
  }
  const listId = idRef.current;

  return (
    <>
      <input
        type="search"
        list={listId}
        value={value}
        onChange={(e) => onChange(e.target.value)}
        placeholder={placeholder || "Търси или избери…"}
        
      />
      <datalist id={listId}>
        {options.map((o) => <option key={o} value={o} />)}
      </datalist>
    </>
  );
}

export function Th({ label, sortKey, sort, onSort }) {
  if (!sortKey) return <th>{label}</th>;
  const active = sort && sort.key === sortKey;
  const arrow = active ? (sort.dir === "asc" ? " ▲" : " ▼") : "";
  return (
    <th onClick={() => onSort(sortKey)} title="Сортирай">
      {label}{arrow}
    </th>
  );
}
