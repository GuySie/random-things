# Seeed reTerminal E1003 Art Display (pure ESPHome)

ESPHome configuration for using a [Seeed reTerminal E1003](https://www.seeedstudio.com/reTerminal-E1003-frame-bundle.html) epaper display as a digital art frame with Home Assistant. Combining the high DPI (1872x1404px resolution) with 16 tints grayscale, this display can convincingly emulate the look of a black and white photography print.

This is the **pure ESPHome** version: it uses ESPHome's official `it8951` display component (added in ESPHome 2026.7.0, [PR #15346](https://github.com/esphome/esphome/pull/15346)) instead of a custom `external_components` driver. See the sibling [`Seeed-reTerminal-E1003/`](../Seeed-reTerminal-E1003/) repository for the original custom-component version, and this repo's [CLAUDE.md](CLAUDE.md) for the differences between the two.

The display wakes from deep sleep on a configurable schedule, loads a random image from a configurable folder in your Home Assistant local www folder, displays it, and immediately returns to deep sleep. By default, recent images are tracked across sleep cycles so the same image is not shown again until roughly half the collection has cycled through — this can be toggled off from Home Assistant to pick images with no restriction. The white buttons on the device step through images sequentially while awake; the green button wakes the display early and loads a random image once wifi connects.

The refresh schedule — how often to wake and during which hours — and the image folder are all configured directly from Home Assistant without reflashing.

### Installation

`image_history.h` in this repo must be placed in the **same directory** as the device's YAML file (`config.yaml` here) before you compile or flash. If you are using the **ESPHome dashboard** (Home Assistant app or standalone), copy it into your ESPHome config directory. Full step-by-step instructions are in the comments at the top of [config.yaml](config.yaml). Unlike the custom-component version, there is no `components/` folder to copy — this configuration only uses stock ESPHome components.

Requires **ESPHome 2026.7.0 or newer** (the first version containing the `it8951` display component).

## Image preparation

Images must be:
- PNG format
- **Portrait** (rotation: 270): 1404×1872 px
- **Landscape** (rotation: 0): 1872×1404 px
- 16-level grayscale (the IT8951 controller renders 4bpp grayscale)

Number images sequentially with no gaps: `1.png`, `2.png`, `3.png`, etc.

## IT8951 component

Uses ESPHome's official [`it8951` display component](https://esphome.io/components/display/it8951/), which ships a built-in `model: seeed-reterminal-e1003` preset matching this board's pins, VCOM, and panel quirks. No custom driver code required. See [CLAUDE.md](CLAUDE.md) for the handful of behavioral differences from the old custom-component version (temperature injection timing, refresh-mode handling) and how they were reconciled.

## Deep sleep optimization

Some great investigative work by [ar0v3r](https://github.com/ar0v3r/reTerminal-E1003-ESPHome/) revealed that the SD-card and touch controllers were still drawing power during deep sleep. Their fixes are implemented here and cut power consumption during deep sleep by half compared to an unoptimized config. Many thanks to u/ar0v3r.

## AI warning

While this is based on an original configuration for the Seeed reTerminal E1002 that was written manually, this version for the E1003 — and its port to ESPHome's official `it8951` component — was made with heavy assistance from Claude Code. If you are against using AI-generated code please do not install this.
