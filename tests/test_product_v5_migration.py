from __future__ import annotations

import os
from pathlib import Path
import sqlite3
import tempfile
import unittest

from scripts.migrate_product_v4_to_v5 import (
    MigrationError,
    PITCHFORK_ALTERNATE_URL,
    PITCHFORK_PRIMARY_URL,
    TABLE_COPIES,
    V4_EXTRA_COLUMNS,
    V4_TABLES,
    _validate_v5_structure,
    migrate_database,
)


ROOT = Path(__file__).resolve().parents[1]
SCHEMA_V5 = ROOT / "schema" / "product_v5.sql"

V4_INTEGER_COLUMNS = {
    "amount_max",
    "amount_min",
    "assertion_id",
    "birth_year",
    "centrality",
    "credit_order",
    "death_year",
    "evidence_id",
    "explicitness",
    "external_id_id",
    "frequency",
    "from_year",
    "intensity",
    "is_estimate",
    "is_preferred",
    "is_verbatim",
    "realism",
    "release_year",
    "source_archive_id",
    "source_id",
    "strength",
    "to_year",
    "value_year",
    "year_end",
    "year_start",
}

V4_FOREIGN_KEYS = {
    ("agents", "entity_id"): ("entities", "id"),
    ("concept_relation_evidence", "assertion_id"): (
        "concept_relations",
        "id",
    ),
    ("concept_relation_evidence", "evidence_id"): ("evidence", "id"),
    ("concept_relations", "object_concept_id"): ("concepts", "entity_id"),
    ("concept_relations", "subject_concept_id"): ("concepts", "entity_id"),
    ("concepts", "entity_id"): ("entities", "id"),
    ("credits", "agent_id"): ("agents", "entity_id"),
    ("credits", "work_id"): ("works", "entity_id"),
    ("evidence", "source_archive_id"): ("source_archives", "id"),
    ("evidence", "source_id"): ("sources", "id"),
    ("external_ids", "entity_id"): ("entities", "id"),
    ("financial_facts", "work_id"): ("works", "entity_id"),
    ("manifestations", "entity_id"): ("entities", "id"),
    ("manifestations", "work_id"): ("works", "entity_id"),
    ("measurements", "entity_id"): ("entities", "id"),
    ("names", "entity_id"): ("entities", "id"),
    ("parent_guide_assertions", "concept_id"): ("concepts", "entity_id"),
    ("parent_guide_assertions", "work_id"): ("works", "entity_id"),
    ("parent_guide_evidence", "assertion_id"): (
        "parent_guide_assertions",
        "id",
    ),
    ("parent_guide_evidence", "evidence_id"): ("evidence", "id"),
    ("remote_assets", "entity_id"): ("entities", "id"),
    ("remote_assets", "external_id_id"): ("external_ids", "id"),
    ("source_archives", "source_id"): ("sources", "id"),
    ("source_urls", "source_id"): ("sources", "id"),
    ("work_concept_evidence", "assertion_id"): ("work_concepts", "id"),
    ("work_concept_evidence", "evidence_id"): ("evidence", "id"),
    ("work_concepts", "concept_id"): ("concepts", "entity_id"),
    ("work_concepts", "work_id"): ("works", "entity_id"),
    ("works", "entity_id"): ("entities", "id"),
}


def immutable_connection(path: Path) -> sqlite3.Connection:
    return sqlite3.connect(
        path.resolve().as_uri() + "?mode=ro&immutable=1",
        uri=True,
    )


def table_names(connection: sqlite3.Connection) -> set[str]:
    return {
        row[0]
        for row in connection.execute(
            """
            SELECT name
            FROM sqlite_schema
            WHERE type = 'table' AND name NOT LIKE 'sqlite_%'
            """
        )
    }


def v4_column_contract() -> dict[str, tuple[str, ...]]:
    contract: dict[str, tuple[str, ...]] = {}
    for copy in TABLE_COPIES:
        columns = list(copy.columns)
        if copy.name == "evidence":
            columns.insert(columns.index("exact_quote"), "source_archive_id")
        contract[copy.name] = tuple(columns)
    contract.update(V4_EXTRA_COLUMNS)
    if set(contract) != V4_TABLES:
        raise AssertionError("test fixture does not cover the v4 table contract")
    return contract


def v4_column_type(table: str, column: str) -> str:
    if column == "id":
        return "TEXT" if table == "entities" else "INTEGER"
    if column in V4_INTEGER_COLUMNS:
        return "INTEGER"
    if column == "confidence" or (
        table == "measurements" and column == "value"
    ):
        return "REAL"
    return "TEXT"


