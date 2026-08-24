from __future__ import annotations

from pathlib import Path
import re
import sqlite3
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SCHEMA = ROOT / "schema" / "product.sql"
CANONICAL_DATABASE = ROOT / "database" / "art-islands.sqlite"


MONTH_ONLY_TEXT = re.compile(
    r"(?:[+-]?\d{4}-(?:0[1-9]|1[0-2]))"
    r"|(?:(?:January|February|March|April|May|June|July|August|September|"
    r"October|November|December) [+-]?\d{1,4})"
)


def columns(connection: sqlite3.Connection, table: str) -> tuple[str, ...]:
    return tuple(str(row[1]) for row in connection.execute(f"PRAGMA table_info({table})"))


class CurrentProductSchemaTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="arachne-product-schema-")
        self.database = Path(self.temporary.name) / "product.sqlite"
        self.connection = sqlite3.connect(self.database)
        self.connection.execute("PRAGMA foreign_keys=ON")
        self.connection.executescript(SCHEMA.read_text(encoding="utf-8"))
        self.connection.executescript(
            """
            INSERT INTO entities(id, entity_type) VALUES
                ('work-000001', 'work'),
                ('work-000002', 'work'),
                ('manifestation-000001', 'manifestation'),
                ('agent-000001', 'person'),
                ('agent-000002', 'group');
            INSERT INTO works(entity_id, medium, date_precision) VALUES
                ('work-000001', 'comic', 'month'),
                ('work-000002', 'performance', 'exact');
            INSERT INTO manifestations(
                entity_id, work_id, manifestation_type, label
            ) VALUES(
                'manifestation-000001', 'work-000001', 'edition', 'First edition'
            );
            INSERT INTO agents(entity_id, agent_type) VALUES
                ('agent-000001', 'person'),
                ('agent-000002', 'group');
            """
        )

    def tearDown(self) -> None:
        self.connection.close()
        self.temporary.cleanup()

    def test_repository_has_one_unversioned_product_schema(self) -> None:
        self.assertEqual(
            [path.name for path in sorted((ROOT / "schema").glob("product*.sql"))],
            ["product.sql"],
        )
        self.assertFalse(list((ROOT / "scripts").glob("migrate_product_v*_to_v*.py")))
        self.assertFalse(list((ROOT / "tests").glob("test_product_v*_migration.py")))
        self.assertEqual(self.connection.execute("PRAGMA user_version").fetchone(), (0,))

    def test_current_relation_tables_and_indexes_are_present(self) -> None:
        tables = {
            str(row[0])
            for row in self.connection.execute(
                "SELECT name FROM sqlite_schema WHERE type='table'"
            )
        }
        self.assertTrue({"work_memberships", "agent_relations", "events"} <= tables)
        self.assertFalse(
            {"holdings", "agent_classifications", "manifestation_facts"} & tables
        )
        indexes = {
            str(row[0])
            for row in self.connection.execute(
                "SELECT name FROM sqlite_schema WHERE type='index'"
            )
        }
        self.assertTrue(
            {
                "work_memberships_child_idx",
                "work_memberships_parent_idx",
                "agent_relations_subject_idx",
                "agent_relations_object_idx",
                "events_entity_idx",
                "events_type_idx",
                "credits_entity_idx",
            }
            <= indexes
        )
        self.assertNotIn("credits_work_idx", indexes)
        measurement_columns = set(columns(self.connection, "measurements"))
        self.assertNotIn("copy_count", measurement_columns)

    def test_memberships_allow_distinct_structure_but_reject_invalid_rows(self) -> None:
        self.connection.execute(
            """
            INSERT INTO work_memberships(
                child_work_id, parent_work_id, membership_type, position,
                position_text
            ) VALUES('work-000002', 'work-000001', 'episode_of', 6, 'S06E06')
            """
        )
        self.connection.execute(
            """
            INSERT INTO work_memberships(
                child_work_id, parent_work_id, membership_type, position_text
            ) VALUES('work-000002', 'work-000001', 'collected_in', 'Volume 3')
            """
        )
        with self.assertRaises(sqlite3.IntegrityError):
            self.connection.execute(
                """
                INSERT INTO work_memberships(
                    child_work_id, parent_work_id, membership_type, position,
                    position_text
                ) VALUES('work-000002', 'work-000001', 'episode_of', 6, 'S06E06')
                """
            )
        with self.assertRaises(sqlite3.IntegrityError):
            self.connection.execute(
                """
                INSERT INTO work_memberships(
                    child_work_id, parent_work_id, membership_type
                ) VALUES('work-000001', 'work-000001', 'part_of')
                """
            )
        with self.assertRaises(sqlite3.IntegrityError):
            self.connection.execute(
                """
                INSERT INTO work_memberships(
                    child_work_id, parent_work_id, membership_type, position
                ) VALUES('work-000002', 'work-000001', 'part_of', -1)
                """
            )

    def test_agent_relations_allow_repeated_periods(self) -> None:
        self.connection.execute(
            """
            INSERT INTO agent_relations(
                subject_agent_id, relation_type, object_agent_id,
                from_year, to_year, period_text, role_text
            ) VALUES(
                'agent-000001', 'member_of', 'agent-000002',
                1994, 1996, 'c. 1994-1996', 'vocals'
            )
            """
        )
        self.connection.execute(
            """
            INSERT INTO agent_relations(
                subject_agent_id, relation_type, object_agent_id,
                from_year, to_year, role_text
            ) VALUES(
                'agent-000001', 'member_of', 'agent-000002',
                2001, 2003, 'guitar'
            )
            """
        )
        with self.assertRaises(sqlite3.IntegrityError):
            self.connection.execute(
                """
                INSERT INTO agent_relations(
                    subject_agent_id, relation_type, object_agent_id,
                    from_year, to_year
                ) VALUES(
                    'agent-000001', 'member_of', 'agent-000002', 2003, 2001
                )
                """
            )
        with self.assertRaises(sqlite3.IntegrityError):
            self.connection.execute(
                """
                INSERT INTO agent_relations(
                    subject_agent_id, relation_type, object_agent_id
                ) VALUES('agent-000001', 'member_of', 'agent-000001')
                """
            )

    def test_credits_target_only_works_or_manifestations(self) -> None:
        roles = (
            "distributor",
            "broadcaster",
            "platform",
            "translator",
            "illustrator",
            "printer",
            "curator",
            "choreographer",
            "narrator",
            "lyricist",
            "songwriter",
            "arranger",
            "sound_engineer",
            "designer",
            "animator",
        )
        for order, role in enumerate(roles):
            target = "work-000001" if order % 2 == 0 else "manifestation-000001"
            self.connection.execute(
                """
                INSERT INTO credits(
                    entity_id, agent_id, role, credit_order, importance
                ) VALUES(?, 'agent-000001', ?, ?, 'supporting')
                """,
                (target, role, order),
            )
        with self.assertRaisesRegex(
            sqlite3.IntegrityError,
            "credit target must be a work or manifestation",
        ):
            self.connection.execute(
                """
                INSERT INTO credits(entity_id, agent_id, role, importance)
                VALUES('agent-000002', 'agent-000001', 'author', 'primary')
                """
            )
        with self.assertRaisesRegex(
            sqlite3.IntegrityError,
            "credit target must be a work or manifestation",
        ):
            self.connection.execute(
                "UPDATE credits SET entity_id='agent-000002' WHERE id=1"
            )

    def test_events_support_month_precision_and_restrict_targets(self) -> None:
        self.connection.execute(
            """
            INSERT INTO events(
                entity_id, event_type, year_start, date_text,
                date_precision, place_text
            ) VALUES(
                'work-000001', 'published', 1983, 'August 1983',
                'month', 'Berlin'
            )
            """
        )
        self.connection.execute(
            """
            INSERT INTO events(
                entity_id, event_type, year_start, year_end, date_precision
            ) VALUES(
                'manifestation-000001', 'released', 1983, 1983, 'year'
            )
            """
        )
        with self.assertRaisesRegex(
            sqlite3.IntegrityError,
            "event target must be a work or manifestation",
        ):
            self.connection.execute(
                """
                INSERT INTO events(entity_id, event_type)
                VALUES('agent-000001', 'performed')
                """
            )
        with self.assertRaises(sqlite3.IntegrityError):
            self.connection.execute(
                """
                INSERT INTO events(
                    entity_id, event_type, year_start, year_end
                ) VALUES('work-000001', 'recorded', 2002, 2001)
                """
            )

    def test_manifestations_remain_versions_not_events(self) -> None:
        self.connection.execute(
            """
            INSERT INTO entities(id, entity_type)
            VALUES('manifestation-000002', 'manifestation')
            """
        )
        with self.assertRaises(sqlite3.IntegrityError):
            self.connection.execute(
                """
                INSERT INTO manifestations(
                    entity_id, work_id, manifestation_type, label
                ) VALUES(
                    'manifestation-000002', 'work-000001', 'broadcast',
                    'Not a version'
                )
                """
            )

    def test_new_media_and_month_precision_are_closed_enums(self) -> None:
        for index, medium in enumerate(("nonfiction", "comic", "performance"), 10):
            entity_id = f"work-{index:06d}"
            self.connection.execute(
                "INSERT INTO entities(id, entity_type) VALUES(?, 'work')",
                (entity_id,),
            )
            self.connection.execute(
                """
                INSERT INTO works(entity_id, medium, date_precision)
                VALUES(?, ?, 'month')
                """,
                (entity_id, medium),
            )
        with self.assertRaises(sqlite3.IntegrityError):
            self.connection.execute(
                """
                INSERT INTO entities(id, entity_type)
                VALUES('work-000099', 'work')
                """
            )
            self.connection.execute(
                """
                INSERT INTO works(entity_id, medium)
                VALUES('work-000099', 'autobiography')
                """
            )

    def test_canonical_obvious_month_values_use_month_precision(self) -> None:
        canonical = sqlite3.connect(
            f"file:{CANONICAL_DATABASE}?mode=ro",
            uri=True,
        )
        try:
            mismatches = [
                ("works", str(entity_id), str(precision), str(value))
                for entity_id, precision, value in canonical.execute(
                    """
                    SELECT entity_id, date_precision, date_start_text
                    FROM works
                    WHERE date_precision IN ('year', 'exact')
                      AND date_start_text IS NOT NULL
                    """
                )
                if MONTH_ONLY_TEXT.fullmatch(str(value))
            ]
            mismatches.extend(
                ("events", str(identifier), str(precision), str(value))
                for identifier, precision, value in canonical.execute(
                    """
                    SELECT id, date_precision, date_text
                    FROM events
                    WHERE date_precision IN ('year', 'exact')
                      AND date_text IS NOT NULL
                    """
                )
                if MONTH_ONLY_TEXT.fullmatch(str(value))
            )
        finally:
            canonical.close()

        self.assertEqual([], mismatches[:20])


if __name__ == "__main__":
    unittest.main()
