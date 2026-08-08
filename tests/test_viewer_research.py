from __future__ import annotations

import hashlib
import json
import sqlite3
import tempfile
import unittest
from pathlib import Path

from viewer.scripts.build_research import build_research


class ViewerResearchTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.database = self.root / "product.sqlite"
        connection = sqlite3.connect(self.database)
        connection.executescript(
            """
            PRAGMA user_version = 6;
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
            """
        )
        connection.commit()
        connection.close()
        self.decisions = self.root / "merge-hint-decisions.json"
        self.decisions.write_text(
            '{"artifact_type":"arachne_merge_hint_decisions_v1",'
            '"format_version":1,"ignored_pairs":[]}\n',
            encoding="utf-8",
        )

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def database_hash(self) -> str:
        return hashlib.sha256(self.database.read_bytes()).hexdigest()

    def catalog(self) -> dict[str, object]:
        return {
            "formatVersion": 1,
            "productSnapshotId": "product-1",
            "databaseSha256": self.database_hash(),
        }

    def write_export(
        self,
        *,
        product_hash: str | None = None,
    ) -> Path:
        export = self.root / "merge-hints-review.json"
        hash_value = product_hash or self.database_hash()
        document: dict[str, object] = {
            "artifactType": "arachne_merge_hint_review_v1",
            "formatVersion": 1,
            "source": {
                "productSha256": hash_value,
                "decisionsSha256": hashlib.sha256(
                    self.decisions.read_bytes()
                ).hexdigest(),
                "ignoredPairCount": 0,
            },
            "items": [
                {
                    "id": "merge-hint:work:work-000001:work-000002",
                    "kind": "merge_hint",
                    "severity": "info",
                    "category": "work_duplicate_candidate",
                    "title": "Possible work duplicate",
                    "message": "same primary artist · exact normalized title",
                    "entityType": "work",
                    "leftId": "work-000001",
                    "leftLabel": "The Work",
                    "rightId": "work-000002",
                    "rightLabel": "Work, The",
                    "similarityScore": 0.91,
                    "textScore": 0.95,
                    "graphScore": 0.5,
                    "contextScore": 1.0,
                    "signals": {"same_year": True},
                }
            ],
        }
        export.write_text(json.dumps(document) + "\n", encoding="utf-8")
        return export

    def test_exports_open_database_issues_and_explicit_hints(self) -> None:
        export = self.write_export()
        output = build_research(
            self.database,
            self.catalog(),
            export,
            self.decisions,
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

    def test_prefers_external_review_export(self) -> None:
        export = self.write_export()

        output = build_research(
            self.database,
            self.catalog(),
            export,
            self.decisions,
        )

        self.assertEqual(output["summary"]["mergeHints"], 1)
        hint = output["items"][1]
        self.assertEqual(hint["entityType"], "work")
        self.assertEqual(hint["similarityScore"], 0.91)
        self.assertEqual(hint["signals"], {"same_year": True})

    def test_requires_explicit_review_export(self) -> None:
        with self.assertRaisesRegex(ValueError, "explicit merge-hint review"):
            build_research(
                self.database,
                self.catalog(),
                self.root / "missing.json",
                self.decisions,
            )

    def test_rejects_stale_review_export(self) -> None:
        export = self.write_export(product_hash="0" * 64)
        with self.assertRaisesRegex(ValueError, "different product database"):
            build_research(
                self.database, self.catalog(), export, self.decisions
            )

    def test_rejects_review_export_for_changed_decisions(self) -> None:
        export = self.write_export()
        self.decisions.write_text(
            '{"artifact_type":"arachne_merge_hint_decisions_v1",'
            '"format_version":1,"ignored_pairs":['
            '{"family":"work","left_id":"work-000001",'
            '"right_id":"work-000002"}]}\n',
            encoding="utf-8",
        )
        with self.assertRaisesRegex(ValueError, "different durable decisions"):
            build_research(
                self.database, self.catalog(), export, self.decisions
            )

    def test_rejects_catalog_for_another_database(self) -> None:
        export = self.write_export()
        catalog = self.catalog()
        catalog["databaseSha256"] = "f" * 64
        with self.assertRaisesRegex(ValueError, "catalog and product database"):
            build_research(self.database, catalog, export, self.decisions)

    def test_rejects_old_product_schema(self) -> None:
        connection = sqlite3.connect(self.database)
        connection.execute("PRAGMA user_version = 5")
        connection.commit()
        connection.close()

        export = self.write_export()
        with self.assertRaisesRegex(RuntimeError, "requires product schema v6"):
            build_research(
                self.database,
                self.catalog(),
                export,
                self.decisions,
            )


if __name__ == "__main__":
    unittest.main()
