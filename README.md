# airhand

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

1. <a id="install-the-bootloader"></a>**Install the bootloader, once per dongle.** Download `airhand-bootloader-<version>.zip` from the [releases](https://github.com/chinmayajha/airhand/releases). Press the dongle's RESET button (the red LED pulses), then:

   ```sh
   pip install nrfutil
   nrfutil install nrf5sdk-tools
   nrfutil nrf5sdk-tools dfu usb-serial -pkg airhand-bootloader-<version>.zip -p <serial port>
   ```

   This is the last time you press RESET. The dongle now shows up as **airhand update** and waits for firmware.

2. **Install the firmware.**

   ```sh
   pip install airhand
   airhand update
   ```

   `airhand update` installs the firmware that ships with the package, over USB. Run it again after you upgrade the package.

   On Linux, do this once before `airhand update`: it lets your user open the dongle and keeps ModemManager from probing it. Then plug the dongle in again:

   ```sh
   echo 'SUBSYSTEM=="tty", ATTRS{idVendor}=="2fe3", ATTRS{idProduct}=="0004|0100", TAG+="uaccess", ENV{ID_MM_DEVICE_IGNORE}="1"' | sudo tee /etc/udev/rules.d/70-airhand.rules
   sudo udevadm control --reload-rules
   ```

3. **Pair.** Open the device's Bluetooth settings and select **airhand**. Pairing needs no code.

   - **iPhone and iPad:** turn on Settings > Accessibility > Touch > AssistiveTouch to use the pointer.
   - **Android, computers and TVs:** no extra settings.

4. **Send input.**

   ```python
   from airhand import Dongle

   with Dongle() as dongle:
       dongle.media("home")
       dongle.tap(0.5, 0.3)
       dongle.type_text("hello\n")
       dongle.swipe((0.5, 0.8), (0.5, 0.3), ms=400)
       dongle.key("cmd+space")
   ```

   `Dongle()` opens the one plugged-in dongle. With several, give a USB serial number from `airhand.serial_numbers()`: `Dongle("<serial>")`. A dongle found by serial number is found again after a replug. Use `Dongle(port="/dev/ttyACM0")` for a fixed port. `touch=True` makes the dongle a touch screen; Android needs it.

## Python API

| Method | Does |
|---|---|
| `tap(x, y, button="left", count=1)` | Click. `count=2` is a double click; `button` is `left`, `right` or `middle`. |
| `hold(x, y, ms, button="left")` | Press and hold: a long press. |
| `move(x, y)` | Move the pointer without pressing. |
| `swipe(start, end, ms, hold_ms=0)` | Press, move, release. With `hold_ms`, a drag that starts with a long press. |
| `wheel(lines, pan=0)` | Turn the scroll wheel by detents. Positive turns it up and right; natural scrolling reverses the content. |
| `type_text(text)` | Type text. |
| `key(combo)` | Press a key or a combination, such as `enter`, `a` or `ctrl+shift+t`. |
| `media(name)` | Press a media or system key. |
| `status()` | The link state, such as `{"conn": "yes", "sec": "2", "interval_ms": "45"}`. |
| `unpair()` | Forget every paired device. |

`airhand.serial_numbers()` lists the plugged-in dongles. `airhand.KEYS`, `MODIFIERS`, `MEDIA` and `BUTTONS` list every name the firmware accepts.

Errors all derive from `airhand.AirhandError`:

| Error | Means |
|---|---|
| `HostUnavailable` | No device took the command within 10 s: it is not connected, or never encrypted the link. Its `reason` says which. |
| `DongleLost` | The dongle is not plugged in or stopped answering. The command may or may not have run. |
| `Unsupported` | The input cannot be sent: a character, key, button or position outside what the firmware supports. |
| `Refused` | The firmware refused the command and sent nothing. Its `reason` holds the firmware's words. |
| `WrongFirmware` | The port answers, but not with airhand firmware of the client's series: client 0.1.x needs firmware 0.1.x. `airhand update` installs the matching firmware. |

The client keeps commands safe across a lost reply. Every request carries an id, and a reply that goes missing is asked for again with `result <id>`, so a tap is never sent twice.

`airhand.testing.FakeDongle` answers the protocol on a pseudo-terminal (macOS and Linux), so you can test your code without hardware:

```python
from airhand import Dongle
from airhand.testing import FakeDongle

with FakeDongle() as fake:
    Dongle(port=fake.port).tap(0.5, 0.5)
    assert fake.commands == [("tap", "0.5000", "0.5000")]
```

## Firmware updates

```sh
pip install --upgrade airhand
airhand update
```

The firmware restarts in update mode, receives the new image in a second flash slot, and restarts on it. A dongle that already runs the package's firmware is left alone. In Python, `airhand.update(serial=None, firmware=None, progress=None)` does the same and returns the new identity; give `firmware` to install an image you built.

If an update goes wrong, the dongle keeps firmware that works:

- The new firmware runs on trial. A client's `version` request, which every `Dongle()` sends, keeps it. If no client talks to it within 60 s, or it stops responding for 30 s, the dongle goes back to the firmware it had.
- An upload that stops halfway leaves the old firmware in place. Run `airhand update` again.
- If the dongle never comes back, hold the SW1 button while you plug it in. It starts in update mode, and `airhand update` installs the firmware again.

Paired devices stay paired through an update.

## Serial protocol

Every language can use the dongle: it is one line per request on a USB serial port. See [docs/protocol.md](docs/protocol.md).

## Platform notes

| Device | Notes |
|---|---|
| iPhone, iPad | Turn on AssistiveTouch for the pointer. A slow drag can count as a tap: move at least 0.75 screen heights per second. iOS has no Back key. |
| Android | Open the dongle with `Dongle(touch=True)` (firmware 0.1.2 or later): Android ignores where a mouse points, so taps and swipes go as touches. Back and Home work with no setup. On the OnePlus 15 we tested, `key("meta")` opens the app drawer and `key("meta+tab")` opens Recents. |
| macOS, Windows, Linux | Use the wheel to scroll; a drag selects text on a computer. |
| TVs and set-top boxes | Use arrow keys, `enter` and the media keys. |

Tested on an iPhone 12 with iOS 26 and a OnePlus 15 with Android 17. Results from other devices are welcome as issues or pull requests.

## Build the firmware

The firmware is a Zephyr app for nRF Connect SDK v3.4.1, in [firmware/](firmware/). One build makes both the app and MCUboot, the bootloader that installs updates:

```sh
west build -b nrf52840dongle/nrf52840 -d build firmware
airhand update --firmware build/firmware/zephyr/zephyr.signed.bin
```

The bootloader package for the first install comes from the same build:

```sh
nrfutil nrf5sdk-tools pkg generate --hw-version 52 --application-version 1 --sd-req=0x00 \
  --application build/mcuboot/zephyr/zephyr.hex airhand-bootloader.zip
```

Only MCUboot goes in through the factory bootloader: it checks a CRC of what it flashed on every boot, so it must never hold the slots that MCUboot rewrites. The firmware version is in `firmware/VERSION`; keep it equal to the package version in `pyproject.toml`.

The protocol core, `firmware/src/proto.c`, has no Zephyr dependency. The tests build it for your computer and drive it through the Python client:

```sh
pip install -e . pytest
pytest
```

## License

Apache-2.0. See [LICENSE](LICENSE).

The released firmware, in the release assets and in the Python package, also contains Zephyr (Apache-2.0) and parts of the nRF Connect SDK, such as the Bluetooth LE controller and the HID service, under the [Nordic 5-Clause license](https://github.com/nrfconnect/sdk-nrf/blob/main/LICENSE). Those parts may only be used with Nordic Semiconductor chips.
