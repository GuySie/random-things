# OpenDisplay Blueprints

Home Assistant automation blueprints that render dashboards onto
e-paper tags (mostly Solum Newton Pro / Newton Core electronic shelf labels)
via the [OpenDisplay integration](https://github.com/OpenDisplay/Home_Assistant_Integration).
Each blueprint targets a specific tag size and pushes a `opendisplay.drawcustom`
JSON payload (text, icons, shapes, bar graphs, etc.) built from live Home
Assistant state.

## Prerequisites

Common to every blueprint below unless noted otherwise:

- [OpenDisplay Integration](https://github.com/OpenDisplay/Home_Assistant_Integration)
  (v3.0.0-beta.8 or later) installed and configured, with one or more
  tags added as OpenDisplay devices.
- For a nicer font than the default `ppb.ttf`, add
  [`GothamRnd-Bold.ttf`](https://github.com/chunkysteveo/OpenEPaperLink-HA-Weatherman/raw/refs/heads/main/GothamRnd-Bold.ttf)
  and point the blueprint's `font_file` input at it.
- Each blueprint (except the Medication Reminder, which is driven by its
  button) exposes its own `automation_trigger` input — pick a schedule (e.g. a
  `time_pattern` trigger) appropriate to how often its data source changes.
- Every OpenDisplay blueprint has an **Accent colour** option for threshold
  highlights and error messages. It defaults to the panel's own accent colour
  (red on BWR, yellow on BWY); set it to Black on black-and-white panels, where
  accent renders invisible.

Blueprint-specific prerequisites (a weather integration, a calendar
integration, specific sensors, etc.) are listed under each blueprint below,
with full details in each file's own description.

## 1.6" — 200 x 200

### [Weatherman — Pollen variant](od-blueprint-1.6-pollen.yaml)

Current weather, next-hour and next-day forecast, weather icons, optional
moon phase, and a switchable wind/precipitation readout. Can show pollen
counts (general or a specific type) via the Kleenex pollen integration.
Optionally flashes the tag's LED when you trigger it with the tag's button.

Requires a weather integration; Moon, Sun and Kleenex pollen integrations are
optional.

## 2.9" — 384 x 168 / 168 x 384

### [Medication Reminder](od-blueprint-2.9-medication-reminder.yaml)

Turns a single physical button on the tag into a medication log: press it
right after taking your medication and the display shows the exact time (and
date) of that press. The tag's LED flashes straight away to confirm the press,
before the ~30 second redraw.

Has no trigger picker: instead of a schedule, you pick the button's own
`event.*` entity (e.g. `event.<your_tag>_button_1`).

### [Weatherman — Pollen variant, Horizontal](od-blueprint-2.9-pollen-horizontal.yaml)

Horizontal (384x168) version of the pollen Weatherman layout: current
weather, next-2-hour forecasts, next-day forecast, weather icons, optional
moon phase, switchable wind/precipitation, and pollen counts. Optionally
flashes the tag's LED when you trigger it with the tag's button.

Requires a weather integration; Moon, Sun and Kleenex pollen integrations are
optional.

### [Weatherman — Pollen variant, Vertical](od-blueprint-2.9-pollen-vertical.yaml)

Vertical (168x384) version of the pollen Weatherman layout: current weather,
next-3-hour forecasts, next-day forecast, weather icons, optional moon phase,
switchable wind/precipitation, and pollen counts. Optionally flashes the tag's
LED when you trigger it with the tag's button.

Requires a weather integration; Moon, Sun and Kleenex pollen integrations are
optional.

## 3.5" — 384 x 180

### [Next Appointment](od-blueprint-3.5-appointment.yaml)

Shows your next calendar appointment(s). A meeting in progress is labeled
"Now" alongside the one after it; otherwise the next two upcoming
appointments are shown. All-day events are ignored.

Requires one or more calendar integrations.

### [Claude Usage Tracker](od-blueprint-3.5-claude-usage.yaml)

Claude (Anthropic) API usage as a stack of up to 4 labeled progress bars,
each with a "resets at" caption and an optional marker for how far through
its period you currently are.

Requires the [Claude Usage integration](https://github.com/trickv/hass-claude-usage) (HACS).

### [Entity Dashboard](od-blueprint-3.5-entities.yaml)

Up to 12 arbitrary Home Assistant entities as two columns of 6 rows. Any
entity works (not just sensors with a specific domain); each row shows the
entity's raw state plus its unit of measurement. Rows pack in order with no
gaps if you leave slots empty.

### [Weatherman — Pollen variant](od-blueprint-3.5-pollen.yaml)

3.5" resize of the 2.9" horizontal pollen Weatherman layout: current
weather, next-2-hour forecasts, next-day forecast, moon phase, switchable
wind/precipitation, and pollen counts.

Requires a weather integration; Moon, Sun and Kleenex pollen integrations are
optional.

### [Rain Forecast](od-blueprint-3.5-rain-forecast.yaml)

Buienradar-style rain forecast bar graph — next 2 hours in 5-minute steps
(24 bars). Combines Buienalarm and Buienradar forecasts (takes the higher of
the two per timeframe) so one source dropping out doesn't blank the graph.

Requires the [Neerslag App integration](https://github.com/aex351/home-assistant-neerslag-app)
with at least one of its two sources enabled (Buienradar only covers the
Netherlands and Belgium).

### [Sun & Moon Position](od-blueprint-3.5-sun-moon.yaml)

Today's sunrise, sunset, dawn, solar noon, and dusk times, alongside a
schematic horizon strip showing the sun's (and optionally the moon's)
current position.

Requires the Sun integration. The Moon integration (phase icon) and the
[Lunar Phase integration](https://github.com/ngocjohn/lunar-phase) (HACS, to
plot the moon on the horizon strip) are optional.

### [Big Weather Icons](od-blueprint-3.5-weather.yaml)

Large current weather condition icon and temperature — built for glanceable
visibility at the coat rack before heading out.

Requires a weather integration; the Sun integration is optional.

## 4.2" — 400 x 300

### [Agenda / Calendar (v2)](od-blueprint-4.2-calendar-v2.yaml)

Multi-day calendar agenda, one block per day with that day's events; block
heights scale automatically to fit however many days and events you have.

Requires one or more calendar integrations. Supersedes the original
[`od-blueprint-4.2-calendar.yaml`](od-blueprint-4.2-calendar.yaml), which is
kept in the folder for existing automations that still use it.

### [Plant Moisture Dashboard](od-blueprint-4.2-plant-moisture.yaml)

Soil moisture readings for up to 22 plants, laid out as two columns of 11
rows. Each row's sensor and label are configurable; plants pack in order
with no gaps. A configurable threshold highlights dry plants in the accent
colour.

Requires one or more soil moisture sensors.

### [Weatherman — Full](od-blueprint-4.2-weather.yaml)

Full weather dashboard: current conditions with wind, pressure, humidity,
dew point, pollen and sunrise/sunset, a 4-hour forecast row, and a 5-day
forecast row, with optional moon phase and pollen forecast display.

Requires a weather integration; Moon, Sun and Kleenex pollen integrations are
optional.

## 7.5" — 800 x 480

### [Weather](od-blueprint-7.5-weather.yaml)

Location name and date/time header, a large current-conditions icon and
temperature, a sunrise/sunset/humidity/wind/precipitation/pressure detail
grid, and a 5-day forecast strip. Adapted from davelee98's
[OpenDisplay Weather blueprint](https://github.com/davelee98/ha-epaper-blueprints/blob/main/opendisplay_weather_800x480_bw.yaml),
developed against a Seeed 7.5" black & white panel.

Requires a weather integration that provides a daily forecast, and a location
name to show in the header; the Sun integration is optional.

## Gicisky Blueprints

The one blueprint below is a variant of an OpenDisplay blueprint adapted for the [hass-ble-esl](https://github.com/eigger/hass-ble-esl) integration instead (the successor to the archived hass-gicisky), which drives Gicisky BLE e-paper tags via its own `ble_esl.write` action rather than `opendisplay.drawcustom`. It does not require the OpenDisplay integration or OpenDisplay-brand hardware.

### [Plant Moisture Dashboard — Gicisky 7.5"](gicisky-blueprint-7.5-plant-moisture.yaml)

Same layout and behavior as [the 4.2" OpenDisplay Plant Moisture Dashboard](od-blueprint-4.2-plant-moisture.yaml) above, resized for an 800 x 480 Gicisky tag, targeting `ble_esl.write`. Since Gicisky's renderer has no "accent" palette alias, low-moisture values render directly in red instead.

Requires the hass-ble-esl integration with your tag paired over Bluetooth,
and one or more soil moisture sensors.

## Design System

Layout and styling conventions shared across these blueprints (color tokens,
header bar component, typography scale, threshold coloring, and so on) are
documented in [DESIGN_SYSTEM.md](DESIGN_SYSTEM.md).

## AI warning

While I wrote the original OpenEPaperLink automations these blueprints were
based on manually, they were converted and optimized automatically using
Claude. If you are against using AI-generated code please do not install these.