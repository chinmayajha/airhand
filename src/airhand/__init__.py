"""Drive any device that accepts a Bluetooth keyboard and mouse, through an airhand dongle on USB serial."""

from airhand._dongle import (
    BUTTONS,
    KEYS,
    MEDIA,
    MODIFIERS,
    AirhandError,
    Dongle,
    DongleLost,
    HostUnavailable,
    Refused,
    Unsupported,
    WrongFirmware,
    port_of,
    serial_numbers,
)

__all__ = [
    "AirhandError",
    "BUTTONS",
    "KEYS",
    "MEDIA",
    "MODIFIERS",
    "Dongle",
    "DongleLost",
    "HostUnavailable",
    "Refused",
    "Unsupported",
    "WrongFirmware",
    "port_of",
    "serial_numbers",
]
