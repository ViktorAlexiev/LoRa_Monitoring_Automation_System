import html
import textwrap

CW = 6.15        # приблизителна ширина на символ при 11.5px
LH = 15.5        # височина на ред
PAD_X = 11
TITLE_H = 20

# Видовете блокове. Цветовете живеят в CSS (теми), тук са само имената.
KINDS = ["init", "fn", "fsm", "queue", "isr", "svc", "hw", "net"]


def wrap_line(text, width_px):
    maxc = max(10, int((width_px - 2 * PAD_X) / CW))
    indent = len(text) - len(text.lstrip(" "))
    body = text.strip()
    parts = textwrap.wrap(body, width=max(8, maxc - indent), break_long_words=False, break_on_hyphens=False) or [""]
    return [(" " * indent) + p for p in parts]


class Box:
    def __init__(self, id, x, y, w, title, lines, kind, badge=None):
        self.id, self.x, self.y, self.w = id, x, y, w
        self.title, self.kind, self.badge = title, kind, badge
        self.rows = []
        for l in lines:
            self.rows.extend(wrap_line(l, w))
        tw = max(1, int((w - 2 * PAD_X - (24 if badge else 0)) / (CW * 1.08)))
        self.title_rows = textwrap.wrap(title, width=tw, break_long_words=False) or [title]
        self.h = 12 + len(self.title_rows) * 17 + 4 + len(self.rows) * LH + 12

    def anchor(self, side, t=0.5):
        if side == "l":
            return (self.x, self.y + self.h * t)
        if side == "r":
            return (self.x + self.w, self.y + self.h * t)
        if side == "t":
            return (self.x + self.w * t, self.y)
        return (self.x + self.w * t, self.y + self.h)


