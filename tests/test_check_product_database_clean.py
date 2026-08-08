from __future__ import annotations

import json
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts" / "check_product_database_clean.py"


class ProductDatabaseCleanTests(unittest.TestCase):
    def database(self, root: Path) -> Path:
        path = root / "product.sqlite"
        connection = sqlite3.connect(path)
        connection.executescript(
            """
            PRAGMA user_version = 6;
            CREATE TABLE entities(id TEXT PRIMARY KEY);
            """
        )
        connection.close()
        return path

    def run_guard(self, database: Path) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, str(SCRIPT), str(database)],
            check=False,
            capture_output=True,
            text=True,
        )

    def test_accepts_valid_database_with_free_pages(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            database = self.database(Path(temporary))
            connection = sqlite3.connect(database)
            connection.execute("CREATE TABLE discarded(value BLOB)")
            connection.executemany(
                "INSERT INTO discarded VALUES(zeroblob(4096))",
                (() for _ in range(8)),
            )
            connection.execute("DROP TABLE discarded")
            connection.commit()
            freelist = int(
                connection.execute("PRAGMA freelist_count").fetchone()[0]
            )
            connection.close()

            result = self.run_guard(database)

            self.assertGreater(freelist, 0)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(json.loads(result.stdout)["status"], "clean")

    def test_rejects_disposable_hint_tables_even_when_empty(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            database = self.database(Path(temporary))
            connection = sqlite3.connect(database)
            connection.executescript(
                """
                CREATE TABLE merge_hints(id INTEGER PRIMARY KEY);
                CREATE TABLE merge_hint_blocks(id INTEGER PRIMARY KEY);
                CREATE TABLE merge_hint_block_members(id INTEGER PRIMARY KEY);
                """
            )
            connection.commit()
            connection.close()

            result = self.run_guard(database)

            self.assertEqual(result.returncode, 3)
            document = json.loads(result.stdout)
            self.assertEqual(document["status"], "dirty")
            self.assertEqual(
                document["disposableTables"],
                [
                    "merge_hint_block_members",
                    "merge_hint_blocks",
                    "merge_hints",
                ],
            )

    def test_rejects_wrong_schema_version(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            database = self.database(Path(temporary))
            connection = sqlite3.connect(database)
            connection.execute("PRAGMA user_version = 5")
            connection.close()

            result = self.run_guard(database)

            self.assertEqual(result.returncode, 3)
            document = json.loads(result.stdout)
            self.assertEqual(document["schemaVersion"], 5)
            self.assertEqual(document["expectedSchemaVersion"], 6)

    def test_rejects_foreign_key_errors(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            database = self.database(Path(temporary))
            connection = sqlite3.connect(database)
            connection.executescript(
                """
                CREATE TABLE parent(id INTEGER PRIMARY KEY);
                CREATE TABLE child(
                    id INTEGER PRIMARY KEY,
                    parent_id INTEGER NOT NULL REFERENCES parent(id)
                );
                INSERT INTO child(parent_id) VALUES(99);
                """
            )
            connection.close()

            result = self.run_guard(database)

            self.assertEqual(result.returncode, 3)
            document = json.loads(result.stdout)
            self.assertTrue(document["foreignKeyErrors"])


if __name__ == "__main__":
    unittest.main()
