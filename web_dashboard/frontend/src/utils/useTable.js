import { useMemo, useState } from "react";

/**
 * Client-side search + sort for a small admin table.
 * searchFields: which row keys the free-text search matches against.
 */
export function useTable(rows, { searchFields = ["id", "name"], defaultSort = null } = {}) {
  const [search, setSearch] = useState("");
  const [sort, setSort] = useState(defaultSort); // { key, dir: "asc"|"desc" }

  const searchOptions = useMemo(() => {
    const set = new Set();
    rows.forEach((row) => {
      searchFields.forEach((f) => {
        if (row[f]) set.add(String(row[f]));
      });
    });
    return Array.from(set).sort();
  }, [rows, searchFields]);

  const visible = useMemo(() => {
    let r = rows;
    if (search.trim()) {
      const q = search.trim().toLowerCase();
      r = r.filter((row) => searchFields.some((f) => String(row[f] ?? "").toLowerCase().includes(q)));
    }
    if (sort) {
      r = [...r].sort((a, b) => {
        const av = a[sort.key] ?? "";
        const bv = b[sort.key] ?? "";
        let cmp;
        if (typeof av === "number" && typeof bv === "number") cmp = av - bv;
        else cmp = String(av).localeCompare(String(bv), "bg");
        return sort.dir === "desc" ? -cmp : cmp;
      });
    }
    return r;
  }, [rows, search, sort, searchFields]);

  function toggleSort(key) {
    setSort((s) => (s && s.key === key ? (s.dir === "asc" ? { key, dir: "desc" } : null) : { key, dir: "asc" }));
  }

  return { search, setSearch, searchOptions, sort, toggleSort, rows: visible };
}
