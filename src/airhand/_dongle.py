import itertools
import json
import logging
import threading
import time
from collections.abc import Callable, Iterator
from contextlib import contextmanager, suppress

from serial import Serial
from serial.tools import list_ports

FIRMWARE = "airhand"
USB_PRODUCT = "airhand"
# A client works with firmware of its own minor series; until 1.0.0 a minor release may change the protocol.
FIRMWARE_SERIES = "0.1"
BAUDRATE = 115200
REPLY_WAIT_S = 2.0
TYPE_WAIT_PER_CHAR_S = 0.05
TEXT_CHUNK = 64
WHEEL_STEP_S = 0.015
HOST_WAIT_S = 10.0
PORT_WAIT_S = 10.0
POLL_S = 0.25
# A host that reconnects encrypts the link a moment later.
HOST_COMING_UP = frozenset({"not connected", "not secure"})

KEYS = frozenset(
    {
        "enter",
        "return",
        "escape",
        "esc",
        "backspace",
        "tab",
        "space",
        "insert",
        "home",
        "pageup",
        "delete",
        "end",
        "pagedown",
        "right",
        "left",
        "down",
        "up",
        "printscreen",
        "scrolllock",
        "pause",
        "numlock",
        "numpaddivide",
        "numpadmultiply",
        "numpadminus",
        "numpadplus",
        "numpadenter",
        "numpaddecimal",
        "numpadperiod",
        "capslock",
        "menu",
        "application",
    }
    | {f"f{n}" for n in range(1, 25)}
    | {f"numpad{n}" for n in range(10)}
)
MODIFIERS = frozenset({"ctrl", "control", "shift", "alt", "option", "cmd", "command", "gui", "meta", "win"})
MEDIA = frozenset(
    {
        "home",
        "back",
        "search",
        "volup",
        "voldown",
        "mute",
        "playpause",
        "next",
        "previous",
        "stop",
        "brightnessup",
        "brightnessdown",
    }
)
BUTTONS = frozenset({"left", "right", "middle"})

log = logging.getLogger("airhand")


class AirhandError(Exception):
    """Base of every error the client raises."""


class DongleLost(AirhandError):
    """The dongle is not plugged in, cannot be opened, or stopped answering; a command may or may not have run."""


class HostUnavailable(AirhandError):
    """No host took the command within the wait: it is not connected, or never encrypted the link."""

    def __init__(self, message: str, reason: str) -> None:
        super().__init__(message)
        self.reason = reason


class WrongFirmware(AirhandError):
    """The serial port answers, but not with airhand firmware of this protocol version."""


class Refused(AirhandError):
    """The dongle refused the command and sent nothing for it."""

    def __init__(self, message: str, reason: str) -> None:
        super().__init__(message)
        self.reason = reason


class Unsupported(Refused):
    """The input cannot be sent at all: a character, key or position outside what the dongle supports."""


