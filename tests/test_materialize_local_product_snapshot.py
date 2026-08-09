from __future__ import annotations

import datetime as dt
import hashlib
import json
import re
import sqlite3
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
MATERIALIZER = ROOT / "scripts" / "materialize_local_product_snapshot.py"
CATALOG_BUILDER = ROOT / "viewer" / "scripts" / "build_catalog.py"
SHA256 = re.compile(r"^[0-9a-f]{64}$")
STABLE_ID = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._:-]{0,127}$")


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def create_database(path: Path, *, stable_primary_keys: bool = True) -> None:
    with sqlite3.connect(path) as connection:
        connection.execute("PRAGMA user_version = 6")
        if stable_primary_keys:
            connection.executescript(
                """
                CREATE TABLE zeta(id INTEGER PRIMARY KEY, value TEXT NOT NULL);
                INSERT INTO zeta(id, value) VALUES(2, 'second'), (1, 'first');
                CREATE TABLE alpha(code TEXT PRIMARY KEY, value REAL) WITHOUT ROWID;
                INSERT INTO alpha(code, value) VALUES('b', 2.5), ('a', 1.5);
                """
            )
        else:
            connection.executescript(
                """
                CREATE TABLE unstable(value TEXT NOT NULL);
                INSERT INTO unstable(value) VALUES('example');
                """
            )


class LocalProductSnapshotMaterializerTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(
            prefix="arachne-local-product-snapshot-"
        )
        self.root = Path(self.temporary.name)
        self.database = self.root / "canonical product.sqlite"
        self.graph_store = self.root / "state graphs"
        self.control = self.root / "product-control.json"
        create_database(self.database)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def invoke(
        self,
        *,
        database: Path | None = None,
        graph_store: Path | None = None,
        control: Path | None = None,
    ) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [
                sys.executable,
                str(MATERIALIZER),
                "--database",
                str(database or self.database),
                "--graph-store",
                str(graph_store or self.graph_store),
                "--output-control",
                str(control or self.control),
            ],
            check=False,
            capture_output=True,
            text=True,
            timeout=20,
        )

    def stored_artifact(self, record: dict[str, Any]) -> Path:
        storage_ref = record["storage_ref"]
        self.assertFalse(Path(storage_ref).is_absolute())
        self.assertNotIn("..", Path(storage_ref).parts)
        path = self.graph_store / storage_ref
        self.assertTrue(path.resolve().is_relative_to(self.graph_store.resolve()))
        return path

    def assert_artifact(
        self,
        record: dict[str, Any],
        media_type: str,
    ) -> Path:
        self.assertEqual(
            set(record),
            {"storage_ref", "sha256", "byte_length", "media_type"},
        )
        self.assertRegex(record["sha256"], SHA256)
        self.assertEqual(record["media_type"], media_type)
        path = self.stored_artifact(record)
        self.assertTrue(path.is_file())
        self.assertFalse(path.is_symlink())
        self.assertEqual(record["sha256"], digest(path))
        self.assertEqual(record["byte_length"], path.stat().st_size)
        return path

    def test_materializes_schema_shaped_verified_snapshot(self) -> None:
        result = self.invoke()

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        control = json.loads(self.control.read_text(encoding="utf-8"))
        self.assertEqual(
            set(control),
            {
                "contract",
                "format_version",
                "snapshot_id",
                "run_id",
                "graph_version",
                "content_sha256",
                "database",
                "exports",
                "activated_at",
                "structural_validation",
                "extensions",
            },
        )
        self.assertEqual(control["contract"], "product_graph_snapshot_v1")
        self.assertEqual(control["format_version"], 1)
        self.assertRegex(control["snapshot_id"], STABLE_ID)
        self.assertRegex(control["run_id"], STABLE_ID)
        self.assertEqual(control["graph_version"], "canonical-schema-v6")
        self.assertEqual(control["content_sha256"], digest(self.database))
        dt.datetime.fromisoformat(control["activated_at"].replace("Z", "+00:00"))
        self.assertEqual(
            control["extensions"],
            {
                "org.ninjaro.arachne.hpc": {
                    "source": "tracked-canonical-product"
                }
            },
        )

        database = self.assert_artifact(
            control["database"], "application/vnd.sqlite3"
        )
        self.assertEqual(database.read_bytes(), self.database.read_bytes())
        self.assertEqual(len(control["exports"]), 1)
        self.assertEqual(control["exports"][0]["kind"], "product-jsonl")
        product_export = self.assert_artifact(
            control["exports"][0]["artifact"], "application/x-ndjson"
        )
        validation = control["structural_validation"]
        self.assertEqual(set(validation), {"passed", "report"})
        self.assertIs(validation["passed"], True)
        report = self.assert_artifact(validation["report"], "application/json")
        self.assertEqual(
            json.loads(report.read_text(encoding="utf-8"))["status"], "clean"
        )

        records = [
            json.loads(line)
            for line in product_export.read_text(encoding="utf-8").splitlines()
        ]
        self.assertEqual(records[0]["table"], "__local_product_identity")
        self.assertEqual(
            records[0]["row"],
            {
                "database_sha256": control["content_sha256"],
                "snapshot_id": "local-" + control["content_sha256"][:16],
            },
        )
        self.assertEqual(
            [(record["table"], record["row"]) for record in records[1:]],
            [
                ("alpha", {"code": "a", "value": 1.5}),
                ("alpha", {"code": "b", "value": 2.5}),
                ("zeta", {"id": 1, "value": "first"}),
                ("zeta", {"id": 2, "value": "second"}),
            ],
        )
        snapshot = database.parent
        self.assertEqual(
            {path.name for path in snapshot.iterdir()},
            {
                "graph.sqlite",
                "product.jsonl",
                "snapshot-control.json",
                "structural-validation.json",
            },
        )

    def test_reuses_only_an_identical_content_addressed_snapshot(self) -> None:
        first = self.invoke()
        second_control = self.root / "second-control.json"
        second = self.invoke(control=second_control)

        self.assertEqual(first.returncode, 0, first.stderr)
        self.assertEqual(second.returncode, 0, second.stderr)
        first_document = json.loads(self.control.read_text(encoding="utf-8"))
        second_document = json.loads(second_control.read_text(encoding="utf-8"))
        self.assertEqual(first_document, second_document)
        self.assertFalse(list(self.graph_store.rglob("*.stage-*")))

    def test_rejects_a_tampered_existing_snapshot(self) -> None:
        first = self.invoke()
        self.assertEqual(first.returncode, 0, first.stderr)
        control = json.loads(self.control.read_text(encoding="utf-8"))
        export = self.stored_artifact(control["exports"][0]["artifact"])
        with export.open("a", encoding="utf-8") as stream:
            stream.write('{"table":"zeta","row":{"id":99}}\n')
        second_control = self.root / "second-control.json"

        second = self.invoke(control=second_control)

        self.assertEqual(second.returncode, 2)
        self.assertIn("differs from deterministic materialization", second.stderr)
        self.assertFalse(second_control.exists())

    def test_rejects_symbolic_link_custody_boundaries(self) -> None:
        database_link = self.root / "database-link.sqlite"
        database_link.symlink_to(self.database)
        linked_database = self.invoke(database=database_link)
        self.assertEqual(linked_database.returncode, 2)
        self.assertIn("must not be a symbolic link", linked_database.stderr)

        real_store = self.root / "real-store"
        real_store.mkdir()
        graph_link = self.root / "graph-link"
        graph_link.symlink_to(real_store, target_is_directory=True)
        linked_store = self.invoke(
            graph_store=graph_link,
            control=self.root / "linked-store-control.json",
        )
        self.assertEqual(linked_store.returncode, 2)
        self.assertIn("graph store must not be a symbolic link", linked_store.stderr)

    def test_rejects_a_symbolic_snapshot_subdirectory(self) -> None:
        self.graph_store.mkdir()
        escaped = self.root / "escaped"
        escaped.mkdir()
        (self.graph_store / "product").symlink_to(
            escaped, target_is_directory=True
        )

        result = self.invoke()

        self.assertEqual(result.returncode, 2)
        self.assertIn("snapshot directory must not be a symbolic link", result.stderr)
        self.assertEqual(list(escaped.iterdir()), [])
        self.assertFalse(self.control.exists())

    def test_rejects_uncheckpointed_database_sidecars(self) -> None:
        Path(f"{self.database}-wal").write_bytes(b"uncheckpointed")

        result = self.invoke()

        self.assertEqual(result.returncode, 2)
        self.assertIn("must be checkpointed", result.stderr)
        self.assertFalse(self.control.exists())


class CatalogExportOnlyTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(
            prefix="arachne-catalog-export-only-"
        )
        self.root = Path(self.temporary.name)
        self.database = self.root / "product.sqlite"
        self.output = self.root / "product.jsonl"
        create_database(self.database)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def invoke(
        self, database: Path, output: Path
    ) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [
                sys.executable,
                str(CATALOG_BUILDER),
                str(database),
                "--product-export",
                str(output),
                "--export-only",
            ],
            check=False,
            capture_output=True,
            text=True,
            timeout=20,
        )

    def test_export_only_does_not_require_a_catalog_output(self) -> None:
        result = self.invoke(self.database, self.output)

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue(self.output.is_file())
        self.assertIn("4 product rows exported", result.stdout)

    def test_export_cannot_replace_the_source_database(self) -> None:
        before = self.database.read_bytes()

        result = self.invoke(self.database, self.database)

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("must not replace the source database", result.stderr)
        self.assertEqual(self.database.read_bytes(), before)

    def test_failed_export_preserves_the_previous_output(self) -> None:
        bad_database = self.root / "unstable.sqlite"
        create_database(bad_database, stable_primary_keys=False)
        self.output.write_text("previous\n", encoding="utf-8")

        result = self.invoke(bad_database, self.output)

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("has no stable primary key", result.stderr)
        self.assertEqual(self.output.read_text(encoding="utf-8"), "previous\n")
        self.assertEqual(list(self.root.glob(f".{self.output.name}.*.tmp")), [])


if __name__ == "__main__":
    unittest.main()
