// Rough landmarks the admin can drop on the site map. w/h are the default
// size in percent of the map.
export const MAP_KINDS = {
  // generic shapes - the admin types what the object is
  rect: { label: "Правоъгълник", w: 20, h: 14, generic: true },
  circle: { label: "Кръг", w: 12, h: 12, generic: true },
  line: { label: "Линия", w: 50, h: 3, generic: true },
  // quick presets with a ready-made name
  building: { label: "Сграда", w: 18, h: 18 },
  greenhouse: { label: "Оранжерия", w: 30, h: 22 },
  field: { label: "Парцел", w: 30, h: 25 },
  gate: { label: "Вход", w: 9, h: 9 },
  portal: { label: "Портал", w: 14, h: 8 },
  // no longer offered as quick presets, but zones that already use them keep drawing correctly
  well: { label: "Кладенец", w: 9, h: 9, hidden: true },
  road: { label: "Път", w: 60, h: 6, hidden: true },
  trees: { label: "Дървета", w: 12, h: 12, hidden: true },
};

// Colours the admin can give an object ("" = the shape's own colour).
export const MAP_COLORS = {
  "": { label: "По подразбиране", swatch: "#e4e4dc" },
  green: { label: "Зелен", swatch: "#bfe3c6" },
  blue: { label: "Син", swatch: "#bcd8f3" },
  yellow: { label: "Жълт", swatch: "#f7e29a" },
  orange: { label: "Оранжев", swatch: "#f7c99a" },
  red: { label: "Червен", swatch: "#f1b3ae" },
  brown: { label: "Кафяв", swatch: "#d9c3a5" },
  purple: { label: "Лилав", swatch: "#d8c7ee" },
  gray: { label: "Сив", swatch: "#cfcfca" },
};

const ROW_BAND = 12; // sensors within this many % of the row's first sensor (vertically) share a row
const MAX_COLS = 4; // longer rows wrap

// Turns the free-form map positions into a compact grid for the list view:
// sensors at roughly the same height form a row (top to bottom), inside a row
// they go left to right in map order. Every sensor gets its own cell, so
// nothing can overlap, however close together they sit on the map.
// Returns { cols, rows, place: sensor_id -> { row, col } } (1-based); sensors
// not on the map get no entry.
export function gridPlacement(sensors, layout) {
  const at = Object.fromEntries(layout.map((l) => [l.sensor_id, l]));
  const on = sensors.filter((s) => at[s.id]).sort((a, b) => at[a.id].y - at[b.id].y || at[a.id].x - at[b.id].x);
  const bands = [];
  on.forEach((s) => {
    const band = bands[bands.length - 1];
    if (band && at[s.id].y - at[band[0].id].y <= ROW_BAND) band.push(s);
    else bands.push([s]);
  });
  const place = {};
  let row = 0;
  let cols = 0;
  bands.forEach((band) => {
    band.sort((a, b) => at[a.id].x - at[b.id].x);
    for (let k = 0; k < band.length; k += MAX_COLS) {
      row += 1;
      band.slice(k, k + MAX_COLS).forEach((s, idx) => {
        place[s.id] = { row, col: idx + 1 };
      });
      cols = Math.max(cols, Math.min(MAX_COLS, band.length - k));
    }
  });
  return { cols, rows: row, place };
}

// Sensors in the order of the list grid (row by row, left to right); sensors
// not on the map go last.
export function sortByMap(sensors, layout) {
  const { place } = gridPlacement(sensors, layout);
  return [...sensors].sort((a, b) => {
    const pa = place[a.id];
    const pb = place[b.id];
    if (!pa && !pb) return a.id.localeCompare(b.id);
    if (!pa) return 1;
    if (!pb) return -1;
    return pa.row !== pb.row ? pa.row - pb.row : pa.col - pb.col;
  });
}

// Text shown on a landmark: what the admin typed; presets fall back to their
// own name, generic shapes stay unlabelled until named.
export function objectLabel(o) {
  const k = MAP_KINDS[o.kind];
  return o.label || (k && !k.generic ? k.label : "");
}
