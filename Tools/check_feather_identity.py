#!/usr/bin/env python3
"""Check local Feather/Pico credentials without printing credential values."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path
import re
from typing import Optional


DEFAULT_FEATHER = Path(
    "/Users/frits/Documents/Codex/amsterdam-sounds-kit/Arduino/VoorburgSoundsKit/LoraSettings.h"
)
DEFAULT_PICO = Path(
    "/Users/frits/Documents/Codex/Voorburg-sounds-kit/Arduino/VoorburgSoundsKit/LoraSettings.h"
)

PICO_FIELDS = {
    "NWKSKEY": ("LORAWAN_NWKSKEY_HEX", "LORAWAN_NWK_SKEY_HEX"),
    "APPSKEY": ("LORAWAN_APPSKEY_HEX", "LORAWAN_APP_SKEY_HEX"),
    "DEVADDR": ("LORAWAN_DEVADDR_HEX", "LORAWAN_DEV_ADDR"),
}


@dataclass(frozen=True)
class FieldResult:
    name: str
    configured: bool
    unique: bool


def strip_comments(source: str) -> str:
    """Remove C/C++ comments while preserving strings and line boundaries."""
    tokens = re.compile(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*[\s\S]*?\*/')

    def replace(match: re.Match[str]) -> str:
        token = match.group(0)
        if token.startswith(("//", "/*")):
            return "".join("\n" if c == "\n" else " " for c in token)
        return token

    return tokens.sub(replace, source)


def numeric_literal(value: str) -> Optional[int]:
    """Accept an integer literal only, never execute a C expression."""
    match = re.fullmatch(r"\s*(0[xX][0-9a-fA-F]+|[0-9]+)(?:[uU](?:[lL]{1,2})?|[lL]{1,2}[uU]?)?\s*", value)
    if not match:
        return None
    token = match.group(1)
    # C literals with a leading zero are octal. Avoid a different interpretation.
    if token.lower().startswith("0x"):
        return int(token, 16)
    if len(token) > 1 and token.startswith("0"):
        return int(token, 8) if re.fullmatch(r"[0-7]+", token) else None
    return int(token, 10)


def array_key(source: str, name: str) -> Optional[bytes]:
    # PROGMEM can precede the type/name or follow the array declarator.
    pattern = rf"\b{re.escape(name)}\s*\[\s*(\d*)\s*\]\s*(?:PROGMEM\s*)?=\s*\{{([^{{}}]*)\}}\s*;"
    matches = list(re.finditer(pattern, source))
    if len(matches) != 1:
        return None
    size, body = matches[0].groups()
    if size and int(size) != 16:
        return None
    parts = body.strip().split(",")
    if parts[-1].strip() == "":
        parts.pop()  # A single trailing comma is legal in an initializer.
    values = [numeric_literal(part) for part in parts]
    if len(values) != 16 or any(v is None or not 0 <= v <= 255 for v in values):
        return None
    key = bytes(values)
    return key if any(key) else None


def integer_address(source: str, name: str) -> Optional[int]:
    matches = list(re.finditer(rf"\b{re.escape(name)}\s*=\s*([^;]+);", source))
    if len(matches) != 1:
        return None
    value = numeric_literal(matches[0].group(1))
    return value if value is not None and 0 < value <= 0xFFFFFFFF else None


def pico_hex(source: str, aliases: tuple[str, ...], byte_count: int) -> Optional[bytes]:
    declarations = []
    for name in aliases:
        # Accept an array constant or a macro, but only literal hexadecimal text.
        declarations.extend(re.finditer(
            rf'\b{re.escape(name)}\s*\[\s*(?:\d+)?\s*\]\s*=\s*"([^"\n]*)"\s*;', source
        ))
        declarations.extend(re.finditer(
            rf'(?m)^\s*#\s*define\s+{re.escape(name)}\s+"([^"\n]*)"\s*$', source
        ))
    if len(declarations) == 1:
        value = declarations[0].group(1)
        if not re.fullmatch(rf"[0-9a-fA-F]{{{byte_count * 2}}}", value):
            return None
        decoded = bytes.fromhex(value)
        return decoded if any(decoded) else None
    if not declarations and byte_count == 4:
        # The compatibility DEV_ADDR spelling can be an integer constant.
        values = [integer_address(source, name) for name in aliases]
        values = [value for value in values if value is not None]
        if len(values) == 1:
            return values[0].to_bytes(4, "big")
    return None


def evaluate(feather_source: str, pico_source: str) -> list[FieldResult]:
    feather = strip_comments(feather_source)
    pico = strip_comments(pico_source)
    feather_values = {
        "NWKSKEY": array_key(feather, "NWKSKEY"),
        "APPSKEY": array_key(feather, "APPSKEY"),
        "DEVADDR": integer_address(feather, "DEVADDR"),
    }
    pico_values = {
        "NWKSKEY": pico_hex(pico, PICO_FIELDS["NWKSKEY"], 16),
        "APPSKEY": pico_hex(pico, PICO_FIELDS["APPSKEY"], 16),
        "DEVADDR": pico_hex(pico, PICO_FIELDS["DEVADDR"], 4),
    }
    pico_address = pico_values["DEVADDR"]
    if pico_address is not None:
        pico_values["DEVADDR"] = int.from_bytes(pico_address, "big")

    results = []
    for board, values, other in (
        ("Feather", feather_values, pico_values),
        ("Pico", pico_values, feather_values),
    ):
        for name in ("NWKSKEY", "APPSKEY", "DEVADDR"):
            value = values[name]
            if name == "DEVADDR":
                unique = value is not None and other[name] is not None and value != other[name]
            else:
                comparisons = [other["NWKSKEY"], other["APPSKEY"]]
                if board == "Feather":
                    comparisons.append(values["APPSKEY" if name == "NWKSKEY" else "NWKSKEY"])
                unique = value is not None and all(v is not None and value != v for v in comparisons)
            field_name = name if board == "Feather" else PICO_FIELDS[name][0]
            results.append(FieldResult(f"{board}.{field_name}", value is not None, unique))
    return results


def read_local(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except (OSError, UnicodeError):
        return ""


def main(argv: Optional[list[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--feather", type=Path, default=DEFAULT_FEATHER)
    parser.add_argument("--pico", type=Path, default=DEFAULT_PICO)
    args = parser.parse_args(argv)
    results = evaluate(read_local(args.feather), read_local(args.pico))
    for result in results:
        print(f"{result.name} configured: {'success' if result.configured else 'failure'}")
        print(f"{result.name} unique: {'success' if result.unique else 'failure'}")
    return 0 if all(result.configured and result.unique for result in results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
