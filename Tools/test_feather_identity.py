#!/usr/bin/env python3
"""Synthetic fixtures for the local, value-redacting identity preflight."""

import contextlib
import io
from pathlib import Path
import tempfile
import unittest

import check_feather_identity as checker


def feather(network=0x11, application=0x22, address="0x260B1234", size=16):
    return (
        "static const PROGMEM u1_t NWKSKEY[] = {"
        + ", ".join(f"0x{network:02X}" for _ in range(size))
        + "};\nstatic const u1_t PROGMEM APPSKEY[16] = {"
        + ", ".join(f"0x{application:02X}" for _ in range(16))
        + f"}};\nstatic const u4_t DEVADDR = {address};\n"
    )


def pico(network=0x33, application=0x44, address="260BAAAA"):
    network_hex = f"{network:02x}" * 16
    application_hex = f"{application:02x}" * 16
    return (
        f'static const char LORAWAN_NWKSKEY_HEX[] = "{network_hex}";\n'
        f'static const char LORAWAN_APPSKEY_HEX[] = "{application_hex}";\n'
        f'static const char LORAWAN_DEVADDR_HEX[] = "{address}";\n'
    )


class IdentityTests(unittest.TestCase):
    def check(self, feather_text=None, pico_text=None):
        results = checker.evaluate(
            feather() if feather_text is None else feather_text,
            pico() if pico_text is None else pico_text,
        )
        return {result.name: result for result in results}

    def test_configured_and_distinct(self):
        self.assertTrue(all(r.configured and r.unique for r in self.check().values()))

    def test_zero_and_missing_fields_fail(self):
        for source, name in (
            (feather(network=0), "Feather.NWKSKEY"),
            (feather(application=0), "Feather.APPSKEY"),
            (feather(address="0"), "Feather.DEVADDR"),
            ("", "Feather.NWKSKEY"),
        ):
            with self.subTest(name=name, source=bool(source)):
                self.assertFalse(self.check(feather_text=source)[name].configured)
        for source, name in (
            (pico(network=0), "Pico.LORAWAN_NWKSKEY_HEX"),
            (pico(application=0), "Pico.LORAWAN_APPSKEY_HEX"),
            (pico(address="00000000"), "Pico.LORAWAN_DEVADDR_HEX"),
            ("", "Pico.LORAWAN_NWKSKEY_HEX"),
        ):
            with self.subTest(name=name, source=bool(source)):
                self.assertFalse(self.check(pico_text=source)[name].configured)

    def test_every_cross_device_key_collision(self):
        for field, network, application in (
            ("NWKSKEY", 0x11, 0x44),
            ("APPSKEY", 0x33, 0x22),
            ("NWKSKEY", 0x33, 0x11),
            ("APPSKEY", 0x22, 0x44),
        ):
            with self.subTest(field=field, network=network, application=application):
                results = self.check(pico_text=pico(network=network, application=application))
                self.assertTrue(all(r.configured for r in results.values()))
                self.assertFalse(results[f"Feather.{field}"].unique)

    def test_feather_keys_must_differ(self):
        results = self.check(feather_text=feather(application=0x11))
        self.assertFalse(results["Feather.NWKSKEY"].unique)
        self.assertFalse(results["Feather.APPSKEY"].unique)

    def test_address_collision_ignores_hex_case(self):
        results = self.check(feather_text=feather(address="0x260bAAAAUL"), pico_text=pico(address="260Baaaa"))
        self.assertTrue(results["Feather.DEVADDR"].configured)
        self.assertFalse(results["Feather.DEVADDR"].unique)
        self.assertFalse(results["Pico.LORAWAN_DEVADDR_HEX"].unique)

    def test_malformed_lengths_and_literals_fail(self):
        for size in (15, 17):
            self.assertFalse(self.check(feather_text=feather(size=size))["Feather.NWKSKEY"].configured)
        for value in ("GG" * 16, "33" * 15, "33" * 17):
            text = pico().replace("33" * 16, value)
            self.assertFalse(self.check(pico_text=text)["Pico.LORAWAN_NWKSKEY_HEX"].configured)
        self.assertFalse(self.check(feather_text=feather().replace("0x11", "0x100", 1))["Feather.NWKSKEY"].configured)
        self.assertFalse(self.check(feather_text=feather().replace("0x11", "KEY_MACRO", 1))["Feather.NWKSKEY"].configured)
        self.assertFalse(self.check(pico_text=pico(address="260B123"))["Pico.LORAWAN_DEVADDR_HEX"].configured)

    def test_comments_do_not_supply_or_duplicate_credentials(self):
        self.assertTrue(all(r.configured and r.unique for r in self.check(
            feather_text="/* " + feather(network=0) + " */\n" + feather(),
            pico_text="// " + pico(network=0).replace("\n", "\n// ") + "\n" + pico(),
        ).values()))
        self.assertFalse(self.check(feather_text="/* " + feather() + " */")["Feather.NWKSKEY"].configured)
        self.assertFalse(self.check(pico_text="// " + pico().replace("\n", "\n// "))["Pico.LORAWAN_NWKSKEY_HEX"].configured)
        text = feather().replace("0x11,", "0x11, /* ignore 0x00 */", 1)
        self.assertTrue(self.check(feather_text=text)["Feather.NWKSKEY"].configured)

    def test_progmem_after_array_and_trailing_comma(self):
        text = feather().replace("NWKSKEY[] =", "NWKSKEY[16] PROGMEM =").replace("0x11};", "0x11,};")
        self.assertTrue(self.check(feather_text=text)["Feather.NWKSKEY"].configured)

    def test_aliases_and_numeric_pico_address(self):
        text = pico().replace("LORAWAN_NWKSKEY_HEX", "LORAWAN_NWK_SKEY_HEX").replace("LORAWAN_APPSKEY_HEX", "LORAWAN_APP_SKEY_HEX")
        text = text.replace('static const char LORAWAN_DEVADDR_HEX[] = "260BAAAA";', "static const uint32_t LORAWAN_DEV_ADDR = 0x260BAAAA;")
        self.assertTrue(all(r.configured and r.unique for r in self.check(pico_text=text).values()))

    def test_duplicate_declarations_fail(self):
        self.assertFalse(self.check(feather_text=feather() + feather())["Feather.NWKSKEY"].configured)
        self.assertFalse(self.check(pico_text=pico() + pico())["Pico.LORAWAN_NWKSKEY_HEX"].configured)

    def test_cli_custom_paths_status_and_redacted_output(self):
        with tempfile.TemporaryDirectory() as directory:
            feather_path = Path(directory) / "feather.h"
            pico_path = Path(directory) / "pico.h"
            feather_path.write_text(feather())
            pico_path.write_text(pico())
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                self.assertEqual(checker.main(["--feather", str(feather_path), "--pico", str(pico_path)]), 0)
            lines = output.getvalue().splitlines()
            self.assertEqual(len(lines), 12)
            for line in lines:
                self.assertRegex(line, r"^(Feather\.(NWKSKEY|APPSKEY|DEVADDR)|Pico\.LORAWAN_(NWKSKEY_HEX|APPSKEY_HEX|DEVADDR_HEX)) (configured|unique): success$")
            pico_path.write_text(pico(network=0x11))
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(checker.main(["--feather", str(feather_path), "--pico", str(pico_path)]), 1)
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(checker.main(["--feather", str(feather_path), "--pico", str(pico_path / "missing")]), 1)


if __name__ == "__main__":
    unittest.main()
