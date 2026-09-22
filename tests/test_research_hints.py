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

from scripts.provider_fixture_adapters import (
    expand_discogs_release,
    expand_open_library_edition,
    normalize_discogs_artist,
    normalize_discogs_label,
    normalize_discogs_master,
    normalize_imdb_title_aka,
    normalize_imdb_title_basics,
    normalize_imdb_title_crew,
    normalize_musicbrainz_label,
    normalize_musicbrainz_release_group,
    normalize_musicbrainz_work,
    normalize_open_library_redirect,
    normalize_open_library_work,
)
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


class WikidataSignalTests(unittest.TestCase):
    def test_movement_and_genre_profiles_become_vocabulary_signals(self) -> None:
        spec = importlib.util.spec_from_file_location(
            "build_external_graph", ROOT / "hpc/wikidata/build_external_graph.py"
        )
        module = importlib.util.module_from_spec(spec)
        assert spec.loader is not None
        spec.loader.exec_module(module)
        signals = module.profile_signals(
            {"genres": ["Q130232", "bad"], "movements": ["Q37068"], "classes": ["Q11424"]}
        )
        self.assertEqual(
            [(s["type"], s["vocabulary_id"], s["metadata"]["property_id"]) for s in signals],
            [
                ("wikidata_movement", "wikidata:Q37068", "P135"),
                ("wikidata_genre", "wikidata:Q130232", "P136"),
            ],
        )


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
        drama = hints[("label:drama", "concept")]
        generic_qid = hints[("id:wikidata:Q130232", "concept")]
        rock = hints[("label:rock", "concept")]
        movement = hints[("id:wikidata:Q37068", "concept")]
        # Discogs style and a differently-spelled IMDb value dedupe into one
        # hint whose independent provider origins raise its priority.
        self.assertEqual(post_punk["independent_origins"], 2)
        self.assertEqual(post_punk["quality_class"], "B")
        self.assertEqual(post_punk["display_value"], "Post-Punk")
        for generic in (drama, generic_qid, rock):
            self.assertEqual(generic["quality_class"], "E")
            self.assertEqual(generic["specificity"], 0.05)
            self.assertLess(generic["research_priority"] * 50, post_punk["research_priority"])
        self.assertGreater(movement["research_priority"], generic_qid["research_priority"])
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
        self.assertEqual(guide["quality_class"], "B")
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
    def test_one_graph_one_materialization_with_non_fatal_optional_failure(self) -> None:
        with tempfile.TemporaryDirectory(prefix="arachne-pass-") as temporary:
            root = Path(temporary)
            base = root / "wikidata.sqlite"
            ObservationGraph.create(base).ingest(
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
                    )
                ],
            )
            with sqlite3.connect(base) as connection:
                connection.execute(
                    "INSERT INTO provider_sources VALUES('wikidata','2026-09-01','wd',?)",
                    ("0" * 64,),
                )
            masters = root / "discogs_masters.xml.gz"
            with gzip.open(masters, "wt", encoding="utf-8") as stream:
                stream.write(
                    '<masters><master id="42"><title>Album</title><year>1980</year>'
                    "<styles><style>Darkwave</style></styles></master></masters>"
                )
            broken = root / "title.basics.tsv"
            broken.write_text("tconst\tprimaryTitle\nnot-an-id\tX\n", encoding="utf-8")
            manifest = root / "manifest.json"
            manifest.write_text(
                json.dumps(
                    {
                        "format": "provider_pass_manifest",
                        "format_version": 1,
                        "base_graph": "wikidata.sqlite",
                        "inputs": [
                            {"provider": "discogs", "kind": "masters",
                             "path": masters.name, "snapshot_id": "20260901"},
                            {"provider": "imdb", "kind": "title-basics",
                             "path": broken.name, "snapshot_id": "2026-09-20"},
                        ],
                    }
                ),
                encoding="utf-8",
            )
            product = root / "product.sqlite"
            with sqlite3.connect(product) as connection:
                connection.executescript((ROOT / "schema/product.sql").read_text("utf-8"))
            priority = root / "priority.json"
            priority.write_text(json.dumps({"wikidata": ["Q9"]}), encoding="utf-8")

            report = run_pass(
                manifest, root / "pass.sqlite", product, priority,
                root / "rebuild.json", root / "hints.sqlite",
            )
            self.assertEqual(report["failed_optional_inputs"], 1)
            self.assertEqual(
                [(item["provider"], item["status"]) for item in report["inputs"]],
                [("discogs", "ingested"), ("imdb", "failed")],
            )
            self.assertEqual(report["primary_metrics"]["N"], 1)
            self.assertEqual(report["research_hints"]["hints"], 1)
            with sqlite3.connect(root / "pass.sqlite") as connection:
                self.assertEqual(
                    {row[0] for row in connection.execute("SELECT provider FROM provider_sources")},
                    {"wikidata", "discogs"},
                )
            with sqlite3.connect(product) as connection:
                self.assertEqual(
                    connection.execute(
                        "SELECT year_start FROM works"
                    ).fetchone()[0],
                    1980,
                )
                self.assertEqual(
                    connection.execute("SELECT count(*) FROM work_concepts").fetchone()[0], 0
                )

            required = json.loads(manifest.read_text(encoding="utf-8"))
            required["inputs"][1]["required"] = True
            manifest.write_text(json.dumps(required), encoding="utf-8")
            with self.assertRaises(ProviderPassError):
                run_pass(
                    manifest, root / "pass2.sqlite", product, priority,
                    root / "rebuild2.json", root / "hints2.sqlite",
                )


if __name__ == "__main__":
    unittest.main()
