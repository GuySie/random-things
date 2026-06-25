# Seeed reTerminal E1003 Art Display

ESPHome configuration for using a [Seeed reTerminal E1003](https://www.seeedstudio.com/reTerminal-E1003-frame-bundle.html) epaper display as a digital art frame with Home Assistant. Combining the high DPI (1872x1404px resolution) with 16 tints grayscale, this display can convincingly emulate the look of a black and white photography print.

The display wakes from deep sleep on a configurable schedule, loads a random image from a configurable folder in your Home Assistant local www folder, displays it, and immediately returns to deep sleep. By default, recent images are tracked across sleep cycles so the same image is not shown again until roughly half the collection has cycled through — this can be toggled off from Home Assistant to pick images with no restriction. The white buttons on the device step through images sequentially while awake; the green button wakes the display early and loads a random image once wifi connects.

The refresh schedule — how often to wake and during which hours — and the image folder are all configured directly from Home Assistant without reflashing.

### Installation

The `components` folder and `image_history.h` in this repo must be placed in the **same directory** as the device's YAML file (`config.yaml` here) before you compile or flash. ESPHome resolves the `local` path relative to the config file. If you are using the **ESPHome dashboard** (Home Assistant app or standalone), copy them into your ESPHome config directory. Full step-by-step instructions are in the comments at the top of [config.yaml](config.yaml). 

## Image preparation

Images must be:
- PNG format
- **Portrait** (rotation: 270): 1404×1872 px
- **Landscape** (rotation: 0): 1872×1404 px
- 16-level grayscale (the IT8951 controller renders 4bpp grayscale)

Number images sequentially with no gaps: `1.png`, `2.png`, `3.png`, etc.

## IT8951 component

ESPHome currently does not provide support for the IT8951 controller used in the E1003, so we provide a local component. This component is a modification of the [code by koosoli](https://github.com/esphome/esphome/pull/15415), modified using Claude Code to work specifically for this art frame purpose. No guarantees it will work for anything else.

## Deep sleep optimization

Some great investigative work by [ar0v3r](https://github.com/ar0v3r/reTerminal-E1003-ESPHome/) revealed that the SD-card and touch controllers were still drawing power during deep sleep. I've implemented their fixes in v0.8.0 of this configuration and have cut power consumption during deep sleep by half! Many thanks to u/ar0v3r.

## Live and partial refresh (interactive mode)

The original art-frame flow does one full-screen GC16 refresh per wake and then sleeps. For an always-on dashboard you usually want to update a small region (a clock, a value) without the full-screen flash and without re-pushing all 2.6 M pixels. The driver exposes a render/push split:

- `render_framebuffer()` runs the ESPHome display lambda once into the PSRAM framebuffer. It does not touch the panel.
- `flush_zone(x, y, w, h, mode = 1)` pushes only the given logical rectangle to the panel. The default mode 1 (DU) is a fast binary waveform with no flash. It does not re-render, so call `render_framebuffer()` first when the content changed.
- `refresh_zone(x, y, w, h, mode = 1)` is the convenience combination of the two for a single isolated zone.
- `full_refresh()` does a full-screen INIT + GC16 pass (the flashy, ghost-free one). Use it at boot and for a periodic deghost.

Example: refresh the whole screen every minute with a single DU pass (no flash), and do a clean full refresh once a day.

```yaml
script:
  - id: live_refresh
    then:
      - lambda: |-
          id(epaper_display).render_framebuffer();
          id(epaper_display).flush_zone(0, 0, 1872, 1404, 1);
  - id: deghost
    then:
      - lambda: 'id(epaper_display).full_refresh();'

time:
  - platform: homeassistant
    on_time:
      - seconds: 0
        then: { script.execute: live_refresh }
      - hours: 5
        minutes: 0
        seconds: 0
        then: { script.execute: deghost }
```

Notes:
- DU (mode 1) accumulates ghosting over many partial updates. Schedule a periodic `full_refresh()` (nightly, for example) to clear it.
- `render_framebuffer()` is cheap on CPU but walks the whole PSRAM buffer, so it takes on the order of a second. Render once, then issue several `flush_zone()` calls when you update multiple small regions in the same pass.

## Displaying images straight from the SD card

The board has a microSD slot on the same SPI bus as the IT8951 (CS on GPIO14). The driver embeds SdFat in SHARED_SPI mode so it cooperates with the display CS line, and can render a full-screen image directly from the card with no Home Assistant download.

Images on the card are raw framebuffer files: 1872x1404, 4 bits per pixel, the exact in-memory layout the panel uses. Name them `/1.raw`, `/2.raw`, and so on at the card root.

```yaml
button:
  - platform: template
    name: "Show SD image 1"
    on_press:
      - lambda: 'id(epaper_display).show_sd_image("/1.raw");'
```

`show_sd_image()` loads the file into the framebuffer and pushes it in GC16 (16-level grayscale). This is how a local photo frame can keep working with no network once the card is written. To produce a `.raw`, convert a 1872x1404 16-level grayscale image and pack two pixels per byte (high nibble first), matching the panel framebuffer.

## Thumbnail gallery

For a picture-menu UI the driver can pre-compute and cache thumbnails on the SD card, then draw them into the framebuffer without triggering a refresh:

- `make_thumbnail(src, dst)` downsamples a full-screen `.raw` by `THUMB_FACTOR` (4 gives 468x351) and writes the thumbnail to the card. It overwrites the framebuffer while loading the source, so call it before you draw a screen, never during a render.
- `sd_file_exists(path)` lets you build the cache lazily (generate only the missing thumbnails).
- `draw_sd_thumbnail(path, dx, dy)` blits a cached thumbnail into the framebuffer at a logical corner. It only writes pixels, so call it from the display lambda before the GC16 push.

Example: generate the missing thumbnails for the current page once, then render a 3x3 grid from the lambda.

```yaml
script:
  - id: build_thumbs
    then:
      - lambda: |-
          for (int n = 1; n <= 9; n++) {
            std::string src   = "/" + std::to_string(n) + ".raw";
            std::string thumb = "/" + std::to_string(n) + "_thumb.raw";
            if (!id(epaper_display).sd_file_exists(thumb.c_str()))
              id(epaper_display).make_thumbnail(src.c_str(), thumb.c_str());
          }
      - component.update: epaper_display

# inside the display lambda, draw a cached thumbnail at a logical corner:
#   id(epaper_display).draw_sd_thumbnail("/1_thumb.raw", 78, 111);
```

## Interactive examples (touch dashboard)

These snippets show the display side of a live touch dashboard built on the calls above. They assume a landscape panel (`rotation: 0`, 1872x1404) and the GT911 touch controller enabled.

### Refresh just the clock every minute

`refresh_zone()` re-renders the framebuffer and pushes only the given rectangle with a fast DU waveform, so the rest of the screen is untouched and there is no flash.

```yaml
script:
  - id: clock_tick
    then:
      # x, y, w, h of the clock box, DU (mode 1)
      - lambda: 'id(epaper_display).refresh_zone(1512, 0, 360, 130, 1);'

time:
  - platform: homeassistant
    on_time:
      - seconds: 0
        then: { script.execute: clock_tick }
```

### A clickable button

Draw a labelled hit area in the display lambda, then test the touch coordinates in `on_touch` and call a Home Assistant action.

```yaml
touchscreen:
  - platform: gt911
    id: touch
    display: epaper_display
    reset_pin: GPIO48
    on_touch:
      - lambda: |-
          // bottom-right corner toggles the gate
          if (touch.x >= 1512 && touch.y >= 800 && touch.y <= 1080)
            id(toggle_gate).execute();

script:
  - id: toggle_gate
    then:
      - homeassistant.action:
          action: cover.toggle
          data:
            entity_id: cover.portail

display:
  - platform: it8951_reterminal_e1003
    id: epaper_display
    pages:
      - id: main
        lambda: |-
          // The driver is inverted: COLOR_OFF is black, COLOR_ON is white.
          // Draw the button label under the touch zone.
          it.printf(1692, 880,  id(font_icon),  COLOR_OFF, TextAlign::TOP_CENTER, "\U000F0299");  // gate
          it.printf(1692, 1020, id(font_label), COLOR_OFF, TextAlign::TOP_CENTER, "Gate");
```

### Thumbnail grid with pagination

Lazily build the cache for the current page, then draw a 3x3 grid from the page lambda. Changing `menu_page` and re-running the script paginates.

```yaml
globals:
  - id: menu_page
    type: int
    initial_value: "0"

script:
  - id: show_menu
    then:
      - lambda: |-
          int base = id(menu_page) * 9;
          for (int i = 0; i < 9; i++) {
            int n = base + i + 1;
            std::string src   = "/" + std::to_string(n) + ".raw";
            std::string thumb = "/" + std::to_string(n) + "_thumb.raw";
            if (!id(epaper_display).sd_file_exists(thumb.c_str()))
              id(epaper_display).make_thumbnail(src.c_str(), thumb.c_str());
          }
      - component.update: epaper_display

display:
  - platform: it8951_reterminal_e1003
    id: epaper_display
    pages:
      - id: gallery
        lambda: |-
          // 3x3 grid of cached thumbnails (cell 624x413, thumb 468x351)
          for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++) {
              int n = id(menu_page) * 9 + r * 3 + c + 1;
              std::string t = "/" + std::to_string(n) + "_thumb.raw";
              id(epaper_display).draw_sd_thumbnail(t.c_str(), c * 624 + 78, 80 + r * 413);
            }
```

### Touch navigation between photos

Tap the right third for the next photo, the left third for the previous one. `show_sd_image()` both loads the frame from the card and pushes it in GC16.

```yaml
substitutions:
  photo_count: "21"

globals:
  - id: photo
    type: int
    initial_value: "1"

touchscreen:
  - platform: gt911
    display: epaper_display
    reset_pin: GPIO48
    on_touch:
      - lambda: |-
          const int W = 1872, COUNT = ${photo_count};
          if (touch.x > (W * 2) / 3)
            id(photo) = (id(photo) >= COUNT) ? 1 : id(photo) + 1;
          else if (touch.x < W / 3)
            id(photo) = (id(photo) <= 1) ? COUNT : id(photo) - 1;
          else
            return;
          std::string p = "/" + std::to_string(id(photo)) + ".raw";
          id(epaper_display).show_sd_image(p.c_str());
```

## AI warning

While this is based on an original configuration for the Seeed reTerminal E1002 that I wrote manually, this version for the E1003 was made with heavy assistance from Claude Code. If you are against using AI-generated code please do not install this.
