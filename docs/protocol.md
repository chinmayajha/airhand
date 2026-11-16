# Serial protocol

The dongle is a USB serial port (CDC ACM; the baud rate does not matter). A client sends one request per line and gets exactly one reply line.

```
request:  <id> <verb> [args]\n
reply:    <id> ok [data]\n
          <id> err <reason>\n
```

- `<id>` is a decimal number of up to 10 digits. A request without an id gets a reply with id `0`.
- A request line can be up to 255 bytes. A longer line gets `err line too long`.
- The dongle also writes event lines, such as `evt connected`, and echo is not guaranteed to be off. A client takes a line as its reply only when it starts with its own id, followed by `ok` or `err`.

## Verbs

Positions are fractions of the screen from `0.0000` to `1.0000`, with `0 0` at the top left. Times are in milliseconds.

| Verb | Arguments | Does |
|---|---|---|
| `version` | | Replies `ok airhand <version> id=<chip id>`. The chip id is the same as the USB serial number. |
| `tap` | `x y [button] [count]` | Moves the pointer, waits 100 ms, then clicks `count` times (1 to 5, default 1). `button` is `left` (default), `right` or `middle`. |
| `hold` | `x y ms [button]` | Moves the pointer, waits 100 ms, presses for `ms` (up to 10000), releases. |
| `move` | `x y` | Moves the pointer with no button pressed. |
| `swipe` | `x1 y1 x2 y2 ms [hold_ms]` | Presses at the start, waits `hold_ms` (up to 10000), moves to the end in steps of 15 ms over `ms`, releases. |
| `wheel` | `lines [pan]` | Turns the wheel by -127 to 127 detents each, one report per detent every 15 ms. Positive `lines` turn it up, away from the user; positive `pan` turns it right. A host with natural scrolling moves the content the other way. |
| `text` | a JSON string | Types it on a US layout. Only ASCII 32 to 126, `\n` and `\t`. |
| `key` | `[modifier+...]key` | Presses and releases. Modifiers: `ctrl`/`control`, `shift`, `alt`/`option`, `cmd`/`command`/`gui`/`meta`/`win`. A key is one typable character, or a name: `enter`/`return`, `escape`/`esc`, `backspace`, `tab`, `space`, `insert`, `delete`, `home`, `end`, `pageup`, `pagedown`, `up`, `down`, `left`, `right`, `capslock`, `menu`/`application`, `printscreen`, `scrolllock`, `pause`, `numlock`, `f1` to `f24`, `numpad0` to `numpad9`, `numpaddivide`, `numpadmultiply`, `numpadminus`, `numpadplus`, `numpadenter`, `numpaddecimal`/`numpadperiod`. Names are not case sensitive. |
| `media` | name | Presses and releases a consumer key: `home`, `back`, `search`, `volup`, `voldown`, `mute`, `playpause`, `next`, `previous`, `stop`, `brightnessup`, `brightnessdown`. |
| `pointer` | `buttons x y` | Sends one raw absolute pointer report; `x` and `y` are 0 to 32767. `a` is a short form. |
| `reset` | | Releases every key and button. Send it before you close the port. |
| `result` | `<id>` | Sends the reply to an earlier request again. The dongle keeps the last 8. |
| `status` | | Replies `ok conn=yes\|no sec=<level> interval_ms=<n> hid_init=<err> uptime_s=<n>`. |
| `unpair` | | Forgets every paired device. |

A line of numbers with no verb is a raw report: 1 number is a consumer usage, 3 numbers a relative mouse report (`buttons dx dy`), 8 numbers a keyboard report.

## Refusals

A command that sends reports does nothing in these cases:

| Reply | Means | What a client does |
|---|---|---|
| `err not connected` | No device is connected. | Retry for a while: a device reconnects by itself after a Bluetooth drop. |
| `err not secure` | A device is connected but has not encrypted the link yet. | Retry: this lasts a moment after every reconnect. If it does not end, pair again. |
| `err unsupported char\|key\|media` | The input is outside the tables above. | Report it; do not retry. |
| `err bad args`, `err bad json`, `err unknown verb`, `err line too long` | The request is malformed. | Report it. |

When a report fails after the first one went out, the reply is `err partial <done>/<total> failed <errno>`, so the client knows the command ran in part. `err busy <errno>` means the first report did not go out.

## A lost reply

When a reply does not arrive in time, ask for it with `result <id>` instead of sending the command again. If that also gets no answer, the link is lost and the command may or may not have run.

## Reports

The dongle is one Bluetooth LE HID device with these input reports:

| ID | Report | Bytes |
|---|---|---|
| 1 | Keyboard | modifiers, reserved, 6 key codes |
| 2 | Relative mouse | buttons, dx, dy, wheel, pan |
| 3 | Consumer control | 16-bit usage |
| 4 | Absolute pointer | buttons, x (16 bits), y (16 bits), 0 to 32767 |

A device caches this layout when it pairs. Firmware that changes it adds a report, so paired devices read the layout again on their next connection.