class Diagram:
    def __init__(self, title, width):
        self.title = title
        self.W = width
        self.boxes = {}
        self.order = []
        self.lanes = []
        self.edges = []
        self.notes = []
        self.maxy = 0

    def lane(self, x, w, label):
        self.lanes.append([x, w, label, 60, 0])

    def box(self, id, x, y, w, title, lines, kind="fn", badge=None):
        b = Box(id, x, y, w, title, lines, kind, badge)
        self.boxes[id] = b
        self.order.append(id)
        self.maxy = max(self.maxy, b.y + b.h)
        return b

    def stack(self, x, w, y, items, gap=26):
        """items: (id,title,lines,kind[,badge])"""
        for it in items:
            id, title, lines, kind = it[:4]
            badge = it[4] if len(it) > 4 else None
            b = self.box(id, x, y, w, title, lines, kind, badge)
            y = b.y + b.h + gap
        return y

    def P(self, id, side, t=0.5):
        return self.boxes[id].anchor(side, t)

    def edge(self, pts, label=None, dashed=False, color=None, lab_at=0, lab_dx=0, lab_dy=-5, anchor="middle", a=None, b=None):
        self.edges.append(dict(pts=pts, label=label, dashed=dashed, color=color, lab_at=lab_at,
                               lab_dx=lab_dx, lab_dy=lab_dy, anchor=anchor, a=a, b=b))

    def h_elbow(self, a, sa, b, sb, xmid, ta=0.5, tb=0.5, **kw):
        pa, pb = self.P(a, sa, ta), self.P(b, sb, tb)
        self.edge([pa, (xmid, pa[1]), (xmid, pb[1]), pb], a=a, b=b, **kw)

    def v_elbow(self, a, sa, b, sb, ymid, ta=0.5, tb=0.5, **kw):
        pa, pb = self.P(a, sa, ta), self.P(b, sb, tb)
        self.edge([pa, (pa[0], ymid), (pb[0], ymid), pb], a=a, b=b, **kw)

    def straight(self, a, sa, b, sb, ta=0.5, tb=0.5, **kw):
        self.edge([self.P(a, sa, ta), self.P(b, sb, tb)], a=a, b=b, **kw)

    def note(self, x, y, text, size=12, color="#4B5563", weight="400", anchor="start"):
        self.notes.append((x, y, text, size, color, weight, anchor))

    # ------------------------------------------------------------------ SVG
    def svg(self, extra_h=60):
        H = int(self.maxy + extra_h)
        o = []
        o.append('<svg id="%s" xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %d %d" role="group" '
                 'aria-label="%s">' % (
                     html.escape(self.title).replace(" ", "_")[:20], self.W, H, self.W, H, html.escape(self.title)))
        o.append('<defs>'
                 '<marker id="ah-%s" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto-start-reverse">'
                 '<path d="M1 1L9 5L1 9z" fill="context-stroke"/></marker></defs>' % self.title)
        for x, w, label, y0, _ in self.lanes:
            o.append('<rect class="lane" x="%d" y="%d" width="%d" height="%d" rx="10"/>' % (x - 10, y0 - 34, w + 20, H - y0 - 4))
            o.append('<text class="lane-l" x="%d" y="%d">%s</text>' % (x, y0 - 14, html.escape(label)))
        # edges below boxes
        for e in self.edges:
            pts = e["pts"]
            d = "M" + " L".join("%.1f %.1f" % p for p in pts)
            cls = "edge"
            if e["dashed"]:
                cls += " dash"
            if e["color"] == "#B45309":
                cls += " notify"
            elif e["color"] == "#6B7380":
                cls += " soft"
            extra = ""
            if e.get("a"):
                extra += ' data-a="%s"' % e["a"]
            if e.get("b"):
                extra += ' data-b="%s"' % e["b"]
            o.append('<path class="%s" d="%s" marker-end="url(#ah-%s)"%s/>' % (cls, d, self.title, extra))
        for id in self.order:
            b = self.boxes[id]
            o.append('<g class="blk k-%s" data-d="%s" data-b="%s" tabindex="0" role="button" aria-label="%s">' % (
                b.kind, self.title, id, html.escape(b.title)))
            o.append('<rect class="bx" x="%.1f" y="%.1f" width="%d" height="%.1f" rx="7"/>' % (b.x, b.y, b.w, b.h))
            ty = b.y + 12
            for tr in b.title_rows:
                ty += 17
                o.append('<text class="bt" x="%.1f" y="%.1f">%s</text>' % (b.x + PAD_X, ty - 4, html.escape(tr)))
            ty += 4
            for r in b.rows:
                ty += LH
                o.append('<text class="bb" x="%.1f" y="%.1f" xml:space="preserve">%s</text>' % (
                    b.x + PAD_X, ty - 4, html.escape(r)))
            if b.badge:
                cx, cy = b.x + b.w - 17, b.y + 17
                o.append('<circle class="bd" cx="%.1f" cy="%.1f" r="11"/><text class="bdn" x="%.1f" y="%.1f" text-anchor="middle">%s</text>' % (
                    cx, cy, cx, cy + 4.5, html.escape(str(b.badge))))
            o.append('</g>')
        for e in self.edges:
            if not e["label"]:
                continue
            pts = e["pts"]
            i = min(e["lab_at"], len(pts) - 2)
            (x1, y1), (x2, y2) = pts[i], pts[i + 1]
            mx, my = (x1 + x2) / 2 + e["lab_dx"], (y1 + y2) / 2 + e["lab_dy"]
            o.append('<text class="elab" x="%.1f" y="%.1f" text-anchor="%s">%s</text>' % (
                mx, my, e["anchor"], html.escape(e["label"])))
        for x, y, text, size, color, weight, anchor in self.notes:
            o.append('<text class="note" x="%d" y="%d" text-anchor="%s">%s</text>' % (x, y, anchor, html.escape(text)))
        o.append('</svg>')
        return "\n".join(o), H


LEGEND_ITEMS = [
    ("init", "старт и инициализация"),
    ("fn", "функция"),
    ("fsm", "автомат"),
    ("queue", "опашка или mutex"),
    ("isr", "прекъсване или таймер"),
    ("svc", "споделена услуга"),
    ("hw", "хардуер"),
    ("net", "мрежа"),
]
LEGEND = ("<div class='legend' role='list'>" +
          "".join("<span role='listitem'><i class='sw k-%s'></i>%s</span>" % (k, t) for k, t in LEGEND_ITEMS) +
          "<span role='listitem' class='lg-line'><i class='ln'></i>поток от данни</span>"
          "<span role='listitem' class='lg-line'><i class='ln n'></i>известие (<code>xTaskNotifyGive</code>)</span></div>")
