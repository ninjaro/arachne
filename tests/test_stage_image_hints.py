from __future__ import annotations

import copy
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
STAGER = ROOT / "viewer" / "scripts" / "stage_image_hints.py"
SHA_A = "a" * 64
SHA_B = "b" * 64
SHA_C = "c" * 64


class StageImageHintsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.artifact = self.root / "wikidata-image-hints.json"
        self.control = self.root / "product-control.json"
        self.output = self.root / "public" / "data" / "wikidata-image-hints.json"
        self.write_artifact()
        self.write_control()

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def write_artifact(
        self,
        *,
        content_sha256: str = SHA_A,
        entities: list[dict[str, Any]] | None = None,
    ) -> None:
        self.artifact.write_text(
            json.dumps(
                {
                    "artifact_type": "wikidata_image_hints_v1",
                    "format_version": 1,
                    "source_snapshot": {
                        "snapshot_id": "wikidata-20260801",
                        "storage_ref": "sources/wikidata-20260801.json.bz2",
                        "sha256": SHA_C,
                    },
                    "product_snapshot": {
                        "snapshot_id": "product-20260801",
                        "content_sha256": content_sha256,
                        "export_sha256": SHA_B,
                    },
                    "entities": entities if entities is not None else [],
                },
                sort_keys=True,
            )
            + "\n",
            encoding="utf-8",
        )

    def write_control(self) -> None:
        self.control.write_text(
            json.dumps(
                {
                    "snapshot_id": "product-20260801",
                    "content_sha256": SHA_A,
                    "exports": [
                        {
                            "kind": "product-jsonl",
                            "artifact": {"sha256": SHA_B},
                        }
                    ],
                }
            )
            + "\n",
            encoding="utf-8",
        )

    def run_stager(self, *arguments: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, str(STAGER), *arguments],
            check=False,
            capture_output=True,
            text=True,
        )

    def test_stages_only_an_artifact_bound_to_the_selected_product(self) -> None:
        result = self.run_stager(
            str(self.artifact),
            str(self.output),
            "--product-snapshot-control",
            str(self.control),
        )

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.output.read_bytes(), self.artifact.read_bytes())

    def test_identity_mismatch_does_not_publish_or_replace_data(self) -> None:
        self.output.parent.mkdir(parents=True)
        self.output.write_text("previous\n", encoding="utf-8")
        self.write_artifact(content_sha256="d" * 64)

        result = self.run_stager(
            str(self.artifact),
            str(self.output),
            "--product-snapshot-control",
            str(self.control),
        )

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("product identity mismatch", result.stderr)
        self.assertEqual(self.output.read_text(encoding="utf-8"), "previous\n")

    def test_local_catalog_mode_checks_the_database_content_hash(self) -> None:
        catalog = self.root / "catalog.json"
        catalog.write_text(
            json.dumps({"databaseSha256": SHA_A}) + "\n",
            encoding="utf-8",
        )

        result = self.run_stager(
            str(self.artifact),
            str(self.output),
            "--catalog",
            str(catalog),
        )

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.output.read_bytes(), self.artifact.read_bytes())

    def test_state_root_accepts_only_confined_relative_artifact_paths(self) -> None:
        state = self.root / "state"
        stored = state / "derived" / "wikidata-image-hints.json"
        stored.parent.mkdir(parents=True)
        stored.write_bytes(self.artifact.read_bytes())

        accepted = self.run_stager(
            "derived/wikidata-image-hints.json",
            str(self.output),
            "--state-root",
            str(state),
            "--product-snapshot-control",
            str(self.control),
        )
        self.assertEqual(
            accepted.returncode,
            0,
            accepted.stdout + accepted.stderr,
        )

        self.output.unlink()
        rejected = self.run_stager(
            str(stored),
            str(self.output),
            "--state-root",
            str(state),
            "--product-snapshot-control",
            str(self.control),
        )
        self.assertNotEqual(rejected.returncode, 0)
        self.assertIn("must be relative", rejected.stderr)
        self.assertFalse(self.output.exists())

        escaped = self.run_stager(
            "../wikidata-image-hints.json",
            str(self.output),
            "--state-root",
            str(state),
            "--product-snapshot-control",
            str(self.control),
        )
        self.assertNotEqual(escaped.returncode, 0)
        self.assertFalse(self.output.exists())

    def test_accepts_valid_records_in_any_producer_order(self) -> None:
        self.write_artifact(
            entities=[
                {
                    "entity_id": "work-000009",
                    "family": "work",
                    "images": [
                        {
                            "file": "Later claim.jpg",
                            "kind": "work_image",
                            "property": "P18",
                            "rank": "normal",
                            "source": "wikimedia_commons",
                            "wikidata_qid": "Q99",
                        },
                        {
                            "file": "Preferred poster.jpg",
                            "kind": "work_poster",
                            "property": "P3383",
                            "rank": "preferred",
                            "source": "wikimedia_commons",
                            "wikidata_qid": "Q99",
                        },
                    ],
                },
                {
                    "entity_id": "agent-000001",
                    "family": "agent",
                    "images": [
                        {
                            "file": "Portrait.jpg",
                            "kind": "agent_portrait",
                            "property": "P18",
                            "rank": "normal",
                            "source": "wikimedia_commons",
                            "wikidata_qid": "Q1",
                        }
                    ],
                },
            ]
        )

        result = self.run_stager(
            str(self.artifact),
            str(self.output),
            "--product-snapshot-control",
            str(self.control),
        )

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_rejects_malformed_entity_and_image_records(self) -> None:
        valid_image: dict[str, Any] = {
            "file": "Poster.jpg",
            "kind": "work_poster",
            "property": "P3383",
            "rank": "preferred",
            "source": "wikimedia_commons",
            "wikidata_qid": "Q123",
        }
        valid_entity: dict[str, Any] = {
            "entity_id": "work-000001",
            "family": "work",
            "images": [valid_image],
        }

        open_entity = copy.deepcopy(valid_entity)
        open_entity["unexpected"] = True
        wrong_kind = copy.deepcopy(valid_entity)
        wrong_kind["images"][0]["kind"] = "agent_portrait"
        malformed_qid = copy.deepcopy(valid_entity)
        malformed_qid["images"][0]["wikidata_qid"] = "Q0"
        open_image = copy.deepcopy(valid_entity)
        open_image["images"][0]["url"] = "https://example.invalid"
        empty_images = copy.deepcopy(valid_entity)
        empty_images["images"] = []
        too_many_images = copy.deepcopy(valid_entity)
        too_many_images["images"] = [
            {**valid_image, "file": f"Poster {index}.jpg"}
            for index in range(4)
        ]
        invalid_rank = copy.deepcopy(valid_entity)
        invalid_rank["images"][0]["rank"] = "deprecated"
        duplicate_filename = copy.deepcopy(valid_entity)
        duplicate_filename["images"] = [
            copy.deepcopy(valid_image),
            {**valid_image, "property": "P18", "kind": "work_image"},
        ]
        duplicate_entity = [
            copy.deepcopy(valid_entity),
            copy.deepcopy(valid_entity),
        ]

        cases: list[tuple[str, list[dict[str, Any]]]] = [
            ("open entity", [open_entity]),
            ("wrong family/property/kind", [wrong_kind]),
            ("malformed qid", [malformed_qid]),
            ("open image", [open_image]),
            ("empty images", [empty_images]),
            ("too many images", [too_many_images]),
            ("invalid rank", [invalid_rank]),
            ("duplicate filename", [duplicate_filename]),
            ("duplicate entity", duplicate_entity),
        ]
        for label, entities in cases:
            with self.subTest(label=label):
                self.output.unlink(missing_ok=True)
                self.write_artifact(entities=entities)
                result = self.run_stager(
                    str(self.artifact),
                    str(self.output),
                    "--product-snapshot-control",
                    str(self.control),
                )
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("image hints not staged", result.stderr)
                self.assertFalse(self.output.exists())

    def test_rejects_open_snapshot_identity_objects(self) -> None:
        document = json.loads(self.artifact.read_text(encoding="utf-8"))
        document["source_snapshot"]["unexpected"] = "not closed"
        self.artifact.write_text(json.dumps(document) + "\n", encoding="utf-8")

        result = self.run_stager(
            str(self.artifact),
            str(self.output),
            "--product-snapshot-control",
            str(self.control),
        )

        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.output.exists())


if __name__ == "__main__":
    unittest.main()
