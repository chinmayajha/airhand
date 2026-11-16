"""Drive any device that accepts a Bluetooth keyboard and mouse, through a hidwire dongle on USB serial."""

from hidwire._dongle import (
    BUTTONS,
    KEYS,
    MEDIA,
    MODIFIERS,
    Dongle,
    DongleLost,
    HidwireError,
    HostUnavailable,
    Refused,
    Unsupported,
    WrongFirmware,
    port_of,
)

__all__ = [
    "BUTTONS",
    "KEYS",
    "MEDIA",
    "MODIFIERS",
    "Dongle",
    "DongleLost",
    "HidwireError",
    "HostUnavailable",
    "Refused",
    "Unsupported",
    "WrongFirmware",
    "port_of",
]
