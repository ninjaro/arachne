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
            product.execute("INSERT INTO entities VALUES('work-000002','work')")
            product.execute(
                "INSERT INTO works(entity_id,medium) VALUES('work-000002','film')"
            )
            product.execute(
                "INSERT INTO external_ids(entity_id,scheme,value) "
                "VALUES('work-000002','tmdb_movie','500')"
            )
        self.dataset = self.root / "ml-25m"
        self.dataset.mkdir()
        (self.dataset / "README.txt").write_text(
            "Summary\n=======\n\nThis dataset (ml-25m) describes 5-star rating and "
            "free-text tagging activity from MovieLens.\n",
            encoding="utf-8",
        )
        self.links = self.dataset / "links.csv"
        # Movie 3's exact IMDb and TMDb links reach two different works.
        self.links.write_text(
            "movieId,imdbId,tmdbId\n1,114709,862\n2,999999,99\n3,114709,500\n",
            encoding="utf-8",
        )
        self.tags = self.dataset / "genome-tags.csv"
        self.tags.write_text(
            "tagId,tag\n1,thought-provoking\n2,atmospheric\n3,bad plot\n",
            encoding="utf-8",
        )
        self.scores = self.dataset / "genome-scores.csv"
        self.scores.write_text(
            "movieId,tagId,relevance\n"
            "1,1,0.93\n"
            "1,2,0.81\n"
            "1,3,0.12\n"
            "2,1,0.99\n"
            "3,1,0.99\n",
            encoding="utf-8",
        )

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def test_only_known_works_strong_tags_and_a_bounded_selection_are_kept(self) -> None:
        output = self.root / "movielens.jsonl"
        report = import_hints(
            self.product,
            self.dataset,
            output,
            dataset="ml-25m",
            minimum_relevance=0.7,
            maximum_tags=1,
        )
        self.assertEqual(report["linked_works"], 1)
        self.assertEqual(
            report["conflicting_links"],
            [
                {
                    "movielens_movie_id": "3",
                    "works": {"imdb_title": "work-000001", "tmdb_movie": "work-000002"},
                }
            ],
        )
        self.assertEqual(report["rows_for_unknown_movies"], 2)
        self.assertEqual(report["rows_below_relevance"], 1)
        # The online top-K kept one descriptor and displaced one.
        self.assertEqual(report["rows_outside_top_k"], 1)
        self.assertEqual(report["signals"], 1)
        self.assertTrue(report["restricted"])
        self.assertEqual(report["dataset"]["id"], "ml-25m")
        self.assertEqual(
            set(report["dataset"]["files"]),
            {"README.txt", "links.csv", "genome-tags.csv", "genome-scores.csv"},
        )
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
                    "category": "tag_genome_descriptor",
                    "type": "movielens_tag",
                    "value": "thought-provoking",
                    "strength": 0.93,
                    "metadata": {
                        "dataset": "ml-25m",
                        "movielens_movie_id": "1",
                        "movielens_tag_id": "1",
                    },
                }
            ],
        )
        with self.assertRaises(MovielensImportError):
            import_hints(self.product, self.dataset, output, dataset="ml-25m")

    def test_dataset_identity_is_verified_not_labelled(self) -> None:
        output = self.root / "other.jsonl"
        with self.assertRaises(MovielensImportError):
            import_hints(self.product, self.dataset, output, dataset="tag-genome-2021")
        (self.dataset / "README.txt").write_text("Tag Genome 2021\n", encoding="utf-8")
        with self.assertRaises(MovielensImportError):
            import_hints(self.product, self.dataset, output)
        (self.dataset / "README.txt").write_text("This dataset (ml-25m)\n", encoding="utf-8")
        self.tags.write_text("item_id,tag\n1,x\n", encoding="utf-8")
        with self.assertRaises(MovielensImportError):
            import_hints(self.product, self.dataset, output)
        self.assertFalse(output.exists())

    def test_import_is_research_only_and_the_build_stays_licence_gated(self) -> None:
        arguments = [
            sys.executable,
            str(ROOT / "scripts/import_movielens_tag_hints.py"),
            "--product", str(self.product),
            "--dataset-dir", str(self.dataset),
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
        report = build(graph, self.product, opted_in, manual_path=manual,
                       allow_restricted=["movielens_tag"])
        self.assertEqual(report["restricted_signals"], {"movielens_tag": 2})
        hints = work_hints(opted_in, "work-000001")
        self.assertEqual(
            [(hint["value"], hint["assignment_quality"]) for hint in hints["hints"]],
            [("atmospheric", "D"), ("thought-provoking", "D")],
        )
        # The artifact itself records that restricted MovieLens data took part.
        with sqlite3.connect(opted_in) as connection:
            manual_info, restricted = connection.execute(
                "SELECT manual_signals_json,restricted_signals_json FROM research_hint_info"
            ).fetchone()
        self.assertEqual(json.loads(manual_info)["datasets"], ["ml-25m"])
        self.assertEqual(json.loads(restricted), {"movielens_tag": 2})


if __name__ == "__main__":
    unittest.main()
