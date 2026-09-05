from __future__ import annotations

import gzip
import json
import sqlite3
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts/ingest_provider_dump.py"


class ProviderDumpIngestTests(unittest.TestCase):
    def test_streams_imdb_and_open_library_into_one_graph(self) -> None:
        with tempfile.TemporaryDirectory(prefix="arachne-provider-dump-") as temporary:
            root = Path(temporary)
            graph = root / "observations.sqlite"
            names = root / "name.basics.tsv.gz"
            with gzip.open(names, "wt", encoding="utf-8", newline="") as output:
                output.write(
                    "nconst\tprimaryName\tbirthYear\tdeathYear\tprimaryProfession\n"
                    "nm0000001\tExample Person\t1900\t\\N\tactor\n"
                )
            first = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--graph",
                    str(graph),
                    "--input",
                    str(names),
                    "--create",
                    "--provider",
                    "imdb",
                    "--kind",
                    "name-basics",
                ],
                cwd=ROOT,
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(first.returncode, 0, first.stderr)

            authors = root / "ol_dump_authors.txt.gz"
            author = {
                "key": "/authors/OL1A",
                "name": "Example Author",
                "remote_ids": {"wikidata": "Q42"},
            }
            with gzip.open(authors, "wt", encoding="utf-8", newline="") as output:
                output.write(
                    "/type/author\t/authors/OL1A\t1\t2026-01-01T00:00:00Z\t"
                    + json.dumps(author)
                    + "\n"
                )
            second = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--graph",
                    str(graph),
                    "--input",
                    str(authors),
                    "--provider",
                    "open-library",
                    "--kind",
                    "authors",
                ],
                cwd=ROOT,
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(second.returncode, 0, second.stderr)
            with sqlite3.connect(graph) as connection:
                self.assertEqual(
                    connection.execute(
                        "SELECT count(*) FROM provider_ids WHERE provider IN "
                        "('imdb','open-library','wikidata')"
                    ).fetchone()[0],
                    3,
                )
                self.assertEqual(
                    connection.execute("PRAGMA foreign_key_check").fetchall(), []
                )


if __name__ == "__main__":
    unittest.main()
