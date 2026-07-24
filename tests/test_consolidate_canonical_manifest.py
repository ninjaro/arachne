from __future__ import annotations

import copy
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock

from scripts import build_canonical_merge_plan
from scripts.build_canonical_merge_plan import PlanError
from scripts.consolidate_canonical_manifest import (
    ConsolidationError,
    consolidate,
    main as consolidate_main,
)


def _manifest() -> dict:
    return {
        "contract": "normalized_product_import_v1",
        "format_version": 1,
        "creators": [
            {
                "local_id": "agent-old",
                "canonical_id": "agent_old",
                "entity_type": "person",
                "names": [
                    {
                        "type": "original",
                        "language": "en",
                        "value": "Example, Alice",
                        "preferred": True,
                    }
                ],
            },
            {
                "local_id": "agent-live",
                "canonical_id": "agent_live",
                "entity_type": "person",
                "names": [
                    {
                        "type": "original",
                        "language": "en",
                        "value": "Alice Example",
                        "preferred": True,
                    }
                ],
            },
        ],
        "works": [
            {
                "local_id": "work-live",
                "canonical_id": "work_live",
                "titles": [
                    {
                        "type": "english",
                        "language": "en",
                        "value": "Example Work",
                        "preferred": True,
                    }
                ],
                "medium": "film",
            }
        ],
        "tags": [
            {
                "local_id": "concept-old",
                "name": "ageing",
                "type": "theme",
                "slug": "ageing",
            },
            {
                "local_id": "concept-live",
                "name": "aging",
                "type": "theme",
                "slug": "aging",
            },
        ],
        "manifestations": [],
        "measurements": [],
        "financial_facts": [],
        "remote_assets": [],
        "credits": [
            {
                "work": "work-live",
                "creator": "agent-old",
                "role": "director",
                "importance": "supporting",
            },
            {
                "work": "work-live",
                "creator": "agent-live",
                "role": "director",
                "importance": "primary",
            },
        ],
        "references": [
            {
                "ref_id": "source-old",
                "source_type": "web_page",
                "url": "https://example.test/source",
                "title": "Example source",
            },
            {
                "ref_id": "source-live",
                "source_type": "web_page",
                "url": "https://example.test/source/",
                "title": "Example source",
            },
        ],
        "assertions": [
            {
                "work": "work-live",
                "tag": "concept-old",
                "relation": "contains",
                "weight": 80,
                "confidence": 0.8,
                "evidence": [
                    {
                        "ref_id": "source-old",
                        "quote": "British spelling.",
                        "stance": "supports",
                    }
                ],
            },
            {
                "work": "work-live",
                "tag": "concept-live",
                "relation": "contains",
                "weight": 95,
                "confidence": 0.99,
                "evidence": [
                    {
                        "ref_id": "source-live",
                        "quote": "American spelling.",
                        "stance": "supports",
                    }
                ],
            },
        ],
        "concept_relations": [],
        "parent_guide_assertions": [],
    }


def _plan() -> dict:
    return {
        "contract": "canonical_merge_plan_v1",
        "format_version": 1,
        "agent_merges": [
            {
                "target": "agent-live",
                "members": ["agent-old", "agent-live"],
                "reason": "reviewed identity",
            }
        ],
        "work_merges": [],
        "concept_merges": [
            {
                "target": "concept-live",
                "members": ["concept-old", "concept-live"],
                "canonical": {
                    "name": "aging",
                    "type": "theme",
                    "slug": "aging",
                },
                "reason": "reviewed spelling equivalence",
            }
        ],
        "source_merges": [
            {
                "target": "source-live",
                "members": ["source-old", "source-live"],
                "reason": "reviewed URL equivalence",
            }
        ],
        "assertion_updates": [],
        "publisher_normalization": {},
        "manifestation_label_normalization": {},
        "blocked": {
            "agents": [],
            "works": [],
            "concepts": [],
            "sources": [],
        },
    }


