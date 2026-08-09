from __future__ import annotations

from contextlib import closing
import os
from pathlib import Path
import json
import sqlite3
import tempfile
import unittest

from scripts.migrate_product_v5_to_v6 import (
    MigrationError,
    TABLE_COPIES,
    V6_INDEXES,
    V6_TABLES,
    V6_TRIGGERS,
    _select_sql,
    _validate_structure,
    migrate_database,
)
from viewer.scripts.build_catalog import build_catalog


ROOT = Path(__file__).resolve().parents[1]
SCHEMA_V5 = ROOT / "schema" / "product_v5.sql"
SCHEMA_V6 = ROOT / "schema" / "product_v6.sql"


def immutable_connection(path: Path) -> sqlite3.Connection:
    return sqlite3.connect(
        path.resolve().as_uri() + "?mode=ro&immutable=1",
        uri=True,
    )


def schema_names(connection: sqlite3.Connection, kind: str) -> set[str]:
    suffix = " AND name NOT LIKE 'sqlite_%'" if kind != "trigger" else ""
    return {
        str(row[0])
        for row in connection.execute(
            f"SELECT name FROM sqlite_schema WHERE type=?{suffix}",
            (kind,),
        )
    }


def durable_snapshot(
    connection: sqlite3.Connection,
) -> dict[str, list[tuple[object, ...]]]:
    return {
        copy.name: [tuple(row) for row in connection.execute(_select_sql(copy))]
        for copy in TABLE_COPIES
    }


