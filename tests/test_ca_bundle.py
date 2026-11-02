#!/usr/bin/env python3
"""
test_ca_bundle.py - Unit and regression tests for curated CA bundle updater
"""

import ssl
import sys
import tempfile
import unittest
from pathlib import Path

# Add repo root to path
REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT))

from scripts.update_ca_bundle import (
    parse_authorities,
    parse_certificates,
    build_bundle_text,
    validate_bundle,
)

SAMPLE_CSV = """Owner,Common Name or Certificate Name
Example Org,Root CA Alpha
Example Org,Root CA Beta
"""

SAMPLE_PEM = """
Root CA Alpha
================
# Fingerprint: 00:11:22
-----BEGIN CERTIFICATE-----
MIIBkTCB+wIJALa123456789MAoGCCqGSM49BAMCMBMxETAPBgNVBAMMCEFscGhh
IENBMB4XDTIwMDEwMTAwMDAwMFoXDTQwMDEwMTAwMDAwMFowEzERMA8GA1UEAwwI
QWxwaGEgQ0EwWTATBgcqhkjOPQIBBggqhkjOPQMBBwNCAARq6FkG07yv0XwZq5vU
-----END CERTIFICATE-----

Root CA Ignored
================
-----BEGIN CERTIFICATE-----
MIIBkTCB+wIJALa987654321MAoGCCqGSM49BAMCMBMxETAPBgNVBAMMCElnbm9y
-----END CERTIFICATE-----
"""


class TestCaBundleUpdater(unittest.TestCase):
    def test_parse_authorities(self):
        auths = parse_authorities(SAMPLE_CSV)
        self.assertEqual(auths, {"Root CA Alpha", "Root CA Beta"})
        self.assertNotIn("Common Name or Certificate Name", auths)

    def test_parse_certificates(self):
        auths = {"Root CA Alpha"}
        matched = parse_certificates(SAMPLE_PEM, auths)
        self.assertEqual(len(matched), 1)
        name, pem = matched[0]
        self.assertEqual(name, "Root CA Alpha")
        self.assertTrue(pem.startswith("-----BEGIN CERTIFICATE-----\n"))
        self.assertTrue(pem.endswith("\n-----END CERTIFICATE-----"))

    def test_active_cacert_bundle_validity(self):
        cacert_path = REPO_ROOT / "payload" / "config" / "cacert.pem"
        self.assertTrue(cacert_path.is_file(), "payload/config/cacert.pem must exist")
        content = cacert_path.read_text(encoding="utf-8")
        count = validate_bundle(content)
        self.assertGreaterEqual(count, 30, "Curated bundle must contain at least 30 valid CAs")

    def test_formatting_has_proper_newlines(self):
        cacert_path = REPO_ROOT / "payload" / "config" / "cacert.pem"
        content = cacert_path.read_text(encoding="utf-8")
        # Ensure no malformed BEGIN lines without newlines
        self.assertNotIn("-----BEGIN CERTIFICATE-----M", content)
        self.assertIn("-----BEGIN CERTIFICATE-----\n", content)


if __name__ == "__main__":
    unittest.main()
