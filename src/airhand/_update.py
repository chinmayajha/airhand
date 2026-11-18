import asyncio
import contextlib
import struct
import time
from collections.abc import Callable
from importlib import resources
from pathlib import Path

from airhand._dongle import UPDATE_PRODUCT, USB_PRODUCT, AirhandError, Dongle, DongleLost, Refused, ports

IMAGE_MAGIC = 0x96F3B83D
# MCUboot's direct-upload numbers: 1 is the running slot, 2 the slot a new image waits in.
PRIMARY, SECONDARY = 1, 2
# MCUboot serial recovery reads at most BOOT_LINE_BUFS lines of BOOT_MAX_LINE_INPUT_LEN encoded bytes per frame.
LINE_LENGTH, LINE_BUFFERS = 128, 8
MODE_WAIT_S = 20.0
# A swap of both slots, or the firmware's 60 s trial and the swap back when the new image never answers.
BOOT_WAIT_S = 120.0
POLL_S = 0.25
BOOTLOADER_HINT = (
    "Install the airhand bootloader once: press the dongle's RESET button and run "
    "'nrfutil nrf5sdk-tools dfu usb-serial -pkg airhand-bootloader-<version>.zip -p <port>'. "
    "See https://github.com/chinmayajha/airhand#install-the-bootloader"
)

Progress = Callable[[int, int], None]


def update(serial: str | None = None, *, firmware: str | Path | None = None, progress: Progress | None = None) -> str:
    """Install firmware on a dongle over USB, with no button; returns the dongle's new identity.

    Uses the firmware this package ships, and leaves a dongle that already runs it alone. An image file given as
    ``firmware`` is always installed. ``progress`` gets the bytes sent and the image size as the upload goes.
    """
    image = Path(firmware).read_bytes() if firmware is not None else shipped_firmware()
    target = image_version(image)
    serial = serial or _only_serial()
    if not ports(serial, UPDATE_PRODUCT):
        current = _enter_update_mode(serial, None if firmware is not None else target)
        if current:
            return current
    port = _wait_for(serial, UPDATE_PRODUCT, MODE_WAIT_S)
    if port is None:
        raise DongleLost(f"The dongle {serial} did not come back in update mode; unplug it and plug it in again")
    asyncio.run(_install(port, image, progress))
    if _wait_for(serial, USB_PRODUCT, BOOT_WAIT_S) is None:
        raise DongleLost(f"The dongle {serial} did not start again after the update; unplug it and plug it in again")
    with Dongle(serial) as dongle:
        identity = dongle.identity
    if identity.split()[1] != target:
        raise AirhandError(f"Firmware {target} did not start, so the dongle went back to {identity}")
    return identity


def shipped_firmware() -> bytes:
    """The firmware image this package ships."""
    shipped = resources.files("airhand").joinpath("firmware.bin")
    if not shipped.is_file():
        raise AirhandError("This airhand build ships no firmware; give the image file with firmware=")
    return shipped.read_bytes()


def image_version(image: bytes) -> str:
    """The version in an MCUboot image header, such as ``0.1.3``."""
    if len(image) < 32 or struct.unpack_from("<I", image)[0] != IMAGE_MAGIC:
        raise AirhandError("The firmware file is not an airhand update image (zephyr.signed.bin)")
    major, minor, revision = struct.unpack_from("<BBH", image, 20)
    return f"{major}.{minor}.{revision}"


def _only_serial() -> str:
    found = sorted({p.serial_number for p in ports(product=USB_PRODUCT) + ports(product=UPDATE_PRODUCT)})
    if not found:
        raise DongleLost("No airhand dongle is plugged in")
    if len(found) > 1:
        raise ValueError(f"Several airhand dongles are plugged in ({', '.join(found)}); give the serial number of one")
    return found[0]


class _AnySeries(Dongle):
    """A dongle opened only to update it, so firmware of another minor series is welcome."""

    _series = None


def _enter_update_mode(serial: str, skip_on: str | None) -> str | None:
    """Asks the running firmware to restart in update mode; returns its identity when it already runs ``skip_on``."""
    dongle = _AnySeries(serial)
    try:
        if dongle.identity.split()[1] == skip_on:
            return dongle.identity
        dongle._simple("update")
    except Refused as exc:
        if exc.reason == "unknown verb":
            raise AirhandError(f"{dongle.identity} cannot update itself. {BOOTLOADER_HINT}") from exc
        raise
    finally:
        dongle.close()
    return None


def _wait_for(serial: str, product: str, wait_s: float) -> str | None:
    deadline = time.monotonic() + wait_s
    while time.monotonic() < deadline:
        found = ports(serial, product)
        if found:
            return found[0].device
        time.sleep(POLL_S)
    return None


async def _install(port: str, image: bytes, progress: Progress | None) -> None:
    # Imported here so that sending input never loads the update stack.
    from smpclient import SMPClient
    from smpclient.exceptions import SMPClientException
    from smpclient.transport.serial import BufferParams, SMPSerialTransport

    transport = SMPSerialTransport(BufferParams(line_length=LINE_LENGTH, line_buffers=LINE_BUFFERS))
    try:
        async with SMPClient(transport, port) as client:
            await _upload(client, image, progress)
    except (SMPClientException, OSError, asyncio.TimeoutError) as exc:
        raise DongleLost(f"The upload stopped ({exc}); the old firmware is still in place, so run it again") from exc


async def _upload(client, image: bytes, progress: Progress | None) -> None:
    from smpclient.generics import error
    from smpclient.requests.image_management import ImageStatesWrite
    from smpclient.requests.os_management import ResetWrite

    has_app = any(state.slot == 0 for state in await _images(client))
    async for sent in client.upload(image, slot=SECONDARY if has_app else PRIMARY):
        if progress:
            progress(sent, len(image))
    if has_app:
        waiting = [state for state in await _images(client) if state.slot == 1]
        if not waiting:
            raise AirhandError("The bootloader did not accept the uploaded image")
        response = await client.request(ImageStatesWrite(hash=waiting[0].hash, confirm=False))
        if error(response):
            raise AirhandError(f"The bootloader refused to test the new image: {response}")
    with contextlib.suppress(asyncio.TimeoutError):
        await client.request(ResetWrite())


async def _images(client) -> list:
    from smpclient.generics import error
    from smpclient.requests.image_management import ImageStatesRead

    response = await client.request(ImageStatesRead())
    if error(response):
        raise AirhandError(f"The bootloader did not list its images: {response}")
    return list(response.images)
