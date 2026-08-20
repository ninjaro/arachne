from __future__ import annotations

from pathlib import Path
import json
import sqlite3
import subprocess
import sys
import tempfile
import unittest

from viewer.scripts.build_catalog import build_catalog


ROOT = Path(__file__).resolve().parents[1]
SCHEMA = ROOT / "schema" / "product.sql"


class ProductProjectionCatalogTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="arachne-catalog-current-")
        self.database = Path(self.temporary.name) / "product.sqlite"
        with sqlite3.connect(self.database) as connection:
            connection.executescript(SCHEMA.read_text(encoding="utf-8"))
            connection.executescript(
                """
                INSERT INTO entities(id,entity_type) VALUES
                  ('work-000001','work'),('work-000002','work'),
                  ('manifestation-000001','manifestation'),
                  ('agent-000001','person'),('agent-000002','organization');
                INSERT INTO works(entity_id,medium,date_precision) VALUES
                  ('work-000001','comic','month'),
                  ('work-000002','performance','exact');
                INSERT INTO manifestations(
                  entity_id,work_id,manifestation_type,label
                ) VALUES(
                  'manifestation-000001','work-000001','release','Regional release'
                );
                INSERT INTO agents(entity_id,agent_type) VALUES
                  ('agent-000001','person'),('agent-000002','organization');
                INSERT INTO names(entity_id,name_type,value,is_preferred) VALUES
                  ('work-000001','original','First work',1),
                  ('work-000002','original','Second work',1),
                  ('agent-000001','original','Creator',1),
                  ('agent-000002','original','Distributor',1);
                INSERT INTO credits(entity_id,agent_id,role,importance) VALUES
                  ('work-000001','agent-000001','artist','primary'),
                  ('manifestation-000001','agent-000002','distributor','key');
                INSERT INTO work_memberships(
                  child_work_id,parent_work_id,membership_type,position_text
                ) VALUES('work-000002','work-000001','part_of','Part II');
                INSERT INTO agent_relations(
                  subject_agent_id,relation_type,object_agent_id,period_text,role_text
                ) VALUES(
                  'agent-000001','member_of','agent-000002','late 1990s','artist'
                );
                INSERT INTO events(
                  entity_id,event_type,year_start,date_text,date_precision,place_text
                ) VALUES
                  ('work-000001','created',2001,'May 2001','month','Berlin'),
                  ('manifestation-000001','released',2002,NULL,'year',NULL);
                """
            )

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def test_catalog_separates_work_and_manifestation_credits(self) -> None:
        catalog = build_catalog(self.database)

        self.assertNotIn("databaseUserVersion", catalog)
        works = {work["id"]: work for work in catalog["works"]}
        first = works["work-000001"]
        self.assertEqual([row["role"] for row in first["contributors"]], ["artist"])
        self.assertEqual([row["eventType"] for row in first["events"]], ["created"])
        manifestation = first["manifestations"][0]
        self.assertEqual(
            [row["role"] for row in manifestation["contributors"]],
            ["distributor"],
        )
        self.assertEqual(
            [row["eventType"] for row in manifestation["events"]],
            ["released"],
        )
        self.assertEqual(catalog["workMemberships"][0]["positionText"], "Part II")
        self.assertEqual(catalog["agentRelations"][0]["periodText"], "late 1990s")
        self.assertEqual(len(catalog["events"]), 2)

    def test_catalog_rejects_a_database_missing_current_tables(self) -> None:
        incomplete = Path(self.temporary.name) / "incomplete.sqlite"
        with sqlite3.connect(incomplete) as connection:
            connection.execute("CREATE TABLE entities(id TEXT, entity_type TEXT)")
        with self.assertRaisesRegex(RuntimeError, "missing table"):
            build_catalog(incomplete)

    def test_direct_graph_projection_keeps_manifestation_context(self) -> None:
        output = Path(self.temporary.name) / "projection.json"
        result = subprocess.run(
            [
                sys.executable,
                str(ROOT / "sqlite_to_projection.py"),
                str(self.database),
                "--output",
                str(output),
            ],
            check=False,
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        projection = json.loads(output.read_text(encoding="utf-8"))
        self.assertTrue(projection["edges"])
        for edge in projection["edges"]:
            self.assertIsInstance(edge["provenance"]["explanation"], str)
            self.assertTrue(edge["provenance"]["explanation"])
        nodes = {node["node_id"]: node for node in projection["nodes"]}
        self.assertEqual(
            nodes["manifestation-000001"]["attributes"]["manifestation_type"],
            "release",
        )
        edges = {
            (edge["edge_type"], edge["source"], edge["target"])
            for edge in projection["edges"]
        }
        self.assertIn(
            (
                "manifestation_of",
                "manifestation-000001",
                "work-000001",
            ),
            edges,
        )
        self.assertIn(
            (
                "credit:distributor",
                "agent-000002",
                "manifestation-000001",
            ),
            edges,
        )
        self.assertIn(("membership:part_of", "work-000002", "work-000001"), edges)
        self.assertIn(
            ("event:released", "manifestation-000001", "event:2"), edges
        )


if __name__ == "__main__":
    unittest.main()
