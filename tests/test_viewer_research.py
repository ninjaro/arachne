from __future__ import annotations

import sqlite3
import tempfile
import unittest
from pathlib import Path

from viewer.scripts.build_catalog import build_catalog
from viewer.scripts.build_research import build_research


class ViewerResearchTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.database = self.root / "product.sqlite"
        connection = sqlite3.connect(self.database)
        connection.executescript(
            """
            PRAGMA user_version = 5;
            CREATE TABLE entities (
                id TEXT PRIMARY KEY,
                entity_type TEXT NOT NULL
            );
            CREATE TABLE names (
                id INTEGER PRIMARY KEY,
                entity_id TEXT NOT NULL,
                value TEXT NOT NULL,
                is_preferred INTEGER NOT NULL
            );
            CREATE TABLE concepts (
                entity_id TEXT PRIMARY KEY,
                slug TEXT NOT NULL
            );
            CREATE TABLE ingest_issues (
                batch_id TEXT NOT NULL,
                code TEXT NOT NULL,
                json_path TEXT NOT NULL,
                message TEXT NOT NULL,
                value_json TEXT,
                status TEXT NOT NULL,
                PRIMARY KEY (batch_id, code, json_path)
            );
            CREATE TABLE merge_hints (
                entity_type TEXT NOT NULL,
                left_id TEXT NOT NULL,
                right_id TEXT NOT NULL,
                score REAL NOT NULL,
                text_score REAL,
                graph_score REAL,
                context_score REAL,
                signals_json TEXT NOT NULL,
                status TEXT NOT NULL,
                PRIMARY KEY (entity_type, left_id, right_id)
            );
            INSERT INTO entities VALUES
                ('work-000001', 'work'),
                ('work-000002', 'work'),
                ('agent-000001', 'person'),
                ('agent-000002', 'person');
            INSERT INTO names(entity_id, value, is_preferred) VALUES
                ('work-000001', 'The Work', 1),
                ('work-000002', 'Work, The', 1),
                ('agent-000001', 'Open Agent', 1),
                ('agent-000002', 'Ignored Agent', 1);
            INSERT INTO ingest_issues VALUES
                (
                    'research-1', 'unknown_reference', '/create/credits/0',
                    'Unknown canonical agent.', '{"id":"agent-999999"}', 'open'
                ),
                (
                    'research-0', 'old_problem', '/update/works/0',
                    'Already resolved.', NULL, 'resolved'
                );
            INSERT INTO merge_hints VALUES
                (
                    'work', 'work-000001', 'work-000002',
                    0.91, 0.95, 0.5, 1.0,
                    '{"same_year":true}', 'open'
                ),
                (
                    'agent', 'agent-000001', 'agent-000002',
                    0.99, 1.0, 0.8, 1.0,
                    '{"exact_alias":true}', 'ignored'
                );
            """
        )
        connection.commit()
        connection.close()

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def test_exports_open_database_issues_and_hints(self) -> None:
        output = build_research(
            self.database,
            {"formatVersion": 1, "productSnapshotId": "product-1"},
        )

        self.assertEqual(output["productSnapshotId"], "product-1")
        self.assertEqual(
            output["summary"],
            {
                "total": 2,
                "qualityGaps": 0,
                "ingestIssues": 1,
                "mergeHints": 1,
                "problems": 1,
                "weak": 0,
                "info": 1,
            },
        )

        issue, hint = output["items"]
        self.assertEqual(issue["kind"], "ingest_issue")
        self.assertEqual(issue["batchId"], "research-1")
        self.assertEqual(issue["jsonPath"], "/create/credits/0")
        self.assertEqual(issue["value"], {"id": "agent-999999"})

        self.assertEqual(hint["kind"], "merge_hint")
        self.assertEqual(hint["leftLabel"], "The Work")
        self.assertEqual(hint["rightLabel"], "Work, The")
        self.assertEqual(hint["similarityScore"], 0.91)
        self.assertEqual(hint["signals"], {"same_year": True})

    def test_rejects_old_product_schema(self) -> None:
        connection = sqlite3.connect(self.database)
        connection.execute("PRAGMA user_version = 4")
        connection.close()

        with self.assertRaisesRegex(RuntimeError, "requires product schema v5"):
            build_research(
                self.database,
                {"formatVersion": 1, "productSnapshotId": "product-1"},
            )
        with self.assertRaisesRegex(
            RuntimeError, "unsupported product schema version 4"
        ):
            build_catalog(self.database)


if __name__ == "__main__":
    unittest.main()
