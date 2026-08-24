from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

from scripts.state_manifest import (
    StateManifestError,
    check,
    refresh,
)


COMMIT = "1" * 40


class StateManifestTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="arachne-state-manifest-")
        self.root = Path(self.temporary.name)
        self.source = self.root / "arachne"
        self.state = self.root / "arachne-data"
        (self.source / "schema").mkdir(parents=True)
        (self.state / "database").mkdir(parents=True)
        (self.source / "schema" / "product.sql").write_text(
            "CREATE TABLE current_schema(id INTEGER PRIMARY KEY);\n",
            encoding="utf-8",
        )
        (self.state / "database" / "art-islands.sqlite").write_bytes(b"product")

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def test_refresh_and_check_bind_distinct_source_and_state_roots(self) -> None:
        written = refresh(self.source, self.state, COMMIT)
        self.assertEqual(check(self.source, self.state), written)
        self.assertEqual(written["product"]["path"], "database/art-islands.sqlite")
        self.assertEqual(written["schema"]["path"], "schema/product.sql")

    def test_product_change_is_rejected(self) -> None:
        refresh(self.source, self.state, COMMIT)
        (self.state / "database" / "art-islands.sqlite").write_bytes(b"new product")
        with self.assertRaisesRegex(StateManifestError, "product bytes"):
            check(self.source, self.state)

    def test_schema_change_is_rejected(self) -> None:
        refresh(self.source, self.state, COMMIT)
        (self.source / "schema" / "product.sql").write_text(
            "CREATE TABLE changed(id INTEGER PRIMARY KEY);\n",
            encoding="utf-8",
        )
        with self.assertRaisesRegex(StateManifestError, "schema"):
            check(self.source, self.state)

    def test_unknown_manifest_field_is_rejected(self) -> None:
        document = refresh(self.source, self.state, COMMIT)
        document["legacy"] = True
        (self.state / "state-manifest.json").write_text(
            json.dumps(document), encoding="utf-8"
        )
        with self.assertRaisesRegex(StateManifestError, "not closed"):
            check(self.source, self.state)


if __name__ == "__main__":
    unittest.main()
