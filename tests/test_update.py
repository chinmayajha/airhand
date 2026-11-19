import struct
import sys

import pytest

from airhand import AirhandError, _update, update
from airhand._update import IMAGE_MAGIC


@pytest.fixture
def image(tmp_path):
    def build(version: tuple[int, int, int]):
        path = tmp_path / "zephyr.signed.bin"
        path.write_bytes(struct.pack("<IIHHII", IMAGE_MAGIC, 0, 32, 0, 0, 0) + struct.pack("<BBHII", *version, 0, 0))
        return path

    return build


def test_firmware_that_predates_updates_names_the_one_time_install(plugged, image):
    fake = plugged(firmware="airhand 0.1.2")
    with pytest.raises(AirhandError, match="airhand-bootloader"):
        update(fake.serial_number, firmware=image((0, 1, 3)))


def test_a_dongle_that_runs_the_shipped_firmware_is_left_alone(plugged, image, monkeypatch):
    monkeypatch.setattr(_update, "shipped_firmware", image((0, 1, 3)).read_bytes)
    fake = plugged(firmware="airhand 0.1.3")
    assert update(fake.serial_number) == "airhand 0.1.3 id=FAKE0001"
    assert ("update",) not in fake.commands


def test_an_install_without_the_update_extra_names_it_before_touching_the_dongle(plugged, image, monkeypatch):
    monkeypatch.setitem(sys.modules, "smpclient", None)
    fake = plugged(firmware="airhand 0.1.3")
    with pytest.raises(AirhandError, match=r"airhand\[update\]"):
        update(fake.serial_number, firmware=image((0, 1, 4)))
    assert ("update",) not in fake.commands


def test_a_file_that_is_not_an_update_image_is_refused(tmp_path):
    path = tmp_path / "zephyr.hex"
    path.write_text(":00000001FF\n")
    with pytest.raises(AirhandError, match="not an airhand update image"):
        update("FAKE0001", firmware=path)
