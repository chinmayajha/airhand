"""A fake dongle on a pseudo-terminal, for testing code that uses :class:`hidwire.Dongle` without hardware.

POSIX only. Pass ``port=fake.port`` to ``Dongle``, or look the fake up by ``fake.serial_number``.
"""

import json
import os
import select
import threading
from collections.abc import Iterable

from hidwire._dongle import FIRMWARE, KEYS, MEDIA, MODIFIERS

REPORT_VERBS = frozenset({"tap", "hold", "move", "swipe", "wheel", "text", "key", "media", "pointer"})
TYPABLE = frozenset({chr(c) for c in range(32, 127)} | {"\n", "\t"})


class FakeDongle:
    """Answers by protocol v2 and records every command it accepted, as a tuple of words."""

    def __init__(
        self,
        *,
        serial_number: str = "FAKE0001",
        host_connected: bool = True,
        unencrypted_replies: int = 0,
        swallow: Iterable[str] = (),
        silent_after: int | None = None,
        echo: bool = False,
        firmware: str = f"{FIRMWARE} 2.2.0",
    ) -> None:
        self.serial_number = serial_number
        self.host_connected = host_connected
        self.unencrypted_replies = unencrypted_replies
        self.swallow = set(swallow)
        self.silent_after = silent_after
        self.echo = echo
        self.firmware = firmware
        self.commands: list[tuple[str, ...]] = []
        self._replies: dict[str, str] = {}
        self._master, slave = os.openpty()
        self.port = os.ttyname(slave)
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._serve, daemon=True)
        self._thread.start()

    @property
    def plugged(self) -> bool:
        return not self._stop.is_set()

    def typed(self) -> str:
        return "".join(json.loads(words[1]) for words in self.commands if words[0] == "text")

    def unplug(self) -> None:
        if self._stop.is_set():
            return
        self._stop.set()
        self._thread.join()
        os.close(self._master)

    def __enter__(self) -> "FakeDongle":
        return self

    def __exit__(self, *exc_info: object) -> None:
        self.unplug()

    def _serve(self) -> None:
        pending = b""
        while not self._stop.is_set():
            if not select.select([self._master], [], [], 0.05)[0]:
                continue
            pending += os.read(self._master, 4096)
            while b"\n" in pending:
                line, pending = pending.split(b"\n", 1)
                self._answer(line.decode())

    def _answer(self, line: str) -> None:
        if self.echo:
            os.write(self._master, f"{line}\n".encode())
        request_id, command, *rest = line.split(" ", 2)
        words = (command, *([rest[0]] if command == "text" else rest[0].split() if rest else []))
        if command not in {"version", "result"}:
            self.commands.append(words)
        if self.silent_after is not None and len(self.commands) > self.silent_after:
            return
        reply = self._reply(words)
        self._replies[request_id] = reply
        if command in self.swallow:
            self.swallow.discard(command)
            return
        os.write(self._master, f"{request_id} {reply}\n".encode())

    def _reply(self, words: tuple[str, ...]) -> str:
        command = words[0]
        if command == "version":
            return f"ok {self.firmware} id={self.serial_number}"
        if command == "result":
            return self._replies.get(words[1], "err unknown id")
        if command == "status":
            return f"ok conn={'yes' if self.host_connected else 'no'} sec={0 if self.unencrypted_replies else 2}"
        if command in REPORT_VERBS and not self.host_connected:
            return "err not connected"
        if command in REPORT_VERBS and self.unencrypted_replies:
            self.unencrypted_replies -= 1
            return "err not secure"
        if command == "text" and not set(json.loads(words[1])) <= TYPABLE:
            return "err unsupported char"
        if command == "key" and not _known_combo(words[1]):
            return "err unsupported key"
        if command == "media" and words[1] not in MEDIA:
            return "err unsupported media"
        return "ok"


def _known_combo(combo: str) -> bool:
    *modifiers, key = combo.lower().split("+")
    return set(modifiers) <= MODIFIERS and (key in KEYS or (len(key) == 1 and key in TYPABLE))