class ProductV6MigrationTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(
            prefix="arachne-product-v6-test-"
        )
        self.directory = Path(self.temporary.name)
        self.database = self.directory / "product.sqlite"

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def staging_files(self) -> list[Path]:
        return list(self.directory.glob(f".{self.database.name}.v6-*"))

    def create_v5_fixture(self) -> None:
        connection = sqlite3.connect(self.database)
        try:
            connection.execute("PRAGMA foreign_keys = ON")
            connection.executescript(SCHEMA_V5.read_text(encoding="utf-8"))
            connection.executemany(
                "INSERT INTO entities(id, entity_type) VALUES (?, ?)",
                (
                    ("work-000001", "work"),
                    ("manifestation-000001", "manifestation"),
                    ("agent-000001", "person"),
                    ("concept-000001", "concept"),
                    ("concept-000002", "concept"),
                ),
            )
            connection.execute(
                """
                INSERT INTO works(
                    entity_id, medium, year_start, year_end, date_precision,
                    date_start_text, date_end_text, date_qualifier,
                    language_code, country_code, production_info_json
                ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
                """,
                (
                    "work-000001",
                    "film",
                    1977,
                    1978,
                    "range",
                    "1977",
                    "1978",
                    "circa",
                    "en",
                    "US",
                    '{"studio":"Example"}',
                ),
            )
            connection.execute(
                """
                INSERT INTO manifestations(
                    entity_id, work_id, manifestation_type, release_year,
                    region_code, language_code, label
                ) VALUES (?, ?, ?, ?, ?, ?, ?)
                """,
                (
                    "manifestation-000001",
                    "work-000001",
                    "release",
                    1978,
                    "DE",
                    "de",
                    "German release",
                ),
            )
            connection.executemany(
                """
                INSERT INTO names(
                    id, entity_id, name_type, language_code, script_code,
                    value, is_preferred
                ) VALUES (?, ?, ?, ?, ?, ?, ?)
                """,
                (
                    (
                        101,
                        "work-000001",
                        "english",
                        "en",
                        "Latn",
                        "Example Work",
                        1,
                    ),
                    (
                        102,
                        "agent-000001",
                        "original",
                        None,
                        "Latn",
                        "Example Agent",
                        1,
                    ),
                ),
            )
            connection.executemany(
                """
                INSERT INTO external_ids(
                    id, entity_id, scheme, value, canonical_url
                ) VALUES (?, ?, ?, ?, ?)
                """,
                (
                    (
                        201,
                        "work-000001",
                        "wikidata",
                        "Q123",
                        "https://www.wikidata.org/wiki/Q123",
                    ),
                    (
                        202,
                        "agent-000001",
                        "wikidata",
                        "Q456",
                        None,
                    ),
                ),
            )
            connection.execute(
                """
                INSERT INTO agents(entity_id, agent_type, birth_year, death_year)
                VALUES (?, ?, ?, ?)
                """,
                ("agent-000001", "person", 1920, 1999),
            )
            connection.execute(
                """
                INSERT INTO credits(
                    id, work_id, agent_id, role, credit_order, importance,
                    credited_as
                ) VALUES (?, ?, ?, ?, ?, ?, ?)
                """,
                (
                    301,
                    "work-000001",
                    "agent-000001",
                    "director",
                    0,
                    "primary",
                    "E. Agent",
                ),
            )
            connection.execute(
                """
                INSERT INTO measurements(
                    id, entity_id, measurement_type, value, unit, qualifier
                ) VALUES (?, ?, ?, ?, ?, ?)
                """,
                (
                    401,
                    "work-000001",
                    "duration",
                    7200.0,
                    "seconds",
                    "approximate",
                ),
            )
            connection.execute(
                """
                INSERT INTO financial_facts(
                    id, work_id, fact_type, amount_min, amount_max,
                    currency_code, value_year, is_estimate, confidence
                ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
                """,
                (
                    501,
                    "work-000001",
                    "budget",
                    100000,
                    120000,
                    "USD",
                    1977,
                    1,
                    0.75,
                ),
            )
            connection.executemany(
                """
                INSERT INTO concepts(entity_id, concept_type, slug)
                VALUES (?, ?, ?)
                """,
                (
                    ("concept-000001", "theme", "example-theme"),
                    ("concept-000002", "motif", "example-motif"),
                ),
            )
            connection.execute(
                """
                INSERT INTO concept_relations(
                    id, subject_concept_id, relation_type, object_concept_id,
                    strength, from_year, to_year, region_code, confidence
                ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
                """,
                (
                    601,
                    "concept-000001",
                    "broader_than",
                    "concept-000002",
                    80,
                    1970,
                    1980,
                    "US",
                    0.8,
                ),
            )
            connection.execute(
                """
                INSERT INTO work_concepts(
                    id, work_id, concept_id, relation_type, centrality,
                    historical_role, confidence
                ) VALUES (?, ?, ?, ?, ?, ?, ?)
                """,
                (
                    701,
                    "work-000001",
                    "concept-000001",
                    "contains",
                    90,
                    "canonical",
                    0.95,
                ),
            )
            connection.execute(
                """
                INSERT INTO sources(
                    id, source_type, title, bibliography_text, author_text,
                    publisher, publication_date, url, doi, isbn, language_code
                ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
                """,
                (
                    801,
                    "article",
                    "Example source",
                    None,
                    "Example Author",
                    "Example Publisher",
                    "2001-02-03",
                    "https://example.test/source",
                    None,
                    None,
                    "en",
                ),
            )
            connection.execute(
                """
                INSERT INTO evidence(
                    id, source_id, exact_quote, quote_language,
                    quote_translation, locator_json, stance
                ) VALUES (?, ?, ?, ?, ?, ?, ?)
                """,
                (
                    901,
                    801,
                    "A concise evidentiary quotation.",
                    "en",
                    None,
                    '{"paragraph":4}',
                    "supports",
                ),
            )
            connection.execute(
                """
                INSERT INTO work_concept_evidence(
                    id, assertion_id, evidence_id
                ) VALUES (?, ?, ?)
                """,
                (1001, 701, 901),
            )
            connection.execute(
                """
                INSERT INTO concept_relation_evidence(
                    id, assertion_id, evidence_id
                ) VALUES (?, ?, ?)
                """,
                (1002, 601, 901),
            )
            connection.execute(
                """
                INSERT INTO parent_guide_assertions(
                    id, work_id, concept_id, category, intensity,
                    explicitness, frequency, centrality, realism,
                    spoiler_level, confidence
                ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
                """,
                (
                    1101,
                    "work-000001",
                    "concept-000002",
                    "frightening",
                    3,
                    2,
                    3,
                    4,
                    5,
                    "mild",
                    0.9,
                ),
            )
            connection.execute(
                """
                INSERT INTO parent_guide_evidence(
                    id, assertion_id, evidence_id
                ) VALUES (?, ?, ?)
                """,
                (1003, 1101, 901),
            )
            connection.execute(
                "INSERT INTO applied_batches(batch_id) VALUES (?)",
                ("batch-fixture",),
            )
            connection.execute(
                """
                INSERT INTO ingest_issues(
                    batch_id, code, json_path, message, value_json, status
                ) VALUES (?, ?, ?, ?, ?, ?)
                """,
                (
                    "batch-fixture",
                    "field_conflict",
                    "$.works[0].year_start",
                    "Conflicting year values",
                    '{"canonical":1977,"incoming":1978}',
                    "resolved",
                ),
            )
            connection.commit()
        finally:
            connection.close()

    def test_schema_v6_has_only_durable_product_objects(self) -> None:
        connection = sqlite3.connect(self.database)
        try:
            connection.execute("PRAGMA foreign_keys = ON")
            connection.executescript(SCHEMA_V6.read_text(encoding="utf-8"))
            _validate_structure(connection, 6)
            self.assertEqual(schema_names(connection, "table"), V6_TABLES)
            self.assertEqual(schema_names(connection, "index"), V6_INDEXES)
            self.assertEqual(schema_names(connection, "trigger"), V6_TRIGGERS)
            legacy = connection.execute(
                """
                SELECT name FROM sqlite_schema
                WHERE name LIKE 'merge_hint%'
                """
            ).fetchall()
            self.assertEqual(legacy, [])
        finally:
            connection.close()

    def test_migration_preserves_every_durable_row_and_compacts_file(self) -> None:
        self.create_v5_fixture()
        os.chmod(self.database, 0o640)
        before_inode = self.database.stat().st_ino
        before_size = self.database.stat().st_size
        with closing(immutable_connection(self.database)) as connection:
            before = durable_snapshot(connection)

        summary = migrate_database(self.database)

        self.assertEqual(summary.source_version, 5)
        self.assertEqual(summary.target_version, 6)
        self.assertEqual(summary.source_bytes, before_size)
        self.assertLess(summary.target_bytes, summary.source_bytes)
        self.assertNotEqual(self.database.stat().st_ino, before_inode)
        self.assertEqual(self.database.stat().st_mode & 0o777, 0o640)
        self.assertEqual(self.staging_files(), [])
        with closing(immutable_connection(self.database)) as connection:
            _validate_structure(connection, 6)
            self.assertEqual(
                connection.execute("PRAGMA integrity_check").fetchall(),
                [("ok",)],
            )
            self.assertEqual(
                connection.execute("PRAGMA foreign_key_check").fetchall(),
                [],
            )
            self.assertEqual(durable_snapshot(connection), before)
            self.assertFalse(
                any(
                    "merge_hint" in name
                    for name in schema_names(connection, "table")
                )
            )
        self.assertEqual(
            summary.rows,
            {name: len(rows) for name, rows in before.items()},
        )
        catalog = build_catalog(self.database)
        self.assertEqual(catalog["databaseUserVersion"], 6)
        self.assertEqual(
            [work["id"] for work in catalog["works"]],
            ["work-000001"],
        )
        expected_agent = {
            "id": "agent-000001",
            "label": "Example Agent",
            "agentType": "person",
            "identifiers": [
                {
                    "scheme": "wikidata",
                    "value": "Q456",
                    "url": None,
                }
            ],
        }
        self.assertEqual(catalog["agents"], [expected_agent])
        self.assertEqual(
            {
                key: value
                for key, value in catalog["works"][0]["contributors"][0].items()
                if key in expected_agent
            },
            expected_agent,
        )

    def test_ignored_decisions_survive_while_disposable_hints_are_removed(self) -> None:
        self.create_v5_fixture()
        connection = sqlite3.connect(self.database)
        try:
            connection.execute("PRAGMA foreign_keys = ON")
            connection.execute(
                """
                INSERT INTO merge_hints(
                    entity_type, left_id, right_id, score, text_score,
                    graph_score, context_score, signals_json, status
                ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
                """,
                (
                    "concept",
                    "concept-000001",
                    "concept-000002",
                    0.9,
                    0.9,
                    0.8,
                    0.7,
                    '{"label":true}',
                    "ignored",
                ),
            )
            connection.execute(
                """
                INSERT INTO merge_hint_blocks(
                    entity_type, block_type, block_key
                ) VALUES ('concept', 'label_fingerprint', 'temporary')
                """
            )
            block_id = int(connection.execute(
                "SELECT id FROM merge_hint_blocks"
            ).fetchone()[0])
            connection.execute(
                """
                INSERT INTO merge_hint_block_members(block_id, entity_id)
                VALUES (?, 'concept-000001')
                """,
                (block_id,),
            )
            connection.commit()
        finally:
            connection.close()
        before_inode = self.database.stat().st_ino

        migrate_database(self.database)

        self.assertNotEqual(self.database.stat().st_ino, before_inode)
        self.assertEqual(self.staging_files(), [])
        with closing(immutable_connection(self.database)) as connection:
            self.assertEqual(
                connection.execute("PRAGMA user_version").fetchone(),
                (6,),
            )
            self.assertEqual(
                connection.execute(
                    "SELECT count(*) FROM sqlite_schema "
                    "WHERE name LIKE 'merge_hint%'"
                ).fetchall(),
                [(0,)],
            )
        decisions = json.loads(
            self.database.with_name("merge-hint-decisions.json").read_text(
                encoding="utf-8"
            )
        )
        self.assertEqual(
            decisions,
            {
                "artifact_type": "arachne_merge_hint_decisions_v1",
                "format_version": 1,
                "ignored_pairs": [
                    {
                        "family": "concept",
                        "left_id": "concept-000001",
                        "right_id": "concept-000002",
                    }
                ],
            },
        )

    def test_migration_rejects_broken_decisions_symlink(self) -> None:
        self.create_v5_fixture()
        decisions = self.database.with_name("merge-hint-decisions.json")
        decisions.symlink_to(self.directory / "missing-decisions.json")
        before_inode = self.database.stat().st_ino

        with self.assertRaisesRegex(
            MigrationError,
            "merge-hint decisions must be a real regular file",
        ):
            migrate_database(self.database)

        self.assertEqual(self.database.stat().st_ino, before_inode)
        self.assertEqual(self.staging_files(), [])

    def test_migration_is_one_way_and_leaves_v6_source_unchanged(self) -> None:
        connection = sqlite3.connect(self.database)
        try:
            connection.executescript(SCHEMA_V6.read_text(encoding="utf-8"))
        finally:
            connection.close()
        before_inode = self.database.stat().st_ino

        with self.assertRaisesRegex(
            MigrationError,
            "expected product schema version 5, found 6",
        ):
            migrate_database(self.database)

        self.assertEqual(self.database.stat().st_ino, before_inode)
        self.assertEqual(self.staging_files(), [])
        with closing(immutable_connection(self.database)) as connection:
            self.assertEqual(
                connection.execute("PRAGMA user_version").fetchone(),
                (6,),
            )


if __name__ == "__main__":
    unittest.main()
