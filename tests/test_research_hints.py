from __future__ import annotations

import gzip
import hashlib
import importlib.util
import json
import sqlite3
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from xml.etree import ElementTree

from scripts.hint_vocabulary import (
    AuthorityTerm,
    Concordance,
    HintVocabularyError,
    SqliteConcordance,
    compile_vocabulary,
    load_concordance,
    normalize_vocabulary_id,
)
from scripts.provider_fixture_adapters import (
    expand_discogs_release,
    expand_musicbrainz_release,
    musicbrainz_release_labels,
    expand_open_library_edition,
    normalize_discogs_artist,
    normalize_discogs_label,
    normalize_discogs_master,
    normalize_gnd_entity,
    normalize_imdb_title_aka,
    normalize_imdb_title_basics,
    normalize_imdb_title_crew,
    normalize_musicbrainz_label,
    normalize_musicbrainz_release_group,
    normalize_musicbrainz_work,
    normalize_open_library_redirect,
    normalize_open_library_work,
)
from scripts.provider_fixture_adapters import ProviderAdapterError
from scripts.provider_observation_graph import ObservationGraph, ObservationGraphError
from scripts.research_hints import (
    ResearchHintError,
    build,
    render_work,
    work_hints,
    work_queue,
)
from scripts.run_provider_pass import ProviderPassError, run_pass


ROOT = Path(__file__).resolve().parents[1]


def identity(provider: str, namespace: str, external_id: str) -> dict[str, str]:
    return {"provider": provider, "namespace": namespace, "external_id": external_id}


def work_record(
    provider: str,
    namespace: str,
    external_id: str,
    *,
    identifiers: list[dict[str, str]] | None = None,
    signals: list[dict[str, object]] | None = None,
    edges: list[dict[str, object]] | None = None,
) -> dict[str, object]:
    return {
        "id": identity(provider, namespace, external_id),
        "entity_type": "work",
        "identifiers": identifiers or [],
        "names": [{"type": "label", "value": f"Work {external_id}"}],
        "facts": [],
        "media": [],
        "edges": edges or [],
        "signals": signals or [],
    }


def concept(value: str, family: str, signal_type: str, **extra: object) -> dict[str, object]:
    return {"kind": "concept", "family": family, "type": signal_type, "value": value, **extra}


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


