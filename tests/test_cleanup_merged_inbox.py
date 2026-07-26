from __future__ import annotations

import base64
import io
import json
import os
import subprocess
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest import mock

from scripts.cleanup_merged_inbox import (
    CleanupError,
    SnapshotEntry,
    _contains_prior,
    _merge_prior_value,
    _non_json_lines,
    _normalize_with_context,
    _pointer_field,
    _retire_exact,
    _snapshot_directories,
    _snapshot_directory,
    _require_same_snapshot,
    _without_trailing_url_slash,
    canonical_id_stability_report,
)
from scripts.normalize_legacy_batches import Limits


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts" / "cleanup_merged_inbox.py"


def sample_batch() -> dict:
    return {
        "format_version": 1,
        "batch_id": "cleanup-example-001",
        "batch_type": "mining",
        "creators": [
            {
                "local_id": "creator-a",
                "name": "Example Artist",
                "entity_type": "person",
                "external_ids": {"wikidata": "Q123"},
            }
        ],
        "works": [
            {
                "local_id": "work-a",
                "titles": [
                    {
                        "value": "Example Work",
                        "type": "original",
                        "preferred": True,
                    }
                ],
                "medium": "film",
                "date": "2001",
                "external_ids": {"imdb_title": "tt0000123"},
                "review_note": {"retain": "exactly"},
            }
        ],
        "credits": [
            {
                "work": "work-a",
                "creator": "creator-a",
                "role": "director",
                "importance": "primary",
            }
        ],
        "tags": [
            {
                "local_id": "tag-a",
                "name": "Dream logic",
                "type": "theme",
            }
        ],
        "references": [
            {
                "ref_id": "ref-a",
                "source_type": "article",
                "title": "A source",
                "doi": "10.1234/cleanup",
            }
        ],
        "assertions": [
            {
                "work": "work-a",
                "tag": "tag-a",
                "relation": "contains",
                "weight": 80,
                "evidence": [
                    {
                        "ref_id": "ref-a",
                        "quote": "A precise quotation.",
                    }
                ],
            }
        ],
    }


def activated_manifest() -> dict:
    return {
        "contract": "normalized_product_import_v1",
        "format_version": 1,
        "creators": [],
        "works": [],
        "credits": [],
        "tags": [],
        "references": [],
        "assertions": [],
        "manifestations": [],
        "concept_relations": [],
        "measurements": [],
        "financial_facts": [],
        "parent_guide_assertions": [],
        "remote_assets": [],
    }


def activated_manifest_v2() -> dict:
    value = activated_manifest()
    value["contract"] = "normalized_product_import_v2"
    value["format_version"] = 2
    value["entity_redirects"] = []
    value["source_redirects"] = []
    return value


def activated_manifest_v3() -> dict:
    value = activated_manifest()
    value["contract"] = "normalized_product_import_v3"
    value["format_version"] = 3
    return value


def audited_problem_batch() -> dict:
    value = sample_batch()
    value["creators"][0]["external_ids"]["isni"] = {
        "value": "0000000120964752",
        "canonical_url": "https://isni.example/invalid-creator",
    }
    value["creators"].append(
        {
            "local_id": "creator-valid-15-character-isni",
            "name": "Foetus",
            "entity_type": "group",
            "external_ids": {"isni": "000000004912841"},
        }
    )
    value["works"][0]["titles"][0]["value"] = "Dr. Caligari"
    value["works"][0]["external_ids"]["isni"] = {
        "value": "bad-work-isni",
        "canonical_url": "https://isni.example/invalid-work",
    }
    value["works"][0]["financial_facts"] = [
        {
            "type": "budget",
            "amount": {"min": 175000, "max": None},
            "currency": "USD",
            "estimated": True,
            "qualifier": "amount offered for production",
        },
        {
            "type": "budget",
            "amount": 750000,
            "currency": "USD",
            "estimated": True,
            "qualifier": "reported total project financing",
        },
    ]
    value["credits"].extend(
        [
            {
                "work": "work-a",
                "creator": "creator-a",
                "role": "director",
                "importance": "key",
                "credit_order": 1,
            },
            {
                "work": "work-a",
                "creator": "creator-a",
                "role": "actor",
                "importance": "supporting",
                "credited_as": "Self",
            },
            {
                "work": "work-a",
                "creator": "creator-a",
                "role": "composer",
                "importance": "supporting",
                "credited_as": "self",
            },
            {
                "work": "work-a",
                "creator": "creator-a",
                "role": "producer",
                "importance": "supporting",
                "credited_as": "uncredited",
            },
        ]
    )
    value["tags"][0]["external_ids"] = {
        "isni": {
            "value": "bad-concept-isni",
            "canonical_url": "https://isni.example/invalid-concept",
        }
    }
    value["manifestations"] = [
        {
            "local_id": "manifestation-a",
            "work": "work-a",
            "type": "release",
            "label": "Restored release",
            "external_ids": {
                "isni": {
                    "value": "bad-manifestation-isni",
                    "canonical_url": "https://isni.example/invalid-manifestation",
                }
            },
        }
    ]
    value["references"].extend(
        [
            {
                "ref_id": "ref-pitchfork-a",
                "source_type": "article",
                "title": "Various Artists: No New York",
                "publisher": "Pitchfork",
                "bibliography": "Pitchfork review dated 10 October 2005.",
                "url": "https://pitchfork.com/reviews/albums/2105-no-new-york",
            },
            {
                "ref_id": "ref-pitchfork-b",
                "source_type": "article",
                "title": "No New York",
                "publisher": "Pitchfork",
                "bibliography": "Pitchfork review dated 22 November 2005.",
                "url": "https://pitchfork.com/reviews/albums/2105-no-new-york/",
            },
            {
                "ref_id": "ref-pitchfork-double-slash",
                "source_type": "article",
                "title": "A distinct double-slash path",
                "publisher": "Pitchfork",
                "bibliography": "This path is not trailing-slash equivalent.",
                "url": "https://pitchfork.com/reviews/albums/2105-no-new-york//",
            },
        ]
    )
    return value


def distinct_batch() -> dict:
    value = sample_batch()
    value["batch_id"] = "cleanup-example-002"
    value["creators"][0]["name"] = "Second Artist"
    value["creators"][0]["external_ids"] = {"wikidata": "Q456"}
    value["works"][0]["titles"][0]["value"] = "Second Work"
    value["works"][0]["date"] = "2002"
    value["works"][0]["external_ids"] = {"imdb_title": "tt0000456"}
    value["tags"][0]["name"] = "Second theme"
    value["references"][0]["doi"] = "10.1234/cleanup-second"
    return value