def create_v4_schema(connection: sqlite3.Connection) -> None:
    """Create only the source shape needed to exercise the one-way converter."""

    for table, columns in v4_column_contract().items():
        definitions: list[str] = []
        for position, column in enumerate(columns):
            definition = f'"{column}" {v4_column_type(table, column)}'
            if position == 0:
                definition += " PRIMARY KEY"
            target = V4_FOREIGN_KEYS.get((table, column))
            if target is not None:
                target_table, target_column = target
                definition += (
                    f' REFERENCES "{target_table}"("{target_column}")'
                )
            definitions.append(definition)
        connection.execute(
            f'CREATE TABLE "{table}" ({", ".join(definitions)}) STRICT'
        )
    connection.execute("PRAGMA user_version = 4")


class ProductV5MigrationTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(
            prefix="arachne-product-v5-test-"
        )
        self.directory = Path(self.temporary.name)
        self.database = self.directory / "product.sqlite"

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def create_v4_fixture(self) -> None:
        connection = sqlite3.connect(self.database)
        try:
            connection.execute("PRAGMA foreign_keys = ON")
            create_v4_schema(connection)
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
                    entity_id, medium, year_start, date_precision,
                    language_code, country_code, production_info_json
                ) VALUES (?, ?, ?, ?, ?, ?, ?)
                """,
                (
                    "work-000001",
                    "film",
                    1977,
                    "year",
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
            connection.execute(
                """
                INSERT INTO external_ids(
                    id, entity_id, scheme, value, canonical_url
                ) VALUES (?, ?, ?, ?, ?)
                """,
                (
                    201,
                    "work-000001",
                    "wikidata",
                    "Q123",
                    "https://www.wikidata.org/wiki/Q123",
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
                (401, "work-000001", "duration", 7200.0, "seconds", "approximate"),
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
                "INSERT INTO concepts(entity_id, concept_type, slug) VALUES (?, ?, ?)",
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
                    6856,
                    "web_page",
                    "Onanie Bomb Meets the Sex Pistols / Pop Tatari / "
                    "Chocolate Synthesizer",
                    None,
                    "Pitchfork",
                    "Pitchfork",
                    "2008-06-13",
                    PITCHFORK_PRIMARY_URL,
                    None,
                    None,
                    "en",
                ),
            )
            connection.execute(
                "INSERT INTO source_urls(id, source_id, url) VALUES (?, ?, ?)",
                (1, 6856, PITCHFORK_ALTERNATE_URL),
            )
            connection.execute(
                """
                INSERT INTO evidence(
                    id, source_id, source_archive_id, exact_quote,
                    quote_language, quote_translation, locator_json, stance
                ) VALUES (?, ?, ?, ?, ?, ?, ?, ?)
                """,
                (
                    801,
                    6856,
                    None,
                    "A concise evidentiary quotation.",
                    "en",
                    None,
                    '{"paragraph":4}',
                    "supports",
                ),
            )
            connection.execute(
                """
                INSERT INTO work_concept_evidence(id, assertion_id, evidence_id)
                VALUES (?, ?, ?)
                """,
                (811, 701, 801),
            )
            connection.execute(
                """
                INSERT INTO concept_relation_evidence(
                    id, assertion_id, evidence_id
                ) VALUES (?, ?, ?)
                """,
                (821, 601, 801),
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
                    901,
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
                INSERT INTO parent_guide_evidence(id, assertion_id, evidence_id)
                VALUES (?, ?, ?)
                """,
                (911, 901, 801),
            )
            connection.commit()
        finally:
            connection.close()

    def test_schema_v5_has_reduced_tables_and_hint_family_guards(self) -> None:
        connection = sqlite3.connect(self.database)
        try:
            connection.execute("PRAGMA foreign_keys = ON")
            connection.executescript(SCHEMA_V5.read_text(encoding="utf-8"))
            self.assertEqual(
                connection.execute("PRAGMA user_version").fetchone(), (5,)
            )
            _validate_v5_structure(connection)
            tables = table_names(connection)
            self.assertFalse(
                {"remote_assets", "source_archives", "source_urls"} & tables
            )
            self.assertTrue(
                {
                    "applied_batches",
                    "ingest_issues",
                    "merge_hint_blocks",
                    "merge_hint_block_members",
                    "merge_hints",
                }
                <= tables
            )
            self.assertNotIn(
                "source_archive_id",
                {
                    row[1]
                    for row in connection.execute("PRAGMA table_info(evidence)")
                },
            )

            indexes = {
                row[0]
                for row in connection.execute(
                    "SELECT name FROM sqlite_schema WHERE type = 'index'"
                )
            }
            self.assertTrue(
                {
                    "ingest_issues_status_idx",
                    "merge_hints_left_idx",
                    "merge_hints_right_idx",
                    "merge_hints_status_score_idx",
                    "merge_hint_block_members_peer_idx",
                    "sources_url_unique",
                }
                <= indexes
            )
            triggers = {
                row[0]
                for row in connection.execute(
                    "SELECT name FROM sqlite_schema WHERE type = 'trigger'"
                )
            }
            self.assertTrue(
                {
                    "merge_hint_block_members_entity_family_insert",
                    "merge_hint_block_members_entity_family_update",
                    "merge_hint_blocks_identity_update_guard",
                    "merge_hint_block_members_remove_orphan",
                }
                <= triggers
            )
            peer_plan = " ".join(
                str(row[3])
                for row in connection.execute(
                    """
                    EXPLAIN QUERY PLAN
                    SELECT entity_id FROM merge_hint_block_members
                    WHERE block_id = ? ORDER BY entity_id LIMIT ?
                    """,
                    (1, 21),
                )
            )
            self.assertIn("merge_hint_block_members_peer_idx", peer_plan)

            connection.executemany(
                "INSERT INTO entities(id, entity_type) VALUES (?, ?)",
                (
                    ("agent-000001", "person"),
                    ("agent-000002", "group"),
                    ("work-000001", "work"),
                ),
            )
            connection.execute(
                """
                INSERT INTO merge_hints(
                    entity_type, left_id, right_id, score, text_score,
                    graph_score, context_score, signals_json
                ) VALUES (?, ?, ?, ?, ?, ?, ?, ?)
                """,
                (
                    "agent",
                    "agent-000001",
                    "agent-000002",
                    0.8,
                    0.9,
                    0.7,
                    0.6,
                    '{"label":"same"}',
                ),
            )
            with self.assertRaisesRegex(
                sqlite3.IntegrityError, "merge hint entity family mismatch"
            ):
                connection.execute(
                    """
                    INSERT INTO merge_hints(
                        entity_type, left_id, right_id, score, signals_json
                    ) VALUES (?, ?, ?, ?, ?)
                    """,
                    (
                        "work",
                        "agent-000001",
                        "agent-000002",
                        0.5,
                        "{}",
                    ),
                )

            agent_block = connection.execute(
                """
                INSERT INTO merge_hint_blocks(
                    entity_type, block_type, block_key
                ) VALUES ('agent','label_fingerprint','example')
                RETURNING id
                """
            ).fetchone()[0]
            connection.execute(
                """
                INSERT INTO merge_hint_block_members(block_id, entity_id)
                VALUES (?, 'agent-000002')
                """,
                (agent_block,),
            )
            with self.assertRaisesRegex(
                sqlite3.IntegrityError,
                "populated merge hint block identity is immutable",
            ):
                connection.execute(
                    """
                    UPDATE merge_hint_blocks SET entity_type = 'work'
                    WHERE id = ?
                    """,
                    (agent_block,),
                )
            with self.assertRaises(sqlite3.IntegrityError):
                connection.execute(
                    """
                    INSERT INTO merge_hint_blocks(
                        entity_type, block_type, block_key
                    ) VALUES ('agent','work_primary_agent','agent-000001')
                    """
                )
            work_block = connection.execute(
                """
                INSERT INTO merge_hint_blocks(
                    entity_type, block_type, block_key
                ) VALUES ('work','label_fingerprint','example')
                RETURNING id
                """
            ).fetchone()[0]
            with self.assertRaisesRegex(
                sqlite3.IntegrityError,
                "merge hint block entity family mismatch",
            ):
                connection.execute(
                    """
                    INSERT INTO merge_hint_block_members(block_id, entity_id)
                    VALUES (?, 'agent-000001')
                    """,
                    (work_block,),
                )

            connection.execute("DELETE FROM entities WHERE id = 'agent-000002'")
            self.assertEqual(
                connection.execute("SELECT count(*) FROM merge_hints").fetchone(),
                (0,),
            )
            self.assertEqual(
                connection.execute(
                    """
                    SELECT count(*) FROM merge_hint_block_members
                    WHERE block_id = ?
                    """,
                    (agent_block,),
                ).fetchone(),
                (0,),
            )
            self.assertEqual(
                connection.execute(
                    "SELECT count(*) FROM merge_hint_blocks WHERE id = ?",
                    (agent_block,),
                ).fetchone(),
                (0,),
            )
            connection.execute(
                "DROP TRIGGER merge_hint_blocks_identity_update_guard"
            )
            with self.assertRaisesRegex(
                MigrationError, "trigger set differs"
            ):
                _validate_v5_structure(connection)
        finally:
            connection.close()

    def test_v5_structure_rejects_missing_core_schema_objects(self) -> None:
        cases = (
            (
                "DROP INDEX names_logical_unique",
                "index set differs",
            ),
            (
                "DROP TRIGGER works_entity_type",
                "trigger set differs",
            ),
        )
        for statement, expected_message in cases:
            with self.subTest(statement=statement):
                connection = sqlite3.connect(":memory:")
                try:
                    connection.execute("PRAGMA foreign_keys = ON")
                    connection.executescript(
                        SCHEMA_V5.read_text(encoding="utf-8")
                    )
                    self.assertEqual(
                        connection.execute(
                            "PRAGMA user_version"
                        ).fetchone(),
                        (5,),
                    )
                    connection.execute(statement)
                    with self.assertRaisesRegex(
                        MigrationError, expected_message
                    ):
                        _validate_v5_structure(connection)
                finally:
                    connection.close()

    def test_migration_preserves_product_rows_and_replaces_database(self) -> None:
        self.create_v4_fixture()
        source_inode = self.database.stat().st_ino
        os.chmod(self.database, 0o640)

        summary = migrate_database(self.database, SCHEMA_V5)

        self.assertEqual(summary.source_version, 4)
        self.assertEqual(summary.target_version, 5)
        self.assertEqual(summary.rows["entities"], 5)
        self.assertEqual(summary.rows["evidence"], 1)
        self.assertNotEqual(self.database.stat().st_ino, source_inode)
        self.assertEqual(self.database.stat().st_mode & 0o777, 0o640)
        self.assertEqual(
            list(self.directory.glob(f".{self.database.name}.v5-*.sqlite")),
            [],
        )

        connection = immutable_connection(self.database)
        try:
            self.assertEqual(
                connection.execute("PRAGMA user_version").fetchone(), (5,)
            )
            self.assertEqual(
                connection.execute("PRAGMA foreign_key_check").fetchall(), []
            )
            self.assertEqual(
                connection.execute("PRAGMA integrity_check").fetchone(), ("ok",)
            )
            tables = table_names(connection)
            self.assertFalse(
                {"remote_assets", "source_archives", "source_urls"} & tables
            )
            self.assertEqual(
                connection.execute(
                    "SELECT url FROM sources WHERE id = 6856"
                ).fetchone(),
                (PITCHFORK_PRIMARY_URL,),
            )
            self.assertEqual(
                connection.execute(
                    """
                    SELECT id, source_id, exact_quote, quote_language,
                           quote_translation, locator_json, stance
                    FROM evidence
                    """
                ).fetchone(),
                (
                    801,
                    6856,
                    "A concise evidentiary quotation.",
                    "en",
                    None,
                    '{"paragraph":4}',
                    "supports",
                ),
            )
            self.assertEqual(
                connection.execute(
                    """
                    SELECT (
                        (SELECT count(*) FROM applied_batches) +
                        (SELECT count(*) FROM ingest_issues) +
                        (SELECT count(*) FROM merge_hint_blocks) +
                        (SELECT count(*) FROM merge_hint_block_members) +
                        (SELECT count(*) FROM merge_hints)
                    )
                    """
                ).fetchone(),
                (0,),
            )
            self.assertEqual(
                connection.execute(
                    """
                    SELECT work_id, agent_id, role, credit_order,
                           importance, credited_as
                    FROM credits WHERE id = 301
                    """
                ).fetchone(),
                (
                    "work-000001",
                    "agent-000001",
                    "director",
                    0,
                    "primary",
                    "E. Agent",
                ),
            )
            self.assertEqual(
                connection.execute(
                    """
                    SELECT assertion_id, evidence_id
                    FROM parent_guide_evidence WHERE id = 911
                    """
                ).fetchone(),
                (901, 801),
            )
        finally:
            connection.close()

    def test_migration_refuses_to_discard_unexpected_legacy_data(self) -> None:
        self.create_v4_fixture()
        connection = sqlite3.connect(self.database)
        try:
            connection.execute("PRAGMA foreign_keys = ON")
            connection.execute(
                """
                INSERT INTO remote_assets(
                    id, entity_id, provider, direct_url, rights_note
                ) VALUES (?, ?, ?, ?, ?)
                """,
                (
                    1001,
                    "work-000001",
                    "example",
                    "https://example.test/asset.jpg",
                    "test fixture",
                ),
            )
            connection.commit()
        finally:
            connection.close()

        original = self.database.read_bytes()
        with self.assertRaisesRegex(
            MigrationError, "refusing to discard data"
        ):
            migrate_database(self.database, SCHEMA_V5)

        self.assertEqual(self.database.read_bytes(), original)
        self.assertEqual(
            list(self.directory.glob(f".{self.database.name}.v5-*.sqlite")),
            [],
        )
        connection = immutable_connection(self.database)
        try:
            self.assertEqual(
                connection.execute("PRAGMA user_version").fetchone(), (4,)
            )
            self.assertEqual(
                connection.execute("SELECT count(*) FROM remote_assets").fetchone(),
                (1,),
            )
        finally:
            connection.close()


if __name__ == "__main__":
    unittest.main()
