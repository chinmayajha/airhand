import pytest

from airhand import Dongle, DongleLost, HostUnavailable, Refused, Unsupported, WrongFirmware, _dongle, serial_numbers


def test_commands_reach_the_dongle_as_fractions(plugged):
    fake = plugged()
    with Dongle(port=fake.port) as dongle:
        dongle.tap(0.5, 0.25)
        dongle.swipe((0.5, 0.8), (0.5, 0.2), 400)
        dongle.key("cmd+space")
        dongle.media("home")
    assert fake.commands == [
        ("tap", "0.5000", "0.2500"),
        ("swipe", "0.5000", "0.8000", "0.5000", "0.2000", "400"),
        ("key", "cmd+space"),
        ("media", "home"),
        ("reset",),
    ]


def test_a_long_string_lands_intact(plugged):
    fake = plugged()
    text = ('pass "quoted" \\ back\nslash ' * 10)[:200]
    Dongle(port=fake.port).type_text(text)
    assert fake.typed() == text


@pytest.mark.parametrize(
    "send", [lambda d: d.type_text("price €5"), lambda d: d.key("hyper+a"), lambda d: d.tap(1.5, 0.5)]
)
def test_input_the_dongle_cannot_send_is_refused(plugged, send):
    fake = plugged()
    with pytest.raises(Unsupported):
        send(Dongle(port=fake.port))


def test_other_firmware_asks_for_a_flash(plugged):
    fake = plugged(firmware="other-firmware 0.1.0")
    with pytest.raises(WrongFirmware, match="Flash it"):
        Dongle(port=fake.port)


@pytest.mark.parametrize(
    ("behaviour", "reason"),
    [({"host_connected": False}, "not connected"), ({"unencrypted_replies": 10**6}, "not secure")],
)
def test_a_host_that_stays_unavailable_is_reported(plugged, behaviour, reason):
    fake = plugged(**behaviour)
    with pytest.raises(HostUnavailable) as unavailable:
        Dongle(port=fake.port).tap(0.1, 0.1)
    assert unavailable.value.reason == reason


def test_a_tap_waits_while_a_reconnected_host_encrypts_the_link(plugged):
    fake = plugged(unencrypted_replies=2)
    Dongle(port=fake.port).tap(0.1, 0.1)
    assert [words[0] for words in fake.commands] == ["tap", "tap", "tap"]


def test_an_echoed_command_is_not_taken_as_its_reply(plugged):
    fake = plugged(echo=True)
    dongle = Dongle(port=fake.port)
    dongle.type_text("hello")
    dongle.tap(0.5, 0.5)
    assert fake.typed() == "hello"


def test_a_lost_reply_is_recovered_without_tapping_twice(plugged):
    fake = plugged(swallow={"tap"})
    Dongle(port=fake.port).tap(0.5, 0.5)
    assert fake.commands == [("tap", "0.5000", "0.5000")]


def test_a_silent_dongle_is_reported_lost(plugged):
    fake = plugged(silent_after=0)
    with pytest.raises(DongleLost):
        Dongle(port=fake.port).tap(0.5, 0.5)


def test_a_replugged_dongle_is_found_again_by_its_serial(plugged):
    first = plugged()
    dongle = Dongle(first.serial_number)
    first.unplug()
    second = plugged()
    dongle.tap(0.5, 0.5)
    assert second.commands == [("tap", "0.5000", "0.5000")]


def test_a_missing_dongle_is_reported_lost(plugged):
    with pytest.raises(DongleLost, match="not plugged in"):
        Dongle("NOSUCH")


def test_other_refusals_keep_their_reason(plugged):
    fake = plugged()
    fake._reply = lambda words: "err partial 1/2 failed -5" if words[0] == "tap" else "ok airhand 0.1.0 id=X"
    with pytest.raises(Refused) as refused:
        Dongle(port=fake.port).tap(0.5, 0.5)
    assert refused.value.reason == "partial 1/2 failed -5"


def test_the_only_plugged_in_dongle_is_found_without_a_serial(plugged):
    with pytest.raises(DongleLost, match="No airhand dongle"):
        Dongle()
    fake = plugged(serial_number="ONLY")
    Dongle().tap(0.5, 0.5)
    assert fake.commands == [("tap", "0.5000", "0.5000")]
    plugged(serial_number="OTHER")
    with pytest.raises(ValueError, match="OTHER, ONLY|ONLY, OTHER"):
        Dongle()


@pytest.mark.parametrize(("left_in", "touch", "expected"), [("pointer", True, "touch"), ("touch", False, "pointer")])
def test_opening_sets_the_mode_whatever_the_last_client_left(plugged, left_in, touch, expected):
    fake = plugged(mode=left_in)
    Dongle(port=fake.port, touch=touch).close()
    assert fake.mode == expected


def test_touch_needs_firmware_that_has_it(plugged):
    fake = plugged(firmware="airhand 0.1.1")
    with pytest.raises(WrongFirmware, match="0.1.2"):
        Dongle(port=fake.port, touch=True)


def test_a_dongle_is_found_where_ports_list_no_product_name(plugged, monkeypatch):
    fake = plugged(serial_number="WIN")

    def windows_port(serial_number, pid):
        return type(
            "Port",
            (),
            {"serial_number": serial_number, "device": fake.port, "product": None, "vid": 0x2FE3, "pid": pid},
        )

    monkeypatch.setattr(
        _dongle.list_ports, "comports", lambda: [windows_port("WIN", 0x0004), windows_port("ZEPHYR", 0x0001)]
    )
    assert serial_numbers() == ["WIN"]
    Dongle().tap(0.5, 0.5)
    assert fake.commands == [("tap", "0.5000", "0.5000")]