class CanonicalConsolidationTests(unittest.TestCase):
    def test_merges_rewire_and_preserve_aliases_provenance(self) -> None:
        result, events, summary = consolidate(_manifest(), _plan())

        self.assertEqual(result["contract"], "normalized_product_import_v2")
        self.assertEqual(result["format_version"], 2)
        self.assertEqual([row["local_id"] for row in result["creators"]], ["agent-live"])
        self.assertEqual([row["local_id"] for row in result["tags"]], ["concept-live"])
        self.assertEqual([row["ref_id"] for row in result["references"]], ["source-live"])

        concept = result["tags"][0]
        self.assertIn("ageing", [row["value"] for row in concept["names"]])
        self.assertEqual(concept["slug_aliases"], ["ageing"])
        source = result["references"][0]
        self.assertEqual(
            source["alternate_urls"], ["https://example.test/source"]
        )

        entity_redirects = {
            row["alias_id"]: row for row in result["entity_redirects"]
        }
        self.assertEqual(
            entity_redirects["agent_old"]["canonical_id"], "agent_live"
        )
        self.assertNotIn("target_id", entity_redirects["agent_old"])
        self.assertIn("con_", entity_redirects[next(
            key for key in entity_redirects if key.startswith("con_")
        )]["canonical_id"])
        self.assertEqual(len(result["source_redirects"]), 1)
        self.assertIn("canonical_id", result["source_redirects"][0])

        self.assertEqual(len(result["credits"]), 1)
        self.assertEqual(result["credits"][0]["creator"], "agent-live")
        self.assertEqual(result["credits"][0]["importance"], "primary")
        self.assertEqual(len(result["assertions"]), 1)
        self.assertEqual(result["assertions"][0]["tag"], "concept-live")
        self.assertEqual(len(result["assertions"][0]["evidence"]), 2)
        self.assertTrue(summary["changed"])
        self.assertTrue(
            any(event["record_type"] == "canonical_merge" for event in events)
        )
        self.assertTrue(
            any(event["record_type"] == "merge_field_conflict" for event in events)
        )
        self.assertNotIn("_reviewed_survivor", json.dumps(events))

    def test_second_pass_is_a_no_op(self) -> None:
        first, _, _ = consolidate(_manifest(), _plan())
        second, events, summary = consolidate(first, _plan())

        self.assertEqual(second, first)
        self.assertEqual(events, [])
        self.assertFalse(summary["changed"])
        self.assertTrue(all(value == 0 for value in summary["delta"].values()))

    def test_blocked_merge_member_is_rejected(self) -> None:
        plan = copy.deepcopy(_plan())
        plan["blocked"]["concepts"] = ["concept-old"]

        with self.assertRaisesRegex(
            ConsolidationError, "blocked concepts appear in merge groups"
        ):
            consolidate(_manifest(), plan)

    def test_blocked_merge_target_is_rejected(self) -> None:
        plan = copy.deepcopy(_plan())
        plan["blocked"]["concepts"] = ["concept-live"]

        with self.assertRaisesRegex(
            ConsolidationError, "blocked concepts appear in merge groups"
        ):
            consolidate(_manifest(), plan)

    def test_redirect_chain_is_rejected(self) -> None:
        manifest = _manifest()
        manifest["contract"] = "normalized_product_import_v2"
        manifest["format_version"] = 2
        for tag in manifest["tags"]:
            tag["canonical_id"] = "con_" + tag["slug"]
            tag["names"] = []
            tag["slug_aliases"] = []
        for source in manifest["references"]:
            source["canonical_id"] = "src_" + source["ref_id"]
            source["alternate_urls"] = []
        manifest["entity_redirects"] = [
            {
                "alias_id": "agent_retired_earlier",
                "canonical_id": "agent_old",
                "entity_type": "person",
            }
        ]
        manifest["source_redirects"] = []

        with self.assertRaisesRegex(ConsolidationError, "redirect chains are forbidden"):
            consolidate(manifest, _plan())

    def test_redirect_type_must_match_target(self) -> None:
        result, _, _ = consolidate(_manifest(), _plan())
        result["entity_redirects"][0]["entity_type"] = "organization"

        with self.assertRaisesRegex(ConsolidationError, "has type"):
            consolidate(result, _plan())

    def test_slug_alias_may_not_repeat_live_slug(self) -> None:
        result, _, _ = consolidate(_manifest(), _plan())
        result["tags"][0]["slug_aliases"].append(result["tags"][0]["slug"])

        with self.assertRaisesRegex(
            ConsolidationError, "concept slug/alias is duplicated"
        ):
            consolidate(result, _plan())

    def test_alternate_url_may_not_repeat_primary_url(self) -> None:
        result, _, _ = consolidate(_manifest(), _plan())
        result["references"][0]["alternate_urls"].append(
            result["references"][0]["url"]
        )

        with self.assertRaisesRegex(
            ConsolidationError, "source primary/alternate URL is duplicated"
        ):
            consolidate(result, _plan())

    def test_evidence_locators_remain_scoped_to_each_assertion(self) -> None:
        manifest = _manifest()
        manifest["works"].append(
            {
                "local_id": "work-other",
                "canonical_id": "work_other",
                "titles": [
                    {
                        "type": "english",
                        "language": "en",
                        "value": "Other Work",
                        "preferred": True,
                    }
                ],
                "medium": "film",
            }
        )
        manifest["assertions"] = [
            {
                "work": "work-live",
                "tag": "concept-live",
                "relation": "contains",
                "weight": 90,
                "confidence": 0.9,
                "evidence": [
                    {
                        "ref_id": "source-live",
                        "quote": "Shared quotation.",
                        "stance": "supports",
                        "locator": {"field": "title"},
                    }
                ],
            },
            {
                "work": "work-other",
                "tag": "concept-live",
                "relation": "contains",
                "weight": 90,
                "confidence": 0.9,
                "evidence": [
                    {
                        "ref_id": "source-live",
                        "quote": "Shared quotation.",
                        "stance": "supports",
                        "locator": {"field": "genre"},
                    }
                ],
            },
        ]

        result, _, _ = consolidate(manifest, _plan())
        locators = {
            row["work"]: row["evidence"][0]["locator"]
            for row in result["assertions"]
        }
        self.assertEqual(locators["work-live"], {"field": "title"})
        self.assertEqual(locators["work-other"], {"field": "genre"})

    def test_financial_fact_stable_key_conflict_is_rejected(self) -> None:
        manifest = _manifest()
        base = {
            "work": "work-live",
            "type": "budget",
            "amount": 100,
            "currency": "USD",
        }
        manifest["financial_facts"] = [
            {**base, "estimated": False},
            {**base, "estimated": True},
        ]

        with self.assertRaisesRegex(
            ConsolidationError, "financial-fact stable-key conflict"
        ):
            consolidate(manifest, _plan())

    def test_remote_asset_stable_key_conflict_is_rejected(self) -> None:
        manifest = _manifest()
        base = {
            "entity": "work-live",
            "provider": "example",
            "remote_key": "123",
        }
        manifest["remote_assets"] = [
            {**base, "rights_note": "licensed"},
            {**base, "rights_note": "unknown"},
        ]

        with self.assertRaisesRegex(
            ConsolidationError, "remote-asset stable-key conflict"
        ):
            consolidate(manifest, _plan())

    def test_missing_history_for_applied_plan_fails_closed(self) -> None:
        result, _, _ = consolidate(_manifest(), _plan())
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest_path = root / "manifest.json"
            plan_path = root / "plan.json"
            provenance_path = root / "missing.jsonl"
            manifest_path.write_text(json.dumps(result), encoding="utf-8")
            plan_path.write_text(json.dumps(_plan()), encoding="utf-8")

            with self.assertRaisesRegex(
                ConsolidationError, "lineage provenance is missing"
            ):
                consolidate_main(
                    [
                        "--manifest",
                        str(manifest_path),
                        "--plan",
                        str(plan_path),
                        "--output",
                        str(manifest_path),
                        "--provenance",
                        str(provenance_path),
                        "--apply",
                    ]
                )

    def test_sidecar_may_not_overwrite_an_input(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest_path = root / "manifest.json"
            plan_path = root / "plan.json"
            output_path = root / "output.json"
            provenance_path = root / "events.jsonl"
            manifest_path.write_text(json.dumps(_manifest()), encoding="utf-8")
            plan_path.write_text(json.dumps(_plan()), encoding="utf-8")

            with self.assertRaisesRegex(
                ConsolidationError, "must not overwrite manifest or plan inputs"
            ):
                consolidate_main(
                    [
                        "--manifest",
                        str(manifest_path),
                        "--plan",
                        str(plan_path),
                        "--output",
                        str(output_path),
                        "--provenance",
                        str(provenance_path),
                        "--summary",
                        str(manifest_path),
                        "--apply",
                    ]
                )

    def test_plan_builder_fails_closed_on_unreviewed_heading(self) -> None:
        report = "### accepted\n\n### deferred\n\n### newly proposed\n"
        with (
            mock.patch.object(
                build_canonical_merge_plan,
                "ACCEPTED_CONCEPT_HEADINGS",
                {"accepted"},
            ),
            mock.patch.object(
                build_canonical_merge_plan,
                "DEFERRED_CONCEPT_HEADINGS",
                {"deferred"},
            ),
        ):
            with self.assertRaisesRegex(PlanError, "unreviewed headings"):
                build_canonical_merge_plan._concept_groups(report, [])


if __name__ == "__main__":
    unittest.main()
