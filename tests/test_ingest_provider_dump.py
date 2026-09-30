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
                    "--snapshot-id",
                    "2026-09-20",
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
                    "--snapshot-id",
                    "2026-09-30",
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
                self.assertEqual(
                    connection.execute(
                        "SELECT provider,snapshot_id FROM provider_sources ORDER BY provider"
                    ).fetchall(),
                    [("imdb", "2026-09-20"), ("open-library", "2026-09-30")],
                )
            self.assertEqual(
                json.loads(first.stdout)["ingest"]["unpersisted"], {"professions": 1}
            )

    def test_general_then_signal_pass_keeps_only_under_mined_signals(self) -> None:
        with tempfile.TemporaryDirectory(prefix="arachne-provider-dump-") as temporary:
            root = Path(temporary)
            graph = root / "observations.sqlite"
            titles = root / "title.basics.tsv"
            titles.write_text(
                "tconst\ttitleType\tprimaryTitle\tgenres\n"
                "tt0000001\tmovie\tKnown\tFilm-Noir\n"
                "tt0000002\tmovie\tUnknown\tFilm-Noir\n",
                encoding="utf-8",
            )
            product = root / "product.sqlite"
            with sqlite3.connect(product) as connection:
                connection.executescript((ROOT / "schema/product.sql").read_text("utf-8"))
                connection.execute("INSERT INTO entities VALUES('work-000001','work')")
                connection.execute(
                    "INSERT INTO works(entity_id,medium) VALUES('work-000001','film')"
                )
                connection.execute(
                    "INSERT INTO external_ids(entity_id,scheme,value) "
                    "VALUES('work-000001','imdb_title','tt0000001')"
                )
            common = [
                sys.executable, str(SCRIPT), "--graph", str(graph), "--input", str(titles),
                "--provider", "imdb", "--kind", "title-basics", "--snapshot-id", "2026-09-20",
            ]
            general = subprocess.run(
                [*common, "--create", "--signals", "none"],
                cwd=ROOT, text=True, capture_output=True, check=False,
            )
            self.assertEqual(general.returncode, 0, general.stderr)
            self.assertEqual(json.loads(general.stdout)["ingest"]["signals_not_persisted"], 2)
            signals = subprocess.run(
                [*common, "--signals-only-for", str(product)],
                cwd=ROOT, text=True, capture_output=True, check=False,
            )
            self.assertEqual(signals.returncode, 0, signals.stderr)
            self.assertEqual(
                json.loads(signals.stdout)["ingest"],
                {"records": 2, "signals": 1, "signals_not_relevant": 1},
            )
            with sqlite3.connect(graph) as connection:
                self.assertEqual(
                    connection.execute(
                        "SELECT i.external_id,s.provider_category FROM provider_signals s "
                        "JOIN provider_ids i ON i.id=s.subject_provider_id"
                    ).fetchall(),
                    [("tt0000001", "genre")],
                )


if __name__ == "__main__":
    unittest.main()
