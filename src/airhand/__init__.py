"""Drive any device that accepts a Bluetooth keyboard and mouse, through an airhand dongle on USB serial."""

from airhand._dongle import (
    BUTTONS,
    KEYS,
    MEDIA,
    MODIFIERS,
    AirhandError,
    Dongle,
    DongleLost,
    Host,
    HostUnavailable,
    Refused,
    Unsupported,
    WrongFirmware,
    port_of,
    serial_numbers,
)
from airhand._update import update

__all__ = [
    "AirhandError",
    "BUTTONS",
    "KEYS",
    "MEDIA",
    "MODIFIERS",
    "Dongle",
    "DongleLost",
    "Host",
    "HostUnavailable",
    "Refused",
    "Unsupported",
    "WrongFirmware",
    "port_of",
    "serial_numbers",
    "update",
]
