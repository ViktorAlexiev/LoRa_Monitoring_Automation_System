import { useCallback, useEffect, useLayoutEffect, useRef, useState } from "react";

const MAX_ZOOM = 8;       // how far in you can go, relative to the "whole map" view
const DRAG_START_PX = 5;  // below this a press is still a click, not a pan

// Bounding box (in map percent) of boxes {x, y, w, h} (x/y = centre), plus a
// margin. The map ends where its elements end, so panning never wanders off
// into empty space. `minSpan` keeps a single small element from zooming in
// absurdly; `include` forces a region to stay inside (the editor keeps the
// base canvas so there is always free room to drag things into).
export function mapBounds(boxes, { margin = 4, minSpan = 30, include = null } = {}) {
  let x0 = Infinity, y0 = Infinity, x1 = -Infinity, y1 = -Infinity;
  const add = (ax0, ay0, ax1, ay1) => {
    x0 = Math.min(x0, ax0); y0 = Math.min(y0, ay0); x1 = Math.max(x1, ax1); y1 = Math.max(y1, ay1);
  };
  boxes.forEach((b) => add(b.x - (b.w || 0) / 2, b.y - (b.h || 0) / 2, b.x + (b.w || 0) / 2, b.y + (b.h || 0) / 2));
  if (include) add(include.x0, include.y0, include.x1, include.y1);
  if (!Number.isFinite(x0)) return { x0: 0, y0: 0, x1: 100, y1: 100 };
  x0 -= margin; y0 -= margin; x1 += margin; y1 += margin;
  if (x1 - x0 < minSpan) { const c = (x0 + x1) / 2; x0 = c - minSpan / 2; x1 = c + minSpan / 2; }
  if (y1 - y0 < minSpan) { const c = (y0 + y1) / 2; y0 = c - minSpan / 2; y1 = c + minSpan / 2; }
  return { x0, y0, x1, y1 };
}

function limitsOf(b, size) {
  const bx0 = (b.x0 / 100) * size.w;
  const bx1 = (b.x1 / 100) * size.w;
  const by0 = (b.y0 / 100) * size.h;
  const by1 = (b.y1 / 100) * size.h;
  const sFit = Math.min(size.w / (bx1 - bx0), size.h / (by1 - by0));
  return { bx0, bx1, by0, by1, sFit, sMax: sFit * MAX_ZOOM };
}

// Keep the view inside the map's bounds: zoomed in, the bounds always cover
// the whole window; zoomed out (smaller than the window), they sit centred.
function clampView(v, L, size) {
  const s = Math.min(L.sMax, Math.max(L.sFit, v.s));
  const axis = (t, lo, hi, win) => (((hi - lo) * s >= win - 0.5)
    ? Math.min(-lo * s, Math.max(win - hi * s, t))
    : (win - (lo + hi) * s) / 2);
  return { s, tx: axis(v.tx, L.bx0, L.bx1, size.w), ty: axis(v.ty, L.by0, L.by1, size.h) };
}

const fitView = (L, size) => clampView({ s: L.sFit, tx: 0, ty: 0 }, L, size);

