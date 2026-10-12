#!/usr/bin/env python3
"""Compare the Feather keys with manually copied TTN session keys, locally."""

from __future__ import annotations

import argparse
import getpass
import hmac
from pathlib import Path
import re
import sys
import warnings

from check_feather_identity import array_key, strip_comments


DEFAULT_FEATHER = Path(__file__).resolve().parents[1] / "Arduino/VoorburgSoundsKit/LoraSettings.h"


def parse_ttn_key(value: str) -> bytes:
    compact = re.sub(r"\s+", "", value)
    if not re.fullmatch(r"[0-9a-fA-F]{32}", compact):
        raise ValueError("Each TTN session key must contain exactly 32 hexadecimal digits.")
    return bytes.fromhex(compact)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--feather", type=Path, default=DEFAULT_FEATHER)
    args = parser.parse_args(argv)
    try:
        source = strip_comments(args.feather.read_text(encoding="utf-8"))
    except (OSError, UnicodeError):
        print("Cannot read the Feather settings header.", file=sys.stderr)
        return 2
    configured = {name: array_key(source, name) for name in ("NWKSKEY", "APPSKEY")}
    if any(value is None for value in configured.values()):
        print("The Feather settings must contain two valid, nonzero 16-byte keys.", file=sys.stderr)
        return 2
    if not sys.stdin.isatty():
        print("Run this checker in an interactive Terminal; key entry must stay hidden.", file=sys.stderr)
        return 2

    print("Copy the current keys from TTN end device lmic-spoorbrug, Session information.")
    print("Paste each key at its prompt and press Return. Input is hidden and never saved.")
    try:
        with warnings.catch_warnings():
            # Refuse getpass's echoing fallback if the terminal cannot hide input.
            warnings.simplefilter("error", getpass.GetPassWarning)
            network = parse_ttn_key(getpass.getpass("TTN Network session key (NwkSKey): "))
            application = parse_ttn_key(getpass.getpass("TTN Application session key (AppSKey): "))
    except ValueError as exc:
        print(str(exc), file=sys.stderr)
        return 2
    except getpass.GetPassWarning:
        print("This terminal cannot hide key entry. Use a normal Terminal window.", file=sys.stderr)
        return 2
    except (EOFError, KeyboardInterrupt):
        print("\nSession comparison cancelled.", file=sys.stderr)
        return 2

    matches = {
        "NwkSKey": hmac.compare_digest(configured["NWKSKEY"], network),
        "AppSKey": hmac.compare_digest(configured["APPSKEY"], application),
    }
    for name, match in matches.items():
        print(f"{name} matches the Feather: {'YES' if match else 'NO'}")
    return 0 if all(matches.values()) else 1


if __name__ == "__main__":
    raise SystemExit(main())
