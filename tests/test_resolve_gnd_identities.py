from __future__ import annotations

import json
import sqlite3
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from scripts.ingest_provider_dump import records_for
from scripts.provider_observation_graph import ObservationGraph
from scripts.resolve_gnd_identities import GndResolutionError, resolve


ROOT = Path(__file__).resolve().parents[1]


class GndIdentityResolverTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="arachne-gnd-")
        self.root = Path(self.temporary.name)
        self.product = self.root / "product.sqlite"
        with sqlite3.connect(self.product) as product:
            product.executescript((ROOT / "schema/product.sql").read_text("utf-8"))
            product.execute("INSERT INTO entities VALUES('agent-000001','person')")
            product.execute(
                "INSERT INTO agents(entity_id,agent_type) VALUES('agent-000001','person')"
            )
            product.execute("INSERT INTO entities VALUES('agent-000002','person')")
            product.execute(
                "INSERT INTO agents(entity_id,agent_type) VALUES('agent-000002','person')"
            )
            for entity, scheme, value in (
                ("agent-000001", "wikidata", "Q5879"),
                ("agent-000002", "viaf", "111111"),
            ):
                product.execute(
                    "INSERT INTO external_ids(entity_id,scheme,value) VALUES(?,?,?)",
                    (entity, scheme, value),
                )
        self.export = self.root / "gnd.jsonl"
        self.export.write_text(
            "\n".join(
                json.dumps(record)
                for record in (
                    {
                        "gnd_id": "118540238",
                        "entity_type": "person",
                        "preferred_name": "Goethe, Johann Wolfgang von",
                        "crosswalks": {"wikidata": "Q5879"},
                        "subjects": [{"gnd_id": "4074195-3", "label": "Lyrik"}],
                    },
                    {
                        "gnd_id": "118607626",
                        "entity_type": "person",
                        "preferred_name": "Unrelated authority record",
                        "crosswalks": {"wikidata": "Q99999999"},
                    },
                    {
                        "gnd_id": "118514768",
                        "entity_type": "person",
                        "preferred_name": "Conflicting record",
                        "crosswalks": {"wikidata": "Q5879", "viaf": "111111"},
                    },
                    {
                        "gnd_id": "118540239",
                        "entity_type": "person",
                        "preferred_name": "",
                        "crosswalks": {"wikidata": "Q5879"},
                    },
                )
            )
            + "\n",
            encoding="utf-8",
        )

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def test_only_already_known_entities_are_resolved(self) -> None:
        output = self.root / "gnd-selection.jsonl"
        report = resolve(self.product, self.export, output)
        self.assertEqual(report["records_read"], 4)
        self.assertEqual(report["resolved"], 1)
        self.assertEqual(report["skipped_unrelated"], 1)
        self.assertEqual(report["matched_by"], {"wikidata": 1})
        self.assertEqual(
            [item["gnd_id"] for item in report["conflicts"]], ["118514768"]
        )
        self.assertEqual([item["gnd_id"] for item in report["rejected"]], ["118540239"])
        selected = [
            json.loads(line) for line in output.read_text(encoding="utf-8").splitlines()
        ]
        self.assertEqual([item["gnd_id"] for item in selected], ["118540238"])

        # The selection is ingestible, and nothing unrelated reaches the graph.
        graph_path = self.root / "graph.sqlite"
        graph = ObservationGraph.create(graph_path)
        graph.ingest("gnd", records_for("gnd", "entities", output))
        with sqlite3.connect(graph_path) as connection:
            self.assertEqual(
                [
                    row[0]
                    for row in connection.execute(
                        "SELECT external_id FROM provider_ids WHERE provider='gnd'"
                    )
                ],
                ["118540238"],
            )
            self.assertEqual(
                connection.execute(
                    "SELECT vocabulary_id FROM provider_signals"
                ).fetchone()[0],
                "gnd:4074195-3",
            )

    def test_resolution_refuses_to_overwrite_and_reports_via_cli(self) -> None:
        output = self.root / "selection.jsonl"
        resolve(self.product, self.export, output)
        with self.assertRaises(GndResolutionError):
            resolve(self.product, self.export, output)
        result = subprocess.run(
            [
                sys.executable,
                str(ROOT / "scripts/resolve_gnd_identities.py"),
                "--product",
                str(self.product),
                "--gnd",
                str(self.export),
                "--output",
                str(self.root / "cli-selection.jsonl"),
            ],
            cwd=ROOT,
            text=True,
            capture_output=True,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout)["resolved"], 1)
        # The product database is only ever read.
        with sqlite3.connect(self.product) as product:
            self.assertEqual(
                product.execute("SELECT count(*) FROM external_ids").fetchone()[0], 2
            )


if __name__ == "__main__":
    unittest.main()
