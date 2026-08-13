from __future__ import annotations

import hashlib
import json
import tempfile
import unittest
from pathlib import Path

from scripts.resolve_site_bundle import BundleError, verify_bundle


class PublicationAdapterTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.site = Path(self.temporary.name) / "site"
        self.bundle = self.site / "bundles" / "site_test"
        (self.bundle / "data").mkdir(parents=True)
        (self.bundle / "index.html").write_text("<h1>Arachne</h1>\n", encoding="utf-8")
        (self.bundle / "app.js").write_text("export {};\n", encoding="utf-8")
        (self.bundle / "data" / "projection.json").write_text("{}\n", encoding="utf-8")
        self.write_manifest()

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def bundle_identity(self) -> tuple[str, int]:
        identity = ""
        total = 0
        for path in sorted(value for value in self.bundle.rglob("*") if value.is_file()):
            content = path.read_bytes()
            identity += (
                f"{path.relative_to(self.bundle).as_posix()}\n"
                f"{hashlib.sha256(content).hexdigest()}\n"
            )
            total += len(content)
        return hashlib.sha256(identity.encode("utf-8")).hexdigest(), total

    def write_manifest(self, storage_ref: str = "bundles/site_test") -> None:
        digest, byte_length = self.bundle_identity()
        (self.site / "active.json").write_text(
            json.dumps(
                {
                    "contract": "site_bundle_v1",
                    "format_version": 1,
                    "entrypoint": "index.html",
                    "bundle": {
                        "storage_ref": storage_ref,
                        "sha256": digest,
                        "byte_length": byte_length,
                    },
                }
            ),
            encoding="utf-8",
        )

    def test_resolves_verified_content_addressed_bundle(self) -> None:
        self.assertEqual(verify_bundle(self.site), self.bundle.resolve())

    def test_rejects_traversal_and_tampered_content(self) -> None:
        self.write_manifest("../outside")
        with self.assertRaises(BundleError):
            verify_bundle(self.site)

        self.write_manifest()
        (self.bundle / "app.js").write_text("tampered\n", encoding="utf-8")
        with self.assertRaises(BundleError):
            verify_bundle(self.site)

    def test_rejects_symlinked_bundle_content(self) -> None:
        outside = Path(self.temporary.name) / "outside.js"
        outside.write_text("outside\n", encoding="utf-8")
        (self.bundle / "link.js").symlink_to(outside)
        with self.assertRaises(BundleError):
            verify_bundle(self.site)


if __name__ == "__main__":
    unittest.main()