class SignalAdapterTests(unittest.TestCase):
    def test_semantic_provider_values_are_signals_never_general_facts(self) -> None:
        imdb = normalize_imdb_title_basics(
            {"tconst": "tt0000001", "primaryTitle": "A", "genres": "Drama,Film-Noir"}
        )
        self.assertNotIn("genres", {fact["field"] for fact in imdb["facts"]})
        self.assertEqual(
            [(s["family"], s["type"], s["value"]) for s in imdb["signals"]],
            [("genre", "imdb_genre", "Drama"), ("genre", "imdb_genre", "Film-Noir")],
        )

        work = normalize_open_library_work(
            {
                "key": "/works/OL1W",
                "title": "Book",
                "subjects": ["Alienation", "Accessible book", "nyt:fiction", "Fiction"],
                "subject_places": ["Dublin"],
            }
        )
        self.assertEqual(
            [(s["family"], s["value"]) for s in work["signals"]],
            [("keyword", "Alienation"), ("keyword", "Fiction"), ("setting", "Dublin")],
        )

        release_group = normalize_musicbrainz_release_group(
            {
                "id": "rg-1",
                "title": "Album",
                "relations": [
                    {"type": "review", "url": {"resource": "https://example.org/review"}},
                    {"type": "purchase for download", "url": {"resource": "https://shop"}},
                    {"type": "wikidata", "url": {"resource": "https://www.wikidata.org/wiki/Q7"}},
                ],
            }
        )
        self.assertEqual(release_group["identifiers"], [identity("wikidata", "item", "Q7")])
        self.assertEqual(
            [(s["kind"], s["url"], s["metadata"]["lead_kind"]) for s in release_group["signals"]],
            [("source_lead", "https://example.org/review", "review")],
        )

    def test_graph_rejects_signals_that_would_be_semantic_assertions(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            graph = ObservationGraph.create(Path(temporary) / "graph.sqlite")
            for signal in (
                {"kind": "concept", "type": "imdb_genre", "value": "Drama"},
                {"kind": "assertion", "family": "genre", "type": "x", "value": "Drama"},
                {"kind": "concept", "family": "vibe", "type": "x", "value": "Drama"},
                concept("Drama", "genre", "x", vocabulary_id="no-scheme"),
            ):
                with self.assertRaises(ObservationGraphError):
                    graph.ingest("imdb", [work_record("imdb", "title", "tt1", signals=[signal])])
            self.assertEqual(graph.counts()["provider_signals"], 0)

    def test_secondary_general_adapters(self) -> None:
        aka = normalize_imdb_title_aka(
            {"titleId": "tt1", "title": "Titre", "language": "FR", "isOriginalTitle": "0",
             "types": r"\N"}
        )
        self.assertEqual(aka["names"], [{"type": "alias", "value": "Titre", "language": "fr"}])
        self.assertIsNone(
            normalize_imdb_title_aka(
                {"titleId": "tt1", "title": "Region only", "language": r"\N",
                 "isOriginalTitle": "0", "region": "US"}
            )
        )
        crew = normalize_imdb_title_crew(
            {"tconst": "tt1", "directors": "nm1,nm2", "writers": r"\N"}
        )
        self.assertEqual(
            [(edge["relation_type"], edge["target"]["external_id"]) for edge in crew["edges"]],
            [("director", "nm1"), ("director", "nm2")],
        )

        work = normalize_musicbrainz_work(
            {
                "id": "w1",
                "title": "Song",
                "relations": [
                    {"type": "composer", "artist": {"id": "a1"}},
                    {"type": "performance", "recording": {"id": "r1"}},
                ],
            }
        )
        self.assertEqual([edge["relation_type"] for edge in work["edges"]], ["composer"])
        label = normalize_musicbrainz_label({"id": "l1", "name": "Label", "country": "GB"})
        self.assertEqual(label["entity_type"], "organization")

        editions = expand_open_library_edition(
            {"key": "/books/OL1M", "publish_date": "1954", "works": [{"key": "/works/OL1W"}]}
        )
        self.assertEqual(editions[0]["id"], identity("open-library", "work", "OL1W"))
        self.assertEqual(editions[0]["facts"], [{"field": "original_date", "value": "1954"}])
        redirect = normalize_open_library_redirect(
            {"key": "/works/OL1W", "location": "/works/OL2W"}
        )
        self.assertEqual(redirect["id"], identity("open-library", "work", "OL2W"))
        self.assertEqual(redirect["identifiers"], [identity("open-library", "work", "OL1W")])

    def test_discogs_masters_releases_artists_and_labels(self) -> None:
        master = normalize_discogs_master(
            ElementTree.fromstring(
                '<master id="42"><main_release>7</main_release>'
                "<artists><artist><id>9</id><name>Band (2)</name><anv>The Band</anv></artist>"
                "<artist><id>194</id><name>Various</name></artist></artists>"
                "<genres><genre>Rock</genre></genres>"
                "<styles><style>Post-Punk</style><style>Darkwave</style></styles>"
                "<year>1980</year><title>Album</title></master>"
            )
        )
        self.assertEqual(master["id"], identity("discogs", "master", "42"))
        self.assertEqual(master["facts"], [{"field": "original_date", "value": "1980"}])
        self.assertEqual(
            [(s["type"], s["value"]) for s in master["signals"]],
            [("discogs_style", "Post-Punk"), ("discogs_style", "Darkwave"),
             ("discogs_genre", "Rock")],
        )
        self.assertEqual(
            [(e["target"]["external_id"], e["metadata"]["credited_as"]) for e in master["edges"]],
            [("9", "The Band")],
        )

        release = expand_discogs_release(
            ElementTree.fromstring(
                '<release id="7"><title>Album</title><released>1979-11-00</released>'
                '<formats><format name="Vinyl"><descriptions><description>LP</description>'
                "<description>Album</description></descriptions></format></formats>"
                '<master_id is_main_release="true">42</master_id></release>'
            )
        )
        self.assertEqual(release[0]["id"], identity("discogs", "master", "42"))
        self.assertEqual(
            release[0]["facts"],
            [{"field": "original_date", "value": "1979-11"},
             {"field": "work_type", "value": "album"}],
        )
        self.assertEqual(
            expand_discogs_release(ElementTree.fromstring('<release id="8"><title>X</title></release>')),
            [],
        )

        artist = normalize_discogs_artist(
            ElementTree.fromstring(
                "<artist><id>9</id><name>Band (2)</name>"
                '<members><id>10</id><name id="10">Member</name></members></artist>'
            )
        )
        self.assertEqual((artist["entity_type"], artist["names"][0]["value"]), ("group", "Band"))
        member = normalize_discogs_artist(
            ElementTree.fromstring(
                '<artist><id>10</id><name>Member</name><groups><name id="9">Band</name></groups></artist>'
            )
        )
        self.assertEqual(member["entity_type"], "unknown")
        self.assertEqual(member["edges"][0]["relation_type"], "member_of")
        label = normalize_discogs_label(
            ElementTree.fromstring(
                '<label><id>3</id><name>Sub</name><parentLabel id="1">Parent</parentLabel></label>'
            )
        )
        self.assertEqual(label["edges"][0]["relation_type"], "subsidiary_of")


    def test_discogs_pages_keep_useful_third_party_links_only(self) -> None:
        artist = normalize_discogs_artist(
            ElementTree.fromstring(
                "<artist><id>9</id><name>Band</name><urls>"
                "<url>https://en.wikipedia.org/wiki/Band</url>"
                "<url>https://www.facebook.com/band</url>"
                "<url>https://www.discogs.com/artist/9</url>"
                "<url>http://band.example.org/press</url>"
                "<url>not-a-url</url></urls></artist>"
            )
        )
        self.assertEqual(
            [(s["kind"], s["type"], s["metadata"].get("lead_kind")) for s in artist["signals"]],
            [
                ("source_lead", "discogs_url", "article"),
                ("search_lead", "discogs_url", None),
            ],
        )
        label = normalize_discogs_label(
            ElementTree.fromstring(
                "<label><id>3</id><name>Imprint</name>"
                "<urls><url>https://www.allmusic.com/label/imprint</url></urls></label>"
            )
        )
        self.assertEqual(label["signals"][0]["metadata"]["lead_kind"], "catalogue")

    def test_musicbrainz_releases_sharpen_dates_without_edition_labels(self) -> None:
        release = {
            "id": "release-1",
            "status": "Official",
            "date": "1980-06-21",
            "release-events": [{"date": "1979-11"}],
            "release-group": {
                "id": "rg1",
                "primary-type": "Album",
                # The aggregate may include excluded statuses; never copied.
                "first-release-date": "1970",
            },
            "label-info": [{"label": {"id": "l1"}, "catalog-number": "X-1"}],
            "media": [],
        }
        records = expand_musicbrainz_release(release)
        self.assertEqual(records[0]["id"], identity("musicbrainz", "release_group", "rg1"))
        self.assertEqual(
            records[0]["facts"],
            [
                {"field": "original_date", "value": "1980-06-21"},
                {"field": "original_date", "value": "1979-11"},
                {"field": "work_type", "value": "album"},
            ],
        )
        # Release-specific labels never become release-group (work) credits.
        self.assertEqual(records[0]["edges"], [])
        self.assertEqual(records[0]["unpersisted"], ["release_label"])
        self.assertEqual(
            musicbrainz_release_labels(release),
            [{"label_id": "l1", "catalog_number": "X-1"}],
        )
        # Bootleg, unknown, and missing statuses are fail-closed: no dates.
        for status in ("Bootleg", "Pseudo-Release", None, 7):
            dated = expand_musicbrainz_release(
                {"id": "r2", "status": status, "date": "1970",
                 "release-group": {"id": "rg1"}}
            )
            self.assertEqual([record["facts"] for record in dated], [[]])
            self.assertEqual(dated[0]["unpersisted"], ["excluded_status_release_date"])
        group = normalize_musicbrainz_release_group(
            {"id": "rg1", "title": "Album", "first-release-date": "1970"}
        )
        self.assertNotIn("original_date", {fact["field"] for fact in group["facts"]})
        self.assertEqual(group["unpersisted"], ["first_release_date"])

    def test_gnd_records_bridge_identity_and_keep_subject_ids(self) -> None:
        record = normalize_gnd_entity(
            {
                "gnd_id": "118540238",
                "entity_type": "differentiated person",
                "preferred_name": "Goethe, Johann Wolfgang von",
                "variant_names": ["Goethe, J. W. von"],
                "dates": {"birth": "1749-08-28"},
                "crosswalks": {"wikidata": "Q5879", "viaf": "24602065"},
                "subjects": [
                    {"gnd_id": "4074195-3", "label": "Lyrik"},
                    {"label": "dropped without an ID"},
                ],
            }
        )
        self.assertEqual(record["id"], identity("gnd", "entity", "118540238"))
        self.assertEqual(record["entity_type"], "person")
        self.assertEqual(
            record["identifiers"],
            [identity("wikidata", "item", "Q5879"), identity("viaf", "entity", "24602065")],
        )
        self.assertEqual(
            [(s["type"], s["vocabulary_id"]) for s in record["signals"]],
            [("gnd_subject", "gnd:4074195-3")],
        )
        with self.assertRaises(ProviderAdapterError):
            normalize_gnd_entity({"gnd_id": "not-a-gnd", "preferred_name": "X"})
        # Types the product cannot represent safely stay unknown.
        for native in ("conference_or_event", "family", "person_other", "place"):
            self.assertEqual(
                normalize_gnd_entity(
                    {"gnd_id": "1234", "entity_type": native, "preferred_name": "X"}
                )["entity_type"],
                "unknown",
            )


class HintVocabularyTests(unittest.TestCase):
    def concordance(self) -> Concordance:
        return Concordance(
            (
                AuthorityTerm(
                    "topical",
                    "Alienation (Social psychology)",
                    {"lcsh": "sh85003435", "gnd": "4014670-1"},
                    ("Entfremdung",),
                ),
                AuthorityTerm("genre_form", "Film noir", {"lcgft": "gf2011026321"}),
            )
        )

    def test_exact_resolution_order_never_crosses_term_kinds(self) -> None:
        vocabulary = self.concordance()
        # An authority ID resolves before any label, including across languages.
        by_id = vocabulary.resolve("keyword", "Entfremdung", "https://d-nb.info/gnd/4014670-1")
        self.assertEqual(by_id.vocabulary_id, "lcsh:sh85003435")
        self.assertEqual(
            vocabulary.resolve("keyword", "entfremdung", None).vocabulary_id,
            "lcsh:sh85003435",
        )
        # A topical term never answers a genre/form hint, and vice versa.
        self.assertIsNone(vocabulary.resolve("genre", "Entfremdung", None))
        self.assertIsNone(vocabulary.resolve("theme", "Film noir", None))
        self.assertEqual(
            vocabulary.resolve("style", "film noir", None).term_kind, "genre_form"
        )
        # An unknown authority ID still identifies its own term; a crosswalk hub
        # such as Wikidata is not an authority vocabulary.
        unmapped = vocabulary.resolve("genre", "Folk horror", "lcgft:gf2014026363")
        self.assertEqual((unmapped.term_kind, unmapped.label), ("genre_form", "Folk horror"))
        self.assertIsNone(normalize_vocabulary_id("wikidata:Q1"))
        self.assertIsNone(vocabulary.resolve("keyword", "Unmapped label", None))

    def test_reviewed_concordance_artifact_is_closed(self) -> None:
        shipped = Concordance.load(ROOT / "contracts/artifacts/hint_vocabulary.example.json")
        self.assertEqual(len(shipped.terms), 3)
        self.assertEqual(shipped.generic_ids, frozenset({"wikidata:Q11424"}))
        # A weaker mapping is context only: the AAT ID identifies its own term,
        # never the concordance term it is only related to.
        self.assertEqual(
            shipped.resolve("keyword", "x", "aat:300055520").vocabulary_ids,
            {"aat": "300055520"},
        )
        self.assertEqual(
            shipped.resolve("keyword", "Entfremdung", None).related_ids,
            [{"scheme": "aat", "id": "300055520", "match": "related"}],
        )
        with tempfile.TemporaryDirectory() as temporary:
            for document in (
                {"artifact_type": "other", "terms": []},
                # Latest-only: an old versioned document is rebuilt, not read.
                {"artifact_type": "hint_vocabulary_v1", "format_version": 1, "terms": []},
                {"artifact_type": "hint_vocabulary", "format_version": 1, "terms": []},
                {"artifact_type": "hint_vocabulary",
                 "terms": [{"term_kind": "topical", "label": "A", "ids": {"nope": "1"}}]},
                {"artifact_type": "hint_vocabulary",
                 "terms": [{"term_kind": "vibe", "label": "A", "ids": {"lcsh": "1"}}]},
                {"artifact_type": "hint_vocabulary",
                 "terms": [{"term_kind": "topical", "label": "A", "ids": {"lcsh": "1"},
                            "unknown": True}]},
                {"artifact_type": "hint_vocabulary",
                 "terms": [{"term_kind": "topical", "label": "A", "ids": {"lcsh": "1"},
                            "related_ids": [{"scheme": "gnd", "id": "2",
                                             "match": "exact"}]}]},
            ):
                path = Path(temporary) / "vocabulary.json"
                path.write_text(json.dumps(document), encoding="utf-8")
                with self.assertRaises(HintVocabularyError):
                    Concordance.load(path)
        with self.assertRaises(HintVocabularyError):
            Concordance(
                (
                    AuthorityTerm("topical", "A", {"lcsh": "sh1"}),
                    AuthorityTerm("topical", "B", {"lcsh": "sh1"}),
                )
            )


class WikidataSignalTests(unittest.TestCase):
    def test_movement_and_genre_profiles_become_vocabulary_signals(self) -> None:
        spec = importlib.util.spec_from_file_location(
            "build_external_graph", ROOT / "hpc/wikidata/build_external_graph.py"
        )
        module = importlib.util.module_from_spec(spec)
        assert spec.loader is not None
        spec.loader.exec_module(module)
        signals = module.profile_signals(
            {
                "genres": ["Q130232", "bad"],
                "movements": ["Q37068"],
                "main_subjects": ["Q131691"],
                "classes": ["Q11424"],
            }
        )
        self.assertEqual(
            [(s["type"], s["family"], s["category"], s["vocabulary_id"],
              s["metadata"]["property_id"]) for s in signals],
            [
                ("wikidata_movement", "movement", "movement", "wikidata:Q37068", "P135"),
                ("wikidata_genre", "genre", "genre", "wikidata:Q130232", "P136"),
                # P921 keeps its provider-native category; theme is analytical.
                ("wikidata_main_subject", "theme", "main_subject", "wikidata:Q131691",
                 "P921"),
            ],
        )
        self.assertEqual(module.PROFILE_ITEM_PROPERTIES["P921"], "main_subjects")


class ResearchHintBuildTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="arachne-hints-")
        self.root = Path(self.temporary.name)
        self.graph_path = self.root / "graph.sqlite"
        self.product_path = self.root / "product.sqlite"
        self.hints_path = self.root / "research-hints.sqlite"
        self.graph = ObservationGraph.create(self.graph_path)
        with sqlite3.connect(self.product_path) as product:
            product.executescript((ROOT / "schema/product.sql").read_text("utf-8"))
            for work_id, qid in (("work-000001", "Q1"), ("work-000002", "Q2")):
                product.execute("INSERT INTO entities VALUES(?, 'work')", (work_id,))
                product.execute("INSERT INTO works(entity_id,medium) VALUES(?,'album')", (work_id,))
                product.execute(
                    "INSERT INTO external_ids(entity_id,scheme,value) VALUES(?,'wikidata',?)",
                    (work_id, qid),
                )
            product.execute("INSERT INTO sources(source_type,url) VALUES('article','https://s')")
            for index in range(16):
                self.add_tag(product, "work-000002", index, "theme")
            self.add_tag(product, "work-000001", 99, "genre", centrality=50)
        self.graph.ingest(
            "wikidata",
            [
                work_record(
                    "wikidata", "item", "Q1",
                    identifiers=[identity("discogs", "master", "42")],
                    signals=[
                        concept("Q130232", "genre", "wikidata_genre",
                                vocabulary_id="wikidata:Q130232"),
                        concept("Q1", "movement", "wikidata_movement",
                                vocabulary_id="wikidata:Q37068"),
                    ],
                    edges=[
                        {
                            "target": identity("wikidata", "item", "Q50"),
                            "target_entity_type": "person",
                            "relation_family": "credit",
                            "relation_type": "performer",
                        }
                    ],
                ),
                work_record(
                    "wikidata", "item", "Q2",
                    signals=[concept("Q3", "genre", "wikidata_genre",
                                     vocabulary_id="wikidata:Q3")],
                ),
            ],
        )
        self.graph.ingest(
            "discogs",
            [
                work_record(
                    "discogs", "master", "42",
                    signals=[
                        concept("Post-Punk", "style", "discogs_style"),
                        concept("Rock", "genre", "discogs_genre"),
                    ],
                )
            ],
        )
        self.graph.ingest(
            "imdb",
            [
                {
                    "id": identity("imdb", "title", "tt1"),
                    "entity_type": "work",
                    "identifiers": [identity("wikidata", "item", "Q1")],
                    "signals": [
                        concept("Drama", "genre", "imdb_genre"),
                        concept("post punk", "style", "imdb_genre"),
                    ],
                }
            ],
        )
        self.graph.ingest(
            "musicbrainz",
            [
                {
                    "id": identity("musicbrainz", "artist", "a1"),
                    "entity_type": "person",
                    "identifiers": [identity("wikidata", "item", "Q50")],
                    "signals": [
                        {
                            "kind": "source_lead",
                            "type": "musicbrainz_url_relation",
                            "value": "https://example.org/interview",
                            "url": "https://example.org/interview",
                            "metadata": {"lead_kind": "interview"},
                        },
                        # Agent concept signals never propagate to works.
                        concept("Agent-only", "theme", "manual_concept"),
                    ],
                },
                {
                    "id": identity("musicbrainz", "release_group", "rg1"),
                    "entity_type": "work",
                    "identifiers": [identity("wikidata", "item", "Q1")],
                    "signals": [
                        concept("Unreviewed", "keyword", "musicbrainz_rating"),
                        concept("Gothic rock", "genre", "musicbrainz_tag"),
                    ],
                },
            ],
        )

    def tearDown(self) -> None:
        self.temporary.cleanup()

    @staticmethod
    def add_tag(
        product: sqlite3.Connection, work_id: str, index: int, family: str, centrality: int = 80
    ) -> None:
        concept_id = f"concept-{index + 1:06d}"
        product.execute("INSERT OR IGNORE INTO entities VALUES(?, 'concept')", (concept_id,))
        product.execute(
            "INSERT OR IGNORE INTO concepts VALUES(?,?,?)",
            (concept_id, family, f"concept-{index}"),
        )
        cursor = product.execute(
            "INSERT INTO work_concepts(work_id,concept_id,relation_type,centrality,"
            "centrality_scale) VALUES(?,?,'exemplifies',?,'graded')",
            (work_id, concept_id, centrality),
        )
        evidence = product.execute(
            "INSERT INTO evidence(source_id,exact_quote,stance) VALUES(1,?,'supports')",
            (f"quote {work_id} {index}",),
        )
        product.execute(
            "INSERT INTO work_concept_evidence(assertion_id,evidence_id) VALUES(?,?)",
            (cursor.lastrowid, evidence.lastrowid),
        )

    def hints(self) -> dict[tuple[str, str], sqlite3.Row]:
        with sqlite3.connect(self.hints_path) as connection:
            connection.row_factory = sqlite3.Row
            return {
                (row["dedup_key"], row["hint_kind"]): row
                for row in connection.execute("SELECT * FROM research_hints")
            }

    def test_under_mined_gate_specificity_dedup_and_license_gate(self) -> None:
        product_digest = digest(self.product_path)
        report = build(self.graph_path, self.product_path, self.hints_path)

        # Hints never touch canonical product state.
        self.assertEqual(digest(self.product_path), product_digest)
        self.assertEqual(report["under_mined_works"], 1)
        self.assertEqual(report["under_mined_works_with_useful_hint"], 1)
        self.assertEqual(
            report["skipped_signals"],
            {
                "license_restricted": {"musicbrainz_tag": 1},
                "no_reviewed_policy": {"musicbrainz_rating": 1},
            },
        )
        hints = self.hints()
        self.assertEqual({row["work_id"] for row in hints.values()}, {"work-000001"})
        self.assertNotIn(("label:agent only", "concept"), hints)

        post_punk = hints[("label:post punk", "concept")]
        movement = hints[("id:wikidata:Q37068", "concept")]
        # Discogs style and a differently-spelled IMDb value dedupe into one
        # hint whose independent provider origins raise its priority.
        self.assertEqual(post_punk["independent_origins"], 2)
        self.assertEqual(post_punk["assignment_quality"], "B")
        self.assertEqual(post_punk["resolution_quality"], "unresolved")
        self.assertEqual(post_punk["display_value"], "Post-Punk")
        self.assertEqual(movement["assignment_quality"], "B")
        # Generic (class E) hints are detected and counted, not persisted.
        for key in ("label:drama", "id:wikidata:Q130232", "label:rock"):
            self.assertNotIn((key, "concept"), hints)
        self.assertEqual(report["suppressed"]["suppressed_generic_hints"], 3)
        self.assertEqual(
            report["suppressed"]["suppressed_generic_signals"],
            {"discogs_genre": 1, "imdb_genre": 1, "wikidata_genre": 1},
        )
        # Every provider-native observation behind a merged hint stays auditable.
        with sqlite3.connect(self.hints_path) as connection:
            self.assertEqual(
                connection.execute(
                    "SELECT s.provider,s.raw_value,s.raw_semantic_family,s.semantic_family,"
                    "s.assignment_quality,s.resolution_basis,s.source_snapshot "
                    "FROM research_hint_signals s JOIN research_hints h ON h.id=s.hint_id "
                    "WHERE h.dedup_key='label:post punk' ORDER BY s.provider"
                ).fetchall(),
                [
                    ("discogs", "Post-Punk", "style", "style", "B", "normalized_label", None),
                    ("imdb", "post punk", "style", "style", "E", "normalized_label", None),
                ],
            )
        # The work already has an evidence-backed genre, so a genre gap is smaller.
        work = work_hints(self.hints_path, "work-000001")
        self.assertEqual(work["evidence_backed_tag_count"], 1)
        self.assertAlmostEqual(work["weighted_coverage"], 0.5 / 16)
        self.assertEqual(work["hints"][0]["value"], "Post-Punk")
        lead = work["source_leads"][0]
        self.assertEqual((lead["lead_kind"], lead["url"]), ("interview", "https://example.org/interview"))
        self.assertEqual(lead["signals"][0]["attachment"], "credited_agent")
        text = render_work(work)
        self.assertIn("Post-Punk", text)
        self.assertIn("interview https://example.org/interview", text)
        self.assertEqual([row["work_id"] for row in work_queue(self.hints_path)], ["work-000001"])
        with self.assertRaises(ResearchHintError):
            work_hints(self.hints_path, "work-000002")

    def test_generic_hints_can_be_kept_explicitly_at_negligible_priority(self) -> None:
        report = build(self.graph_path, self.product_path, self.hints_path, keep_generic=True)
        self.assertEqual(report["suppressed"]["suppressed_generic_hints"], 0)
        hints = self.hints()
        post_punk = hints[("label:post punk", "concept")]
        for key in ("label:drama", "id:wikidata:Q130232", "label:rock"):
            generic = hints[(key, "concept")]
            self.assertEqual(generic["assignment_quality"], "E")
            self.assertEqual(generic["specificity"], 0.05)
            self.assertLess(generic["research_priority"] * 50, post_punk["research_priority"])

    def test_restricted_opt_in_is_explicit_and_rebuild_is_deterministic(self) -> None:
        with self.assertRaises(ResearchHintError):
            build(self.graph_path, self.product_path, self.hints_path,
                  allow_restricted=["discogs_style"])
        build(self.graph_path, self.product_path, self.hints_path,
              allow_restricted=["musicbrainz_tag"])
        self.assertIn(("label:gothic rock", "concept"), self.hints())
        second = self.root / "again.sqlite"
        build(self.graph_path, self.product_path, second, allow_restricted=["musicbrainz_tag"])
        with sqlite3.connect(self.hints_path) as left, sqlite3.connect(second) as right:
            self.assertEqual(list(left.iterdump()), list(right.iterdump()))
        with self.assertRaises(ResearchHintError):
            build(self.graph_path, self.product_path, second)
        with self.assertRaises(ResearchHintError):
            build(self.graph_path, self.product_path, self.product_path)

    def test_shared_upstream_is_not_independent_and_manual_content_signals(self) -> None:
        self.graph.ingest(
            "open-library",
            [
                work_record(
                    "open-library", "work", "OL1W",
                    identifiers=[identity("wikidata", "item", "Q1")],
                    signals=[concept("Post-punk", "style", "open_library_subject",
                                     metadata={"upstream": "discogs"})],
                )
            ],
        )
        manual = self.root / "manual.jsonl"
        manual.write_text(
            json.dumps(
                {
                    "work_id": "work-000001",
                    "kind": "content_signal",
                    "family": "content_warning",
                    "type": "parents_guide",
                    "value": "Graphic violence",
                    "strength": 5,
                    "metadata": {"category": "violence", "severity": "severe"},
                }
            )
            + "\n"
            + json.dumps(
                {"work_id": "work-000002", "kind": "concept", "family": "theme",
                 "type": "manual_concept", "value": "Ignored"}
            )
            + "\n",
            encoding="utf-8",
        )
        report = build(self.graph_path, self.product_path, self.hints_path, manual_path=manual)
        hints = self.hints()
        self.assertEqual(hints[("label:post punk", "concept")]["independent_origins"], 2)
        guide = hints[("label:graphic violence", "content_signal")]
        self.assertEqual(guide["assignment_quality"], "B")
        self.assertIn(
            {"kind": "manual_signal_not_under_mined", "work_id": "work-000002", "line": 2},
            report["issues"],
        )
        self.assertIn("parents_guide: severe", render_work(work_hints(self.hints_path, "work-000001")))

        bad = self.root / "bad.jsonl"
        bad.write_text(
            json.dumps({"work_id": "work-000001", "kind": "concept", "family": "theme",
                        "type": "imdb_genre", "value": "Spoofed provider"}) + "\n",
            encoding="utf-8",
        )
        with self.assertRaises(ResearchHintError):
            build(self.graph_path, self.product_path, self.root / "x.sqlite", manual_path=bad)

    def test_authority_terms_dedupe_multilingual_subjects_without_fuzzy_matching(
        self,
    ) -> None:
        self.graph.ingest(
            "gnd",
            [
                work_record(
                    "gnd", "entity", "4000001-1",
                    identifiers=[identity("wikidata", "item", "Q1")],
                    signals=[concept("Entfremdung", "keyword", "gnd_subject",
                                     vocabulary_id="gnd:4014670-1")],
                )
            ],
        )
        self.graph.ingest(
            "open-library",
            [
                work_record(
                    "open-library", "work", "OL9W",
                    identifiers=[identity("wikidata", "item", "Q1")],
                    signals=[concept("Alienation (Social psychology)", "keyword",
                                     "open_library_subject")],
                )
            ],
        )
        vocabulary = ROOT / "contracts/artifacts/hint_vocabulary.example.json"

        # Without the concordance the two spellings stay separate leads.
        plain = self.root / "plain.sqlite"
        build(self.graph_path, self.product_path, plain)
        with sqlite3.connect(plain) as connection:
            self.assertEqual(
                {
                    row[0]
                    for row in connection.execute(
                        "SELECT dedup_key FROM research_hints WHERE semantic_family='keyword'"
                    )
                },
                {"id:gnd:4014670-1", "label:alienation social psychology"},
            )

        report = build(
            self.graph_path, self.product_path, self.hints_path, vocabulary_path=vocabulary
        )
        self.assertEqual(report["authority_resolved_hints"], 1)
        hint = self.hints()[("id:lcsh:sh85003435", "concept")]
        self.assertEqual(hint["independent_origins"], 2)
        # GND's own assignment is class A by policy; the Open Library subject
        # stays class C even though its term resolved exactly.
        self.assertEqual(hint["assignment_quality"], "A")
        self.assertEqual(hint["resolution_quality"], "reviewed_crosswalk")
        self.assertEqual(hint["term_kind"], "topical")
        self.assertEqual(
            json.loads(hint["authority_ids_json"]),
            {"gnd": "4014670-1", "lcsh": "sh85003435", "rameau": "FRBNF11930652"},
        )
        self.assertEqual(hint["display_value"], "Alienation (Social psychology)")
        work = work_hints(self.hints_path, "work-000001")
        resolved = next(item for item in work["hints"] if item["value"].startswith("Alienation"))
        # Every provider-native value stays visible to the miner, with how it
        # was resolved and which snapshot produced it.
        self.assertEqual(
            sorted(
                (
                    signal["provider"],
                    signal["raw_value"],
                    signal["raw_vocabulary_id"],
                    signal["assignment_quality"],
                    signal["resolution_basis"],
                )
                for signal in resolved["signals"]
            ),
            [
                ("gnd", "Entfremdung", "gnd:4014670-1", "A", "reviewed_crosswalk"),
                ("open-library", "Alienation (Social psychology)", None, "C",
                 "concordance_label"),
            ],
        )
        self.assertEqual(
            resolved["related_authority_ids"],
            [{"scheme": "aat", "id": "300055520", "match": "related"}],
        )
        text = render_work(work)
        self.assertIn("lcsh:sh85003435", text)
        self.assertIn("gnd:4014670-1", text)

    def test_generic_suppression_applies_after_authority_resolution(self) -> None:
        self.graph.ingest(
            "discogs",
            [
                work_record(
                    "discogs", "master", "42",
                    signals=[
                        # An alias of a reviewed generic term, and a raw ID of it.
                        concept("Komödie", "style", "discogs_style"),
                        concept("Lustspiel", "style", "discogs_style",
                                vocabulary_id="lcgft:gf2011026147"),
                    ],
                )
            ],
        )
        self.graph.ingest(
            "wikidata",
            [
                work_record(
                    "wikidata", "item", "Q1",
                    signals=[concept("Q11424", "theme", "wikidata_main_subject",
                                     vocabulary_id="wikidata:Q11424")],
                )
            ],
        )
        plain = self.root / "plain.sqlite"
        build(self.graph_path, self.product_path, plain)
        with sqlite3.connect(plain) as connection:
            kept = {row[0] for row in connection.execute("SELECT dedup_key FROM research_hints")}
        self.assertIn("label:komödie", kept)
        self.assertIn("id:wikidata:Q11424", kept)

        vocabulary = ROOT / "contracts/artifacts/hint_vocabulary.example.json"
        report = build(
            self.graph_path, self.product_path, self.hints_path, vocabulary_path=vocabulary
        )
        hints = self.hints()
        for key in ("id:lcgft:gf2011026147", "id:wikidata:Q11424", "label:komödie"):
            self.assertNotIn((key, "concept"), hints)
        self.assertEqual(report["suppressed"]["suppressed_generic_hints"], 5)

    def test_prolific_agent_leads_attach_to_a_bounded_number_of_works(self) -> None:
        with sqlite3.connect(self.product_path) as product:
            product.execute("INSERT INTO entities VALUES('work-000003', 'work')")
            product.execute("INSERT INTO works(entity_id,medium) VALUES('work-000003','album')")
            product.execute(
                "INSERT INTO external_ids(entity_id,scheme,value) "
                "VALUES('work-000003','wikidata','Q4')"
            )
        self.graph.ingest(
            "wikidata",
            [
                work_record(
                    "wikidata", "item", "Q4",
                    edges=[
                        {
                            "target": identity("wikidata", "item", "Q50"),
                            "target_entity_type": "person",
                            "relation_family": "credit",
                            "relation_type": "performer",
                        }
                    ],
                )
            ],
        )
        report = build(self.graph_path, self.product_path, self.hints_path, max_agent_works=1)
        self.assertEqual(report["suppressed"]["capped_credited_agent_attachments"], 1)
        leads = [
            (row["work_id"], row["hint_kind"])
            for row in self.hints().values()
            if row["hint_kind"] == "source_lead"
        ]
        # The neediest work (no evidence-backed tags yet) keeps the lead.
        self.assertEqual(leads, [("work-000003", "source_lead")])

    def test_compiled_sqlite_concordance_matches_the_json_form(self) -> None:
        document = json.loads(
            (ROOT / "contracts/artifacts/hint_vocabulary.example.json").read_text("utf-8")
        )
        terms = self.root / "terms.jsonl"
        terms.write_text(
            "".join(json.dumps(term) + "\n" for term in document["terms"])
            + "".join(
                json.dumps({"generic_id": value}) + "\n" for value in document["generic_ids"]
            ),
            encoding="utf-8",
        )
        compiled = self.root / "vocabulary.sqlite"
        report = compile_vocabulary(terms, compiled, source="test")
        self.assertEqual((report["terms"], report["generic_ids"]), (3, 1))
        indexed = load_concordance(compiled)
        self.assertIsInstance(indexed, SqliteConcordance)
        try:
            shipped = Concordance.load(ROOT / "contracts/artifacts/hint_vocabulary.example.json")
            for family, label, vocabulary_id in (
                ("keyword", "Entfremdung", None),
                ("keyword", "x", "https://d-nb.info/gnd/4014670-1"),
                ("style", "films noirs", None),
                ("genre", "Entfremdung", None),
                ("genre", "Folk horror", "lcgft:gf2014026363"),
            ):
                results = [
                    vocabulary.resolve_with_basis(family, label, vocabulary_id)
                    for vocabulary in (indexed, shipped)
                ]
                self.assertEqual(
                    *[
                        None
                        if term is None
                        else (term.term_kind, term.label, dict(term.ids), term.related,
                              term.generic, basis)
                        for term, basis in results
                    ]
                )
            self.assertEqual(indexed.generic_ids, shipped.generic_ids)
        finally:
            indexed.close()
        duplicate = self.root / "duplicate.jsonl"
        duplicate.write_text(
            json.dumps({"term_kind": "topical", "label": "A", "ids": {"lcsh": "sh1"}}) + "\n"
            + json.dumps({"term_kind": "topical", "label": "B", "ids": {"lcsh": "sh1"}}) + "\n",
            encoding="utf-8",
        )
        with self.assertRaises(HintVocabularyError):
            compile_vocabulary(duplicate, self.root / "duplicate.sqlite")
        self.assertFalse((self.root / "duplicate.sqlite").exists())
        # A build accepts the compiled form exactly like the JSON form.
        self.graph.ingest(
            "gnd",
            [
                work_record(
                    "gnd", "entity", "4000001-1",
                    identifiers=[identity("wikidata", "item", "Q1")],
                    signals=[concept("Entfremdung", "keyword", "gnd_subject",
                                     vocabulary_id="gnd:4014670-1")],
                )
            ],
        )
        build(self.graph_path, self.product_path, self.hints_path, vocabulary_path=compiled)
        self.assertIn(("id:lcsh:sh85003435", "concept"), self.hints())

    def test_movielens_relevance_is_licence_gated_hint_strength(self) -> None:
        manual = self.root / "movielens.jsonl"
        manual.write_text(
            json.dumps(
                {
                    "work_id": "work-000001",
                    "kind": "concept",
                    "family": "keyword",
                    "type": "movielens_tag",
                    "value": "thought-provoking",
                    "strength": 0.93,
                    "metadata": {"dataset": "movielens-tag-genome", "movielens_movie_id": "1"},
                }
            )
            + "\n",
            encoding="utf-8",
        )
        gated = build(self.graph_path, self.product_path, self.root / "gated.sqlite",
                      manual_path=manual)
        self.assertEqual(gated["skipped_signals"]["license_restricted"]["movielens_tag"], 1)

        build(self.graph_path, self.product_path, self.hints_path, manual_path=manual,
              allow_restricted=["movielens_tag"])
        hint = self.hints()[("label:thought provoking", "concept")]
        self.assertEqual(hint["assignment_quality"], "D")
        work = work_hints(self.hints_path, "work-000001")
        signal = next(
            signal
            for item in work["hints"]
            for signal in item["signals"]
            if signal["signal_type"] == "movielens_tag"
        )
        # Relevance is provider strength only; it never becomes confidence.
        self.assertEqual((signal["provider"], signal["strength"]), ("movielens", 0.93))

    def test_cli_build_and_query(self) -> None:
        script = ROOT / "scripts/research_hints.py"
        built = subprocess.run(
            [sys.executable, str(script), "build", "--graph", str(self.graph_path),
             "--product", str(self.product_path), "--output", str(self.hints_path)],
            cwd=ROOT, text=True, capture_output=True, check=False,
        )
        self.assertEqual(built.returncode, 0, built.stderr)
        queried = subprocess.run(
            [sys.executable, str(script), "work", "--hints", str(self.hints_path),
             "--work-id", "work-000001"],
            cwd=ROOT, text=True, capture_output=True, check=False,
        )
        self.assertEqual(queried.returncode, 0, queried.stderr)
        self.assertIn("High-priority hints:", queried.stdout)


class ProviderPassTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="arachne-pass-")
        self.root = Path(self.temporary.name)
        base = self.root / "wikidata.sqlite"
        graph = ObservationGraph.create(base)
        graph.ingest(
            "wikidata",
            [
                work_record(
                    "wikidata", "item", "Q1",
                    identifiers=[identity("discogs", "master", "42")],
                    edges=[
                        {
                            "target": identity("wikidata", "item", "Q9"),
                            "target_entity_type": "person",
                            "relation_family": "credit",
                            "relation_type": "performer",
                        }
                    ],
                    signals=[concept("Q37068", "movement", "wikidata_movement",
                                     vocabulary_id="wikidata:Q37068")],
                ),
                # Never selected: its base-graph signal has no consumer.
                work_record(
                    "wikidata", "item", "Q77",
                    signals=[concept("Q37069", "movement", "wikidata_movement",
                                     vocabulary_id="wikidata:Q37069")],
                ),
            ],
        )
        graph.record_source_file("wikidata", "dump", "2026-09-01", "wd", "0" * 64)
        self.masters = self.root / "discogs_masters.xml.gz"
        with gzip.open(self.masters, "wt", encoding="utf-8") as stream:
            stream.write(
                '<masters><master id="42"><title>Album</title><year>1980</year>'
                "<styles><style>Darkwave</style></styles></master>"
                '<master id="43"><title>Other</title>'
                "<styles><style>Coldwave</style></styles></master></masters>"
            )
        broken = self.root / "title.basics.tsv"
        broken.write_text("tconst\tprimaryTitle\nnot-an-id\tX\n", encoding="utf-8")
        self.manifest = self.root / "manifest.json"
        self.manifest.write_text(
            json.dumps(
                {
                    "format": "provider_pass_manifest",
                    "base_graph": "wikidata.sqlite",
                    "inputs": [
                        {"provider": "discogs", "kind": "masters",
                         "path": self.masters.name, "snapshot_id": "20260901"},
                        {"provider": "imdb", "kind": "title-basics",
                         "path": broken.name, "snapshot_id": "2026-09-20"},
                    ],
                }
            ),
            encoding="utf-8",
        )
        self.product = self.root / "product.sqlite"
        with sqlite3.connect(self.product) as connection:
            connection.executescript((ROOT / "schema/product.sql").read_text("utf-8"))
        self.priority = self.root / "priority.json"
        self.priority.write_text(json.dumps({"wikidata": ["Q9"]}), encoding="utf-8")

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def run_pass(self, name: str, **options: object) -> dict[str, object]:
        return run_pass(
            self.manifest, self.root / f"{name}.sqlite", self.product, self.priority,
            self.root / f"{name}-rebuild.json", self.root / f"{name}-hints.sqlite",
            **options,
        )

    def test_one_graph_one_materialization_with_non_fatal_optional_failure(self) -> None:
        report = self.run_pass("pass")
        self.assertEqual(report["failed_optional_inputs"], 1)
        self.assertEqual(
            [(item["provider"], item["status"]) for item in report["inputs"]],
            [("discogs", "ingested"), ("imdb", "failed")],
        )
        # The general pass counts signals instead of storing them.
        self.assertEqual(report["inputs"][0]["signals_not_persisted"], 2)
        self.assertEqual(report["general"]["status"], "succeeded")
        self.assertEqual(report["general"]["primary_metrics"]["N"], 1)
        hints = report["research_hints"]
        self.assertEqual(hints["status"], "succeeded")
        # Only the selected album's Darkwave style is stored; master 43's
        # Coldwave and the unselected base-graph movement never persist.
        self.assertEqual(hints["signal_pass"]["pruned_base_signals"], 1)
        self.assertEqual(
            hints["signal_pass"]["inputs"],
            [{"provider": "discogs", "kind": "masters", "records": 2, "signals": 1,
              "signals_not_relevant": 1}],
        )
        self.assertEqual(hints["build"]["hints"], 2)
        with sqlite3.connect(self.root / "pass.sqlite") as connection:
            self.assertEqual(
                {row[0] for row in connection.execute("SELECT provider FROM provider_sources")},
                {"wikidata", "discogs"},
            )
            self.assertEqual(
                sorted(row[0] for row in connection.execute("SELECT value FROM provider_signals")),
                ["Darkwave", "Q37068"],
            )
        with sqlite3.connect(self.product) as connection:
            self.assertEqual(
                connection.execute("SELECT year_start FROM works").fetchone()[0], 1980
            )
            self.assertEqual(
                connection.execute("SELECT count(*) FROM work_concepts").fetchone()[0], 0
            )
        with sqlite3.connect(self.root / "pass-hints.sqlite") as connection:
            snapshots = json.loads(
                connection.execute(
                    "SELECT provider_snapshots_json FROM research_hint_info"
                ).fetchone()[0]
            )
        self.assertEqual(snapshots["discogs"]["snapshot_id"], "20260901")
        self.assertEqual(
            snapshots["discogs"]["files"],
            [{"kind": "masters", "sha256": digest(self.masters)}],
        )

        required = json.loads(self.manifest.read_text(encoding="utf-8"))
        required["inputs"][1]["required"] = True
        self.manifest.write_text(json.dumps(required), encoding="utf-8")
        with self.assertRaises(ProviderPassError):
            self.run_pass("pass2")

    def test_hint_inputs_are_preflighted_before_product_mutation(self) -> None:
        manual = self.root / "manual.jsonl"
        manual.write_text('{"work_id":"work-000001","kind":"nope"}\n', encoding="utf-8")
        before = digest(self.product)
        with self.assertRaisesRegex(ProviderPassError, "preflight"):
            self.run_pass("pass", manual_signals=manual)
        with self.assertRaisesRegex(ProviderPassError, "preflight"):
            self.run_pass("pass", allow_restricted=["discogs_style"])
        self.assertEqual(digest(self.product), before)
        self.assertFalse((self.root / "pass.sqlite").exists())

    def test_hint_failure_never_blurs_the_committed_general_pass(self) -> None:
        (self.root / "pass-hints.sqlite").write_text("occupied", encoding="utf-8")
        report = self.run_pass("pass")
        self.assertEqual(report["general"]["status"], "succeeded")
        self.assertEqual(report["research_hints"]["status"], "failed")
        self.assertIn("already exists", report["research_hints"]["reason"])
        with sqlite3.connect(self.product) as connection:
            self.assertEqual(connection.execute("SELECT count(*) FROM works").fetchone()[0], 1)

    def test_old_versioned_manifest_is_rejected(self) -> None:
        document = json.loads(self.manifest.read_text(encoding="utf-8"))
        document["format_version"] = 1
        self.manifest.write_text(json.dumps(document), encoding="utf-8")
        with self.assertRaises(ProviderPassError):
            self.run_pass("pass")


if __name__ == "__main__":
    unittest.main()
