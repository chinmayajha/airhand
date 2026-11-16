# hidwire

Drive any device that accepts a Bluetooth keyboard and mouse: phones, tablets, computers and TVs. Nothing is installed on the device. A $10 nRF52840 USB dongle pairs with it as a keyboard, mouse and media remote, and your computer sends it commands over USB serial.

```
your computer ──USB serial──▶ nRF52840 dongle ──Bluetooth LE HID──▶ phone · tablet · computer · TV
```

Use it to automate devices that you cannot or do not want to instrument: real phones without developer mode, locked-down apps, kiosks, set-top boxes. Add a camera over the screen if your automation needs to see.

## What it can send

| Input | Commands |
|---|---|
| Pointer at an exact position | tap, double tap, right click, long press, hover, swipe, drag |
| Scroll wheel | vertical and horizontal |
| Keyboard | text on a US layout, any key with `ctrl` / `shift` / `alt` / `cmd`, F1 to F24, the numpad |
| Media and system keys | home, back, search, volume, mute, play/pause, next, previous, brightness |

Positions are fractions of the screen, `0.0` to `1.0`, so the same script works at any resolution.

## Hardware

- A **Nordic nRF52840 Dongle** (PCA10059). It costs about $10.
- A device with Bluetooth LE that accepts a Bluetooth keyboard and mouse.

## Quick start

1. **Flash the firmware.** Download `hidwire-firmware-<version>.zip` from the [releases](https://github.com/chinmayajha/hidwire/releases). Press the dongle's RESET button (the red LED pulses), then:

   ```sh
   pip install nrfutil
   nrfutil install nrf5sdk-tools
   nrfutil nrf5sdk-tools dfu usb-serial -pkg hidwire-firmware-<version>.zip -p <serial port>
   ```

   After the first flash, `Dongle.enter_bootloader()` restarts the dongle into the bootloader, so you do not need the button again.

2. **Pair.** Open the device's Bluetooth settings and select **hidwire**. Pairing needs no code.

   - **iPhone and iPad:** turn on Settings > Accessibility > Touch > AssistiveTouch to use the pointer.
   - **Android, computers and TVs:** no extra settings.

3. **Send input.**

   ```sh
   pip install hidwire
   ```

   ```python
   from hidwire import Dongle

   with Dongle() as dongle:
       dongle.media("home")
       dongle.tap(0.5, 0.3)
       dongle.type_text("hello\n")
       dongle.swipe((0.5, 0.8), (0.5, 0.3), ms=400)
       dongle.key("cmd+space")
   ```

   `Dongle()` opens the one plugged-in dongle. With several, give a USB serial number from `hidwire.serial_numbers()`: `Dongle("<serial>")`. A dongle found by serial number is found again after a replug. Use `Dongle(port="/dev/ttyACM0")` for a fixed port.

## Python API

| Method | Does |
|---|---|
| `tap(x, y, button="left", count=1)` | Click. `count=2` is a double click; `button` is `left`, `right` or `middle`. |
| `hold(x, y, ms, button="left")` | Press and hold: a long press. |
| `move(x, y)` | Move the pointer without pressing. |
| `swipe(start, end, ms, hold_ms=0)` | Press, move, release. With `hold_ms`, a drag that starts with a long press. |
| `wheel(lines, pan=0)` | Turn the scroll wheel. Positive scrolls up and right. |
| `type_text(text)` | Type text. |
| `key(combo)` | Press a key or a combination, such as `enter`, `a` or `ctrl+shift+t`. |
| `media(name)` | Press a media or system key. |
| `status()` | The link state, such as `{"conn": "yes", "sec": "2", "interval_ms": "45"}`. |
| `unpair()` | Forget every paired device. |
| `enter_bootloader()` | Restart into the USB bootloader to flash new firmware. |

`hidwire.serial_numbers()` lists the plugged-in dongles. `hidwire.KEYS`, `MODIFIERS`, `MEDIA` and `BUTTONS` list every name the firmware accepts.

Errors all derive from `hidwire.HidwireError`:

| Error | Means |
|---|---|
| `HostUnavailable` | No device took the command within 10 s: it is not connected, or never encrypted the link. Its `reason` says which. |
| `DongleLost` | The dongle is not plugged in or stopped answering. The command may or may not have run. |
| `Unsupported` | The input cannot be sent: a character, key, button or position outside what the firmware supports. |
| `Refused` | The firmware refused the command and sent nothing. Its `reason` holds the firmware's words. |
| `WrongFirmware` | The port answers, but not with hidwire firmware of this protocol version. |

The client keeps commands safe across a lost reply. Every request carries an id, and a reply that goes missing is asked for again with `result <id>`, so a tap is never sent twice.

`hidwire.testing.FakeDongle` answers the protocol on a pseudo-terminal (macOS and Linux), so you can test your code without hardware:

```python
from hidwire import Dongle
from hidwire.testing import FakeDongle

with FakeDongle() as fake:
    Dongle(port=fake.port).tap(0.5, 0.5)
    assert fake.commands == [("tap", "0.5000", "0.5000")]
```

## Serial protocol

Every language can use the dongle: it is one line per request on a USB serial port. See [docs/protocol.md](docs/protocol.md).

## Platform notes

| Device | Notes |
|---|---|
| iPhone, iPad | Turn on AssistiveTouch for the pointer. A slow drag can count as a tap: move at least 0.75 screen heights per second. iOS has no Back key. |
| Android | Back, Home and the pointer work with no setup. |
| macOS, Windows, Linux | Use the wheel to scroll; a drag selects text on a computer. |
| TVs and set-top boxes | Use arrow keys, `enter` and the media keys. |

Tested on an iPhone 12 with iOS 26. Results from other devices are welcome as issues or pull requests.

## Build the firmware

The firmware is a Zephyr app for nRF Connect SDK v3.4.1, in [firmware/](firmware/):

```sh
west build -b nrf52840dongle/nrf52840 -d build firmware
nrfutil nrf5sdk-tools pkg generate --hw-version 52 --application-version 1 --sd-req=0x00 \
  --application build/firmware/zephyr/zephyr.hex hidwire-firmware.zip
```

The protocol core, `firmware/src/proto.c`, has no Zephyr dependency. The tests build it for your computer and drive it through the Python client:

```sh
pip install -e . pytest
pytest
```

## License

Apache-2.0. See [LICENSE](LICENSE).

The released firmware image also contains Zephyr (Apache-2.0) and parts of the nRF Connect SDK, such as the Bluetooth LE controller and the HID service, under the [Nordic 5-Clause license](https://github.com/nrfconnect/sdk-nrf/blob/main/LICENSE). Those parts may only be used with Nordic Semiconductor chips.