// A map window you can zoom (buttons, Ctrl + wheel, pinch) and drag. Children
// live in a "stage" whose percent coordinates are the same as before (0..100 =
// the base canvas, anything can sit outside it); `bounds` (percent) says where
// the map ends. children may be a function receiving { scale, width, height }:
// the stage's current zoom and its unzoomed size in px.
//
// Mark draggable things inside (editor handles) with data-nodrag so pressing
// them does not pan the map. Extra props go to the window element.
export default function PanZoom({ bounds, freezeBounds = false, stageRef, children, className = "", ...rest }) {
  const winRef = useRef(null);
  const ownStage = useRef(null);
  const stage = stageRef || ownStage;
  const [size, setSize] = useState({ w: 0, h: 0 });
  const [view, setView] = useState({ s: 1, tx: 0, ty: 0 });
  const frozen = useRef(bounds);
  if (!freezeBounds) frozen.current = bounds;
  const b = frozen.current;
  const touched = useRef(false); // the person moved the map: stop auto-fitting when data arrives
  const viewRef = useRef(view);
  const limRef = useRef(null);
  const sizeRef = useRef(size);

  const L = size.w > 0 ? limitsOf(b, size) : null;
  limRef.current = L;
  sizeRef.current = size;

  useLayoutEffect(() => {
    const el = winRef.current;
    if (!el) return undefined;
    const measure = () => {
      const w = el.clientWidth;
      const h = el.clientHeight;
      if (w > 0) setSize((o) => (o.w === w && o.h === h ? o : { w, h }));
    };
    measure();
    const ro = new ResizeObserver(measure);
    ro.observe(el);
    return () => ro.disconnect();
  }, []);

  // New size or new bounds: fit the whole map until the person starts moving it,
  // after that only keep the view legal.
  useLayoutEffect(() => {
    if (!L) return;
    setView((v) => (touched.current ? clampView(v, L, size) : fitView(L, size)));
  }, [size.w, size.h, b.x0, b.y0, b.x1, b.y1]); // eslint-disable-line react-hooks/exhaustive-deps

  useEffect(() => { viewRef.current = view; }, [view]);

  const apply = useCallback((fn) => {
    const L2 = limRef.current;
    if (!L2) return;
    touched.current = true;
    setView((v) => clampView(fn(v), L2, sizeRef.current));
  }, []);

  const zoomAt = useCallback((factor, cx, cy) => {
    apply((v) => {
      const s = v.s * factor;
      return { s, tx: cx - (cx - v.tx) * (s / v.s), ty: cy - (cy - v.ty) * (s / v.s) };
    });
  }, [apply]);

  const zoomCentre = (factor) => zoomAt(factor, sizeRef.current.w / 2, sizeRef.current.h / 2);
  const reset = () => { touched.current = false; if (limRef.current) setView(fitView(limRef.current, sizeRef.current)); };

  // pointer pan + pinch + Ctrl-wheel, as native listeners so they can be non-passive
  useEffect(() => {
    const el = winRef.current;
    if (!el) return undefined;
    const pts = new Map(); // pointerId -> {x, y}
    let pan = null;        // { id, x, y, moved }
    let pinch = null;      // { d, mx, my, v }
    let swallowClick = false;
    const local = (e) => { const r = el.getBoundingClientRect(); return { x: e.clientX - r.left, y: e.clientY - r.top }; };

    const down = (e) => {
      if (e.target.closest("[data-nodrag]")) return;
      if (e.pointerType === "mouse" && e.button !== 0) return;
      pts.set(e.pointerId, local(e));
      if (pts.size === 1) pan = { id: e.pointerId, ...local(e), moved: false };
      if (pts.size === 2) {
        const [p, q] = [...pts.values()];
        pinch = { d: Math.hypot(p.x - q.x, p.y - q.y) || 1, mx: (p.x + q.x) / 2, my: (p.y + q.y) / 2, v: viewRef.current };
        pan = null;
      }
    };
    const move = (e) => {
      if (!pts.has(e.pointerId)) return;
      const cur = local(e);
      pts.set(e.pointerId, cur);
      if (pinch && pts.size >= 2) {
        const [p, q] = [...pts.values()];
        const d = Math.hypot(p.x - q.x, p.y - q.y) || 1;
        const mx = (p.x + q.x) / 2;
        const my = (p.y + q.y) / 2;
        swallowClick = true;
        apply(() => {
          const s = pinch.v.s * (d / pinch.d);
          return { s, tx: mx - (pinch.mx - pinch.v.tx) * (s / pinch.v.s), ty: my - (pinch.my - pinch.v.ty) * (s / pinch.v.s) };
        });
        return;
      }
      if (!pan || pan.id !== e.pointerId) return;
      const dx = cur.x - pan.x;
      const dy = cur.y - pan.y;
      if (!pan.moved) {
        if (Math.hypot(dx, dy) < DRAG_START_PX) return;
        pan.moved = true;
        swallowClick = true;
        try { el.setPointerCapture(e.pointerId); } catch (err) { /* not capturable: fine */ }
        el.classList.add("panzoom-dragging");
      }
      pan.x = cur.x;
      pan.y = cur.y;
      apply((v) => ({ s: v.s, tx: v.tx + dx, ty: v.ty + dy }));
    };
    const up = (e) => {
      pts.delete(e.pointerId);
      if (pts.size < 2) pinch = null;
      if (pan && pan.id === e.pointerId) pan = null;
      if (pts.size === 0) {
        el.classList.remove("panzoom-dragging");
        setTimeout(() => { swallowClick = false; }, 0);
      }
    };
    const click = (e) => {
      if (swallowClick) { e.preventDefault(); e.stopPropagation(); swallowClick = false; }
    };
    const wheel = (e) => {
      if (!e.ctrlKey && !e.metaKey) return; // plain wheel keeps scrolling the page
      e.preventDefault();
      const { x, y } = local(e);
      zoomAt(Math.exp(-e.deltaY * 0.0025), x, y);
    };
    const noDrag = (e) => e.preventDefault(); // a link inside must not start the browser's own drag

    el.addEventListener("pointerdown", down);
    el.addEventListener("pointermove", move);
    el.addEventListener("pointerup", up);
    el.addEventListener("pointercancel", up);
    el.addEventListener("click", click, true);
    el.addEventListener("wheel", wheel, { passive: false });
    el.addEventListener("dragstart", noDrag);
    return () => {
      el.removeEventListener("pointerdown", down);
      el.removeEventListener("pointermove", move);
      el.removeEventListener("pointerup", up);
      el.removeEventListener("pointercancel", up);
      el.removeEventListener("click", click, true);
      el.removeEventListener("wheel", wheel);
      el.removeEventListener("dragstart", noDrag);
    };
  }, [apply, zoomAt]);

  const zoomed = L ? view.s > L.sFit * 1.02 : false;
  const canIn = L ? view.s < L.sMax * 0.999 : false;
  const canOut = zoomed;

  return (
    <div className="panzoom-wrap">
      <div
        {...rest}
        ref={winRef}
        // zoomed out a finger should still scroll the page; zoomed in it moves the map
        className={`site-map panzoom ${zoomed ? "panzoom-zoomed" : ""} ${className}`}
      >
        <div
          ref={stage}
          className="panzoom-stage"
          style={{
            width: size.w || "100%", height: size.h || "100%",
            transform: `translate(${view.tx}px, ${view.ty}px) scale(${view.s})`,
            "--inv": 1 / view.s,
            "--gx": `${size.w / 10}px`, "--gy": `${size.h / 10}px`,
          }}
        >
          <div
            className="panzoom-ground"
            style={{
              left: `${b.x0}%`, top: `${b.y0}%`, width: `${b.x1 - b.x0}%`, height: `${b.y1 - b.y0}%`,
              backgroundPosition: `${(-b.x0 / 100) * size.w}px ${(-b.y0 / 100) * size.h}px`,
            }}
          />
          {typeof children === "function" ? children({ scale: view.s, width: size.w, height: size.h }) : children}
        </div>
        <div className="panzoom-buttons" data-nodrag>
          <button type="button" className="panzoom-btn" aria-label="Приближи" disabled={!canIn} onClick={() => zoomCentre(1.5)}>+</button>
          <button type="button" className="panzoom-btn" aria-label="Отдалечи" disabled={!canOut} onClick={() => zoomCentre(1 / 1.5)}>−</button>
          <button type="button" className="panzoom-btn" aria-label="Цялата карта" disabled={!canOut} onClick={reset} title="Цялата карта">⤢</button>
        </div>
      </div>
      <div className="panzoom-hint">Приближаване: бутоните +/−, Ctrl + колелце или два пръста. Движение: влачи картата.</div>
    </div>
  );
}
