import argparse
import logging
import sys

from airhand._dongle import AirhandError, Dongle
from airhand._update import update


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="airhand")
    commands = parser.add_subparsers(dest="command", required=True)
    install = commands.add_parser("update", help="install this package's firmware on a dongle over USB")
    install.add_argument("--serial", help="the dongle's USB serial number, when several are plugged in")
    install.add_argument("--firmware", help="an update image (zephyr.signed.bin) to install instead")
    listing = commands.add_parser("hosts", help="list the paired hosts and which one is connected")
    listing.add_argument("--serial", help="the dongle's USB serial number, when several are plugged in")
    choose = commands.add_parser("use", help="let only one paired host connect, or 'any' to let them all")
    choose.add_argument("host", help="the host's name or address, or 'any'")
    choose.add_argument("--serial", help="the dongle's USB serial number, when several are plugged in")
    args = parser.parse_args(argv)
    if args.command in {"hosts", "use"}:
        return _hosts(args)
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


def _hosts(args: argparse.Namespace) -> int:
    try:
        with Dongle(args.serial) as dongle:
            if args.command == "use":
                chosen = dongle.use(None if args.host == "any" else args.host)
                print(f"Connected to {chosen.name or chosen.address}" if chosen else "Any paired host may connect")
                return 0
            for host in dongle.hosts():
                marks = ", ".join(m for m, on in (("connected", host.connected), ("chosen", host.chosen)) if on)
                print(f"{host.address}  {host.name or '?'}{f'  ({marks})' if marks else ''}")
    except AirhandError as exc:
        print(exc, file=sys.stderr)
        return 1
    return 0
