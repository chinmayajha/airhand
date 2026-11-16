import sys

import pytest

from hidwire import _dongle
from hidwire.testing import FakeDongle

collect_ignore_glob = ["*.py"] if sys.platform == "win32" else []


@pytest.fixture
def fast(monkeypatch):
    monkeypatch.setattr(_dongle, "HOST_WAIT_S", 0.5)
    monkeypatch.setattr(_dongle, "PORT_WAIT_S", 1.0)
    monkeypatch.setattr(_dongle, "REPLY_WAIT_S", 0.3)


@pytest.fixture
def plugged(monkeypatch, fast):
    fakes: list[FakeDongle] = []

    def plug(**behaviour) -> FakeDongle:
        fakes.append(FakeDongle(**behaviour))
        return fakes[-1]

    monkeypatch.setattr(
        _dongle.list_ports,
        "comports",
        lambda: [type("Port", (), {"serial_number": f.serial_number, "device": f.port}) for f in fakes if f.plugged],
    )
    yield plug
    for fake in fakes:
        fake.unplug()
