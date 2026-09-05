from __future__ import annotations

import sqlite3
import tempfile
import unittest
from pathlib import Path

from scripts.provider_fixture_adapters import (
    ProviderAdapterError,
    expand_imdb_title_episode,
    expand_musicbrainz_release,
    normalize_imdb_name_basics,
    normalize_imdb_title_basics,
    normalize_imdb_title_episode,
    normalize_imdb_title_principal,
    normalize_musicbrainz_artist,
    normalize_musicbrainz_release_group,
    normalize_open_library_author,
    normalize_open_library_work,
)
from scripts.provider_observation_graph import ObservationGraph, ObservationGraphError


def identity(provider: str, namespace: str, external_id: str) -> dict[str, str]:
    return {
        "provider": provider,
        "namespace": namespace,
        "external_id": external_id,
    }


class ProviderObservationGraphTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="arachne-provider-graph-")
        self.path = Path(self.temporary.name) / "observations.sqlite"
        self.graph = ObservationGraph.create(self.path)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def cluster_for(self, provider: str, namespace: str, external_id: str) -> int:
        with sqlite3.connect(self.path) as connection:
            row = connection.execute(
                "SELECT cluster_id FROM provider_ids "
                "WHERE provider=? AND namespace=? AND external_id=?",
                (provider, namespace, external_id),
            ).fetchone()
        self.assertIsNotNone(row)
        return int(row[0])

    def test_real_provider_shapes_unify_only_through_exact_identifiers(self) -> None:
        artist_mbid = "11111111-1111-1111-1111-111111111111"
        release_group_mbid = "22222222-2222-2222-2222-222222222222"

        self.graph.ingest(
            "musicbrainz",
            [
                normalize_musicbrainz_artist(
                    {
                        "id": artist_mbid,
                        "type": "Person",
                        "name": "Example Artist",
                        "aliases": [{"name": "E. Artist", "locale": "en"}],
                        "life-span": {"begin": "1915-12-12", "end": "1987"},
                        "country": "US",
                        "relations": [
                            {
                                "url": {
                                    "resource": "https://www.wikidata.org/wiki/Q42"
                                }
                            }
                        ],
                    }
                ),
                normalize_musicbrainz_release_group(
                    {
                        "id": release_group_mbid,
                        "title": "Example Work",
                        "primary-type": "Album",
                        "first-release-date": "1971-03",
                        "artist-credit": [
                            {
                                "name": "Example Artist",
                                "artist": {"id": artist_mbid},
                            }
                        ],
                        "relations": [
                            {
                                "url": {
                                    "resource": "https://www.wikidata.org/wiki/Q100"
                                }
                            }
                        ],
                    }
                ),
            ],
        )
        self.graph.ingest(
            "open-library",
            [
                normalize_open_library_author(
                    {
                        "key": "/authors/OL1A",
                        "name": "Example Author",
                        "alternate_names": ["E. Author"],
                        "birth_date": "12 December 1915",
                        "remote_ids": {
                            "wikidata": "Q42",
                            "viaf": "1234",
                        },
                        "photos": [17],
                    }
                ),
                normalize_open_library_work(
                    {
                        "key": "/works/OL1W",
                        "title": "Example Work",
                        "first_publish_date": "1971",
                        "remote_ids": {"wikidata": "Q100"},
                        "covers": [23],
                        "authors": [{"author": {"key": "/authors/OL1A"}}],
                    }
                ),
            ],
        )
        self.graph.ingest(
            "imdb",
            [
                normalize_imdb_name_basics(
                    {
                        "nconst": "nm0000001",
                        "primaryName": "Example Performer",
                        "birthYear": "1915",
                        "deathYear": r"\N",
                        "primaryProfession": "actor,writer",
                    }
                ),
                normalize_imdb_title_basics(
                    {
                        "tconst": "tt0000001",
                        "titleType": "tvMovie",
                        "primaryTitle": "Example Work",
                        "originalTitle": "Original Example",
                        "isAdult": "0",
                        "startYear": "1971",
                        "endYear": r"\N",
                        "runtimeMinutes": "94",
                        "genres": "Drama,Music",
                    }
                ),
                normalize_imdb_title_principal(
                    {
                        "tconst": "tt0000001",
                        "ordering": "1",
                        "nconst": "nm0000001",
                        "category": "actor",
                        "job": r"\N",
                        "characters": '["Example Character"]',
                    }
                ),
                normalize_imdb_title_episode(
                    {
                        "tconst": "tt0000002",
                        "parentTconst": "tt0000003",
                        "seasonNumber": "2",
                        "episodeNumber": "7",
                    }
                ),
            ],
        )

        # Wikidata is the exact crosswalk source for IMDb IDs. The graph does
        # not infer this join from labels, dates, or titles.
        self.graph.ingest(
            "wikidata",
            [
                {
                    "id": identity("wikidata", "item", "Q42"),
                    "entity_type": "person",
                    "identifiers": [identity("imdb", "name", "nm0000001")],
                    "names": [{"type": "label", "language": "en", "value": "Artist"}],
                    "facts": [],
                    "media": [],
                    "edges": [],
                },
                {
                    "id": identity("wikidata", "item", "Q100"),
                    "entity_type": "work",
                    "identifiers": [identity("imdb", "title", "tt0000001")],
                    "names": [],
                    "facts": [],
                    "media": [],
                    "edges": [],
                },
            ],
        )

        person_cluster = self.cluster_for("wikidata", "item", "Q42")
        self.assertEqual(
            {
                self.cluster_for("musicbrainz", "artist", artist_mbid),
                self.cluster_for("open-library", "author", "OL1A"),
                self.cluster_for("imdb", "name", "nm0000001"),
            },
            {person_cluster},
        )
        work_cluster = self.cluster_for("wikidata", "item", "Q100")
        self.assertEqual(
            {
                self.cluster_for("musicbrainz", "release_group", release_group_mbid),
                self.cluster_for("open-library", "work", "OL1W"),
                self.cluster_for("imdb", "title", "tt0000001"),
            },
            {work_cluster},
        )

        with sqlite3.connect(self.path) as connection:
            name_providers = {
                row[0]
                for row in connection.execute(
                    "SELECT observation_provider FROM clustered_provider_names "
                    "WHERE cluster_id=?",
                    (person_cluster,),
                )
            }
            self.assertEqual(
                name_providers,
                {"imdb", "musicbrainz", "open-library", "wikidata"},
            )

            facts = {
                (row[0], row[1], row[2])
                for row in connection.execute(
                    "SELECT observation_provider,field,value_json "
                    "FROM clustered_provider_facts WHERE cluster_id IN (?,?)",
                    (person_cluster, work_cluster),
                )
            }
            self.assertIn(("imdb", "birth_year", "1915"), facts)
            self.assertIn(("musicbrainz", "birth_date", '"1915-12-12"'), facts)
            self.assertIn(
                ("open-library", "birth_date", '"12 December 1915"'), facts
            )
            self.assertIn(("imdb", "runtime_minutes", "94"), facts)

            media = {
                (row[0], row[1], row[2])
                for row in connection.execute(
                    "SELECT observation_provider,media_kind,media_key "
                    "FROM clustered_provider_media"
                )
            }
            self.assertIn(("open-library", "portrait", "17"), media)
            self.assertIn(("open-library", "cover", "23"), media)

            edges = {
                (row[0], row[1], row[2])
                for row in connection.execute(
                    "SELECT observation_provider,relation_family,relation_type "
                    "FROM clustered_provider_edges"
                )
            }
            self.assertIn(("imdb", "credit", "actor"), edges)
            self.assertIn(("musicbrainz", "credit", "performer"), edges)
            self.assertIn(("open-library", "credit", "author"), edges)
            self.assertIn(("imdb", "work_membership", "episode_of"), edges)

            identity_sources = {
                row[0]
                for row in connection.execute(
                    "SELECT DISTINCT observation_provider "
                    "FROM provider_identity_links"
                )
            }
            self.assertEqual(
                identity_sources, {"musicbrainz", "open-library", "wikidata"}
            )
            self.assertEqual(connection.execute("PRAGMA foreign_key_check").fetchall(), [])
            self.assertEqual(connection.execute("PRAGMA integrity_check").fetchone()[0], "ok")

    def test_provider_ingest_rolls_back_every_record_on_late_failure(self) -> None:
        self.graph.ingest(
            "imdb",
            [
                normalize_imdb_name_basics(
                    {
                        "nconst": "nm0000001",
                        "primaryName": "Existing Person",
                        "birthYear": r"\N",
                        "deathYear": r"\N",
                        "primaryProfession": r"\N",
                    }
                )
            ],
        )
        before = self.graph.counts()

        valid = normalize_musicbrainz_artist(
            {
                "id": "33333333-3333-3333-3333-333333333333",
                "type": "Person",
                "name": "Transient Person",
                "relations": [],
            }
        )
        invalid = {
            "id": identity("musicbrainz", "artist", "bad-late-record"),
            "entity_type": "person",
            "identifiers": [],
            "names": [],
            "facts": [{"field": "birth_date", "value": {"not-json": {1, 2}}}],
            "media": [],
            "edges": [],
        }

        with self.assertRaises(ObservationGraphError):
            self.graph.ingest("musicbrainz", [valid, invalid])

        self.assertEqual(self.graph.counts(), before)
        with sqlite3.connect(self.path) as connection:
            self.assertEqual(connection.execute("PRAGMA foreign_key_check").fetchall(), [])
            self.assertEqual(connection.execute("PRAGMA integrity_check").fetchone()[0], "ok")

    def test_imdb_adapter_rejects_malformed_exact_identifiers(self) -> None:
        with self.assertRaises(ProviderAdapterError):
            normalize_imdb_title_basics(
                {
                    "tconst": "not-a-title-id",
                    "primaryTitle": "Invalid",
                    "titleType": "movie",
                }
            )

    def test_dump_relations_expand_explicit_seasons_and_album_tracks(self) -> None:
        episode_records = expand_imdb_title_episode(
            {
                "tconst": "tt0000002",
                "parentTconst": "tt0000003",
                "seasonNumber": "2",
                "episodeNumber": "7",
            }
        )
        self.assertEqual(len(episode_records), 2)
        self.assertEqual(
            [record["edges"][0]["relation_type"] for record in episode_records],
            ["episode_of", "season_of"],
        )
        self.assertEqual(
            episode_records[0]["edges"][0]["target"]["external_id"],
            "tt0000003:season:2",
        )

        tracks = expand_musicbrainz_release(
            {
                "id": "release-id",
                "release-group": {"id": "album-id"},
                "media": [
                    {
                        "position": 1,
                        "tracks": [
                            {
                                "position": 3,
                                "title": "Track title",
                                "recording": {"id": "recording-id"},
                            }
                        ],
                    }
                ],
            }
        )
        self.assertEqual(len(tracks), 1)
        self.assertEqual(tracks[0]["edges"][0]["relation_type"], "track_of")
        self.assertEqual(
            tracks[0]["edges"][0]["metadata"],
            {"position": 3, "position_text": "1.3"},
        )

    def test_untyped_musicbrainz_artist_remains_ambiguous(self) -> None:
        record = normalize_musicbrainz_artist(
            {
                "id": "33333333-3333-3333-3333-333333333333",
                "type": None,
                "name": "Unknown artist kind",
            }
        )
        self.assertEqual(record["entity_type"], "unknown")


if __name__ == "__main__":
    unittest.main()
