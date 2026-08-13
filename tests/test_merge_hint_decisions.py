from __future__ import annotations

from contextlib import closing
import json
from pathlib import Path
import sqlite3
import tempfile
import unittest

from scripts.validate_repository import (
    CheckFailure,
    check_merge_hint_decision_references,
    check_merge_hint_decisions,
)


class MergeHintDecisionArtifactTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(
            prefix="arachne-merge-hint-decisions-"
        )
        self.root = Path(self.temporary.name)
        (self.root / "database").mkdir()
        self.path = self.root / "database" / "merge-hint-decisions.json"

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def write(self, pairs: list[dict[str, str]]) -> None:
        self.path.write_text(
            json.dumps(
                {
                    "artifact_type": "arachne_merge_hint_decisions_v1",
                    "format_version": 1,
                    "ignored_pairs": pairs,
                }
            ),
            encoding="utf-8",
        )

    def create_database(
        self,
        *,
        agents: tuple[str, ...] = (),
        works: tuple[str, ...] = (),
        concepts: tuple[str, ...] = (),
    ) -> None:
        with closing(
            sqlite3.connect(self.root / "database" / "art-islands.sqlite")
        ) as connection:
            with connection:
                connection.executescript(
                    """
                    CREATE TABLE agents(entity_id TEXT PRIMARY KEY);
                    CREATE TABLE works(entity_id TEXT PRIMARY KEY);
                    CREATE TABLE concepts(entity_id TEXT PRIMARY KEY);
                    """
                )
                connection.executemany(
                    "INSERT INTO agents(entity_id) VALUES (?)",
                    ((entity_id,) for entity_id in agents),
                )
                connection.executemany(
                    "INSERT INTO works(entity_id) VALUES (?)",
                    ((entity_id,) for entity_id in works),
                )
                connection.executemany(
                    "INSERT INTO concepts(entity_id) VALUES (?)",
                    ((entity_id,) for entity_id in concepts),
                )

    def test_accepts_canonical_ignored_pairs(self) -> None:
        self.write(
            [
                {
                    "family": "agent",
                    "left_id": "agent-000001",
                    "right_id": "agent-000002",
                }
            ]
        )
        check_merge_hint_decisions(self.root)

    def test_rejects_duplicate_or_unsorted_pairs(self) -> None:
        pair = {
            "family": "work",
            "left_id": "work-000001",
            "right_id": "work-000002",
        }
        self.write([pair, pair])
        with self.assertRaises(CheckFailure):
            check_merge_hint_decisions(self.root)

    def test_accepts_pairs_referencing_the_declared_database_family(self) -> None:
        self.write(
            [
                {
                    "family": "agent",
                    "left_id": "agent-000001",
                    "right_id": "agent-000002",
                },
                {
                    "family": "concept",
                    "left_id": "concept-000001",
                    "right_id": "concept-000002",
                },
                {
                    "family": "work",
                    "left_id": "work-000001",
                    "right_id": "work-000002",
                },
            ]
        )
        self.create_database(
            agents=("agent-000001", "agent-000002"),
            concepts=("concept-000001", "concept-000002"),
            works=("work-000001", "work-000002"),
        )

        check_merge_hint_decision_references(self.root)

    def test_rejects_missing_canonical_identifier(self) -> None:
        self.write(
            [
                {
                    "family": "work",
                    "left_id": "work-000001",
                    "right_id": "work-999999",
                }
            ]
        )
        self.create_database(works=("work-000001",))

        with self.assertRaisesRegex(
            CheckFailure,
            r"ignored_pairs\[0\]\.right_id.*canonical work: work-999999",
        ):
            check_merge_hint_decision_references(self.root)

    def test_rejects_identifier_from_a_different_family(self) -> None:
        self.write(
            [
                {
                    "family": "work",
                    "left_id": "agent-000001",
                    "right_id": "agent-000002",
                }
            ]
        )
        self.create_database(agents=("agent-000001", "agent-000002"))

        with self.assertRaisesRegex(
            CheckFailure,
            r"ignored_pairs\[0\]\.left_id.*canonical work: agent-000001",
        ):
            check_merge_hint_decision_references(self.root)


if __name__ == "__main__":
    unittest.main()