def normalization_collision_batch() -> dict:
    value = sample_batch()
    value["creators"].extend(
        [
            {
                "local_id": "creator-lcnaf",
                "name": "LOC Alias A",
                "entity_type": "person",
                "external_ids": {
                    "lcnaf": {
                        "value": "n123",
                        "canonical_url": "https://id.example/alias-a",
                    }
                },
            },
            {
                "local_id": "creator-loc",
                "name": "LOC Alias B",
                "entity_type": "person",
                "external_ids": {
                    "loc": {
                        "value": "n123",
                        "canonical_url": "https://id.example/alias-b",
                    }
                },
            },
        ]
    )
    value["references"].extend(
        [
            {
                "ref_id": "ref-doi-case",
                "source_type": "article",
                "title": "Case-variant DOI",
                "doi": "10.1234/CLEANUP",
            },
            {
                "ref_id": "ref-isbn-dashed",
                "source_type": "book",
                "title": "Dashed ISBN",
                "isbn": "978-0-306-40615-7",
            },
            {
                "ref_id": "ref-isbn-compact",
                "source_type": "book",
                "title": "Compact ISBN",
                "isbn": "9780306406157",
            },
        ]
    )
    return value


class CleanupMergedInboxTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="arachne-cleanup-test-")
        self.root = Path(self.temporary.name)
        self.inbox = self.root / "inbox"
        self.inbox.mkdir()
        self.manifest = self.root / "artifacts" / "manifest.json"
        self.issues = self.root / "artifacts" / "issues.jsonl"
        self.database = self.root / "database" / "canonical.sqlite"
        self.import_log = self.root / "import-log.json"
        self.binary = self.root / "fake-arachne"
        self.write_importer(exit_code=0)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def write_importer(self, *, exit_code: int) -> None:
        source = f"""#!/usr/bin/env python3
import json
import pathlib
import sys

arguments = sys.argv[1:]
pathlib.Path({str(self.import_log)!r}).write_text(json.dumps(arguments), encoding='utf-8')
if {exit_code} == 0:
    database = pathlib.Path(arguments[arguments.index('--database') + 1])
    database.parent.mkdir(parents=True, exist_ok=True)
    database.write_text('activated by Penelope test double', encoding='utf-8')
    print(json.dumps({{'activated': str(database)}}))
sys.exit({exit_code})
"""
        self.binary.write_text(source, encoding="utf-8")
        self.binary.chmod(0o755)

    def write_directory_importer(self) -> None:
        source = f"""#!/usr/bin/env python3
import pathlib
import sys

arguments = sys.argv[1:]
database = pathlib.Path(arguments[arguments.index('--database') + 1])
database.parent.mkdir(parents=True, exist_ok=True)
database.mkdir()
sys.exit(0)
"""
        self.binary.write_text(source, encoding="utf-8")
        self.binary.chmod(0o755)

    def write_mutating_importer(
        self, source_path: Path, replacement: dict
    ) -> None:
        replacement_text = json.dumps(replacement)
        source = f"""#!/usr/bin/env python3
import json
import pathlib
import sys

arguments = sys.argv[1:]
pathlib.Path({str(self.import_log)!r}).write_text(json.dumps(arguments), encoding='utf-8')
pathlib.Path({str(source_path)!r}).write_text({replacement_text!r}, encoding='utf-8')
database = pathlib.Path(arguments[arguments.index('--database') + 1])
database.parent.mkdir(parents=True, exist_ok=True)
database.write_text('activated before inbox mutation was detected', encoding='utf-8')
print(json.dumps({{'activated': str(database)}}))
"""
        self.binary.write_text(source, encoding="utf-8")
        self.binary.chmod(0o755)

    def write_json_batch(self, name: str = "batch.json") -> Path:
        destination = self.inbox / name
        destination.write_text(json.dumps(sample_batch()), encoding="utf-8")
        return destination

    def write_activated_v2(self, value: dict) -> None:
        self.manifest.parent.mkdir(parents=True, exist_ok=True)
        self.manifest.write_text(json.dumps(value), encoding="utf-8")

    def write_zip_batch(self) -> Path:
        destination = self.inbox / "opaque.zip"
        with zipfile.ZipFile(destination, "w") as archive:
            archive.writestr("nested/batch.json", json.dumps(sample_batch()))
            archive.writestr("source-review.txt", "line one\nline two\n")
            archive.writestr("source-review.bin", b"\xff\x00\x81")
        return destination

    def run_cleanup(
        self, *extra: str, input_path: Path | None = None
    ) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [
                sys.executable,
                str(SCRIPT),
                "--input",
                str(input_path or self.inbox),
                "--manifest",
                str(self.manifest),
                "--jsonl",
                str(self.issues),
                "--database",
                str(self.database),
                "--arachne-binary",
                str(self.binary),
                *extra,
            ],
            check=False,
            capture_output=True,
            text=True,
            cwd=ROOT,
            timeout=30,
        )

    def test_default_is_a_write_free_dry_run(self) -> None:
        source = self.write_zip_batch()

        result = self.run_cleanup()

        self.assertEqual(result.returncode, 0, result.stderr)
        summary = json.loads(result.stdout)
        self.assertEqual(summary["mode"], "dry-run")
        self.assertEqual(summary["container_count"], 1)
        self.assertTrue(source.is_file())
        self.assertFalse(self.manifest.exists())
        self.assertFalse(self.issues.exists())
        self.assertFalse(self.database.exists())
        self.assertFalse(self.import_log.exists())

    def test_apply_activates_import_then_retires_and_preserves_zip_bytes(self) -> None:
        self.write_zip_batch()

        result = self.run_cleanup("--apply")

        self.assertEqual(result.returncode, 0, result.stderr)
        summary = json.loads(result.stdout)
        self.assertEqual(summary["retired_containers"], 1)
        self.assertTrue(self.manifest.is_file())
        self.assertTrue(self.issues.is_file())
        self.assertTrue(self.database.is_file())
        self.assertEqual(list(self.inbox.iterdir()), [])
        self.assertFalse((self.root / ".inbox.cleanup-staging").exists())

        arguments = json.loads(self.import_log.read_text(encoding="utf-8"))
        self.assertEqual(arguments[0:2], ["product", "import-normalized"])
        imported_manifest = Path(
            arguments[arguments.index("--manifest") + 1]
        )
        self.assertEqual(imported_manifest.parent, self.manifest.parent)
        self.assertTrue(
            imported_manifest.name.startswith(f".{self.manifest.name}.")
        )
        self.assertFalse(imported_manifest.exists())
        lines = [
            json.loads(line)
            for line in self.issues.read_text(encoding="utf-8").splitlines()
        ]
        text_member = next(
            line
            for line in lines
            if line.get("record_type") == "archive_member"
            and line["source"]["member"] == "source-review.txt"
        )
        self.assertEqual(text_member["encoding"], "utf-8")
        self.assertEqual(text_member["value"], "line one\nline two\n")
        binary_member = next(
            line
            for line in lines
            if line.get("record_type") == "archive_member"
            and line["source"]["member"] == "source-review.bin"
        )
        self.assertEqual(binary_member["encoding"], "base64")
        self.assertEqual(binary_member["value"], "/wCB")
        note = next(
            line
            for line in lines
            if line.get("json_pointer") == "/works/0/review_note"
        )
        self.assertEqual(note["value"], {"retain": "exactly"})
        targets = note["context"]["canonical_targets"]
        self.assertTrue(any(target["kind"] == "work" for target in targets))
        agents = note["context"]["primary_or_key_agents"]
        self.assertEqual(agents[0]["role"], "director")

    def test_failed_import_never_retires_source(self) -> None:
        source = self.write_json_batch()
        self.write_importer(exit_code=9)

        result = self.run_cleanup("--apply")

        self.assertEqual(result.returncode, 2)
        self.assertIn("normalized import failed", result.stderr)
        self.assertTrue(source.is_file())
        self.assertFalse((self.root / ".inbox.cleanup-staging").exists())
        self.assertFalse(self.database.exists())

    def test_failed_v2_import_preserves_prior_canonical_artifacts(self) -> None:
        source = self.write_json_batch()
        prior = activated_manifest_v2()
        self.write_activated_v2(prior)
        self.issues.write_text(
            json.dumps(
                {
                    "record_type": "quarantine",
                    "format_version": 1,
                    "category": "prior",
                }
            )
            + "\n",
            encoding="utf-8",
        )
        self.database.parent.mkdir(parents=True, exist_ok=True)
        self.database.write_text("prior database", encoding="utf-8")
        manifest_before = self.manifest.read_bytes()
        issues_before = self.issues.read_bytes()
        database_before = self.database.read_bytes()
        self.write_importer(exit_code=9)

        result = self.run_cleanup("--apply")

        self.assertEqual(result.returncode, 2)
        self.assertIn("normalized import failed", result.stderr)
        self.assertTrue(source.is_file())
        self.assertEqual(self.manifest.read_bytes(), manifest_before)
        self.assertEqual(self.issues.read_bytes(), issues_before)
        self.assertEqual(self.database.read_bytes(), database_before)

    def test_inbox_mutation_during_successful_import_withholds_artifacts(
        self,
    ) -> None:
        source = self.write_json_batch()
        prior = activated_manifest_v2()
        self.write_activated_v2(prior)
        self.issues.write_text(
            json.dumps(
                {
                    "record_type": "quarantine",
                    "format_version": 1,
                    "category": "prior",
                }
            )
            + "\n",
            encoding="utf-8",
        )
        self.database.parent.mkdir(parents=True, exist_ok=True)
        self.database.write_text("prior database", encoding="utf-8")
        manifest_before = self.manifest.read_bytes()
        issues_before = self.issues.read_bytes()
        self.write_mutating_importer(source, distinct_batch())

        raced = self.run_cleanup("--apply")

        self.assertEqual(raced.returncode, 2)
        self.assertIn("inbox changed after analysis", raced.stderr)
        self.assertEqual(self.manifest.read_bytes(), manifest_before)
        self.assertEqual(self.issues.read_bytes(), issues_before)
        self.assertTrue(source.is_file())
        self.assertEqual(
            json.loads(source.read_text(encoding="utf-8")),
            distinct_batch(),
        )
        self.assertEqual(
            self.database.read_text(encoding="utf-8"),
            "activated before inbox mutation was detected",
        )

        self.write_importer(exit_code=0)
        converged = self.run_cleanup("--apply")

        self.assertEqual(converged.returncode, 0, converged.stderr)
        self.assertFalse(source.exists())
        merged = json.loads(self.manifest.read_text(encoding="utf-8"))
        titles = {
            title["value"]
            for work in merged["works"]
            for title in work.get("titles", [])
        }
        self.assertEqual(titles, {"Second Work"})

    def test_zero_exit_importer_must_activate_a_regular_database(self) -> None:
        source = self.write_json_batch()
        self.write_directory_importer()

        result = self.run_cleanup("--apply")

        self.assertEqual(result.returncode, 2)
        self.assertIn("regular database", result.stderr)
        self.assertTrue(self.database.is_dir())
        self.assertTrue(source.is_file())
        self.assertFalse((self.root / ".inbox.cleanup-staging").exists())

    def test_empty_inbox_is_idempotent_and_does_not_import(self) -> None:
        result = self.run_cleanup("--apply")

        self.assertEqual(result.returncode, 0, result.stderr)
        summary = json.loads(result.stdout)
        self.assertTrue(summary["empty_inbox"])
        self.assertFalse(self.import_log.exists())
        self.assertFalse(self.manifest.exists())
        self.assertFalse(self.issues.exists())

    def test_recovery_reanalyzes_and_reimports_before_retirement(self) -> None:
        staging = self.root / ".inbox.cleanup-staging"
        staging.mkdir()
        (staging / "remaining.json").write_text("{}", encoding="utf-8")
        self.manifest.parent.mkdir(parents=True)
        self.manifest.write_text(
            json.dumps(activated_manifest()), encoding="utf-8"
        )
        self.issues.write_text("", encoding="utf-8")
        self.database.parent.mkdir(parents=True)
        self.database.write_text("canonical", encoding="utf-8")

        result = self.run_cleanup("--apply")

        self.assertEqual(result.returncode, 0, result.stderr)
        summary = json.loads(result.stdout)
        self.assertTrue(summary["recovered"])
        self.assertEqual(summary["retired_containers"], 1)
        self.assertFalse(staging.exists())
        self.assertEqual(list(self.inbox.iterdir()), [])
        self.assertTrue(self.import_log.is_file())
        arguments = json.loads(self.import_log.read_text(encoding="utf-8"))
        self.assertEqual(arguments[0:2], ["product", "import-normalized"])

    def test_recovery_validates_artifacts_before_staged_deletion(self) -> None:
        staging = self.root / ".inbox.cleanup-staging"
        staging.mkdir()
        retained = staging / "remaining.json"
        retained.write_text("{}", encoding="utf-8")
        self.manifest.parent.mkdir(parents=True)
        self.manifest.write_text("{}", encoding="utf-8")
        self.issues.write_text("not JSON\n", encoding="utf-8")
        self.database.parent.mkdir(parents=True)
        self.database.write_text("canonical", encoding="utf-8")

        result = self.run_cleanup("--apply")

        self.assertEqual(result.returncode, 2)
        self.assertTrue(retained.is_file())
        self.assertFalse(self.import_log.exists())

    def test_nonempty_rerun_preserves_prior_manifest_and_adds_new_data(self) -> None:
        self.write_json_batch("first.json")
        first = self.run_cleanup("--apply")
        self.assertEqual(first.returncode, 0, first.stderr)
        prior = json.loads(self.manifest.read_text(encoding="utf-8"))

        (self.inbox / "second.json").write_text(
            json.dumps(distinct_batch()), encoding="utf-8"
        )
        second = self.run_cleanup("--apply")

        self.assertEqual(second.returncode, 0, second.stderr)
        summary = json.loads(second.stdout)
        merged = json.loads(self.manifest.read_text(encoding="utf-8"))
        for collection in (
            "creators",
            "works",
            "credits",
            "tags",
            "references",
            "assertions",
        ):
            for prior_record in prior[collection]:
                self.assertTrue(
                    any(
                        _contains_prior(prior_record, current_record)
                        for current_record in merged[collection]
                    ),
                    collection,
                )
        self.assertEqual(len(merged["creators"]), 2)
        self.assertEqual(len(merged["works"]), 2)
        prior_ids = {
            record["canonical_id"]
            for record in prior["creators"] + prior["works"]
        }
        merged_ids = {
            record["canonical_id"]
            for record in merged["creators"] + merged["works"]
        }
        self.assertTrue(prior_ids < merged_ids)
        self.assertEqual(
            summary["canonical_id_stability"]["canonical_id_removals"], []
        )
        self.assertTrue(
            summary["canonical_id_stability"]["canonical_id_additions"]
        )
        self.assertEqual(list(self.inbox.iterdir()), [])

    def test_v2_manifest_upgrades_to_product_only_v3_ids(self) -> None:
        prior = activated_manifest_v2()
        prior["creators"] = [
            {
                "local_id": "agent-900001",
                "canonical_id": "agent-900001",
                "entity_type": "person",
                "name": "Retained Person",
            }
        ]
        prior["entity_redirects"] = [
            {
                "alias_id": "agent-899999",
                "canonical_id": "agent-900001",
                "entity_type": "person",
            }
        ]
        prior_source_id = "src_" + "d" * 64
        prior_source_alias = "src_" + "e" * 64
        prior["references"] = [
            {
                "ref_id": "source-900001",
                "canonical_id": prior_source_id,
                "source_type": "web_page",
                "title": "Retained source",
                "url": "https://example.test/retained-source",
                "alternate_urls": [],
            }
        ]
        prior["source_redirects"] = [
            {
                "alias_id": prior_source_alias,
                "canonical_id": prior_source_id,
            }
        ]
        self.write_activated_v2(prior)
        self.write_json_batch()

        result = self.run_cleanup("--apply")

        self.assertEqual(result.returncode, 0, result.stderr)
        merged = json.loads(self.manifest.read_text(encoding="utf-8"))
        self.assertEqual(merged["contract"], "normalized_product_import_v3")
        self.assertEqual(merged["format_version"], 3)
        self.assertNotIn("entity_redirects", merged)
        self.assertNotIn("source_redirects", merged)

        concept = next(
            value for value in merged["tags"] if value["name"] == "Dream logic"
        )
        self.assertEqual(concept["canonical_id"], concept["local_id"])
        self.assertRegex(concept["canonical_id"], r"^concept-[0-9]{6,}$")
        self.assertEqual(concept["names"], [])
        self.assertNotIn("slug_aliases", concept)

        source = next(
            value for value in merged["references"]
            if value.get("doi") == "10.1234/cleanup"
        )
        self.assertNotIn("canonical_id", source)
        self.assertRegex(source["ref_id"], r"^source-[0-9]{6,}$")
        self.assertEqual(source["alternate_urls"], [])
        encoded = json.dumps(merged)
        self.assertNotIn('"entity_redirects"', encoded)
        self.assertNotIn('"source_redirects"', encoded)
        self.assertNotIn('"slug_aliases"', encoded)
        self.assertNotIn('"con_', encoded)
        self.assertNotIn('"src_', encoded)

    def test_v2_primary_concept_slug_reuses_prior_local_and_canonical_ids(
        self,
    ) -> None:
        prior = activated_manifest_v2()
        canonical_id = "con_" + "a" * 64
        prior["tags"] = [
            {
                "local_id": "concept-900001",
                "canonical_id": canonical_id,
                "name": "Dream logic",
                "names": [],
                "type": "theme",
                "slug": "dream-logic",
                "slug_aliases": ["oneiric-logic"],
            }
        ]
        self.write_activated_v2(prior)
        self.write_json_batch()

        result = self.run_cleanup("--apply")

        self.assertEqual(result.returncode, 0, result.stderr)
        merged = json.loads(self.manifest.read_text(encoding="utf-8"))
        self.assertEqual(len(merged["tags"]), 1)
        self.assertEqual(merged["tags"][0]["local_id"], "concept-900001")
        self.assertEqual(
            merged["tags"][0]["canonical_id"], "concept-900001"
        )
        self.assertNotIn("slug_aliases", merged["tags"][0])
        self.assertEqual(merged["assertions"][0]["tag"], "concept-900001")
        self.assertNotEqual(merged["assertions"][0]["tag"], canonical_id)

    def test_v2_concept_slug_alias_reuses_live_concept_endpoint(self) -> None:
        prior = activated_manifest_v2()
        canonical_id = "con_" + "b" * 64
        prior["tags"] = [
            {
                "local_id": "concept-900002",
                "canonical_id": canonical_id,
                "name": "Dream logic",
                "names": [],
                "type": "theme",
                "slug": "dream-logic",
                "slug_aliases": ["oneiric-logic"],
            }
        ]
        batch = sample_batch()
        batch["tags"][0]["name"] = "Oneiric logic"
        self.write_activated_v2(prior)
        (self.inbox / "batch.json").write_text(
            json.dumps(batch), encoding="utf-8"
        )

        result = self.run_cleanup("--apply")

        self.assertEqual(result.returncode, 0, result.stderr)
        merged = json.loads(self.manifest.read_text(encoding="utf-8"))
        self.assertEqual(len(merged["tags"]), 1)
        concept = merged["tags"][0]
        self.assertEqual(concept["local_id"], "concept-900002")
        self.assertEqual(concept["canonical_id"], "concept-900002")
        self.assertEqual(concept["slug"], "dream-logic")
        self.assertNotIn("slug_aliases", concept)
        self.assertEqual(merged["assertions"][0]["tag"], "concept-900002")
        aliases = {
            name["value"]
            for name in concept.get("names", [])
            if name.get("preferred") is False
        }
        self.assertIn("Oneiric logic", aliases)

    def test_v2_source_alternate_url_reuses_live_source_endpoint(self) -> None:
        prior = activated_manifest_v2()
        canonical_id = "src_" + "c" * 64
        alternate_url = "https://example.test/source-without-slash"
        prior["references"] = [
            {
                "ref_id": "source-900001",
                "canonical_id": canonical_id,
                "source_type": "article",
                "title": "A source",
                "url": "https://example.test/source/",
                "alternate_urls": [alternate_url],
            }
        ]
        batch = sample_batch()
        batch["references"][0].pop("doi")
        batch["references"][0]["url"] = alternate_url
        self.write_activated_v2(prior)
        (self.inbox / "batch.json").write_text(
            json.dumps(batch), encoding="utf-8"
        )

        result = self.run_cleanup("--apply")

        self.assertEqual(result.returncode, 0, result.stderr)
        merged = json.loads(self.manifest.read_text(encoding="utf-8"))
        self.assertEqual(len(merged["references"]), 1)
        source = merged["references"][0]
        self.assertEqual(source["ref_id"], "source-900001")
        self.assertNotIn("canonical_id", source)
        self.assertEqual(source["url"], "https://example.test/source/")
        self.assertEqual(source["alternate_urls"], [alternate_url])
        self.assertEqual(
            merged["assertions"][0]["evidence"][0]["ref_id"],
            "source-900001",
        )

    def test_v2_work_never_matches_on_title_date_and_medium_alone(
        self,
    ) -> None:
        prior = activated_manifest_v2()
        prior["works"] = [
            {
                "local_id": "work-900001",
                "canonical_id": "work-900001",
                "titles": [
                    {
                        "value": "Example Work",
                        "type": "original",
                        "preferred": True,
                    }
                ],
                "medium": "film",
                "date": "2001",
            }
        ]
        value = sample_batch()
        value["works"][0].pop("external_ids")
        self.write_activated_v2(prior)
        (self.inbox / "batch.json").write_text(
            json.dumps(value), encoding="utf-8"
        )

        result = self.run_cleanup("--apply")

        self.assertEqual(result.returncode, 0, result.stderr)
        merged = json.loads(self.manifest.read_text(encoding="utf-8"))
        self.assertEqual(len(merged["works"]), 2)
        self.assertIn(
            "work-900001", {work["local_id"] for work in merged["works"]}
        )

    def test_v2_ambiguous_creator_and_dependent_credit_are_quarantined(
        self,
    ) -> None:
        prior = activated_manifest_v2()
        prior["creators"] = [
            {
                "local_id": "agent-900001",
                "canonical_id": "agent-900001",
                "entity_type": "person",
                "name": "Example Artist",
            },
            {
                "local_id": "agent-900002",
                "canonical_id": "agent-900002",
                "entity_type": "person",
                "name": "Example Artist",
            },
        ]
        batch = sample_batch()
        batch["creators"][0].pop("external_ids")
        self.write_activated_v2(prior)
        (self.inbox / "batch.json").write_text(
            json.dumps(batch), encoding="utf-8"
        )

        result = self.run_cleanup("--apply")

        self.assertEqual(result.returncode, 0, result.stderr)
        merged = json.loads(self.manifest.read_text(encoding="utf-8"))
        self.assertEqual(
            {creator["local_id"] for creator in merged["creators"]},
            {"agent-900001", "agent-900002"},
        )
        self.assertFalse(merged["credits"])
        lines = [
            json.loads(line)
            for line in self.issues.read_text(encoding="utf-8").splitlines()
        ]
        ambiguity = [
            line
            for line in lines
            if "ambig" in str(line.get("category", "")).casefold()
            and "identity" in str(line.get("category", "")).casefold()
        ]
        self.assertTrue(ambiguity, lines)
        encoded = json.dumps(ambiguity, ensure_ascii=False, sort_keys=True)
        self.assertIn("Example Artist", encoded)
        self.assertIn("agent-900001", encoded)
        self.assertIn("agent-900002", encoded)
        quarantined_credit = [
            line
            for line in lines
            if "quarant" in json.dumps(line, ensure_ascii=False).casefold()
            and '"role": "director"' in json.dumps(
                line, ensure_ascii=False, sort_keys=True
            )
        ]
        self.assertTrue(quarantined_credit, lines)
        self.assertIn(
            "Example Work",
            json.dumps(
                quarantined_credit, ensure_ascii=False, sort_keys=True
            ),
        )

    def test_v3_ambiguous_creator_and_dependent_credit_are_quarantined(
        self,
    ) -> None:
        prior = activated_manifest_v3()
        prior["creators"] = [
            {
                "local_id": "agent-900001",
                "canonical_id": "agent-900001",
                "entity_type": "person",
                "name": "Example Artist",
            },
            {
                "local_id": "agent-900002",
                "canonical_id": "agent-900002",
                "entity_type": "person",
                "name": "Example Artist",
            },
        ]
        batch = sample_batch()
        batch["creators"][0].pop("external_ids")
        self.write_activated_v2(prior)
        (self.inbox / "batch.json").write_text(
            json.dumps(batch), encoding="utf-8"
        )

        result = self.run_cleanup("--apply")

        self.assertEqual(result.returncode, 0, result.stderr)
        merged = json.loads(self.manifest.read_text(encoding="utf-8"))
        self.assertEqual(
            {creator["local_id"] for creator in merged["creators"]},
            {"agent-900001", "agent-900002"},
        )
        self.assertFalse(merged["credits"])
        lines = [
            json.loads(line)
            for line in self.issues.read_text(encoding="utf-8").splitlines()
        ]
        ambiguity = next(
            line
            for line in lines
            if line.get("category") == "canonical_identity_ambiguity"
        )
        self.assertEqual(
            ambiguity["identity"]["candidate_transport_ids"],
            ["agent-900001", "agent-900002"],
        )
        self.assertEqual(ambiguity["field"], "identity")
        dependency = next(
            line
            for line in lines
            if line.get("category")
            == "quarantined_ambiguous_identity_dependency"
        )
        self.assertEqual(
            dependency["identity"]["dependent_collection"], "credits"
        )
        self.assertEqual(
            dependency["occurrences"][0]["value"]["role"], "director"
        )
        self.assertIn(
            "Example Work",
            json.dumps(
                dependency["occurrences"][0]["context"],
                ensure_ascii=False,
                sort_keys=True,
            ),
        )

    def test_staged_new_bytes_are_merged_and_transactionally_reimported(self) -> None:
        self.write_json_batch("first.json")
        first = self.run_cleanup("--apply")
        self.assertEqual(first.returncode, 0, first.stderr)
        self.import_log.unlink()
        staging = self.root / ".inbox.cleanup-staging"
        staging.mkdir()
        staged = staging / "added-after-crash.json"
        staged.write_text(json.dumps(distinct_batch()), encoding="utf-8")

        recovery = self.run_cleanup("--apply")

        self.assertEqual(recovery.returncode, 0, recovery.stderr)
        summary = json.loads(recovery.stdout)
        self.assertTrue(summary["recovered"])
        self.assertTrue(summary["recovery_input"])
        self.assertTrue(self.import_log.is_file())
        merged = json.loads(self.manifest.read_text(encoding="utf-8"))
        self.assertEqual(len(merged["creators"]), 2)
        self.assertFalse(staging.exists())

    def test_staged_duplicate_round_trips_prior_manifest_exactly(self) -> None:
        source = self.write_json_batch("first.json")
        original = source.read_bytes()
        first = self.run_cleanup("--apply")
        self.assertEqual(first.returncode, 0, first.stderr)
        prior = json.loads(self.manifest.read_text(encoding="utf-8"))
        self.import_log.unlink()
        staging = self.root / ".inbox.cleanup-staging"
        staging.mkdir()
        (staging / "first.json").write_bytes(original)

        recovery = self.run_cleanup("--apply")

        self.assertEqual(recovery.returncode, 0, recovery.stderr)
        self.assertTrue(self.import_log.is_file())
        round_tripped = json.loads(self.manifest.read_text(encoding="utf-8"))
        self.assertEqual(round_tripped, prior)

    def test_preferred_label_conflict_keeps_prior_and_only_adds_alias(self) -> None:
        prior = [
            {"value": "Prior title", "type": "original", "preferred": True}
        ]
        current = [
            {"value": "New title", "type": "original", "preferred": True},
            {"value": "Safe alias", "type": "alias", "preferred": False},
        ]
        conflicts: list[tuple[str, object, object]] = []

        merged = _merge_prior_value(prior, current, "titles", conflicts)

        self.assertIn(prior[0], merged)
        self.assertIn(current[1], merged)
        self.assertNotIn(current[0], merged)
        self.assertEqual(conflicts[0][0], "titles")

    def test_evidence_merge_uses_penelope_logical_identity(self) -> None:
        prior = [
            {
                "ref_id": "source-1",
                "quote": "Exact words.",
                "locator": {"page": 4},
                "stance": "supports",
                "language": "en",
                "translation": "Prior translation",
                "payload": {"edition": "prior"},
            }
        ]
        current = [
            {
                "ref_id": "source-1",
                "quote": "Exact words.",
                "locator": {"page": 4},
                "language": "de",
                "translation": "Current translation",
                "payload": {"edition": "current", "page_image": 7},
            }
        ]
        conflicts: list[tuple[str, object, object]] = []

        merged = _merge_prior_value(prior, current, "evidence", conflicts)

        self.assertEqual(len(merged), 1)
        self.assertEqual(merged[0]["language"], "en")
        self.assertEqual(merged[0]["translation"], "Prior translation")
        self.assertEqual(merged[0]["payload"]["edition"], "prior")
        self.assertEqual(merged[0]["payload"]["page_image"], 7)
        self.assertEqual(
            {path for path, _, _ in conflicts},
            {
                "evidence.language",
                "evidence.payload.edition",
                "evidence.translation",
            },
        )

    def test_evidence_merge_conflict_is_exported_as_canonical_jsonl(self) -> None:
        first_batch = sample_batch()
        first_batch["assertions"][0]["evidence"][0]["language"] = "en"
        (self.inbox / "first.json").write_text(
            json.dumps(first_batch), encoding="utf-8"
        )
        first = self.run_cleanup("--apply")
        self.assertEqual(first.returncode, 0, first.stderr)

        second_batch = sample_batch()
        second_batch["batch_id"] = "cleanup-example-002"
        second_batch["assertions"][0]["evidence"][0]["language"] = "de"
        (self.inbox / "second.json").write_text(
            json.dumps(second_batch), encoding="utf-8"
        )

        second = self.run_cleanup("--apply")

        self.assertEqual(second.returncode, 0, second.stderr)
        lines = [
            json.loads(line)
            for line in self.issues.read_text(encoding="utf-8").splitlines()
        ]
        conflict = next(
            line
            for line in lines
            if line.get("category") == "canonical_manifest_merge_conflict"
            and line.get("field") == "evidence.language"
        )
        self.assertEqual(
            [item["value"] for item in conflict["occurrences"]], ["en", "de"]
        )
        merged = json.loads(self.manifest.read_text(encoding="utf-8"))
        self.assertEqual(
            merged["assertions"][0]["evidence"][0]["language"], "en"
        )

    def test_normalization_uses_captured_bytes_despite_live_mutation(self) -> None:
        source = self.write_json_batch()
        captured = _snapshot_directory(self.inbox)
        source.write_text(json.dumps(distinct_batch()), encoding="utf-8")

        manifest, _, _, _, _ = _normalize_with_context(
            self.inbox, snapshot=captured
        )

        titles = {
            title["value"]
            for work in manifest["works"]
            for title in work.get("titles", [])
        }
        self.assertEqual(titles, {"Example Work"})
        source.write_bytes(captured[0].content)
        self.assertEqual(_require_same_snapshot(self.inbox, captured), captured)

    def test_retirement_never_unlinks_a_concurrently_recreated_name(self) -> None:
        staging = self.root / "race-staging"
        staging.mkdir()
        source = staging / "batch.json"
        source.write_bytes(b"captured bytes")
        captured = _snapshot_directory(staging)
        directories = _snapshot_directories(staging)
        real_rename = os.rename

        def rename_then_recreate(old: object, new: object) -> None:
            real_rename(old, new)
            if Path(old) == source:
                source.write_bytes(b"concurrent new input")

        with mock.patch(
            "scripts.cleanup_merged_inbox.os.rename",
            side_effect=rename_then_recreate,
        ):
            with self.assertRaises(OSError):
                _retire_exact(staging, captured, directories)

        self.assertEqual(source.read_bytes(), b"concurrent new input")

    def test_retirement_restores_isolated_file_when_bytes_mismatch(self) -> None:
        staging = self.root / "mismatch-staging"
        staging.mkdir()
        source = staging / "batch.json"
        source.write_bytes(b"captured bytes")
        captured = _snapshot_directory(staging)
        directories = _snapshot_directories(staging)
        real_rename = os.rename

        def rename_then_tamper(old: object, new: object) -> None:
            real_rename(old, new)
            if Path(old) == source:
                Path(new).write_bytes(b"changed after isolation")

        with mock.patch(
            "scripts.cleanup_merged_inbox.os.rename",
            side_effect=rename_then_tamper,
        ):
            with self.assertRaises(CleanupError):
                _retire_exact(staging, captured, directories)

        self.assertEqual(source.read_bytes(), b"changed after isolation")

    def test_retirement_rejects_unsnapshotted_empty_directory(self) -> None:
        staging = self.root / "directory-race-staging"
        staging.mkdir()
        source = staging / "batch.json"
        source.write_bytes(b"captured bytes")
        captured = _snapshot_directory(staging)
        directories = _snapshot_directories(staging)
        new_directory = staging / "new-empty-directory"
        new_directory.mkdir()

        with self.assertRaises(CleanupError):
            _retire_exact(staging, captured, directories)

        self.assertTrue(source.is_file())
        self.assertTrue(new_directory.is_dir())

    def test_retirement_isolates_snapshotted_empty_directories(self) -> None:
        staging = self.root / "empty-directory-staging"
        empty_directory = staging / "nested" / "empty"
        empty_directory.mkdir(parents=True)
        captured = _snapshot_directory(staging)
        directories = _snapshot_directories(staging)

        _retire_exact(staging, captured, directories)

        self.assertFalse(staging.exists())

    def test_retirement_preserves_concurrently_recreated_directory(self) -> None:
        staging = self.root / "directory-recreation-staging"
        tracked = staging / "tracked-empty"
        tracked.mkdir(parents=True)
        captured = _snapshot_directory(staging)
        directories = _snapshot_directories(staging)
        real_rename = os.rename

        def rename_then_recreate(old: object, new: object) -> None:
            real_rename(old, new)
            if Path(old) == tracked:
                tracked.mkdir()

        with mock.patch(
            "scripts.cleanup_merged_inbox.os.rename",
            side_effect=rename_then_recreate,
        ):
            with self.assertRaises(OSError):
                _retire_exact(staging, captured, directories)

        self.assertTrue(tracked.is_dir())

    def test_trailing_slash_audit_groups_root_but_not_repeated_paths(self) -> None:
        self.assertEqual(
            _without_trailing_url_slash("https://example.test"),
            _without_trailing_url_slash("https://example.test/"),
        )
        self.assertEqual(
            _without_trailing_url_slash("https://example.test/path"),
            _without_trailing_url_slash("https://example.test/path/"),
        )
        self.assertIsNone(
            _without_trailing_url_slash("https://example.test/path//")
        )

    def test_failed_staged_reimport_never_retires_staged_bytes(self) -> None:
        self.write_json_batch("first.json")
        first = self.run_cleanup("--apply")
        self.assertEqual(first.returncode, 0, first.stderr)
        staging = self.root / ".inbox.cleanup-staging"
        staging.mkdir()
        staged = staging / "added-after-crash.json"
        staged.write_text(json.dumps(distinct_batch()), encoding="utf-8")
        self.write_importer(exit_code=9)

        recovery = self.run_cleanup("--apply")

        self.assertEqual(recovery.returncode, 2)
        self.assertIn("normalized import failed", recovery.stderr)
        self.assertTrue(staged.is_file())

    def test_oversized_json_bytes_are_preserved_for_files_and_zip_members(self) -> None:
        regular = b'{"long":"abcdefghij"}'
        archive_stream = io.BytesIO()
        archived = b"\xff\x00oversized-json"
        with zipfile.ZipFile(archive_stream, "w") as archive:
            archive.writestr("batch.json", b"{}")
            archive.writestr("large.json", archived)
        lines = _non_json_lines(
            (
                SnapshotEntry("large.json", regular),
                SnapshotEntry("large.zip", archive_stream.getvalue()),
            ),
            Limits(
                maximum_json_bytes=8,
                maximum_zip_members=10,
                maximum_zip_uncompressed_bytes=1024,
            ),
        )

        file_line = next(
            line
            for line in lines
            if line["category"] == "oversized_json_container"
        )
        self.assertEqual(file_line["encoding"], "utf-8")
        self.assertEqual(file_line["value"].encode("utf-8"), regular)
        member_line = next(
            line
            for line in lines
            if line["category"] == "oversized_json_archive_member"
        )
        self.assertEqual(member_line["encoding"], "base64")
        self.assertEqual(base64.b64decode(member_line["value"]), archived)
        container_line = next(
            line
            for line in lines
            if line["category"] == "archive_level_json_observation_limit"
        )
        self.assertEqual(container_line["encoding"], "base64")
        self.assertEqual(
            base64.b64decode(container_line["value"]),
            archive_stream.getvalue(),
        )
        self.assertEqual(container_line["oversized_members"], ["large.json"])

    def test_ambiguous_archive_preserves_exact_container_bytes(self) -> None:
        archive_stream = io.BytesIO()
        with zipfile.ZipFile(archive_stream, "w") as archive:
            archive.writestr("first.json", json.dumps(sample_batch()))
            archive.writestr("second.json", json.dumps(distinct_batch()))
        captured = archive_stream.getvalue()

        lines = _non_json_lines(
            (SnapshotEntry("ambiguous.zip", captured),)
        )

        container = next(
            line
            for line in lines
            if line["category"] == "ambiguous_archive_container"
        )
        self.assertEqual(container["byte_length"], len(captured))
        self.assertEqual(
            container["candidate_members"], ["first.json", "second.json"]
        )
        restored = (
            container["value"].encode("utf-8")
            if container["encoding"] == "utf-8"
            else base64.b64decode(container["value"])
        )
        self.assertEqual(restored, captured)

    def test_normalization_collisions_are_exported_with_entity_context(self) -> None:
        (self.inbox / "collisions.json").write_text(
            json.dumps(normalization_collision_batch()), encoding="utf-8"
        )

        result = self.run_cleanup("--apply")

        self.assertEqual(result.returncode, 0, result.stderr)
        lines = [
            json.loads(line)
            for line in self.issues.read_text(encoding="utf-8").splitlines()
        ]
        external = next(
            line
            for line in lines
            if line.get("category")
            == "external_identifier_normalization_collision"
        )
        self.assertEqual(external["identity"]["canonical_scheme"], "loc")
        self.assertEqual(
            {
                occurrence["value"]["canonical_url"]
                for occurrence in external["occurrences"]
            },
            {"https://id.example/alias-a", "https://id.example/alias-b"},
        )
        self.assertTrue(
            all(
                occurrence["context"]["canonical_targets"]
                for occurrence in external["occurrences"]
            )
        )
        self.assertTrue(
            any(line.get("category") == "normalized_doi_collision" for line in lines)
        )
        self.assertTrue(
            any(line.get("category") == "normalized_isbn_collision" for line in lines)
        )

    def test_supplemental_audits_are_deterministic_and_entity_specific(self) -> None:
        destination = self.inbox / "audited.json"
        destination.write_text(json.dumps(audited_problem_batch()), encoding="utf-8")

        first = self.run_cleanup("--apply")

        self.assertEqual(first.returncode, 0, first.stderr)
        lines = [
            json.loads(line)
            for line in self.issues.read_text(encoding="utf-8").splitlines()
        ]
        placeholders = [
            line for line in lines if line.get("category") == "placeholder_credited_as"
        ]
        self.assertEqual(
            [line["value"] for line in placeholders], ["Self", "self", "uncredited"]
        )
        invalid_isni = next(
            line
            for line in lines
            if line.get("category") == "invalid_isni"
            and line["identity"]["supplied_isni"] == "0000000120964752"
        )
        self.assertEqual(invalid_isni["field"], "external_ids.isni")
        self.assertEqual(
            invalid_isni["identity"]["supplied_isni"], "0000000120964752"
        )
        self.assertEqual(
            invalid_isni["occurrences"][0]["context"]["labels"],
            ["Example Artist"],
        )
        self.assertEqual(
            invalid_isni["occurrences"][0]["value"]["canonical_url"],
            "https://isni.example/invalid-creator",
        )
        invalid_isni_lines = [
            line for line in lines if line.get("category") == "invalid_isni"
        ]
        self.assertEqual(
            {
                occurrence["context"]["collection"]
                for line in invalid_isni_lines
                for occurrence in line["occurrences"]
            },
            {"creators", "works", "tags", "manifestations"},
        )
        quarantined_isni_text = json.dumps(
            invalid_isni_lines
        )
        self.assertNotIn("000000004912841", quarantined_isni_text)
        duplicate_credit = next(
            line
            for line in lines
            if line.get("category") == "duplicate_logical_credit_order_importance"
        )
        self.assertEqual(duplicate_credit["identity"]["role"], "director")
        budget = next(
            line for line in lines if line.get("category") == "multiple_budget_values"
        )
        self.assertEqual(budget["field"], "financial_facts")
        self.assertEqual(
            budget["occurrences"][0]["context"]["field"], "financial_facts"
        )
        self.assertIn(
            "Dr. Caligari",
            budget["occurrences"][0]["context"]["labels"],
        )
        self.assertTrue(
            any(
                occurrence["value"]["source_record"].get("amount")
                == {"min": 175000, "max": None}
                for occurrence in budget["occurrences"]
            )
        )
        pitchfork = next(
            line
            for line in lines
            if line.get("category") == "trailing_slash_source_metadata_conflict"
        )
        self.assertIn("pitchfork.com", json.dumps(pitchfork).lower())
        self.assertEqual(len(pitchfork["occurrences"]), 2)
        self.assertTrue(
            all(
                not occurrence["value"]["url"].endswith("//")
                for occurrence in pitchfork["occurrences"]
            )
        )
        activated_once = self.issues.read_bytes()

        # Empty-inbox reruns are a no-op and cannot perturb activated JSONL.
        second = self.run_cleanup("--apply")
        self.assertEqual(second.returncode, 0, second.stderr)
        self.assertEqual(self.issues.read_bytes(), activated_once)

    def test_nested_pointer_context_uses_record_field_not_array_index(self) -> None:
        self.assertEqual(
            _pointer_field("/works/2/financial_facts/0"), "financial_facts"
        )
        self.assertEqual(
            _pointer_field("/manifestations/4/measurements/3"), "measurements"
        )
        self.assertEqual(
            _pointer_field("/manifestations/4/measurements/3/value"), "value"
        )

    def test_work_conflict_context_aggregates_agents_across_documents(self) -> None:
        credited = sample_batch()
        uncredited_occurrence = sample_batch()
        uncredited_occurrence["batch_id"] = "cleanup-example-002"
        uncredited_occurrence["creators"][0]["local_id"] = "creator-b"
        uncredited_occurrence["works"][0]["local_id"] = "work-b"
        uncredited_occurrence["works"][0]["date"] = "2002"
        uncredited_occurrence["credits"] = []
        uncredited_occurrence["assertions"] = []
        (self.inbox / "credited.json").write_text(
            json.dumps(credited), encoding="utf-8"
        )
        (self.inbox / "uncredited-occurrence.json").write_text(
            json.dumps(uncredited_occurrence), encoding="utf-8"
        )

        result = self.run_cleanup("--apply")

        self.assertEqual(result.returncode, 0, result.stderr)
        lines = [
            json.loads(line)
            for line in self.issues.read_text(encoding="utf-8").splitlines()
        ]
        date_conflict = next(
            line
            for line in lines
            if line.get("record_type") == "conflict" and line.get("field") == "date"
        )
        second_batch = next(
            occurrence
            for occurrence in date_conflict["occurrences"]
            if occurrence["source"].get("batch_id") == "cleanup-example-002"
        )
        self.assertEqual(
            second_batch["context"]["primary_or_key_agents"][0]["labels"],
            ["Example Artist"],
        )

    def test_output_inside_inbox_is_rejected_before_writes(self) -> None:
        source = self.write_json_batch()
        self.manifest = self.inbox / "manifest.json"

        result = self.run_cleanup("--apply")

        self.assertEqual(result.returncode, 2)
        self.assertIn("outside inbox", result.stderr)
        self.assertTrue(source.exists())
        self.assertFalse(self.import_log.exists())

    def test_symbolic_link_container_is_rejected(self) -> None:
        outside = self.root / "outside.json"
        outside.write_text(json.dumps(sample_batch()), encoding="utf-8")
        os.symlink(outside, self.inbox / "linked.json")

        result = self.run_cleanup()

        self.assertEqual(result.returncode, 2)
        self.assertIn("non-regular corpus entry", result.stderr)
        self.assertTrue(outside.exists())

    def test_canonical_id_stability_gate_fails_closed(self) -> None:
        previous = {
            "contract": "normalized_product_import_v1",
            "format_version": 1,
            "creators": [
                {
                    "local_id": "agent-old",
                    "canonical_id": "agent-old",
                    "entity_type": "person",
                    "name": "Stable Person",
                    "external_ids": {"wikidata": "Q999"},
                }
            ],
            "works": [],
            "credits": [],
            "tags": [],
            "manifestations": [],
        }
        current = json.loads(json.dumps(previous))
        current["creators"][0]["local_id"] = "agent-new"
        current["creators"][0]["canonical_id"] = "agent-new"

        with self.assertRaises(CleanupError):
            canonical_id_stability_report(previous, current)

        unchanged = canonical_id_stability_report(previous, previous)
        self.assertEqual(unchanged["matched_identities"], 1)
        self.assertTrue(unchanged["canonical_id_universe_equal"])

        duplicate = json.loads(json.dumps(previous["creators"][0]))
        duplicate["local_id"] = "agent-duplicate"
        duplicate["canonical_id"] = "agent-duplicate"
        previous_with_duplicate = json.loads(json.dumps(previous))
        previous_with_duplicate["creators"].append(duplicate)
        consolidated = canonical_id_stability_report(
            previous_with_duplicate, previous
        )
        self.assertEqual(consolidated["changed_duplicate_signature_count"], 1)
        self.assertEqual(
            consolidated["canonical_id_removals"], ["agent-duplicate"]
        )

    def test_v2_source_compatibility_id_is_ignored_during_upgrade(self) -> None:
        previous = activated_manifest_v2()
        previous["references"] = [
            {
                "ref_id": "source-1",
                "canonical_id": "src_" + "a" * 64,
                "source_type": "web_page",
                "title": "Stable source",
                "url": "https://example.test/stable-source",
                "alternate_urls": [],
            }
        ]
        current = json.loads(json.dumps(previous))
        current["references"][0]["canonical_id"] = "src_" + "b" * 64

        report = canonical_id_stability_report(previous, current)
        self.assertEqual(report["matched_identities"], 1)
        self.assertTrue(report["canonical_id_universe_equal"])
        self.assertEqual(report["canonical_id_removals"], [])

    def test_cleanup_implementation_never_opens_sqlite(self) -> None:
        source = SCRIPT.read_text(encoding="utf-8").lower()
        self.assertNotIn("import sqlite3", source)
        self.assertNotIn("from sqlite3", source)


if __name__ == "__main__":
    unittest.main()
