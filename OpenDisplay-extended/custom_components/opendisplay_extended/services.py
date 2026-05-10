"""Service registration for the OpenDisplay Extended integration."""

import contextlib
import os
import random
import uuid
from typing import Any

import aiohttp

import voluptuous as vol

from homeassistant.components.media_source import async_browse_media
from homeassistant.const import ATTR_DEVICE_ID
from homeassistant.core import HomeAssistant, ServiceCall, callback
from homeassistant.exceptions import ServiceValidationError
from homeassistant.helpers import config_validation as cv
from homeassistant.helpers.aiohttp_client import async_get_clientsession
from homeassistant.helpers.selector import MediaSelector, MediaSelectorConfig

from .const import DOMAIN

ATTR_FOLDER = "folder"
ATTR_IMAGE_URL = "image_url"
ATTR_ROTATION = "rotation"
ATTR_DITHER_MODE = "dither_mode"
ATTR_REFRESH_MODE = "refresh_mode"
ATTR_FIT_MODE = "fit_mode"
ATTR_TONE_COMPRESSION = "tone_compression"

_OPENDISPLAY_DOMAIN = "opendisplay"

_MIME_TO_EXT: dict[str, str] = {
    "image/jpeg": ".jpg",
    "image/png": ".png",
    "image/gif": ".gif",
    "image/webp": ".webp",
    "image/bmp": ".bmp",
}

SCHEMA_UPLOAD_RANDOM_IMAGE = vol.Schema(
    {
        vol.Required(ATTR_DEVICE_ID): cv.string,
        vol.Required(ATTR_FOLDER): MediaSelector(
            MediaSelectorConfig(accept=["image/*"])
        ),
        vol.Optional(ATTR_ROTATION, default=0): vol.Coerce(int),
        vol.Optional(ATTR_DITHER_MODE, default="burkes"): cv.string,
        vol.Optional(ATTR_REFRESH_MODE, default="full"): cv.string,
        vol.Optional(ATTR_FIT_MODE, default="contain"): cv.string,
        vol.Optional(ATTR_TONE_COMPRESSION): vol.All(
            vol.Coerce(float), vol.Range(min=0.0, max=100.0)
        ),
    }
)


async def _async_upload_random_image(call: ServiceCall) -> None:
    """Handle the upload_random_image service call."""
    folder_data: dict[str, Any] = call.data[ATTR_FOLDER]
    image_content_id: str = folder_data["media_content_id"]
    folder_content_id = image_content_id.rsplit("/", 1)[0]

    browsed = await async_browse_media(call.hass, folder_content_id)
    children = browsed.children or []
    image_children = [
        child
        for child in children
        if child.can_play
        and isinstance(child.media_content_type, str)
        and child.media_content_type.startswith("image/")
    ]
    if not image_children:
        raise ServiceValidationError(
            translation_domain=DOMAIN,
            translation_key="no_images_in_folder",
        )

    chosen = random.choice(image_children)

    service_data: dict[str, Any] = {
        ATTR_DEVICE_ID: call.data[ATTR_DEVICE_ID],
        "image": {
            "media_content_id": chosen.media_content_id,
            "media_content_type": chosen.media_content_type,
        },
        ATTR_ROTATION: call.data[ATTR_ROTATION],
        ATTR_DITHER_MODE: call.data[ATTR_DITHER_MODE],
        ATTR_REFRESH_MODE: call.data[ATTR_REFRESH_MODE],
        ATTR_FIT_MODE: call.data[ATTR_FIT_MODE],
    }
    if (tone_compression_pct := call.data.get(ATTR_TONE_COMPRESSION)) is not None:
        service_data[ATTR_TONE_COMPRESSION] = tone_compression_pct

    await call.hass.services.async_call(
        _OPENDISPLAY_DOMAIN,
        "upload_image",
        service_data,
        blocking=True,
    )


SCHEMA_UPLOAD_IMAGE_FROM_URL = vol.Schema(
    {
        vol.Required(ATTR_DEVICE_ID): cv.string,
        vol.Required(ATTR_IMAGE_URL): cv.url,
        vol.Optional(ATTR_ROTATION, default=0): vol.Coerce(int),
        vol.Optional(ATTR_DITHER_MODE, default="burkes"): cv.string,
        vol.Optional(ATTR_REFRESH_MODE, default="full"): cv.string,
        vol.Optional(ATTR_FIT_MODE, default="contain"): cv.string,
        vol.Optional(ATTR_TONE_COMPRESSION): vol.All(
            vol.Coerce(float), vol.Range(min=0.0, max=100.0)
        ),
    }
)


async def _async_upload_image_from_url(call: ServiceCall) -> None:
    """Handle the upload_image_from_url service call."""
    image_url: str = call.data[ATTR_IMAGE_URL]

    session = async_get_clientsession(call.hass)
    try:
        async with session.get(image_url) as resp:
            resp.raise_for_status()
            content_type = resp.content_type or "image/jpeg"
            image_bytes = await resp.read()
    except aiohttp.ClientError as err:
        raise ServiceValidationError(
            translation_domain=DOMAIN,
            translation_key="image_download_failed",
        ) from err

    local_media_dir = call.hass.config.media_dirs.get("local")
    if local_media_dir is None:
        raise ServiceValidationError(
            translation_domain=DOMAIN,
            translation_key="no_local_media_dir",
        )

    ext = _MIME_TO_EXT.get(content_type, ".jpg")
    filename = f"opendisplay_tmp_{uuid.uuid4().hex}{ext}"
    tmp_dir = os.path.join(local_media_dir, "tmp")
    tmp_path = os.path.join(tmp_dir, filename)

    def _write() -> None:
        os.makedirs(tmp_dir, exist_ok=True)
        with open(tmp_path, "wb") as f:
            f.write(image_bytes)

    await call.hass.async_add_executor_job(_write)

    try:
        service_data: dict[str, Any] = {
            ATTR_DEVICE_ID: call.data[ATTR_DEVICE_ID],
            "image": {
                "media_content_id": f"media-source://media_source/local/tmp/{filename}",
                "media_content_type": content_type,
            },
            ATTR_ROTATION: call.data[ATTR_ROTATION],
            ATTR_DITHER_MODE: call.data[ATTR_DITHER_MODE],
            ATTR_REFRESH_MODE: call.data[ATTR_REFRESH_MODE],
            ATTR_FIT_MODE: call.data[ATTR_FIT_MODE],
        }
        if (tone_compression_pct := call.data.get(ATTR_TONE_COMPRESSION)) is not None:
            service_data[ATTR_TONE_COMPRESSION] = tone_compression_pct

        await call.hass.services.async_call(
            _OPENDISPLAY_DOMAIN,
            "upload_image",
            service_data,
            blocking=True,
        )
    finally:
        def _delete() -> None:
            with contextlib.suppress(FileNotFoundError):
                os.unlink(tmp_path)

        await call.hass.async_add_executor_job(_delete)


@callback
def async_setup_services(hass: HomeAssistant) -> None:
    """Register OpenDisplay Extended services."""
    hass.services.async_register(
        DOMAIN,
        "upload_random_image",
        _async_upload_random_image,
        schema=SCHEMA_UPLOAD_RANDOM_IMAGE,
    )
    hass.services.async_register(
        DOMAIN,
        "upload_image_from_url",
        _async_upload_image_from_url,
        schema=SCHEMA_UPLOAD_IMAGE_FROM_URL,
    )
