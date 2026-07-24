from __future__ import annotations

import bz2
import hashlib
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
WORKER = ROOT / "hpc" / "wikidata" / "build_external_graph.py"


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def item_claim(qid: str) -> dict[str, object]:
    return {
        "rank": "normal",
        "mainsnak": {
            "snaktype": "value",
            "datavalue": {"value": {"entity-type": "item", "id": qid}},
        },
    }


def time_claim(value: str) -> dict[str, object]:
    return {
        "rank": "normal",
        "mainsnak": {
            "snaktype": "value",
            "datavalue": {"value": {"time": value}},
        },
    }


class WikidataHpcWorkerTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="arachne-hpc-test-")
        self.root = Path(self.temporary.name)
        self.artifacts = self.root / "artifacts"
        self.graphs = self.root / "graphs"
        self.controls = self.root / "controls"
        self.work = self.root / "work"
        for path in (self.artifacts, self.graphs, self.controls, self.work):
            path.mkdir()

        entities = [
            {
                "id": "Q1001",
                "labels": {"en": {"value": "Creative subclass"}},
                "claims": {"P279": [item_claim("Q1000")]},
            },
            {
                "id": "Q1",
                "labels": {"en": {"value": "Covered work"}},
                "claims": {
                    "P31": [item_claim("Q1001")],
                    "P170": [item_claim("Q10")],
                },
            },
            {
                "id": "Q2",
                "labels": {"en": {"value": "Uncovered work"}},
                "claims": {
                    "P31": [item_claim("Q1000")],
                    "P170": [item_claim("Q10"), item_claim("Q11")],
                },
            },
            {
                "id": "Q4",
                "labels": {"en": {"value": "Frontier work"}},
                "claims": {
                    "P31": [item_claim("Q1000")],
                    "P170": [item_claim("Q11")],
                },
            },
            {
                "id": "Q3",
                "labels": {"en": {"value": "Orphan work"}},
                "claims": {"P31": [item_claim("Q1000")]},
            },
            {
                "id": "Q10",
                "labels": {"en": {"value": "Example creator"}},
                "claims": {
                    "P27": [item_claim("Q183")],
                    "P106": [item_claim("Q1028181")],
                    "P569": [time_claim("+1900-01-01T00:00:00Z")],
                },
            },
            {
                "id": "Q11",
                "labels": {"en": {"value": "Frontier creator"}},
                "claims": {"P27": [item_claim("Q30")]},
            },
        ]
        dump_bytes = b"[\n" + b",\n".join(
            json.dumps(entity, separators=(",", ":")).encode("utf-8")
            for entity in entities
        ) + b"\n]\n"
        self.dump = self.artifacts / "bulk" / "tiny.json.bz2"
        self.dump.parent.mkdir()
        self.dump.write_bytes(bz2.compress(dump_bytes))

        self.source_control = self.controls / "source.json"
        self.source_control.write_text(
            json.dumps(
                {
                    "contract": "acquired_artifact_v1",
                    "format_version": 1,
                    "artifact_id": "wikidata-tiny-snapshot",
                    "request_id": "wikidata-tiny-request",
                    "door_id": "wikidata",
                    "operation": "bulk_snapshot",
                    "source_locator": "https://dumps.wikimedia.org/wikidatawiki/entities/latest-all.json.bz2",
                    "transport": {
                        "status": "delivered",
                        "attempts": 1,
                        "delivery_mode": "fetched",
                    },
                    "artifact": {
                        "storage_ref": "bulk/tiny.json.bz2",
                        "sha256": digest(self.dump),
                        "byte_length": self.dump.stat().st_size,
                    },
                    "response_metadata": {
                        "status_code": 200,
                        "effective_url": "https://dumps.wikimedia.org/wikidatawiki/entities/latest-all.json.bz2",
                        "headers": [
                            {
                                "name": "Content-Type",
                                "value": "application/octet-stream",
                            }
                        ],
                        "redirect_chain": [],
                        "started_at": "2026-07-20T03:00:00Z",
                        "completed_at": "2026-07-20T03:01:00Z",
                    },
                    "acquired_at": "2026-07-20T03:01:00Z",
                }
            ),
            encoding="utf-8",
        )

        export_bytes = "\n".join(
            json.dumps(value, separators=(",", ":"))
            for value in (
                {"table": "works", "row": {"entity_id": "product-work-1"}},
                {
                    "table": "external_ids",
                    "row": {
                        "entity_id": "product-work-1",
                        "scheme": "wikidata",
                        "value": "Q1",
                    },
                },
            )
        ) + "\n"
        self.product_export = self.graphs / "product" / "exports" / "tiny.jsonl"
        self.product_export.parent.mkdir(parents=True)
        self.product_export.write_text(export_bytes, encoding="utf-8")
        self.product_database = self.graphs / "product" / "tiny.sqlite3"
        self.product_database.write_bytes(b"test product database evidence")
        self.validation_report = (
            self.graphs / "product" / "reports" / "tiny.json"
        )
        self.validation_report.parent.mkdir(parents=True)
        self.validation_report.write_text(
            '{"passed":true}\n', encoding="utf-8"
        )
        self.product_control = self.controls / "product.json"
        self.product_control.write_text(
            json.dumps(
                {
                    "contract": "product_graph_snapshot_v1",
                    "format_version": 1,
                    "snapshot_id": "product-tiny-snapshot",
                    "run_id": "product-tiny-run",
                    "graph_version": "test-1",
                    "content_sha256": digest(self.product_database),
                    "database": {
                        "storage_ref": "product/tiny.sqlite3",
                        "sha256": digest(self.product_database),
                        "byte_length": self.product_database.stat().st_size,
                    },
                    "exports": [
                        {
                            "kind": "product-jsonl",
                            "artifact": {
                                "storage_ref": "product/exports/tiny.jsonl",
                                "sha256": digest(self.product_export),
                                "byte_length": self.product_export.stat().st_size,
                            },
                        }
                    ],
                    "cocoon_ids": ["env_test"],
                    "activated_at": "2026-07-20T02:59:00Z",
                    "structural_validation": {
                        "passed": True,
                        "report": {
                            "storage_ref": "product/reports/tiny.json",
                            "sha256": digest(self.validation_report),
                            "byte_length": self.validation_report.stat().st_size,
                        },
                    },
                    "extensions": {"org.ninjaro.penelope": {}},
                }
            ),
            encoding="utf-8",
        )
        self.config = self.root / "worker-config.json"
        self.config.write_text(
            json.dumps(
                {
                    "format_version": 1,
                    "languages": ["en"],
                    "work_root_qids": ["Q1000"],
                    "agent_properties": ["P170"],
                }
            ),
            encoding="utf-8",
        )
        self.candidate_policy = self.root / "candidate-policy.json"
        self.candidate_policy.write_text(
            json.dumps(
                {
                    "format_version": 1,
                    "candidate_rebuild": {
                        "sources": {
                            "wikidata": {
                                "candidate_pool_size": 4,
                                "gray_bonus_basis_points": 2000,
                            }
                        }
                    },
                }
            ),
            encoding="utf-8",
        )
        self.output = self.root / "results" / "external-graph.json"
        self.report = self.root / "results" / "run-report.json"

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def run_worker(self, *extra: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [
                sys.executable,
                str(WORKER),
                "--source-control",
                str(self.source_control),
                "--artifact-store",
                str(self.artifacts),
                "--product-snapshot-control",
                str(self.product_control),
                "--graph-store",
                str(self.graphs),
                "--config",
                str(self.config),
                "--candidate-policy-config",
                str(self.candidate_policy),
                "--output",
                str(self.output),
                "--work-directory",
                str(self.work),
                "--report",
                str(self.report),
                *extra,
            ],
            check=False,
            capture_output=True,
            text=True,
            timeout=30,
        )

    def test_streams_dump_and_binds_source_and_product_coverage(self) -> None:
        result = self.run_worker()

        self.assertEqual(result.returncode, 0, result.stderr)
        graph = json.loads(self.output.read_text(encoding="utf-8"))
        self.assertEqual(graph["artifact_type"], "external_candidate_source_graph_v1")
        self.assertEqual(graph["source_snapshot"]["sha256"], digest(self.dump))
        works = {item["id"]: item for item in graph["works"]}
        self.assertTrue(works["Q1"]["covered"])
        self.assertFalse(works["Q2"]["covered"])
        self.assertFalse(works["Q4"]["covered"])
        self.assertNotIn("Q3", works)
        self.assertEqual(
            {item["id"] for item in graph["agents"]}, {"Q10", "Q11"}
        )
        creator = next(item for item in graph["agents"] if item["id"] == "Q10")
        self.assertEqual(creator["label"], "Example creator")
        self.assertEqual(creator["profile"]["countries"], ["Q183"])
        self.assertEqual(creator["profile"]["activity_year"], 1925)
        self.assertEqual(len(graph["edges"]), 4)
        report = json.loads(self.report.read_text(encoding="utf-8"))
        self.assertEqual(report["transport"]["status"], "verified")
        self.assertEqual(report["algorithm"]["status"], "succeeded")
        self.assertEqual(report["algorithm"]["covered_product_qids"], 1)
        self.assertEqual(
            report["algorithm"]["statistics"]["ranked_pool_agents"], 2
        )
        self.assertEqual(list(self.work.iterdir()), [])

    def test_tampered_source_fails_before_algorithm_or_output(self) -> None:
        control = json.loads(self.source_control.read_text(encoding="utf-8"))
        control["artifact"]["sha256"] = "0" * 64
        self.source_control.write_text(json.dumps(control), encoding="utf-8")

        result = self.run_worker()

        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.output.exists())
        report = json.loads(self.report.read_text(encoding="utf-8"))
        self.assertEqual(report["transport"]["status"], "failed")
        self.assertEqual(report["algorithm"]["status"], "not_started")

    def test_incomplete_or_stale_source_receipt_fails_closed(self) -> None:
        control = json.loads(self.source_control.read_text(encoding="utf-8"))
        control["transport"]["delivery_mode"] = "cache_validated"
        control["semantic_confidence"] = 1.0
        self.source_control.write_text(json.dumps(control), encoding="utf-8")

        result = self.run_worker()

        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.output.exists())
        report = json.loads(self.report.read_text(encoding="utf-8"))
        self.assertEqual(report["transport"]["status"], "failed")
        self.assertEqual(report["algorithm"]["status"], "not_started")

    def test_unknown_worker_configuration_version_fails_closed(self) -> None:
        configuration = json.loads(self.config.read_text(encoding="utf-8"))
        configuration["format_version"] = 2
        self.config.write_text(json.dumps(configuration), encoding="utf-8")

        result = self.run_worker()

        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.output.exists())
        report = json.loads(self.report.read_text(encoding="utf-8"))
        self.assertEqual(report["transport"]["status"], "verified")
        self.assertEqual(report["algorithm"]["status"], "failed")

    def test_unbounded_decompress_threads_fail_before_transport(self) -> None:
        result = self.run_worker("--decompress-threads", "0")

        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.output.exists())
        report = json.loads(self.report.read_text(encoding="utf-8"))
        self.assertEqual(report["transport"]["status"], "failed")
        self.assertEqual(report["algorithm"]["status"], "not_started")

    def test_unbounded_candidate_policy_fails_closed(self) -> None:
        configuration = json.loads(
            self.candidate_policy.read_text(encoding="utf-8")
        )
        configuration["candidate_rebuild"]["sources"]["wikidata"][
            "candidate_pool_size"
        ] = 100_001
        self.candidate_policy.write_text(
            json.dumps(configuration), encoding="utf-8"
        )

        result = self.run_worker()

        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.output.exists())
        report = json.loads(self.report.read_text(encoding="utf-8"))
        self.assertEqual(report["transport"]["status"], "verified")
        self.assertEqual(report["algorithm"]["status"], "failed")

    def test_compression_is_detected_from_verified_bytes_not_filename(self) -> None:
        renamed = self.dump.with_suffix(".payload")
        self.dump.rename(renamed)
        control = json.loads(self.source_control.read_text(encoding="utf-8"))
        control["artifact"]["storage_ref"] = "bulk/tiny.json.payload"
        self.source_control.write_text(json.dumps(control), encoding="utf-8")

        result = self.run_worker()

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(self.output.is_file())


if __name__ == "__main__":
    unittest.main()
