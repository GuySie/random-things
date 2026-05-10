# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

**opendisplay-extended** is a Home Assistant custom component that extends the upstream `opendisplay` integration with additional services. It registers two services:

- `upload_random_image` — accepts a media folder, randomly selects an image from it, and delegates to `opendisplay.upload_image`.
- `upload_image_from_url` — accepts an HTTP/HTTPS URL, downloads the image, stages it as a temp file in `<hass.config.media_dirs["local"]>/tmp/`, and delegates to `opendisplay.upload_image`. Temp files use the prefix `opendisplay_tmp_` and are deleted after the upload (but may be orphaned on crash).

## Project Structure

```
custom_components/opendisplay_extended/
├── __init__.py           # async_setup entry point — registers services
├── services.py           # Service schema, handler, and registration
├── services.yaml         # Home Assistant UI schema for the service
├── manifest.json         # Component metadata and dependencies
├── const.py              # Domain name constant
└── strings.json          # Translated error messages
```

## Dependencies & Requirements

- **opendisplay** integration: The upstream HA component this wraps (domain: `opendisplay`)
- **media_source** component: Used for folder browsing via `async_browse_media`

## Development Workflow

### Setup
Place `custom_components/opendisplay_extended/` inside a Home Assistant instance's `custom_components/` directory and restart. The integration auto-loads via `async_setup`.

### Testing
Manual via Home Assistant Developer Tools → Actions → `opendisplay_extended.upload_random_image` or `opendisplay_extended.upload_image_from_url`. Monitor errors in Settings → Logs.

## Upstream `opendisplay` Integration

Source: https://github.com/home-assistant/core/tree/dev/homeassistant/components/opendisplay  
Docs: https://www.home-assistant.io/integrations/opendisplay/

Key facts:

### `upload_image` service (the one we delegate to)
Defined in upstream `services.py`. Schema (`SCHEMA_UPLOAD_IMAGE`) fields:
- `device_id` (string) — HA device registry ID
- `image` — MediaSelector dict with `media_content_id` and `media_content_type`
- `rotation` — int coerced to `Rotation` enum (0/90/180/270), default `ROTATE_0`
- `dither_mode` — lowercase enum name string, default `"burkes"`
- `refresh_mode` — lowercase enum name string, default `"full"`
- `fit_mode` — lowercase enum name string, default `"contain"`
- `tone_compression` — optional float 0–100 (%)

The upstream handler converts `tone_compression` percent → `float / 100` (or `"auto"` if absent) before passing to `device.upload_image()`. It also cancels any in-progress upload for the same entry before starting a new one (`entry.runtime_data.upload_task`).

### Upstream error classes
`AuthenticationFailedError`, `AuthenticationRequiredError` → trigger re-auth flow.  
`BLEConnectionError`, `BLETimeoutError`, `OpenDisplayError` → raise `HomeAssistantError`.

### Device types
- **Standard devices**: only the `upload_image` service; no HA entities
- **Flex devices** (`is_flex=True`): additionally expose a `sensor` entity (battery voltage) and one `event` entity per physical button (`button_down` / `button_up`). Button events are detected by diffing consecutive passive BLE advertisements — no active connection required.

### Bluetooth requirements
Active BLE connections are needed for image uploads. Supported adapters: built-in adapters and ESPHome proxies (firmware ≥ 2022.9.3). **Shelly BLE proxies are not supported** (no active connection support). Displays with 40-pin or 60-pin connectors are also unsupported.

### Encryption
Optional AES-128. If enabled, the device shows a 32-character hex key on boot; this is stored in `entry.data[CONF_ENCRYPTION_KEY]`. Authentication failures trigger `entry.async_start_reauth()`.

### `OpenDisplayRuntimeData` (on `entry.runtime_data`)
- `coordinator`: passive BLE advertisement coordinator
- `firmware`: `FirmwareVersion` dict
- `device_config`: `GlobalConfig` (passed as `config=` to `OpenDisplayDevice` to avoid a second BLE read)
- `is_flex`: bool — determines which HA platforms are loaded
- `upload_task`: `asyncio.Task | None` — tracks the current upload; cancelled on new upload or entry unload

## Key Implementation Details

### Folder extraction trick
The `ATTR_FOLDER` field uses `MediaSelector(accept=["image/*"])` — the user picks an image file, not a folder. The handler strips the last path segment with `rsplit("/", 1)[0]` to derive the parent folder's `media_content_id`, then calls `async_browse_media` on that.

### URL image staging
`upload_image_from_url` downloads the image via HA's shared aiohttp session (`async_get_clientsession`), writes it to `<hass.config.media_dirs["local"]>/tmp/opendisplay_tmp_<uuid>.<ext>` in an executor job, then passes `media-source://media_source/local/tmp/<filename>` as the `media_content_id` to `opendisplay.upload_image`. Note: `hass.config.media_dirs["local"]` is used (not `hass.config.path("media")`) because the media source resolves content IDs against the former — they can differ (e.g. `/media` vs `/config/media` in Docker installs). The upstream resolves this to a local file path and loads it with PIL. The temp file is deleted in a `finally` block.

### Delegation pattern
String and int values (`dither_mode`, `refresh_mode`, `fit_mode`, `rotation`, `tone_compression`) are passed directly to `opendisplay.upload_image` without conversion. No `py-opendisplay` types are used; the upstream service validates the values against its own schema.

### Error handling
`ServiceValidationError` (with `translation_domain=DOMAIN` and a `translation_key`) is used for user-facing validation failures. Translation keys must exist in `strings.json`.