class Dongle:
    """One airhand dongle on USB serial; every request carries an id and gets exactly one reply.

    Positions are fractions of the host's screen, 0.0 to 1.0. Give the dongle's USB ``serial`` number,
    which survives a replug, or a fixed ``port``; with neither, the one plugged-in dongle is used.
    With ``touch``, taps and swipes reach the host as a touch screen, as Android needs, instead of a pointer.
    """

    def __init__(self, serial: str | None = None, *, port: str | None = None, touch: bool = False) -> None:
        if serial is not None and port is not None:
            raise ValueError("give serial or port, not both")
        if port is None and serial is None:
            serial = _only_dongle()
        self._find_port: Callable[[], str | None] = (lambda: port) if port else (lambda: port_of(serial))
        self._lock = threading.Lock()
        self._ids = itertools.count(1)
        self._link: Serial | None = None
        self._buffer = b""
        self._touch = touch
        self.identity = self._connect(wait_s=0)

    def __enter__(self) -> "Dongle":
        return self

    def __exit__(self, *exc_info: object) -> None:
        self.close()

    def tap(self, x: float, y: float, button: str = "left", count: int = 1) -> None:
        """Click at a position; ``count=2`` is a double click."""
        words = [_fraction(x), _fraction(y)]
        if (button, count) != ("left", 1):
            words += [_button(button), str(count)]
        self._call("tap", *words)

    def hold(self, x: float, y: float, ms: int, button: str = "left") -> None:
        """Press at a position for ``ms`` milliseconds: a long press, or a right-button hold."""
        self._call(
            "hold",
            _fraction(x),
            _fraction(y),
            str(ms),
            *([_button(button)] if button != "left" else []),
            extra_wait_s=ms / 1000,
        )

    def move(self, x: float, y: float) -> None:
        """Move the pointer without pressing: a hover."""
        self._call("move", _fraction(x), _fraction(y))

    def swipe(self, start: tuple[float, float], end: tuple[float, float], ms: int, hold_ms: int = 0) -> None:
        """Press at ``start``, wait ``hold_ms``, move to ``end`` over ``ms`` and release: a swipe, or a drag."""
        words = [*map(_fraction, (*start, *end)), str(ms)] + ([str(hold_ms)] if hold_ms else [])
        self._call("swipe", *words, extra_wait_s=(ms + hold_ms) / 1000)

    def wheel(self, lines: int, pan: int = 0) -> None:
        """Turn the wheel by ``lines`` detents; positive is up, away from the user, and positive ``pan`` is right."""
        steps = max(abs(lines), abs(pan))
        self._call("wheel", str(lines), *([str(pan)] if pan else []), extra_wait_s=steps * WHEEL_STEP_S)

    def type_text(self, text: str) -> None:
        for start in range(0, len(text), TEXT_CHUNK):
            chunk = text[start : start + TEXT_CHUNK]
            self._call("text", json.dumps(chunk, ensure_ascii=False), extra_wait_s=len(chunk) * TYPE_WAIT_PER_CHAR_S)

    def key(self, combo: str) -> None:
        """Press and release a key such as ``enter``, ``a`` or ``cmd+space``."""
        self._call("key", combo)

    def media(self, name: str) -> None:
        """Press and release a consumer key: ``home``, ``back``, ``volup`` or ``voldown``."""
        self._call("media", name)

    def status(self) -> dict[str, str]:
        """The dongle's link state, such as ``{"conn": "yes", "sec": "2", "interval_ms": "45"}``."""
        with self._lock:
            status, data = self._request(("status",), REPLY_WAIT_S)
        if status != "ok":
            raise _refusal("status", data)
        return dict(word.split("=", 1) for word in data.split() if "=" in word)

    def unpair(self) -> None:
        """Forget every host the dongle has paired with."""
        self._simple("unpair")

    def close(self) -> None:
        with self._lock:
            if self._link is None:
                return
            try:
                self._exchange(("reset",), REPLY_WAIT_S)
            except OSError as exc:
                log.debug("reset on close failed: %s", exc)
            self._drop_link()

    def _simple(self, verb: str) -> None:
        with self._lock:
            status, data = self._request((verb,), REPLY_WAIT_S)
        if status != "ok":
            raise _refusal(verb, data)

    def _call(self, *words: str, extra_wait_s: float = 0.0) -> str:
        wait_s = REPLY_WAIT_S + extra_wait_s
        deadline = time.monotonic() + HOST_WAIT_S
        with self._lock:
            status, data = self._request(words, wait_s)
            while status == "err" and data in HOST_COMING_UP and time.monotonic() < deadline:
                time.sleep(POLL_S)
                status, data = self._request(words, wait_s)
        if status == "ok":
            return data
        raise _refusal(words[0], data)

    def _request(self, words: tuple[str, ...], wait_s: float) -> tuple[str, str]:
        if self._link is None:
            self.identity = self._connect(wait_s=PORT_WAIT_S)
        try:
            request_id = self._send(words)
        except OSError:
            self._drop_link()
            self.identity = self._connect(wait_s=PORT_WAIT_S)
            with self._lost_during(words[0]):
                request_id = self._send(words)
        with self._lost_during(words[0]):
            reply = self._read_reply(request_id, wait_s)
            if reply is None:
                reply = self._read_reply(self._send(("result", str(request_id))), REPLY_WAIT_S)
        if reply is None:
            self._drop_link()
            raise DongleLost(f"The dongle stopped answering during '{words[0]}'; it was not resent")
        return reply

    @contextmanager
    def _lost_during(self, command: str) -> Iterator[None]:
        try:
            yield
        except OSError as exc:
            self._drop_link()
            raise DongleLost(f"The dongle link dropped during '{command}': {exc}") from exc

    def _connect(self, wait_s: float) -> str:
        port = self._find_port()
        deadline = time.monotonic() + wait_s
        while port is None and time.monotonic() < deadline:
            time.sleep(POLL_S)
            port = self._find_port()
        if port is None:
            raise DongleLost("The dongle is not plugged in")
        try:
            self._link = Serial(port, baudrate=BAUDRATE, timeout=POLL_S)
            self._link.reset_input_buffer()
            self._buffer = b""
            reply = self._exchange(("version",), REPLY_WAIT_S)
        except OSError as exc:
            self._drop_link()
            raise DongleLost(f"Could not open the dongle on {port}: {exc}") from exc
        try:
            identity = _checked_identity(reply, port)
            mode = self._exchange(("mode", "touch"), REPLY_WAIT_S) if self._touch else ("ok", "")
            if mode is None:
                raise OSError("no reply to 'mode touch'")
            if mode != ("ok", ""):
                raise WrongFirmware(f"The dongle on {port} runs {identity}; touch needs {FIRMWARE} 0.1.2 or later")
        except WrongFirmware:
            self._drop_link()
            raise
        except OSError as exc:
            self._drop_link()
            raise DongleLost(f"Could not open the dongle on {port}: {exc}") from exc
        return identity

    def _exchange(self, words: tuple[str, ...], wait_s: float) -> tuple[str, str] | None:
        return self._read_reply(self._send(words), wait_s)

    def _send(self, words: tuple[str, ...]) -> int:
        if self._link is None:
            raise OSError("The dongle link is closed")
        request_id = next(self._ids)
        self._link.write((" ".join((str(request_id), *words)) + "\n").encode("utf-8"))
        self._link.flush()
        return request_id

    def _read_reply(self, request_id: int, wait_s: float) -> tuple[str, str] | None:
        deadline = time.monotonic() + wait_s
        while time.monotonic() < deadline:
            line = self._read_line()
            if line is None:
                continue
            reply_id, _, rest = line.partition(" ")
            status, _, data = rest.partition(" ")
            if reply_id == str(request_id) and status in ("ok", "err"):
                return status, data
            log.debug("skipped a line of %d characters that is not this reply", len(line))
        return None

    def _read_line(self) -> str | None:
        if self._link is None:
            raise OSError("The dongle link is closed")
        self._buffer += self._link.read_until(b"\n")
        if not self._buffer.endswith(b"\n"):
            return None
        line, self._buffer = self._buffer, b""
        return line.decode("utf-8", errors="replace").strip()

    def _drop_link(self) -> None:
        if self._link is not None:
            with suppress(OSError):
                self._link.close()
        self._link = None


