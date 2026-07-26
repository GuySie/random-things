# OpenDisplay Blueprints — Design System

This documents the design conventions already in use across the blueprints in
this folder, inferred from the existing files. It's descriptive (this is what
we do) rather than aspirational — treat deviations from it as things to
justify, not as bugs by default.

## Two layout archetypes

**Full-bleed icon layout** — `od-blueprint-1.6-pollen.yaml`,
`od-blueprint-2.9-pollen-horizontal.yaml`, `od-blueprint-2.9-pollen-vertical.yaml`,
`od-blueprint-3.5-weather.yaml`. No chrome. A large condition icon anchored to
a corner, a hero temperature number, secondary readouts near an edge, thin
divider lines separating "now" from "forecast." Descends from an external
author's (chunkysteveo) original Weatherman design.

**Header-bar dashboard layout** — `od-blueprint-3.5-appointment.yaml`,
`od-blueprint-3.5-claude-usage.yaml`, `od-blueprint-3.5-rain-forecast.yaml`,
`od-blueprint-4.2-calendar.yaml`, `od-blueprint-4.2-plant-moisture.yaml`. All
originally authored by Claude/Guy Sie. Every one opens the payload with a
title bar component (see below). `od-blueprint-3.5-weather.yaml` straddles
both: it has the header bar but otherwise behaves like the icon-first family.

New blueprints should default to the header-bar archetype unless there's a
specific reason to go full-bleed (e.g. a single glanceable value meant to be
readable across a room).

## Color tokens

| Token | Meaning | Where used |
|---|---|---|
| `black` | Default content color | Everywhere |
| `white` | Content drawn on the header bar | Header bar icon/title/meta text only |
| `accent` | "This value crossed a configured threshold" | Every `severe_*`/`warning_*` comparison |
| `red` | Hardcoded error/unavailable state | Only in the `default:` fallback branch of the automation's `choose:` |
| `half_black` | Progress-bar track background | `claude-usage` bars only |

Rules:
- `accent` is *earned* — it only ever appears as the result of a threshold
  comparison against a `severe_*_low`/`severe_*_high`/`warning_threshold`
  input. Never hardcode it for decoration.
- `red` is reserved exclusively for the "no data / misconfigured entity"
  fallback message. Don't use it for in-bounds severe values — that's what
  `accent` is for. Keeping them distinct means a red screen always means
  "something is broken," not "something is high."

## Typography

- Every blueprint exposes a `font_file` input (default `ppb.ttf`), and every
  description recommends `GothamRnd-Bold.ttf` for a nicer result. There is
  exactly one font family in play at a time — no mixing.
- Size scale in points:

  | Role | Size range |
  |---|---|
  | Hero number (temp, time range) | 38–84 |
  | Header title | 18–20 (up to 43 for an oversized header) |
  | Body / label | 14–20 |
  | Caption / meta | 12–13 |
  | Chart micro-label | 9 |

- Always pass `"font": "{{ font_file }}"` on text elements, even short glance
  labels (weekday/hour abbreviations). Some existing text elements in
  `2.9-pollen-vertical` omit it and fall back to the device default font —
  this is an inconsistency to fix opportunistically, not a pattern to copy.

## Layout

- **Edge margin**: 14px is the norm on 3.5" canvases. Dense grids (plant
  moisture) tighten this to 5px to fit more columns.
- **Header bar height**: 26px standard; scales up (45–50px) when the header
  carries a larger icon/title (e.g. the 4.2" calendar).
- **Positioning units**: absolute pixel coordinates for all real content.
  Percent-based positioning (`"x": "50%"`) is reserved *only* for the
  canvas-size-agnostic error fallback text — it's the one payload element that
  has to work without knowing the specific layout math for that blueprint.
- **Proportional packing**: when the number of rows/columns is
  user-configurable, compute row height from remaining canvas space divided
  by active item count rather than hardcoding it. Used independently in
  `claude-usage` (bar rows), `calendar` (day blocks), and `plant-moisture`
  (fixed 11-row columns, packed with no gaps). Prefer this over a fixed grid
  when item count varies.

## Components

### Header bar
Full-width filled black rectangle, drawn first in the payload:
```json
{ "type": "rectangle", "x_start": 0, "x_end": <canvas_width>, "y_start": 0, "y_end": 26, "fill": "black", "outline": "black", "width": 0 }
{ "type": "icon", "value": "mdi:...", "x": 2, "y": 1, "size": 24, "color": "white" }
{ "type": "text", "value": "<title>", "font": "{{ font_file }}", "x": 28, "y": 13, "size": 18, "color": "white", "anchor": "lm" }
{ "type": "text", "value": "<meta, e.g. updated_text>", "font": "{{ font_file }}", "x": <canvas_width - 4>, "y": 13, "size": 12, "color": "white", "anchor": "rm" }
```
Icon at 24px leaves the title starting at `x=28` (2px icon inset + 24px icon +
2px gap). Meta text (timestamp, week number, etc.) is right-aligned in the
same bar.

### Threshold coloring
The single most consistent idea in the repo: a value is compared against a
`severe_*`/`warning_*` input, and both its fill/icon color and its text color
switch from `black` to `accent` together when the threshold is crossed. Never
color only the icon or only the text — they move as a pair.

### Fallback / error state
The `default:` branch of the automation's `choose:` always renders exactly
one centered red text block:
```json
{ "type": "text", "value": "<translated message>", "font": "{{ font_file }}", "x": "50%", "y": "50%", "size": 20-24, "color": "red", "anchor": "mm", "max_width": <canvas_width - ~40> }
```

## Blueprint-level (non-visual) conventions

These aren't layout, but they're as much a "system" as the pixels:

- **Input skeleton**: `displays` → `automation_trigger` → a collapsed
  `optional` group (`icon: mdi:cog`) containing `automation_conditions` plus
  every other tunable — `time_format` (12h/24h), `weekday_format` (8
  languages: en/de/nl/fr/es/pl/hr/sv), `font_file`, and `rotate`
  (0/90/180/270, always all four options exposed regardless of native panel
  orientation, with the description clarifying native vs. upright).
- **`language_map`**: an identical-shaped Jinja dict per blueprint keyed by
  the 8 supported language codes, holding weekday/month names and every
  user-facing string. Copy the shape exactly when adding a blueprint —
  don't invent a different i18n structure.
- **Day/night condition-icon maps**: the same `day_conditions_map` /
  `night_conditions_map` MDI lookup tables are copy-pasted verbatim into
  every blueprint that renders weather conditions.
- **Payload-array comma trick**: JSON array elements are joined with a
  `{% set ns_out = namespace(first=true) %}` / `{% if not ns_out.first %},{% endif %}`
  pattern so conditionally-included elements don't produce trailing/leading
  commas. Reuse this rather than a different join strategy.

## Open items

- No shared macro/include file exists yet — `language_map`,
  `day_conditions_map`/`night_conditions_map`, and the header-bar snippet are
  duplicated per-file. If Home Assistant blueprint templating ever supports
  shared includes, that duplication is the first thing to collapse.
