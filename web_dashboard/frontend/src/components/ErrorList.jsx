import { useState } from "react";
import { errorTitle } from "../errorLabels.js";

const RANK = { critical: 3, error: 2, warning: 1 };
const fmt = (iso) => new Date(iso).toLocaleString("bg-BG", { day: "2-digit", month: "2-digit", hour: "2-digit", minute: "2-digit" });

function plural(n) {
  return n === 1 ? "1 проблем" : `${n} проблема`;
}

// One compact bar ("2 проблема") that expands into the full list on click,
// so a page with many alarms doesn't push the real content off the screen.
export default function ErrorList({ errors }) {
  const [open, setOpen] = useState(false);
  if (!errors || errors.length === 0) return null;
  const worst = errors.reduce((a, b) => (RANK[b.severity] > RANK[a.severity] ? b : a));
  return (
    <div className="error-list">
      <div className={`error-summary sev-${worst.severity}`}>
        <span>{worst.severity === "warning" ? "⚠" : "✖"}</span>
        <b className="error-summary-text">
          {plural(errors.length)}
          {errors.length === 1 && ` — ${errorTitle(errors[0].error_code)}`}
        </b>
        <button className="btn btn-sm" onClick={() => setOpen((v) => !v)} aria-expanded={open}>
          {open ? "Скрий" : "Покажи"}
        </button>
      </div>
      {open && errors.map((e) => (
        <div className={`error-banner sev-${e.severity}`} key={e.id}>
          <span>{e.severity === "warning" ? "⚠" : "✖"}</span>
          <div>
            <div><b>{errorTitle(e.error_code)}</b></div>
            <div>{e.description}</div>
            <div className="meta">Открита {fmt(e.detected_at)}</div>
          </div>
        </div>
      ))}
    </div>
  );
}
