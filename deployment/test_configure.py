#!/usr/bin/env python3
"""Behavioral contracts for deployment settings; no SDK or service required."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from configure import settings, profile_qml, resources


class DeploymentSettings(unittest.TestCase):
    def configured(self):
        return {"FERNWEH_BROKER_ENABLED": "1", "FERNWEH_BROKER_HOST": "broker.example.org",
                "FERNWEH_BROKER_PINS": "a" * 64,
                "FERNWEH_PASSKEY_RP_ID": "ipa.example.org"}

    def test_default_build_has_no_endpoint_trust_or_updates(self):
        self.assertEqual(settings({}), {"BrokerEnabled": False, "BrokerHost": "", "BrokerPort": 29000,
                         "BrokerPins": "", "PasskeyRpId": "", "UpdatesEnabled": False})

    def test_operator_settings_are_normalized(self):
        config = self.configured()
        config.update(FERNWEH_BROKER_HOST=" Broker.Example.ORG ",
                      FERNWEH_BROKER_PORT="29443", FERNWEH_PASSKEY_RP_ID="IPA.EXAMPLE.ORG",
                      FERNWEH_BROKER_PINS="A" * 64 + "," + "b" * 64 + "," + "a" * 64,
                      FERNWEH_UPDATES_ENABLED="1")
        result = settings(config)
        self.assertEqual(result["BrokerHost"], "broker.example.org")
        self.assertEqual(result["BrokerPort"], 29443)
        self.assertEqual(result["BrokerPins"], "a" * 64 + "," + "b" * 64)
        self.assertEqual(result["PasskeyRpId"], "ipa.example.org")
        self.assertTrue(result["UpdatesEnabled"])

    def test_broker_and_trust_must_be_provisioned_together(self):
        for config in ({"FERNWEH_BROKER_ENABLED": "1", "FERNWEH_BROKER_HOST": "broker.example.org"},
                       {"FERNWEH_BROKER_ENABLED": "1", "FERNWEH_BROKER_PINS": "a" * 64}):
            with self.subTest(config=list(config)), self.assertRaises(ValueError):
                settings(config)

    def test_port_bounds_and_non_integer_inputs(self):
        for value in ("0", "65536", "-1", "1.5", "1e3", "NaN", "", " 29000", "1;exit"):
            with self.subTest(value=value), self.assertRaises(ValueError):
                settings({"FERNWEH_BROKER_ENABLED": "1", "FERNWEH_BROKER_PORT": value})
        for value in ("1", "65535"):
            self.assertEqual(settings({"FERNWEH_BROKER_ENABLED": "1", "FERNWEH_BROKER_PORT": value})["BrokerPort"], int(value))

    def test_addresses_cannot_inject_build_commands_or_cpp(self):
        for value in ('x";system("exit")', "$(exit)", "`exit`", "host;exit", "bad host",
                      "https://broker.example.org", "broker.example.org:443", "-broker.example.org"):
            with self.subTest(value=value), self.assertRaises(ValueError):
                settings(dict(self.configured(), FERNWEH_BROKER_HOST=value))

    def test_ipv4_broker_is_allowed_but_ip_passkey_identity_is_not(self):
        self.assertEqual(settings(dict(self.configured(), FERNWEH_BROKER_HOST="192.0.2.10"))[
                         "BrokerHost"], "192.0.2.10")
        for value in ("192.0.2.10", "2001:db8::1", "ipa.example.org.", "ipa..org", "ipa-.org"):
            with self.subTest(value=value), self.assertRaises(ValueError):
                settings(dict(self.configured(), FERNWEH_PASSKEY_RP_ID=value))

    def test_malformed_pin_lists_are_rejected_in_full(self):
        for value in ("a" * 63, "g" * 64, "a" * 64 + ",", "a" * 64 + ",bad"):
            with self.subTest(value=value), self.assertRaises(ValueError):
                settings(dict(self.configured(), FERNWEH_BROKER_PINS=value))

    def test_updates_are_opt_in_and_require_trust(self):
        self.assertFalse(settings(self.configured())["UpdatesEnabled"])
        for config in ({"FERNWEH_BROKER_ENABLED": "1", "FERNWEH_UPDATES_ENABLED": "1"},
                       dict(self.configured(), FERNWEH_UPDATES_ENABLED="true")):
            with self.subTest(config=list(config)), self.assertRaises(ValueError):
                settings(config)

    def test_cli_is_idempotent_and_rejects_invalid_reconfiguration(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "config.h"
            command = [sys.executable, str(Path(__file__).with_name("configure.py")),
                       "--output", str(output)]
            environ = {key: value for key, value in os.environ.items()
                       if not key.startswith("FERNWEH_")}
            subprocess.run(command, env=environ, check=True)
            before = (output.read_bytes(), output.stat().st_mtime_ns)
            subprocess.run(command, env=environ, check=True)
            self.assertEqual((output.read_bytes(), output.stat().st_mtime_ns), before)
            result = subprocess.run(command, env=dict(environ, FERNWEH_BROKER_ENABLED="1", FERNWEH_BROKER_HOST="bad\"host"),
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 2)
            self.assertNotIn('bad"host', result.stderr)
            self.assertEqual(output.read_bytes(), before[0])

    def test_disabling_broker_ignores_existing_studio_environment(self):
        config = dict(self.configured(), FERNWEH_BROKER_ENABLED="0", FERNWEH_UPDATES_ENABLED="1",
                      FERNWEH_BROKER_PORT="invalid", FERNWEH_BROKER_PINS="invalid")
        self.assertEqual(settings(config), settings({}))

    def test_broker_capability_requires_explicit_opt_in(self):
        self.assertTrue(settings({"FERNWEH_BROKER_ENABLED": "1"})["BrokerEnabled"])
        for value in ("true", "yes", "2", "", "0;exit"):
            with self.subTest(value=value), self.assertRaises(ValueError):
                settings({"FERNWEH_BROKER_ENABLED": value})

    def test_qml_selection_is_balanced_and_preserves_common_content(self):
        source = "common\n// @fernweh-broker-begin\noptional\n// @fernweh-broker-end\ncommon2\n"
        self.assertEqual(profile_qml(source, False), "common\ncommon2\n")
        self.assertEqual(profile_qml(source, True), "common\noptional\ncommon2\n")
        for source in ("// @fernweh-broker-begin\n", "// @fernweh-broker-end\n"):
            with self.subTest(source=source), self.assertRaises(ValueError):
                profile_qml(source, False)

    def test_broker_resources_are_removed_when_reconfiguring_same_directory(self):
        import xml.etree.ElementTree as ET
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            def aliases():
                return {e.get("alias") for e in ET.parse(output / "qml.qrc").iter("file")}
            resources(output, True)
            self.assertIn("gui/RemoteView.qml", aliases())
            self.assertIn("backend/fernweh-install-update.ps1", aliases())
            resources(output, False)
            self.assertIn("gui/PcView.qml", aliases())
            self.assertNotIn("gui/RemoteView.qml", aliases())
            self.assertNotIn("backend/fernweh-install-update.ps1", aliases())


if __name__ == "__main__":
    unittest.main()
