import shutil
import subprocess
from pathlib import Path

import pytest
from serial import Serial

from hidwire import BUTTONS, KEYS, MEDIA, MODIFIERS, Dongle, HostUnavailable, Refused, Unsupported

FIRMWARE = Path(__file__).resolve().parent.parent / "firmware"
KEYS_REPORT, MOUSE, CONSUMER, POINTER = range(4)
RELEASE = (KEYS_REPORT, 0, 0, 0, 0, 0, 0, 0, 0)


@pytest.fixture(scope="session")
def radio(tmp_path_factory) -> Path:
    compiler = shutil.which("cc")
    if compiler is None:
        pytest.skip("needs a C compiler to build the protocol core for this computer")
    binary = tmp_path_factory.mktemp("radio") / "fake_radio"
    subprocess.run(
        [
            compiler,
            "-std=c11",
            "-D_DEFAULT_SOURCE",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-o",
            str(binary),
            str(FIRMWARE / "tests" / "fake_radio.c"),
            str(FIRMWARE / "src" / "proto.c"),
        ],
        check=True,
    )
    return binary


class Radio:
    def __init__(self, binary: Path, log: Path, host: bool) -> None:
        self.log = log
        log.write_text("")
        self.proc = subprocess.Popen([str(binary), str(log), "1" if host else "0"], stdout=subprocess.PIPE, text=True)
        self.port = self.proc.stdout.readline().strip()

    def reports(self) -> list[tuple]:
        return [tuple(map(int, line.split())) for line in self.log.read_text().splitlines()]

    def clear(self) -> None:
        self.log.write_text("")


@pytest.fixture
def serve(radio, tmp_path):
    started: list[Radio] = []

    def start(host: bool = True) -> Radio:
        started.append(Radio(radio, tmp_path / f"reports{len(started)}.log", host))
        return started[-1]

    yield start
    for running in started:
        running.proc.kill()


def key(mod: int, code: int) -> list[tuple]:
    return [(KEYS_REPORT, mod, 0, code, 0, 0, 0, 0, 0), RELEASE]


def test_pointer_commands_become_reports(serve):
    radio = serve()
    dongle = Dongle(port=radio.port)
    assert dongle.identity == "hidwire 2.2.0 id=HOST"

    dongle.tap(0.5, 0.25)
    assert radio.reports() == [(POINTER, b, 0, 64, 0, 32) for b in (0, 1, 0)]

    radio.clear()
    dongle.tap(0.5, 0.25, button="right", count=2)
    assert [r[1] for r in radio.reports()] == [0, 2, 0, 2, 0]

    radio.clear()
    dongle.hold(0.5, 0.25, 30)
    dongle.move(0.0, 1.0)
    assert [r[1] for r in radio.reports()] == [0, 1, 0, 0]
    assert radio.reports()[-1] == (POINTER, 0, 0, 0, 255, 127)

    radio.clear()
    dongle.swipe((0.0, 0.0), (1.0, 1.0), 30, hold_ms=20)
    swipe = radio.reports()
    assert [r[1] for r in swipe] == [0, 1, 1, 1, 0]
    assert swipe[-1] == (POINTER, 0, 255, 127, 255, 127)

    radio.clear()
    dongle.wheel(-3, 2)
    assert radio.reports() == [(MOUSE, 0, 0, 0, 253, 2)]


def test_keyboard_commands_become_reports(serve):
    radio = serve()
    dongle = Dongle(port=radio.port)

    dongle.type_text('Hi #~"\n')
    assert radio.reports() == key(2, 11) + key(0, 12) + key(0, 44) + key(2, 32) + key(2, 53) + key(2, 52) + key(0, 40)

    radio.clear()
    dongle.key("cmd+space")
    dongle.key("CTRL+a")
    dongle.key("f24")
    dongle.media("home")
    assert radio.reports() == key(8, 44) + key(1, 4) + key(0, 115) + [(CONSUMER, 0x23, 0x02), (CONSUMER, 0, 0)]

    radio.clear()
    dongle.close()
    assert radio.reports() == [RELEASE, (MOUSE, 0, 0, 0, 0, 0), (CONSUMER, 0, 0), (POINTER, 0, 0, 0, 0, 0)]


def test_every_published_name_is_accepted(serve):
    dongle = Dongle(port=serve().port)
    for name in KEYS:
        dongle.key(name)
    for modifier in MODIFIERS:
        dongle.key(f"{modifier}+a")
    for name in MEDIA:
        dongle.media(name)
    for button in BUTTONS:
        dongle.tap(0.5, 0.5, button=button)


@pytest.mark.parametrize(
    "send",
    [lambda d: d.type_text("café"), lambda d: d.key("hyper+a"), lambda d: d.key("f25"), lambda d: d.media("power")],
)
def test_names_outside_the_tables_are_unsupported(serve, send):
    with pytest.raises(Unsupported):
        send(Dongle(port=serve().port))


def test_bad_arguments_are_refused(serve):
    dongle = Dongle(port=serve().port)
    with pytest.raises(Refused) as refused:
        dongle.tap(0.5, 0.5, count=9)
    assert refused.value.reason == "bad args"


def test_status_and_unpair_send_no_input(serve):
    radio = serve()
    dongle = Dongle(port=radio.port)
    assert dongle.status() == {"conn": "yes"}
    dongle.unpair()
    assert radio.reports() == []


def test_raw_lines_keep_working(serve):
    radio = serve()
    with Serial(radio.port, timeout=1) as link:

        def ask(line: str) -> str:
            link.write(f"{line}\n".encode())
            return link.readline().decode().strip()

        assert ask("7 key enter") == "7 ok"
        assert ask("8 result 7") == "8 ok"
        assert ask("9 result 6") == "9 err unknown id"
        assert ask("10 text " + '"' + "a" * 300 + '"') == "10 err line too long"
        assert ask("11 bogus") == "11 err unknown verb"
        assert ask("12 tap 1.5 0.5") == "12 err bad args"
        assert ask('13 text "unterminated') == "13 err bad json"
        radio.clear()
        assert ask("547") == "0 ok"
        assert ask("0 0 4 0 0 0 0 0") == "0 ok"
        assert ask("a 0 100 200") == "0 ok"
        assert radio.reports() == [
            (CONSUMER, 0x23, 0x02),
            (KEYS_REPORT, 0, 0, 4, 0, 0, 0, 0, 0),
            (POINTER, 0, 100, 0, 200, 0),
        ]


def test_without_a_host_nothing_is_sent(serve, fast):
    radio = serve(host=False)
    dongle = Dongle(port=radio.port)
    with pytest.raises(HostUnavailable):
        dongle.tap(0.5, 0.5)
    dongle.close()
    assert radio.reports() == []
