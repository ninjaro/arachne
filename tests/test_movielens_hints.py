from __future__ import annotations

import json
import sqlite3
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from scripts.import_movielens_tag_hints import MovielensImportError, import_hints
from scripts.research_hints import build, work_hints


ROOT = Path(__file__).resolve().parents[1]


class MovielensTagHintTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="arachne-movielens-")
        self.root = Path(self.temporary.name)
        self.product = self.root / "product.sqlite"
        with sqlite3.connect(self.product) as product:
            product.executescript((ROOT / "schema/product.sql").read_text("utf-8"))
            product.execute("INSERT INTO entities VALUES('work-000001','work')")
            product.execute(
                "INSERT INTO works(entity_id,medium) VALUES('work-000001','film')"
            )
            product.execute(
                "INSERT INTO external_ids(entity_id,scheme,value) "
                "VALUES('work-000001','imdb_title','tt0114709')"
            )
        self.links = self.root / "links.csv"
        self.links.write_text(
            "movieId,imdbId,tmdbId\n1,114709,862\n2,999999,99\n", encoding="utf-8"
        )
        self.tags = self.root / "genome-tags.csv"
        self.tags.write_text(
            "tagId,tag\n1,thought-provoking\n2,atmospheric\n3,bad plot\n",
            encoding="utf-8",
        )
        self.scores = self.root / "genome-scores.csv"
        self.scores.write_text(
            "movieId,tagId,relevance\n"
            "1,1,0.93\n"
            "1,2,0.81\n"
            "1,3,0.12\n"
            "2,1,0.99\n",
            encoding="utf-8",
        )

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def test_only_known_works_strong_tags_and_a_bounded_selection_are_kept(self) -> None:
        output = self.root / "movielens.jsonl"
        report = import_hints(
            self.product,
            self.links,
            self.tags,
            self.scores,
            output,
            dataset="ml-25m",
            minimum_relevance=0.7,
            maximum_tags=1,
        )
        self.assertEqual(report["linked_works"], 1)
        self.assertEqual(report["rows_for_unknown_movies"], 1)
        self.assertEqual(report["rows_below_relevance"], 1)
        self.assertEqual(report["signals"], 1)
        self.assertTrue(report["restricted"])
        signals = [
            json.loads(line) for line in output.read_text(encoding="utf-8").splitlines()
        ]
        self.assertEqual(
            signals,
            [
                {
                    "work_id": "work-000001",
                    "kind": "concept",
                    "family": "keyword",
                    "type": "movielens_tag",
                    "value": "thought-provoking",
                    "strength": 0.93,
                    "metadata": {"dataset": "ml-25m", "movielens_movie_id": "1"},
                }
            ],
        )
        with self.assertRaises(MovielensImportError):
            import_hints(
                self.product, self.links, self.tags, self.scores, output,
                dataset="ml-25m",
            )

    def test_import_is_research_only_and_the_build_stays_licence_gated(self) -> None:
        arguments = [
            sys.executable,
            str(ROOT / "scripts/import_movielens_tag_hints.py"),
            "--product", str(self.product),
            "--links", str(self.links),
            "--tags", str(self.tags),
            "--scores", str(self.scores),
            "--output", str(self.root / "signals.jsonl"),
        ]
        refused = subprocess.run(
            arguments, cwd=ROOT, text=True, capture_output=True, check=False
        )
        self.assertEqual(refused.returncode, 2)
        self.assertIn("research-only", refused.stderr)
        accepted = subprocess.run(
            [*arguments, "--acknowledge-research-only"],
            cwd=ROOT, text=True, capture_output=True, check=False,
        )
        self.assertEqual(accepted.returncode, 0, accepted.stderr)
        self.assertEqual(
            json.loads(accepted.stdout)["license"], "GroupLens-Research-NonCommercial"
        )

        graph = self.root / "graph.sqlite"
        from scripts.provider_observation_graph import ObservationGraph

        ObservationGraph.create(graph)
        manual = self.root / "signals.jsonl"
        gated = build(graph, self.product, self.root / "gated.sqlite", manual_path=manual)
        self.assertEqual(
            gated["skipped_signals"]["license_restricted"]["movielens_tag"], 2
        )
        opted_in = self.root / "hints.sqlite"
        build(graph, self.product, opted_in, manual_path=manual,
              allow_restricted=["movielens_tag"])
        hints = work_hints(opted_in, "work-000001")
        self.assertEqual(
            [(hint["value"], hint["quality_class"]) for hint in hints["hints"]],
            [("atmospheric", "D"), ("thought-provoking", "D")],
        )


if __name__ == "__main__":
    unittest.main()
