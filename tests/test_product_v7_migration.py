from __future__ import annotations

from contextlib import closing
import os
from pathlib import Path
import sqlite3
import tempfile
import unittest

from scripts.migrate_product_v6_to_v7 import (
    MigrationError,
    V6_TABLE_COPIES,
    V7_TABLE_COPIES,
    _select_sql,
    _validate_structure,
    migrate_database,
)


ROOT = Path(__file__).resolve().parents[1]
SCHEMA_V6 = ROOT / "schema" / "product_v6.sql"
SCHEMA_V7 = ROOT / "schema" / "product_v7.sql"


def immutable_connection(path: Path) -> sqlite3.Connection:
    return sqlite3.connect(
        path.resolve().as_uri() + "?mode=ro&immutable=1", uri=True
    )


def v6_snapshot(
    connection: sqlite3.Connection,
) -> dict[str, list[tuple[object, ...]]]:
    return {
        copy.name: [tuple(row) for row in connection.execute(_select_sql(copy))]
        for copy in V6_TABLE_COPIES
    }


class ProductV7MigrationTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(
            prefix="arachne-product-v7-test-"
        )
        self.directory = Path(self.temporary.name)
        self.database = self.directory / "product.sqlite"

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def staging_files(self) -> list[Path]:
        return list(self.directory.glob(f".{self.database.name}.v7-*"))

    def create_v6_fixture(self) -> None:
        connection = sqlite3.connect(self.database)
        try:
            connection.execute("PRAGMA foreign_keys = ON")
            connection.executescript(SCHEMA_V6.read_text(encoding="utf-8"))
            connection.executemany(
                "INSERT INTO entities(id,entity_type) VALUES(?,?)",
                (
                    ("work-000001", "work"),
                    ("concept-000001", "concept"),
                    ("concept-000002", "concept"),
                    ("concept-000003", "concept"),
                ),
            )
            connection.execute(
                "INSERT INTO works(entity_id,medium) VALUES('work-000001','film')"
            )
            connection.executemany(
                "INSERT INTO concepts(entity_id,concept_type,slug) VALUES(?,?,?)",
                (
                    ("concept-000001", "genre", "migration-genre"),
                    ("concept-000002", "theme", "migration-theme"),
                    ("concept-000003", "technique", "migration-technique"),
                ),
            )
            connection.execute(
                "INSERT INTO sources(id,source_type,url) "
                "VALUES(1,'book','https://example.test/migration')"
            )
            connection.executemany(
                "INSERT INTO evidence(id,source_id,exact_quote,stance) "
                "VALUES(?,1,?,'supports')",
                ((1, "one"), (2, "two"), (3, "three")),
            )
            connection.executemany(
                """
                INSERT INTO work_concepts(
                    id,work_id,concept_id,relation_type,centrality,
                    historical_role,confidence
                ) VALUES(?, 'work-000001', ?, ?, ?, ?, ?)
                """,
                (
                    (1, "concept-000001", "exemplifies", 1, "canonical", 0.2),
                    (2, "concept-000002", "contains", 50, "peripheral", 0.8),
                    (3, "concept-000003", "anticipates", 100, None, None),
                ),
            )
            connection.executemany(
                "INSERT INTO work_concept_evidence(id,assertion_id,evidence_id) "
                "VALUES(?,?,?)",
                ((1, 1, 1), (2, 2, 2), (3, 3, 3)),
            )
            connection.execute(
                "INSERT INTO applied_batches(batch_id) VALUES('v6-fixture')"
            )
            connection.execute(
                "INSERT INTO ingest_issues(batch_id,code,json_path,message) "
                "VALUES('v6-fixture','review','/work','preserve me')"
            )
            connection.commit()
        finally:
            connection.close()

    def test_v7_schema_has_closed_pair_level_scale(self) -> None:
        connection = sqlite3.connect(self.database)
        try:
            connection.executescript(SCHEMA_V7.read_text(encoding="utf-8"))
            self.assertEqual(
                connection.execute("PRAGMA user_version").fetchone(), (7,)
            )
            self.assertEqual(
                tuple(
                    str(row[1])
                    for row in connection.execute("PRAGMA table_info(work_concepts)")
                ),
                next(
                    copy.columns
                    for copy in V7_TABLE_COPIES
                    if copy.name == "work_concepts"
                ),
            )
            _validate_structure(connection, 7)
            sql = next(
                str(row[0])
                for row in connection.execute(
                    "SELECT sql FROM sqlite_schema WHERE name='work_concepts'"
                )
            )
            for scale in ("none", "binary", "ordinal", "graded"):
                self.assertIn(f"'{scale}'", sql)
        finally:
            connection.close()

    def test_migration_is_mechanical_and_preserves_all_v6_product_rows(self) -> None:
        self.create_v6_fixture()
        os.chmod(self.database, 0o640)
        with closing(immutable_connection(self.database)) as source:
            before = v6_snapshot(source)
        before_inode = self.database.stat().st_ino

        summary = migrate_database(self.database)

        self.assertEqual(summary.source_version, 6)
        self.assertEqual(summary.target_version, 7)
        self.assertEqual(summary.rows["work_concepts"], 3)
        self.assertNotEqual(self.database.stat().st_ino, before_inode)
        self.assertEqual(self.database.stat().st_mode & 0o777, 0o640)
        self.assertEqual(self.staging_files(), [])
        with closing(immutable_connection(self.database)) as target:
            self.assertEqual(target.execute("PRAGMA user_version").fetchone(), (7,))
            self.assertEqual(
                [tuple(row) for row in target.execute(
                    "SELECT id,centrality,centrality_scale "
                    "FROM work_concepts ORDER BY id"
                )],
                [(1, 1, "none"), (2, 50, "none"), (3, 100, "none")],
            )
            self.assertEqual(v6_snapshot(target), before)
            self.assertEqual(
                [tuple(row) for row in target.execute("PRAGMA foreign_key_check")],
                [],
            )
            self.assertEqual(
                target.execute("PRAGMA integrity_check").fetchall(), [("ok",)]
            )
            self.assertEqual(target.execute("PRAGMA freelist_count").fetchone(), (0,))

    def test_migration_never_classifies_from_context(self) -> None:
        self.create_v6_fixture()

        migrate_database(self.database)

        with closing(immutable_connection(self.database)) as connection:
            self.assertEqual(
                connection.execute(
                    "SELECT count(DISTINCT centrality_scale),min(centrality_scale),"
                    "max(centrality_scale) FROM work_concepts"
                ).fetchone(),
                (1, "none", "none"),
            )

    def test_migration_is_one_way_and_leaves_v7_source_unchanged(self) -> None:
        connection = sqlite3.connect(self.database)
        try:
            connection.executescript(SCHEMA_V7.read_text(encoding="utf-8"))
        finally:
            connection.close()
        before_inode = self.database.stat().st_ino

        with self.assertRaisesRegex(
            MigrationError, "expected product schema version 6, found 7"
        ):
            migrate_database(self.database)

        self.assertEqual(self.database.stat().st_ino, before_inode)
        self.assertEqual(self.staging_files(), [])

    def test_migration_rejects_unvalidated_schema_and_sidecars(self) -> None:
        self.create_v6_fixture()
        alternate = self.directory / "product_v7.sql"
        alternate.write_text(SCHEMA_V7.read_text(encoding="utf-8"), encoding="utf-8")
        with self.assertRaisesRegex(MigrationError, "canonical schema/product_v7"):
            migrate_database(self.database, alternate)

        sidecar = Path(str(self.database) + "-wal")
        sidecar.touch()
        with self.assertRaisesRegex(MigrationError, "SQLite sidecars"):
            migrate_database(self.database)
        self.assertEqual(self.staging_files(), [])


if __name__ == "__main__":
    unittest.main()
