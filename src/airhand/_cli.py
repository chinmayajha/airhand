import argparse
import logging
import sys

from airhand._dongle import AirhandError
from airhand._update import update


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="airhand")
    commands = parser.add_subparsers(dest="command", required=True)
    install = commands.add_parser("update", help="install this package's firmware on a dongle over USB")
    install.add_argument("--serial", help="the dongle's USB serial number, when several are plugged in")
    install.add_argument("--firmware", help="an update image (zephyr.signed.bin) to install instead")
    args = parser.parse_args(argv)
    logging.getLogger("smpclient").setLevel(logging.ERROR)
    try:
        identity = update(args.serial, firmware=args.firmware, progress=_show)
    except (AirhandError, ValueError) as exc:
        print(exc, file=sys.stderr)
        return 1
    print(f"\r{identity}")
    return 0


def _show(sent: int, total: int) -> None:
    print(f"\rUploading {sent * 100 // total}%", end="", flush=True)
