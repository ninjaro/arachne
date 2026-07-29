from __future__ import annotations

import json
from pathlib import Path
import sqlite3
import tempfile
import unittest

from scripts.compact_merge_hints import Selection, compact_copy


class CompactMergeHintsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.database = self.root / "product.sqlite"
        connection = sqlite3.connect(self.database)
        connection.executescript(
            """
            PRAGMA foreign_keys = ON;
            PRAGMA user_version = 5;

            CREATE TABLE entities (
                id TEXT PRIMARY KEY,
                entity_type TEXT NOT NULL
            );
            CREATE TABLE names (
                id INTEGER PRIMARY KEY,
                entity_id TEXT NOT NULL REFERENCES entities(id),
                value TEXT NOT NULL,
                is_preferred INTEGER NOT NULL
            );
            CREATE TABLE concepts (
                entity_id TEXT PRIMARY KEY REFERENCES entities(id),
                slug TEXT NOT NULL
            );
            CREATE TABLE merge_hints (
                entity_type TEXT NOT NULL,
                left_id TEXT NOT NULL REFERENCES entities(id),
                right_id TEXT NOT NULL REFERENCES entities(id),
                score REAL NOT NULL,
                text_score REAL,
                graph_score REAL,
                context_score REAL,
                signals_json TEXT NOT NULL,
                status TEXT NOT NULL,
                PRIMARY KEY (entity_type, left_id, right_id)
            );
            CREATE TABLE merge_hint_blocks (
                id INTEGER PRIMARY KEY,
                entity_type TEXT NOT NULL,
                block_type TEXT NOT NULL,
                block_key TEXT NOT NULL,
                UNIQUE (entity_type, block_type, block_key)
            );
            CREATE TABLE merge_hint_block_members (
                id INTEGER PRIMARY KEY,
                block_id INTEGER NOT NULL
                    REFERENCES merge_hint_blocks(id) ON DELETE CASCADE,
                entity_id TEXT NOT NULL
                    REFERENCES entities(id) ON DELETE CASCADE,
                UNIQUE (entity_id, block_id)
            );

            INSERT INTO entities VALUES
                ('work-000001', 'work'),
                ('work-000002', 'work'),
                ('work-000003', 'work'),
                ('agent-000001', 'person'),
                ('agent-000002', 'person');
            INSERT INTO names(entity_id, value, is_preferred) VALUES
                ('work-000001', 'Alpha', 1),
                ('work-000002', 'Alpha Cut', 1),
                ('work-000003', 'Alpha Edition', 1),
                ('agent-000001', 'One', 1),
                ('agent-000002', 'Two', 1);
            INSERT INTO merge_hints VALUES
                (
                    'work', 'work-000001', 'work-000002',
                    0.95, 0.95, 0.70, 0.80, '{"reason":"a"}', 'open'
                ),
                (
                    'work', 'work-000001', 'work-000003',
                    0.90, 0.90, 0.60, 0.70, '{"reason":"b"}', 'open'
                ),
                (
                    'agent', 'agent-000001', 'agent-000002',
                    0.99, 0.99, 0.90, 0.80, '{"reason":"ignored"}', 'ignored'
                );
            INSERT INTO merge_hint_blocks VALUES
                (1, 'work', 'label_fingerprint', 'alpha'),
                (2, 'work', 'label_trigram', 'alp');
            INSERT INTO merge_hint_block_members(block_id, entity_id) VALUES
                (1, 'work-000001'),
                (1, 'work-000002'),
                (2, 'work-000001'),
                (2, 'work-000003');
            """
        )
        connection.commit()
        connection.close()

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def test_exports_limited_review_and_removes_disposable_state(self) -> None:
        review, before, after = compact_copy(
            self.database,
            Selection(minimum_score=0.65, per_type=10, per_entity=1),
        )

        self.assertEqual(before.open_hints, 2)
        self.assertEqual(before.ignored_hints, 1)
        self.assertEqual(len(review["items"]), 1)
        self.assertEqual(review["items"][0]["leftLabel"], "Alpha")
        self.assertEqual(review["items"][0]["rightLabel"], "Alpha Cut")
        self.assertEqual(review["items"][0]["signals"], {"reason": "a"})
        self.assertEqual(after.open_hints, 0)
        self.assertEqual(after.ignored_hints, 1)
        self.assertEqual(after.blocks, 0)
        self.assertEqual(after.block_members, 0)

        connection = sqlite3.connect(self.database)
        try:
            self.assertEqual(
                connection.execute(
                    "SELECT status FROM merge_hints"
                ).fetchall(),
                [("ignored",)],
            )
            self.assertEqual(
                connection.execute("PRAGMA integrity_check").fetchone()[0],
                "ok",
            )
            self.assertEqual(
                connection.execute("PRAGMA foreign_key_check").fetchall(),
                [],
            )
        finally:
            connection.close()

    def test_review_document_is_json_serializable(self) -> None:
        review, _, _ = compact_copy(
            self.database,
            Selection(minimum_score=0.65, per_type=10, per_entity=5),
        )
        encoded = json.dumps(review, ensure_ascii=True, sort_keys=True)
        self.assertIn("arachne_merge_hint_review_v1", encoded)


if __name__ == "__main__":
    unittest.main()
