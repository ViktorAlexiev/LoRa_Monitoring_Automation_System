// Computes when a valve will next switch on/off according to the zone's
// clock-mode schedules. Uses the browser's local clock (server-time syncing
// is out of scope for now - see description_updated.docx discussion).

function parseTime(hhmm) {
  const [h, m] = hhmm.split(":").map(Number);
  return { h, m };
}

function atTime(date, hhmm) {
  const { h, m } = parseTime(hhmm);
  const d = new Date(date);
  d.setHours(h, m, 0, 0);
  return d;
}

// JS getDay(): 0=Sun..6=Sat. Our days_mask bit i (0=Mon..6=Sun).
function jsDayToMaskBit(jsDay) {
  return (jsDay + 6) % 7;
}

export function nextTransition(schedules, valveId, now = new Date()) {
  const relevant = schedules.filter((s) => s.enabled && s.valve_ids.includes(valveId));
  if (relevant.length === 0) return null;

  let best = null;
  for (const s of relevant) {
    for (let offset = 0; offset < 8; offset++) {
      const day = new Date(now);
      day.setDate(day.getDate() + offset);
      const bit = jsDayToMaskBit(day.getDay());
      if (!(s.days_mask & (1 << bit))) continue;

      const start = atTime(day, s.start_time);
      const end = atTime(day, s.end_time);

      if (start > now && (!best || start < best.at)) best = { at: start, type: "on" };
      if (end > now && (!best || end < best.at)) best = { at: end, type: "off" };
    }
  }
  return best;
}

export function fmtTransition(t) {
  if (!t) return null;
  const time = t.at.toLocaleTimeString("bg-BG", { hour: "2-digit", minute: "2-digit" });
  const dayLabel = t.at.toDateString() === new Date().toDateString() ? "днес" : "утре";
  return `${t.type === "on" ? "включва се" : "изключва се"} в ${time} (${dayLabel})`;
}