def port_of(serial_number: str | None) -> str | None:
    """The serial port of the dongle with this USB serial number, or None when it is not plugged in."""
    for port in list_ports.comports():
        if port.serial_number == serial_number:
            return port.device
    return None


def serial_numbers() -> list[str]:
    """The USB serial numbers of the plugged-in airhand dongles."""
    return sorted(
        port.serial_number for port in list_ports.comports() if port.product == USB_PRODUCT and port.serial_number
    )


def _only_dongle() -> str:
    found = serial_numbers()
    if not found:
        raise DongleLost("No airhand dongle is plugged in")
    if len(found) > 1:
        raise ValueError(f"Several airhand dongles are plugged in ({', '.join(found)}); give the serial number of one")
    return found[0]


def _fraction(value: float) -> str:
    if not 0.0 <= value <= 1.0:
        raise Unsupported(f"Position {value:.4f} is outside the screen", "position")
    return f"{value:.4f}"


def _button(name: str) -> str:
    if name not in BUTTONS:
        raise Unsupported(f"Button '{name}' is not one of {', '.join(sorted(BUTTONS))}", "button")
    return name


def _refusal(command: str, reason: str) -> AirhandError:
    if reason == "not connected":
        return HostUnavailable("No host is connected to the dongle: pair it, or turn on the host's Bluetooth", reason)
    if reason == "not secure":
        return HostUnavailable(
            "The host connected but did not encrypt the link: forget the dongle on the host and pair it again", reason
        )
    if reason.startswith("unsupported"):
        return Unsupported(f"The dongle refused '{command}': {reason}", reason)
    return Refused(f"The dongle refused '{command}': {reason}", reason)


def _checked_identity(reply: tuple[str, str] | None, port: str) -> str:
    words = reply[1].split() if reply and reply[0] == "ok" else []
    series = words[1].rsplit(".", 1)[0] if len(words) > 1 else ""
    if len(words) < 2 or words[0] != FIRMWARE or series != FIRMWARE_SERIES:
        raise WrongFirmware(
            f"The dongle on {port} does not run {FIRMWARE} firmware {FIRMWARE_SERIES}.x "
            f"(it answered {reply!r} to 'version'). Flash it with {FIRMWARE} {FIRMWARE_SERIES}.x firmware."
        )
    return " ".join(words)
