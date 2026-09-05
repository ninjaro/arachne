from __future__ import annotations

import json
import sqlite3
import tempfile
import unittest
from pathlib import Path

from scripts.materialize_provider_rebuild import (
    Edge,
    Graph,
    Identity,
    anomalies,
    bundle_for,
    date_value,
    materialize,
    ordinary_selection,
    series_allowed,
    tail_metrics,
)
from scripts.provider_observation_graph import ObservationGraph


ROOT = Path(__file__).resolve().parents[1]


def identity(external_id: str) -> dict[str, str]:
    return {
        "provider": "wikidata",
        "namespace": "item",
        "external_id": external_id,
    }


def record(
    external_id: str,
    entity_type: str,
    *,
    name: str,
    facts: list[dict[str, object]] | None = None,
    media: list[dict[str, object]] | None = None,
    edges: list[dict[str, object]] | None = None,
) -> dict[str, object]:
    return {
        "id": identity(external_id),
        "entity_type": entity_type,
        "identifiers": [],
        "names": [{"type": "label", "language": "en", "value": name}],
        "facts": facts or [],
        "media": media or [],
        "edges": edges or [],
    }


def edge(
    target: str, target_type: str, family: str, relation: str
) -> dict[str, object]:
    return {
        "target": identity(target),
        "target_entity_type": target_type,
        "relation_family": family,
        "relation_type": relation,
        "metadata": {},
    }


class ProviderRebuildMaterializerTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="arachne-materialize-")
        self.root = Path(self.temporary.name)
        self.graph_path = self.root / "observations.sqlite"
        self.database_path = self.root / "product.sqlite"
        self.priority_path = self.root / "priority.json"
        self.report_path = self.root / "report.json"
        self.graph = ObservationGraph.create(self.graph_path)
        with sqlite3.connect(self.database_path) as connection:
            connection.executescript((ROOT / "schema/product.sql").read_text("utf-8"))

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def write_priority(self, value: dict[str, list[str]]) -> None:
        self.priority_path.write_text(
            json.dumps(value, sort_keys=True) + "\n", encoding="utf-8"
        )

    def test_priority_agent_materializes_complete_work_bundle_without_budget(self) -> None:
        self.graph.ingest(
            "wikidata",
            [
                record(
                    "Q1",
                    "person",
                    name="Artist",
                    facts=[{"field": "birth_date", "value": "1900-05-03"}],
                ),
                record(
                    "Q2",
                    "work",
                    name="Album",
                    facts=[
                        {"field": "medium", "value": "album"},
                        {"field": "original_date", "value": "1970-04"},
                    ],
                    edges=[edge("Q1", "person", "credit", "performer")],
                ),
                record(
                    "Q3",
                    "work",
                    name="Track",
                    facts=[{"field": "medium", "value": "composition"}],
                    edges=[
                        edge("Q1", "person", "credit", "performer"),
                        edge("Q2", "work", "work_membership", "track_of"),
                    ],
                ),
            ],
        )
        self.write_priority({"wikidata": ["Q1", "Q999"]})

        report = materialize(
            self.graph_path,
            self.database_path,
            self.priority_path,
            self.report_path,
        )

        self.assertEqual(
            json.loads(self.priority_path.read_text("utf-8")),
            {"wikidata": ["Q999"]},
        )
        self.assertEqual(report["priority"]["materialized"], 1)
        self.assertEqual(report["selection"]["ordinary_budget"], 0)
        self.assertEqual(report["primary_metrics"]["N"], 2)
        self.assertEqual(report["primary_metrics"]["T"], 2)
        with sqlite3.connect(self.database_path) as connection:
            self.assertEqual(connection.execute("SELECT count(*) FROM works").fetchone()[0], 2)
            self.assertEqual(connection.execute("SELECT count(*) FROM agents").fetchone()[0], 1)
            self.assertEqual(
                connection.execute(
                    "SELECT medium,year_start,date_precision,date_start_text "
                    "FROM works JOIN external_ids ON external_ids.entity_id=works.entity_id "
                    "WHERE scheme='wikidata' AND value='Q2'"
                ).fetchone(),
                ("album", 1970, "month", "1970-04"),
            )
            self.assertEqual(
                connection.execute(
                    "SELECT count(*) FROM work_memberships WHERE membership_type='track_of'"
                ).fetchone()[0],
                1,
            )
            self.assertEqual(connection.execute("PRAGMA foreign_key_check").fetchall(), [])

    def test_non_null_refresh_updates_anomaly_but_does_not_erase_absent_date(self) -> None:
        with sqlite3.connect(self.database_path) as connection:
            connection.execute("INSERT INTO entities VALUES('work-000001','work')")
            connection.execute(
                "INSERT INTO works(entity_id,medium,year_start,date_precision,date_start_text,"
                "language_code,country_code,production_info_json) "
                "VALUES('work-000001','film',1999,'year','1999','en','US','{\"studio\":\"old\"}')"
            )
            connection.execute(
                "INSERT INTO external_ids(entity_id,scheme,value) "
                "VALUES('work-000001','wikidata','Q10')"
            )
            connection.execute(
                "INSERT INTO names(entity_id,name_type,language_code,value,is_preferred) "
                "VALUES('work-000001','english','en','Old work',1)"
            )
            connection.execute(
                "INSERT INTO remote_assets(entity_id,provider,remote_key,media_kind) "
                "VALUES('work-000001','wikidata','old-image','image')"
            )
            for field, value in (
                ("language_code", "en"),
                ("country_code", "US"),
                ("production_info", {"studio": "old"}),
                ("original_date", {"year": 1999, "precision": "year", "text": "1999"}),
                (
                    "names",
                    [
                        {
                            "type": "label",
                            "language": "en",
                            "script": None,
                            "value": "Old work",
                        }
                    ],
                ),
                (
                    "media",
                    [
                        {
                            "kind": "image",
                            "provider": "wikidata",
                            "remote_key": "old-image",
                        }
                    ],
                ),
            ):
                connection.execute(
                    "INSERT INTO provider_general_facts("
                    "entity_id,provider,external_id,field,value_json) VALUES(?,?,?,?,?)",
                    ("work-000001", "wikidata", "Q10", field, json.dumps(value)),
                )

        self.graph.ingest(
            "wikidata",
            [
                record(
                    "Q10",
                    "work",
                    name="Changed work",
                    facts=[
                        {"field": "language_code", "value": "fr"},
                        {"field": "country_code", "value": "US"},
                        {"field": "production_info", "value": {"studio": "new"}},
                    ],
                    media=[{"kind": "poster", "remote_key": "new-image"}],
                )
            ],
        )
        self.write_priority({})

        report = materialize(
            self.graph_path,
            self.database_path,
            self.priority_path,
            self.report_path,
        )

        self.assertEqual(len(report["anomalies"]), 1)
        self.assertGreater(report["anomalies"][0]["change_ratio"], 0.5)
        with sqlite3.connect(self.database_path) as connection:
            self.assertEqual(
                connection.execute(
                    "SELECT medium,year_start,language_code,country_code,production_info_json "
                    "FROM works WHERE entity_id='work-000001'"
                ).fetchone(),
                ("film", 1999, "fr", "US", '{"studio":"new"}'),
            )
            self.assertEqual(
                json.loads(
                    connection.execute(
                        "SELECT value_json FROM provider_general_facts "
                        "WHERE provider='wikidata' AND external_id='Q10' "
                        "AND field='original_date'"
                    ).fetchone()[0]
                ),
                {"year": 1999, "precision": "year", "text": "1999"},
            )
            self.assertEqual(
                connection.execute(
                    "SELECT value,is_preferred FROM names WHERE entity_id='work-000001'"
                ).fetchall(),
                [("Changed work", 1)],
            )
            self.assertEqual(
                connection.execute(
                    "SELECT remote_key,media_kind FROM remote_assets "
                    "WHERE entity_id='work-000001'"
                ).fetchall(),
                [("new-image", "poster")],
            )

    def test_exact_half_change_is_not_anomalous_and_dates_keep_precision(self) -> None:
        with sqlite3.connect(self.database_path) as connection:
            connection.execute("INSERT INTO entities VALUES('work-000001','work')")
            connection.execute(
                "INSERT INTO works(entity_id,medium) VALUES('work-000001','unknown')"
            )
            for field, value in (("country_code", "US"), ("language_code", "en")):
                connection.execute(
                    "INSERT INTO provider_general_facts("
                    "entity_id,provider,external_id,field,value_json) VALUES(?,?,?,?,?)",
                    ("work-000001", "wikidata", "Q20", field, json.dumps(value)),
                )
            anomalous, rows = anomalies(
                connection,
                {("wikidata", "Q20"): {"country_code": "GB", "language_code": "en"}},
            )
        self.assertEqual(anomalous, set())
        self.assertEqual(rows, [])
        self.assertEqual(
            date_value(["1971", "1971-05", "1972-01-03"]),
            (1971, "month", "1971-05"),
        )
        with sqlite3.connect(self.database_path) as connection:
            self.assertEqual(tail_metrics(connection), (1, 1, 0))

    def test_fixed_bundle_costs_and_recurrent_actor_gate(self) -> None:
        identities = {
            cluster: [Identity("wikidata", "item", f"Q{cluster}")]
            for cluster in range(1, 30)
        }
        album_identity = identities[4][0]
        album = Graph(
            types={1: "person", 2: "work", 4: "work", 5: "work"},
            identities={key: identities[key] for key in (1, 2, 4, 5)},
            names={},
            facts={
                4: {"medium": [("album", "wikidata", album_identity)]},
            },
            media={},
            edges=[
                Edge(2, 1, "wikidata", "credit", "artist", {}),
                Edge(4, 1, "wikidata", "credit", "artist", {}),
                Edge(5, 1, "wikidata", "credit", "artist", {}),
                Edge(5, 4, "wikidata", "work_membership", "track_of", {}),
            ],
        )
        conflicts: list[dict[str, object]] = []
        selected, _ranking, spent = ordinary_selection(
            album, {2}, 4, set(), conflicts, set()
        )
        self.assertEqual((selected, spent), (set(), 0))
        selected, _ranking, spent = ordinary_selection(
            album, {2}, 5, set(), conflicts, set()
        )
        self.assertEqual((selected, spent), ({4, 5}, 5))
        forward = {5: {4}}
        reverse = {4: {5}}
        self.assertEqual(bundle_for(album, 4, forward, reverse, conflicts), ({4, 5}, 5))

        episode_ids = set(range(10, 20))
        series_edges = [
            Edge(episode, 9, "wikidata", "work_membership", "episode_of", {})
            for episode in episode_ids
        ]
        for actor in (20, 21):
            series_edges.extend(
                Edge(episode, actor, "imdb", "credit", "actor", {})
                for episode in sorted(episode_ids)[:8]
            )
        series = Graph(
            types={
                9: "work",
                **{episode: "work" for episode in episode_ids},
                20: "person",
                21: "person",
            },
            identities={
                key: identities[key]
                for key in ({9, 20, 21} | episode_ids)
            },
            names={},
            facts={},
            media={},
            edges=series_edges,
        )
        self.assertFalse(series_allowed(series, 9, episode_ids | {9}))
        series.edges = [edge for edge in series.edges if edge.object != 21]
        self.assertTrue(series_allowed(series, 9, episode_ids | {9}))

    def test_provider_disagreement_is_reported_without_dropping_field(self) -> None:
        self.graph.ingest(
            "wikidata",
            [
                {
                    **record(
                        "Q30",
                        "work",
                        name="Conflicted work",
                        facts=[{"field": "country_code", "value": "US"}],
                    ),
                    "identifiers": [
                        {
                            "provider": "imdb",
                            "namespace": "title",
                            "external_id": "tt30",
                        }
                    ],
                }
            ],
        )
        self.graph.ingest(
            "imdb",
            [
                {
                    "id": {
                        "provider": "imdb",
                        "namespace": "title",
                        "external_id": "tt30",
                    },
                    "entity_type": "work",
                    "identifiers": [],
                    "names": [],
                    "facts": [{"field": "country_code", "value": "GB"}],
                    "media": [],
                    "edges": [],
                }
            ],
        )
        self.write_priority({"wikidata": ["Q30"]})

        report = materialize(
            self.graph_path,
            self.database_path,
            self.priority_path,
            self.report_path,
        )

        self.assertEqual(len(report["conflicts"]), 1)
        self.assertEqual(report["conflicts"][0]["field"], "country_code")
        self.assertEqual(report["conflicts"][0]["selected_value"], "GB")
        with sqlite3.connect(self.database_path) as connection:
            self.assertEqual(
                connection.execute("SELECT country_code FROM works").fetchone()[0],
                "GB",
            )

    def test_refresh_does_not_bypass_budget_for_new_membership_bundle(self) -> None:
        with sqlite3.connect(self.database_path) as connection:
            connection.execute("INSERT INTO entities VALUES('work-000001','work')")
            connection.execute(
                "INSERT INTO works(entity_id,medium) VALUES('work-000001','unknown')"
            )
            connection.execute(
                "INSERT INTO external_ids(entity_id,scheme,value) "
                "VALUES('work-000001','wikidata','Q40')"
            )
        self.graph.ingest(
            "wikidata",
            [
                record(
                    "Q40",
                    "work",
                    name="Existing episode",
                    facts=[{"field": "language_code", "value": "en"}],
                    edges=[edge("Q41", "work", "work_membership", "episode_of")],
                ),
                record("Q41", "work", name="New series"),
            ],
        )
        self.write_priority({})

        report = materialize(
            self.graph_path,
            self.database_path,
            self.priority_path,
            self.report_path,
        )

        self.assertEqual(report["selection"]["ordinary_budget"], 0)
        with sqlite3.connect(self.database_path) as connection:
            self.assertEqual(connection.execute("SELECT count(*) FROM works").fetchone()[0], 1)
            self.assertEqual(
                connection.execute(
                    "SELECT language_code FROM works WHERE entity_id='work-000001'"
                ).fetchone()[0],
                "en",
            )
            self.assertEqual(
                connection.execute("SELECT count(*) FROM work_memberships").fetchone()[0],
                0,
            )

    def test_changed_non_empty_provider_credit_set_replaces_prior_set(self) -> None:
        self.graph.ingest(
            "wikidata",
            [
                record("Q50", "person", name="Old artist"),
                record(
                    "Q51",
                    "work",
                    name="Work",
                    edges=[edge("Q50", "person", "credit", "artist")],
                ),
            ],
        )
        self.write_priority({"wikidata": ["Q51"]})
        materialize(
            self.graph_path,
            self.database_path,
            self.priority_path,
            self.report_path,
        )

        replacement_graph_path = self.root / "replacement-observations.sqlite"
        replacement_graph = ObservationGraph.create(replacement_graph_path)
        replacement_graph.ingest(
            "wikidata",
            [
                record("Q51", "work", name="Work"),
                record("Q52", "person", name="New artist"),
                record(
                    "Q51",
                    "work",
                    name="Work",
                    edges=[edge("Q52", "person", "credit", "artist")],
                ),
            ],
        )
        second_report = self.root / "second-report.json"
        materialize(
            replacement_graph_path,
            self.database_path,
            self.priority_path,
            second_report,
        )

        with sqlite3.connect(self.database_path) as connection:
            self.assertEqual(
                connection.execute(
                    "SELECT x.value FROM credits c "
                    "JOIN external_ids x ON x.entity_id=c.agent_id "
                    "WHERE x.scheme='wikidata'"
                ).fetchall(),
                [("Q52",)],
            )
            self.assertIsNone(
                connection.execute(
                    "SELECT 1 FROM external_ids WHERE scheme='wikidata' AND value='Q50'"
                ).fetchone()
            )


if __name__ == "__main__":
    unittest.main()
