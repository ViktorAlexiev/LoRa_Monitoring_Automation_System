---
name: Firmware Viewer (Прошивка)
description: A study instrument for 5,000 lines of ESP-IDF code, drawn as a logic-analyzer capture with a dark scope theme and a light graph-paper print theme.
colors:
  scope-black: "#080C11"
  panel: "#0D131A"
  raise: "#17212C"
  ink: "#E6EEF4"
  ink-muted: "#AEBCC8"
  ink-faint: "#7F8E9B"
  cursor-yellow: "#F5D547"
  channel-blue: "#5AA9FF"
  channel-violet: "#B28CFF"
  channel-green: "#52D6A0"
  channel-orange: "#FF9F43"
  channel-red: "#FF6B5E"
  channel-brown: "#B98A62"
  channel-grey: "#9AA7B3"
  paper: "#EEF2F0"
  paper-panel: "#FAFCFB"
  paper-ink: "#0E1A22"
typography:
  display:
    fontFamily: "JetBrains Mono, ui-monospace, Consolas, monospace"
    fontSize: "22px"
    fontWeight: 700
    lineHeight: 1.2
    letterSpacing: "-0.02em"
  headline:
    fontFamily: "Geologica, Segoe UI, system-ui, sans-serif"
    fontSize: "24px"
    fontWeight: 700
    lineHeight: 1.2
    letterSpacing: "-0.02em"
  body:
    fontFamily: "Geologica, Segoe UI, system-ui, sans-serif"
    fontSize: "14.5px"
    fontWeight: 400
    lineHeight: 1.5
  code:
    fontFamily: "JetBrains Mono, ui-monospace, Consolas, monospace"
    fontSize: "13px"
    fontWeight: 400
    lineHeight: 1.55
  label:
    fontFamily: "Geologica, Segoe UI, system-ui, sans-serif"
    fontSize: "12px"
    fontWeight: 500
rounded:
  sm: "5px"
  md: "9px"
  lg: "14px"
  pill: "999px"
spacing:
  sm: "8px"
  md: "12px"
  lg: "18px"
components:
  tab:
    textColor: "{colors.ink-muted}"
    rounded: "{rounded.md}"
    padding: "7px 14px"
  tab-active:
    backgroundColor: "{colors.raise}"
    textColor: "{colors.ink}"
  chip:
    textColor: "{colors.ink-muted}"
    rounded: "{rounded.pill}"
    padding: "4px 10px"
  popup:
    backgroundColor: "{colors.raise}"
    textColor: "{colors.ink-muted}"
    rounded: "{rounded.lg}"
    padding: "14px 16px"
---

# Design System: Firmware Viewer (Прошивка)

## Overview

**Creative North Star: "The Capture"**

The viewer is a recording from a logic analyzer. Every function is a decoded frame on a timeline, the line you stand on is a cursor, and the arrows on the block diagrams are signals that travel when you click. The page is a working instrument for a student who reads for hours, so the world is expressed in structure and in color roles, never in decoration.

Two themes share one grid of graduated hairlines. The dark theme is the scope screen: near-black with a blue-green cast. The light theme is a printed capture on graph paper. Color is never a mood; the eight channel colors follow the resistor code (brown, red, orange, yellow, green, blue, violet, grey) and each is bound to a role.

**Key Characteristics:**
- Timeline of decoded frames above the code, with a window that tracks the visible lines and a cursor on the active line.
- Channel colors as semantics: function blue, type violet, macro orange, global green, constant yellow, external brown.
- Instrument readouts in monospace with tabular numerals; prose in a humanist sans with Bulgarian Cyrillic support.
- Motion explains: a pulse along the arrows, a capture flash where you land, a frame ping on the timeline. Nothing loops.

## Colors

One neutral ramp per theme and eight channels. Only the cursor yellow acts as the interface accent.

