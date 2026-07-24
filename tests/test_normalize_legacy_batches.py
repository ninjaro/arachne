from __future__ import annotations

import copy
import json
import tempfile
import unittest
import zipfile
from pathlib import Path

from scripts.normalize_legacy_batches import (
    Findings,
    MANIFEST_ARRAYS,
    NormalizationError,
    load_documents,
    normalize_corpus,
    write_outputs,
)


def batch(batch_id: str, suffix: str, *, with_authority: bool = True) -> dict:
    creator = {
        "local_id": f"creator-{suffix}",
        "name": "Example Artist",
        "entity_type": "person",
        "names": [
            {
                "value": "E. Artist",
                "type": "alias",
                "language_code": "en",
                "preferred": False,
            }
        ],
    }
    work = {
        "local_id": f"work-{suffix}",
        "titles": [
            {
                "value": "Example Work",
                "type": "original",
                "language_code": "en",
                "script_code": "Latn",
                "preferred": True,
            }
        ],
        "medium": "film",
        "date": "2001",
        "production_info_json": {"processes": ["stop motion"]},
    }
    if with_authority:
        creator["external_ids"] = {"wikidata": "Q1"}
        work["external_ids"] = {"imdb_title": "tt0000001"}
    return {
        "format_version": 1,
        "batch_id": batch_id,
        "batch_type": "mining",
        "creators": [creator],
        "works": [work],
        "credits": [
            {
                "work": f"work-{suffix}",
                "creator": f"creator-{suffix}",
                "role": "director",
                "importance": "primary",
            }
        ],
        "concepts": [
            {
                "local_id": f"concept-{suffix}",
                "name": "Dream logic",
                "type": "theme",
            }
        ],
        "references": [
            {
                "ref_id": f"ref-{suffix}",
                "source_type": "article",
                "title": "A scholarly source",
                "doi": "10.1234/example",
                "language_code": "en",
            }
        ],
        "evidence": [
            {
                "local_id": f"evidence-{suffix}",
                "ref_id": f"ref-{suffix}",
                "exact_quote": "A precise quotation.",
                "locator_json": {"page": 4},
            }
        ],
        "assertions": [
            {
                "local_id": f"assertion-{suffix}",
                "work": f"work-{suffix}",
                "concept": f"concept-{suffix}",
                "relation": "contains",
                "centrality": 80,
                "evidence_refs": [f"evidence-{suffix}"],
            }
        ],
    }


class NormalizeLegacyBatchesTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.inbox = self.root / "inbox"
        self.inbox.mkdir()

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def write(self, name: str, value: object) -> None:
        (self.inbox / name).write_text(
            json.dumps(value, ensure_ascii=False), encoding="utf-8"
        )

    def test_normalizes_aliases_and_groups_authority_dependencies(self) -> None:
        self.write("second.json", batch("batch-b", "b"))
        self.write("first.json", batch("batch-a", "a"))

        manifest, unresolved = normalize_corpus(self.inbox)

        self.assertEqual(set(MANIFEST_ARRAYS), set(manifest) - {"contract", "format_version"})
        self.assertEqual(len(manifest["creators"]), 1)
        self.assertEqual(len(manifest["works"]), 1)
        self.assertEqual(len(manifest["credits"]), 1)
        self.assertEqual(len(manifest["tags"]), 1)
        self.assertEqual(len(manifest["references"]), 1)
        self.assertEqual(len(manifest["assertions"]), 1)
        creator = manifest["creators"][0]
        self.assertEqual(creator["canonical_id"], creator["local_id"])
        self.assertEqual(creator["entity_type"], "person")
        self.assertEqual(creator["names"][0]["language"], "en")
        self.assertIs(creator["names"][0]["preferred"], False)
        work = manifest["works"][0]
        self.assertEqual(work["production_info"], {"processes": ["stop motion"]})
        self.assertEqual(work["titles"][0]["script"], "Latn")
        assertion = manifest["assertions"][0]
        self.assertEqual(assertion["weight"], 80)
        self.assertEqual(assertion["evidence"][0]["stance"], "supports")
        self.assertEqual(assertion["evidence"][0]["locator"], {"page": 4})
        self.assertEqual(unresolved["artifact_type"], "consolidated_corpus_unresolved_v1")
        encoded = json.dumps(manifest, sort_keys=True)
        self.assertNotIn('"batch_id"', encoded)
        self.assertNotIn('"batch_type"', encoded)
        self.assertNotIn('"sha256"', encoded)
        self.assertNotIn('"run_id"', encoded)

    def test_load_documents_uses_only_captured_bytes_when_provided(self) -> None:
        captured = batch("captured-batch", "captured")
        live = batch("live-batch", "live")
        self.write("same-name.json", live)

        documents = load_documents(
            self.inbox,
            Findings(),
            captured_files={
                "same-name.json": json.dumps(captured).encode("utf-8")
            },
        )

        self.assertEqual([document.source.batch_id for document in documents], [
            "captured-batch"
        ])
        self.assertEqual(
            documents[0].value["works"][0]["local_id"], "work-captured"
        )

    def test_result_is_independent_of_record_and_archive_member_order(self) -> None:
        left = batch("batch-a", "a")
        right = batch("batch-b", "b")
        right["works"][0].pop("external_ids")
        right["works"][0]["titles"][0]["preferred"] = False
        self.write("a.json", left)
        self.write("b.json", right)
        first, _ = normalize_corpus(self.inbox)

        for path in self.inbox.iterdir():
            path.unlink()
        for value in (left, right):
            for key, item in list(value.items()):
                if isinstance(item, list):
                    value[key] = list(reversed(item))
        self.write("z.json", right)
        self.write("y.json", left)
        second, _ = normalize_corpus(self.inbox)

        self.assertEqual(first, second)
        self.assertEqual(len(first["works"]), 1)
        self.assertEqual(len(first["works"][0]["titles"]), 1)
        self.assertIs(first["works"][0]["titles"][0]["preferred"], True)

    def test_content_detects_zip_variant_and_preserves_sidecar(self) -> None:
        value = batch("enrichment-a", "a")
        value["batch_type"] = "enrichment"
        archive = self.inbox / "opaque.zip"
        with zipfile.ZipFile(archive, "w") as output:
            output.writestr("validation.json", '{"status":"informational"}')
            output.writestr("nested/payload.json", json.dumps(value))

        manifest, unresolved = normalize_corpus(self.inbox)

        self.assertEqual(len(manifest["works"]), 1)
        sidecars = [
            item for item in unresolved["remainders"]
            if item["category"] == "non_batch_sidecar"
        ]
        self.assertEqual(sidecars[0]["value"], {"status": "informational"})
        self.assertEqual(sidecars[0]["source"]["member"], "validation.json")
        self.assertFalse((self.inbox / "nested").exists())

    def test_unknown_values_and_fields_are_exact_structured_remainders(self) -> None:
        value = batch("batch-a", "a")
        value["works"][0]["medium"] = "video_game"
        value["creators"][0]["research_note"] = {"keep": [1, 2, 3]}
        self.write("batch.json", value)

        manifest, unresolved = normalize_corpus(self.inbox)

        self.assertEqual(manifest["works"], [])
        exact = [
            item for item in unresolved["remainders"]
            if item["json_pointer"].endswith("/research_note")
        ]
        self.assertEqual(exact[0]["value"], {"keep": [1, 2, 3]})
        rejected = [
            item for item in unresolved["remainders"]
            if item["category"] == "unimportable_work"
        ]
        self.assertEqual(rejected[0]["value"]["medium"], "video_game")

    def test_preferred_name_valid_titles_and_all_production_keys_survive(self) -> None:
        value = batch("batch-a", "a")
        value["creators"][0].pop("name")
        value["creators"][0]["names"][0]["preferred"] = True
        value["works"][0]["titles"].append(
            {"value": "Unsafe label", "type": "marketing_title", "preferred": False}
        )
        value["works"][0]["production_info_json"] = {
            "materials": ["canvas"],
            "instruments": ["prepared piano"],
            "tools": ["optical printer"],
            "supports": ["wood panel"],
            "processes": ["stop motion"],
            "formats": ["35mm"],
        }
        self.write("batch.json", value)

        manifest, unresolved = normalize_corpus(self.inbox)

        self.assertNotIn("name", manifest["creators"][0])
        self.assertEqual(
            [
                item["value"] for item in manifest["creators"][0]["names"]
                if item["preferred"]
            ],
            ["E. Artist"],
        )
        self.assertEqual(
            set(manifest["works"][0]["production_info"]),
            {"materials", "instruments", "tools", "supports", "processes", "formats"},
        )
        self.assertEqual(len(manifest["works"][0]["titles"]), 1)
        self.assertTrue(
            any(item["category"] == "invalid_title" for item in unresolved["remainders"])
        )

    def test_explicit_false_sole_title_is_not_promoted(self) -> None:
        value = batch("batch-a", "a")
        value["works"][0]["titles"][0]["preferred"] = False
        self.write("batch.json", value)

        manifest, unresolved = normalize_corpus(self.inbox)

        self.assertEqual(manifest["works"], [])
        self.assertTrue(
            any(
                item["category"] == "ambiguous_preferred_title"
                for item in unresolved["remainders"]
            )
        )

    def test_names_alone_never_merge_creator_identity(self) -> None:
        first = batch("batch-a", "a", with_authority=False)
        second = batch("batch-b", "b", with_authority=False)
        second["works"][0]["date"] = "2002"
        self.write("a.json", first)
        self.write("b.json", second)

        manifest, _ = normalize_corpus(self.inbox)

        self.assertEqual(len(manifest["creators"]), 2)

    def test_incomplete_work_identity_and_concept_asset_are_handled_safely(self) -> None:
        value = batch("batch-a", "a")
        value["works"][0].pop("external_ids")
        value["works"][0].pop("date")
        value["remote_assets"] = [
            {
                "entity": "concept-a",
                "provider": "example",
                "direct_url": "https://example.test/concept-a.jpg",
            }
        ]
        self.write("batch.json", value)

        manifest, unresolved = normalize_corpus(self.inbox)

        self.assertEqual(manifest["works"], [])
        self.assertEqual(len(manifest["remote_assets"]), 1)
        self.assertEqual(manifest["remote_assets"][0]["entity"], manifest["tags"][0]["local_id"])
        self.assertTrue(
            any(
                item["category"] == "unresolved_work_identity"
                for item in unresolved["remainders"]
            )
        )

    def test_rejects_unsafe_zip_without_extracting(self) -> None:
        with zipfile.ZipFile(self.inbox / "unsafe.zip", "w") as archive:
            archive.writestr("../escape.json", json.dumps(batch("batch-a", "a")))

        manifest, unresolved = normalize_corpus(self.inbox)

        self.assertTrue(all(not manifest[key] for key in MANIFEST_ARRAYS))
        self.assertEqual(unresolved["remainders"][0]["category"], "unsafe_archive")
        self.assertFalse((self.root / "escape.json").exists())

    def test_output_cannot_overwrite_the_inbox(self) -> None:
        self.write("batch.json", batch("batch-a", "a"))
        manifest, unresolved = normalize_corpus(self.inbox)

        with self.assertRaises(NormalizationError):
            write_outputs(
                manifest,
                unresolved,
                self.inbox,
                self.inbox / "manifest.json",
                self.root / "unresolved.json",
            )

    def test_quarantines_every_duplicate_batch_document(self) -> None:
        first = batch("duplicate", "a")
        second = batch("duplicate", "b")
        self.write("z.json", first)
        self.write("a.json", second)

        manifest, unresolved = normalize_corpus(self.inbox)

        self.assertTrue(all(not manifest[key] for key in MANIFEST_ARRAYS))
        conflict = next(
            item for item in unresolved["conflicts"]
            if item["category"] == "duplicate_batch_identifier"
        )
        self.assertEqual(len(conflict["occurrences"]), 2)
        self.assertEqual(
            {item["value"]["works"][0]["local_id"] for item in conflict["occurrences"]},
            {"work-a", "work-b"},
        )

    def test_preserves_non_object_collection_members(self) -> None:
        value = batch("batch-a", "a")
        value["creators"].append("not-an-object")
        self.write("batch.json", value)

        _, unresolved = normalize_corpus(self.inbox)

        item = next(
            entry for entry in unresolved["remainders"]
            if entry["category"] == "invalid_record"
        )
        self.assertEqual(item["json_pointer"], "/creators/1")
        self.assertEqual(item["value"], "not-an-object")

    def test_ambiguous_zip_preserves_each_candidate_body(self) -> None:
        with zipfile.ZipFile(self.inbox / "ambiguous.zip", "w") as archive:
            archive.writestr("one.json", json.dumps(batch("batch-a", "a")))
            archive.writestr("two.json", json.dumps(batch("batch-b", "b")))

        manifest, unresolved = normalize_corpus(self.inbox)

        self.assertTrue(all(not manifest[key] for key in MANIFEST_ARRAYS))
        candidates = [
            item for item in unresolved["remainders"]
            if item["category"] == "ambiguous_archive_candidate"
        ]
        self.assertEqual({item["value"]["batch_id"] for item in candidates}, {"batch-a", "batch-b"})
        self.assertEqual({item["source"]["member"] for item in candidates}, {"one.json", "two.json"})

    def test_keep_separate_strips_globally_duplicate_authority_id(self) -> None:
        self.write("a.json", batch("batch-a", "a"))
        self.write("b.json", batch("batch-b", "b"))
        reconciliation = {
            "format_version": 1,
            "batch_id": "reconciliation",
            "batch_type": "reconciliation",
            "separation_resolutions": [
                {
                    "resolution_id": "separate-example-artists",
                    "action": "keep_separate",
                    "status": "accepted",
                    "entities": [
                        {"batch_id": "batch-a", "local_id": "creator-a"},
                        {"batch_id": "batch-b", "local_id": "creator-b"},
                    ],
                }
            ],
        }
        self.write("reconciliation.json", reconciliation)

        manifest, unresolved = normalize_corpus(self.inbox)

        self.assertEqual(len(manifest["creators"]), 2)
        self.assertTrue(
            all(
                creator.get("external_ids", {}).get("wikidata") is None
                for creator in manifest["creators"]
            )
        )
        conflict = next(
            item for item in unresolved["conflicts"]
            if item["category"] == "separated_external_identifier_conflict"
        )
        self.assertEqual(len(conflict["occurrences"]), 2)

    def test_duplicate_evidence_ids_are_all_quarantined(self) -> None:
        value = batch("batch-a", "a")
        value["evidence"].append(copy.deepcopy(value["evidence"][0]))
        self.write("batch.json", value)

        manifest, unresolved = normalize_corpus(self.inbox)

        self.assertEqual(manifest["assertions"], [])
        conflict = next(
            item for item in unresolved["conflicts"]
            if item["category"] == "duplicate_local_identifier"
        )
        self.assertEqual(len(conflict["occurrences"]), 2)

    def test_groups_db_identity_optional_conflicts(self) -> None:
        value = batch("batch-a", "a")
        value["financial_facts"] = [
            {
                "work": "work-a", "type": "budget", "amount": 100,
                "currency": "USD", "estimated": True, "confidence": 0.7,
            },
            {
                "work": "work-a", "type": "budget", "amount": 100,
                "currency": "USD", "estimated": True, "confidence": 0.8,
            },
        ]
        value["remote_assets"] = [
            {
                "entity": "concept-a", "provider": "example",
                "direct_url": "https://example.test/a", "rights_note": "A",
            },
            {
                "entity": "concept-a", "provider": "example",
                "direct_url": "https://example.test/a", "rights_note": "B",
            },
        ]
        self.write("batch.json", value)

        manifest, unresolved = normalize_corpus(self.inbox)

        self.assertEqual(len(manifest["financial_facts"]), 1)
        self.assertNotIn("confidence", manifest["financial_facts"][0])
        self.assertEqual(len(manifest["remote_assets"]), 1)
        self.assertNotIn("rights_note", manifest["remote_assets"][0])
        self.assertTrue(
            {"financial_fact_conflict", "remote_asset_conflict"}
            <= {item["category"] for item in unresolved["conflicts"]}
        )

    def test_rejects_duplicate_keys_and_non_finite_json_numbers(self) -> None:
        (self.inbox / "duplicate.json").write_text(
            '{"format_version":1,"batch_id":"a","batch_id":"b","batch_type":"mining"}',
            encoding="utf-8",
        )
        (self.inbox / "nan.json").write_text(
            '{"format_version":1,"batch_id":"nan","batch_type":"mining","measurements":[{"value":NaN}]}',
            encoding="utf-8",
        )

        manifest, unresolved = normalize_corpus(self.inbox)

        self.assertTrue(all(not manifest[key] for key in MANIFEST_ARRAYS))
        invalid = [
            item for item in unresolved["remainders"]
            if item["category"] == "invalid_document"
        ]
        self.assertEqual(len(invalid), 2)
        self.assertTrue(all(isinstance(item["value"], str) for item in invalid))

    def test_reconciliation_overlaps_are_quarantined_independent_of_order(self) -> None:
        self.write("batch.json", batch("batch-a", "a"))
        reconciliation = {
            "format_version": 1,
            "batch_id": "reconciliation-a",
            "batch_type": "reconciliation",
            "entity_resolutions": [
                {
                    "resolution_id": "entity-a",
                    "action": "merge",
                    "status": "accepted_high_confidence",
                    "members": [
                        {"batch_id": "batch-a", "local_id": "creator-a"}
                    ],
                    "canonical_entity": {"preferred_name": "First Name"},
                },
                {
                    "resolution_id": "entity-b",
                    "action": "merge",
                    "status": "accepted_high_confidence",
                    "members": [
                        {"batch_id": "batch-a", "local_id": "creator-a"}
                    ],
                    "canonical_entity": {"preferred_name": "Second Name"},
                },
            ],
            "alias_resolutions": [
                {
                    "resolution_id": "alias-a",
                    "action": "keep_one_person",
                    "status": "confirmed_by_authority_anchor",
                    "canonical_member": {
                        "batch_id": "batch-a",
                        "local_id": "creator-a",
                    },
                    "preferred_name": "Third Name",
                    "aliases": ["Alias A"],
                },
                {
                    "resolution_id": "alias-b",
                    "action": "keep_one_person",
                    "status": "confirmed_by_authority_anchor",
                    "canonical_member": {
                        "batch_id": "batch-a",
                        "local_id": "creator-a",
                    },
                    "preferred_name": "Fourth Name",
                    "aliases": ["Alias B"],
                },
            ],
            "work_resolutions": [
                {
                    "resolution_id": "work-a",
                    "action": "merge",
                    "status": "accepted",
                    "members": [
                        {"batch_id": "batch-a", "local_id": "work-a"}
                    ],
                    "canonical_work": {"date": "2002"},
                },
                {
                    "resolution_id": "work-b",
                    "action": "merge",
                    "status": "accepted",
                    "members": [
                        {"batch_id": "batch-a", "local_id": "work-a"}
                    ],
                    "canonical_work": {"date": "2003"},
                },
            ],
            "schema_repairs": [
                {
                    "repair_id": "repair-a",
                    "action": "replace_malformed_scheme",
                    "batch_id": "batch-a",
                    "work": "work-a",
                    "remove": {},
                    "add": {"catalogue": "A"},
                },
                {
                    "repair_id": "repair-b",
                    "action": "replace_malformed_scheme",
                    "batch_id": "batch-a",
                    "work": "work-a",
                    "remove": {},
                    "add": {"catalogue": "B"},
                },
            ],
        }
        self.write("reconciliation.json", reconciliation)

        first_manifest, first_unresolved = normalize_corpus(self.inbox)
        for key in (
            "entity_resolutions",
            "alias_resolutions",
            "work_resolutions",
            "schema_repairs",
        ):
            reconciliation[key].reverse()
        self.write("reconciliation.json", reconciliation)
        second_manifest, second_unresolved = normalize_corpus(self.inbox)

        self.assertEqual(first_manifest, second_manifest)
        self.assertEqual(first_manifest["creators"][0]["name"], "Example Artist")
        self.assertEqual(first_manifest["works"][0]["date"], "2001")

        def semantic_conflicts(unresolved: dict) -> list[tuple]:
            return sorted(
                (
                    item["identity"],
                    item["field"],
                    tuple(
                        sorted(
                            json.dumps(occurrence["value"], sort_keys=True)
                            for occurrence in item["occurrences"]
                        )
                    ),
                )
                for item in unresolved["conflicts"]
                if item["category"] == "reconciliation_assignment_conflict"
            )

        self.assertEqual(
            semantic_conflicts(first_unresolved),
            semantic_conflicts(second_unresolved),
        )
        self.assertEqual(len(semantic_conflicts(first_unresolved)), 4)

    def test_identical_reconciliation_assignments_deduplicate(self) -> None:
        self.write("batch.json", batch("batch-a", "a"))
        resolution = {
            "action": "keep_one_person",
            "status": "confirmed_by_authority_anchor",
            "canonical_member": {
                "batch_id": "batch-a",
                "local_id": "creator-a",
            },
            "preferred_name": "Canonical Artist",
            "aliases": ["Known Alias"],
        }
        reconciliation = {
            "format_version": 1,
            "batch_id": "reconciliation-a",
            "batch_type": "reconciliation",
            "alias_resolutions": [
                {**resolution, "resolution_id": "alias-a"},
                {**resolution, "resolution_id": "alias-b"},
            ],
        }
        self.write("reconciliation.json", reconciliation)

        manifest, unresolved = normalize_corpus(self.inbox)

        self.assertEqual(manifest["creators"][0]["name"], "Canonical Artist")
        aliases = [
            item
            for item in manifest["creators"][0]["names"]
            if item["value"] == "Known Alias"
        ]
        self.assertEqual(len(aliases), 1)
        self.assertFalse(
            any(
                item["category"] == "reconciliation_assignment_conflict"
                for item in unresolved["conflicts"]
            )
        )

    def test_imports_nested_facts_measurements_aliases_and_evidence_alias(self) -> None:
        value = batch("batch-a", "a")
        value["creators"][0]["aliases"] = [
            "Stage Name",
            {"value": "Unmapped Full Name", "type": "full_name"},
            "",
        ]
        value["works"][0]["financial_facts"] = [
            {
                "type": "budget",
                "amount": {"min": 100, "max": 150},
                "currency": "usd",
                "estimated": True,
                "source_note": "Preserve this note.",
            },
            "invalid financial fragment",
        ]
        value["works"].append(
            {
                "local_id": "unimportable-work",
                "titles": [
                    {"value": "Bad Work", "type": "original", "preferred": True}
                ],
                "medium": "not_a_medium",
                "date": "2002",
                "financial_facts": [
                    {
                        "work": "work-a",
                        "type": "budget",
                        "amount": 999,
                        "currency": "USD",
                        "estimated": False,
                    }
                ],
            }
        )
        manifestation_titles = [
            {
                "value": "Release Title",
                "type": "credited",
                "preferred": True,
                "language": "en",
            }
        ]
        value["manifestations"] = [
            {
                "local_id": "manifestation-a",
                "work": "work-a",
                "type": "release",
                "label": "Existing release label",
                "titles": manifestation_titles,
                "measurements": [
                    {"type": "duration", "value": 120, "unit": "seconds"},
                    "invalid measurement fragment",
                ],
            },
            {
                "local_id": "unimportable-manifestation",
                "work": "work-a",
                "type": "not_a_manifestation_type",
                "label": "Bad manifestation",
                "measurements": [
                    {
                        "work": "work-a",
                        "type": "duration",
                        "value": 999,
                        "unit": "seconds",
                    }
                ],
            },
        ]
        value["manifestation_measurements"] = [
            {
                "entity": "manifestation-a",
                "measurement_type": "duration",
                "value": 121,
                "unit": "seconds",
                "source_refs": ["ref-a"],
            }
        ]
        value["evidence_records"] = value.pop("evidence")
        value["evidence_records"].append("invalid evidence fragment")
        self.write("batch.json", value)

        manifest, unresolved = normalize_corpus(self.inbox)

        aliases = [
            item
            for item in manifest["creators"][0]["names"]
            if item["value"] == "Stage Name"
        ]
        self.assertEqual(
            aliases, [{"type": "alias", "value": "Stage Name", "preferred": False}]
        )
        self.assertEqual(
            manifest["financial_facts"],
            [
                {
                    "work": manifest["works"][0]["local_id"],
                    "type": "budget",
                    "amount": {"min": 100, "max": 150},
                    "currency": "USD",
                    "estimated": True,
                }
            ],
        )
        self.assertEqual(
            sorted(item["value"] for item in manifest["measurements"]),
            [120, 121],
        )
        self.assertEqual(manifest["manifestations"][0]["label"], "Existing release label")
        self.assertEqual(
            manifest["assertions"][0]["evidence"][0]["quote"],
            "A precise quotation.",
        )
        exact_values = [item["value"] for item in unresolved["remainders"]]
        self.assertIn(manifestation_titles, exact_values)
        self.assertIn("invalid financial fragment", exact_values)
        self.assertIn("invalid measurement fragment", exact_values)
        self.assertIn("invalid evidence fragment", exact_values)
        self.assertIn({"value": "Unmapped Full Name", "type": "full_name"}, exact_values)
        self.assertIn("", exact_values)
        self.assertIn("Preserve this note.", exact_values)
        self.assertIn(["ref-a"], exact_values)
        self.assertFalse(
            any(
                item["category"] == "noncanonical_top_level_field"
                and item["json_pointer"] == "/manifestation_measurements"
                for item in unresolved["remainders"]
            )
        )

    def test_uses_only_sole_explicit_nested_label_fallbacks(self) -> None:
        value = batch("batch-a", "a")
        concept_names = [
            {
                "value": "Nested Concept Name",
                "type": "alias",
                "preferred": True,
                "language": "en",
            }
        ]
        value["concepts"][0].pop("name")
        value["concepts"][0].pop("slug", None)
        value["concepts"][0]["names"] = concept_names
        manifestation_titles = [
            {
                "value": "Nested Release Label",
                "type": "credited",
                "preferred": True,
                "language": "en",
            }
        ]
        value["manifestations"] = [
            {
                "local_id": "manifestation-a",
                "work": "work-a",
                "type": "release",
                "titles": manifestation_titles,
            }
        ]
        self.write("batch.json", value)

        manifest, unresolved = normalize_corpus(self.inbox)

        self.assertEqual(manifest["tags"][0]["name"], "Nested Concept Name")
        self.assertEqual(
            manifest["manifestations"][0]["label"], "Nested Release Label"
        )
        exact_values = [item["value"] for item in unresolved["remainders"]]
        self.assertIn(concept_names, exact_values)
        self.assertIn(manifestation_titles, exact_values)

        value["concepts"][0]["name"] = "Explicit Concept Name"
        value["manifestations"][0]["label"] = "Explicit release label"
        self.write("batch.json", value)
        manifest, _ = normalize_corpus(self.inbox)
        self.assertEqual(manifest["tags"][0]["name"], "Explicit Concept Name")
        self.assertEqual(
            manifest["manifestations"][0]["label"], "Explicit release label"
        )

    def test_validates_isni_without_changing_identity_spelling(self) -> None:
        value = batch("batch-a", "a")
        value["creators"][0]["external_ids"] = {
            "ISNI": "0000-0000-5677-804x"
        }
        value["creators"].extend(
            [
                {
                    "local_id": "creator-short-isni",
                    "name": "Short ISNI",
                    "entity_type": "person",
                    "external_ids": {"isni": "000000004912841"},
                },
                {
                    "local_id": "creator-invalid-isni",
                    "name": "Invalid ISNI",
                    "entity_type": "person",
                    "external_ids": {"isni": "0000 0001 2103 2684"},
                },
            ]
        )
        self.write("batch.json", value)

        manifest, unresolved = normalize_corpus(self.inbox)

        by_name = {creator["name"]: creator for creator in manifest["creators"]}
        self.assertEqual(
            by_name["Example Artist"]["external_ids"]["ISNI"],
            "0000-0000-5677-804x",
        )
        self.assertEqual(
            by_name["Short ISNI"]["external_ids"]["isni"],
            "000000004912841",
        )
        self.assertEqual(
            by_name["Invalid ISNI"]["external_ids"]["isni"],
            "0000 0001 2103 2684",
        )
        invalid = [
            item
            for item in unresolved["remainders"]
            if item["category"] == "invalid_external_identifier"
            and item["value"] == "0000 0001 2103 2684"
        ]
        self.assertEqual(len(invalid), 1)

    def test_preserves_junctions_only_for_unmaterialized_assertions(self) -> None:
        value = batch("batch-a", "a")
        rejected = copy.deepcopy(value["assertions"][0])
        rejected["local_id"] = "assertion-rejected"
        rejected["relation"] = "not_a_relation"
        rejected.pop("evidence_refs")
        value["assertions"].append(rejected)
        accepted_link = {
            "assertion_id": "assertion-a",
            "evidence_id": "evidence-a",
        }
        rejected_link = {
            "assertion_id": "assertion-rejected",
            "evidence_id": "evidence-a",
        }
        missing_link = {
            "assertion_id": "assertion-missing",
            "evidence_id": "evidence-a",
        }
        value["assertion_evidence"] = [
            accepted_link, rejected_link, missing_link
        ]
        self.write("batch.json", value)

        manifest, unresolved = normalize_corpus(self.inbox)

        self.assertEqual(len(manifest["assertions"]), 1)
        preserved = [
            item["value"]
            for item in unresolved["remainders"]
            if item["category"] == "unmaterialized_assertion_evidence_link"
        ]
        self.assertCountEqual(preserved, [rejected_link, missing_link])
        self.assertNotIn(accepted_link, preserved)

    def test_preserves_unmapped_canonical_work_fields(self) -> None:
        self.write("batch.json", batch("batch-a", "a"))
        preferred_title = {
            "value": "Resolution-only title",
            "language": "en",
        }
        editorial_note = {"basis": ["catalogue", "authority"]}
        reconciliation = {
            "format_version": 1,
            "batch_id": "reconciliation-a",
            "batch_type": "reconciliation",
            "work_resolutions": [
                {
                    "resolution_id": "work-resolution-a",
                    "action": "merge",
                    "status": "accepted",
                    "members": [
                        {"batch_id": "batch-a", "local_id": "work-a"}
                    ],
                    "canonical_work": {
                        "date": "2002",
                        "preferred_title": preferred_title,
                        "editorial_note": editorial_note,
                    },
                }
            ],
        }
        self.write("reconciliation.json", reconciliation)

        manifest, unresolved = normalize_corpus(self.inbox)

        self.assertEqual(manifest["works"][0]["date"], "2002")
        unmapped = {
            item["json_pointer"]: item["value"]
            for item in unresolved["remainders"]
            if item["category"]
            == "unmapped_reconciliation_canonical_work_field"
        }
        self.assertEqual(
            unmapped[
                "/work_resolutions/0/canonical_work/preferred_title"
            ],
            preferred_title,
        )
        self.assertEqual(
            unmapped[
                "/work_resolutions/0/canonical_work/editorial_note"
            ],
            editorial_note,
        )


if __name__ == "__main__":
    unittest.main()
