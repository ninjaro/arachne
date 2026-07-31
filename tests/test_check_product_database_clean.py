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
            CREATE TABLE merge_hints(status TEXT NOT NULL);
            CREATE TABLE merge_hint_blocks(id INTEGER PRIMARY KEY);
            CREATE TABLE merge_hint_block_members(id INTEGER PRIMARY KEY);
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

    def test_accepts_empty_disposable_state_and_ignored_decisions(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            database = self.database(Path(temporary))
            connection = sqlite3.connect(database)
            connection.execute("INSERT INTO merge_hints VALUES('ignored')")
            connection.commit()
            connection.close()

            result = self.run_guard(database)

            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(json.loads(result.stdout)["status"], "clean")

    def test_rejects_open_hints_and_blocks(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            database = self.database(Path(temporary))
            connection = sqlite3.connect(database)
            connection.execute("INSERT INTO merge_hints VALUES('open')")
            connection.execute("INSERT INTO merge_hint_blocks DEFAULT VALUES")
            connection.execute(
                "INSERT INTO merge_hint_block_members DEFAULT VALUES"
            )
            connection.commit()
            connection.close()

            result = self.run_guard(database)

            self.assertEqual(result.returncode, 3)
            document = json.loads(result.stdout)
            self.assertEqual(document["status"], "dirty")
            self.assertEqual(document["disposable"]["open_hints"], 1)
            self.assertEqual(document["disposable"]["blocks"], 1)
            self.assertEqual(document["disposable"]["block_members"], 1)


if __name__ == "__main__":
    unittest.main()