### Primary
- **Cursor Yellow** (#F5D547 dark, #B07A00 light): the active line, focus rings, the notify edges and every landing flash. It is the only color that points at where you are.

### Secondary
- **Channel Blue** (#5AA9FF): functions and the first tone of timeline frames.
- **Channel Violet** (#B28CFF): types, structs, principle links, the alternate frame tone.
- **Channel Green** (#52D6A0): global variables and the logo mark.
- **Channel Orange** (#FF9F43): macros and the network block kind.
- **Channel Red** (#FF6B5E): strings in code, interrupt and timer blocks.
- **Channel Brown** (#B98A62): external names, hardware blocks, comment density on the timeline.

### Neutral
- **Scope Black** (#080C11): page ground. **Panel** (#0D131A): header, rail, popups' neighbors. **Raise** (#17212C): the active tab, popups.
- **Ink** (#E6EEF4), **Ink Muted** (#AEBCC8), **Ink Faint** (#7F8E9B): primary, body-secondary and instrument labels.
- Light theme: **Paper** (#EEF2F0), **Paper Panel** (#FAFCFB), **Paper Ink** (#0E1A22); channel hues darken to hold 4.5:1 on paper.

### Named Rules
**The Resistor Code Rule.** Channel colors map to roles and never to decoration. A new role takes the next free channel, not a new hue.
**The One Pointer Rule.** Cursor yellow marks position only. It never decorates a surface or a label.

## Typography

**Display Font:** JetBrains Mono (with ui-monospace, Consolas)
**Body Font:** Geologica (with Segoe UI, system-ui)
**Label/Mono Font:** JetBrains Mono

**Character:** A mono voice for everything that is measured or executed, a calm humanist sans for explanation. Both carry Cyrillic. Localized Cyrillic forms are switched off globally because Geologica draws Bulgarian д as a latin-looking g at small sizes.

### Hierarchy
- **Display** (700, 22px, 1.2): device names above each diagram, in mono.
- **Headline** (700, 24px, 1.2): section titles in the glossary.
- **Body** (400, 14.5px, 1.5): explanations, kept to 65 to 78 characters.
- **Code** (400, 13px, 1.55): the source column, wrapped, tab size 4.
- **Label** (500, 12px): chips, tab captions, readouts.

### Named Rules
**The Tabular Rule.** Every number that can change on screen uses tabular numerals.

## Layout

A fixed header of 54px, then one full-height page. The code page is a 288px rail, a file header, the timeline and the table. The table columns are line number (64px), code (48%) and explanation. The diagrams page scrolls vertically with a sticky device bar; each diagram sits in a scrollable canvas with a graticule of 100px and 20px cells.

At 900px and below the tabs move to a bottom bar, the rail becomes a drawer, rows become two stacked lines (code, then note), and popups become bottom sheets.

## Elevation & Depth

Tonal layering, not shadow. Surfaces separate by value (ground, panel, raise) and a 1px hairline. The only shadow belongs to popups (`0 6px 14px` soft and `0 1px 3px` contact) because they overlap content.

### Named Rules
**The Hairline-or-Shadow Rule.** A surface is defined either by its hairline or by a lift. Popups carry the hairline and a tight shadow, never a wide diffuse glow.

## Shapes

Instrument rectangles with modest radius: 5px for inline code and frames, 9px for tabs and inputs, 14px for the diagram canvas and popups. Chips are pills. Frames, windows and cursors are crisp rectangles and lines; there are no circles except the channel dots.

## Components

### Tabs
Segmented control with a sliding indicator (translate and width, 260ms ease-out). Icons are authored SVG at 1.7 stroke.

### Timeline
Frames proportional to function length, alternating blue and violet tints; the current function takes the cursor tint. Below them a density strip (code ink, comment brown). A window outlines the visible lines; a cursor marks A and, with shift-click, B with a range band.

### Code table
Gutter, code and note. The active row tints with cursor yellow at 13%. Identifiers are underlined dotted on hover and open a popup.

### Popup
Raise surface, 14px radius, kind dot in the role color, mono title, explanation, source preview and actions. Opens in 160ms from the click point; becomes a bottom sheet on phones.

### Diagram blocks
Eight kinds, one channel each. Stroke is the channel color, fill is that color mixed 12% into panel. Click sends a pulse along connected edges and opens the code.

## Do's and Don'ts

### Do:
- **Do** bind every new color to a role in the channel table.
- **Do** keep motion under 300ms, ease-out, on transform and opacity; the one exception is the stroke pulse on diagram edges.
- **Do** honor `prefers-reduced-motion` by dropping movement and keeping opacity changes.
- **Do** keep numerals tabular and code on 13px mono.

### Don't:
- **Don't** add a decorative grid behind code; the graticule belongs to the diagram canvas and the timeline scale.
- **Don't** use cursor yellow as a fill color for decoration.
- **Don't** put a kicker above any heading.
- **Don't** replace the SVG icon set with text glyphs.
