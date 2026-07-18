from __future__ import annotations

import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
BINARY_ENV = os.environ.get("ARACHNE_TEST_BINARY")


@unittest.skipUnless(
    BINARY_ENV,
    "ARACHNE_TEST_BINARY is not set; build arachne_app and point the variable at it",
)
class OperationsCliTests(unittest.TestCase):
    def setUp(self) -> None:
        assert BINARY_ENV is not None
        self.binary = Path(BINARY_ENV).resolve()
        if not self.binary.is_file() or not os.access(self.binary, os.X_OK):
            self.skipTest(
                f"ARACHNE_TEST_BINARY is missing or not executable: {self.binary}"
            )
        self.temporary = tempfile.TemporaryDirectory(prefix="arachne-cli-test-")
        self.root = Path(self.temporary.name)
        self.queue = self.root / "queue"
        self.legacy = self.root / "legacy-inbox"
        self.queue.mkdir()
        self.legacy.mkdir()
        self.config_path = self.root / "arachne.json"
        self.write_config()

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def write_config(self, threshold: int = 15) -> None:
        document = {
            "format_version": 1,
            "project_timezone": "Europe/Berlin",
            "paths": {
                "legacy_inbox": str(self.legacy),
                "queue": str(self.queue),
                "remainders": str(self.root / "remainders"),
                "ledger": str(self.root / "state" / "ledger.sqlite3"),
                "graph_store": str(self.root / "graphs"),
                "artifact_store": str(self.root / "artifacts"),
                "lock_root": str(self.root / "locks"),
                "viewer_templates": str(ROOT / "viewer"),
                "site_output": str(self.root / "site"),
                "legacy_inbox_baseline": str(
                    self.root / "state" / "legacy-baseline.json"
                ),
            },
            "product_integration": {
                "local_hour": 3,
                "queued_batch_threshold": threshold,
                "lock_stale_seconds": 21600,
            },
            "candidate_rebuild": {
                "sources": {
                    "wikidata": {
                        "refresh_days": 60,
                        "candidate_pool_size": 4,
                        "final_target": 3,
                        "group_count": 2,
                        "gray_bonus_basis_points": 2000,
                        "quality_weight": 0.65,
                    }
                },
                "lock_stale_seconds": 21600,
            },
            "security": {"submission_max_bytes": 1024 * 1024},
            "publication": {
                "require_reviewed_change": True,
                "pages_artifact_name": "test-site",
            },
        }
        self.config_path.write_text(
            json.dumps(document, indent=2) + "\n", encoding="utf-8"
        )

    def run_cli(self, *arguments: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [str(self.binary), *arguments],
            check=False,
            capture_output=True,
            text=True,
            timeout=20,
        )

    def run_cli_with_home(
        self, home: Path, *arguments: str
    ) -> subprocess.CompletedProcess[str]:
        environment = os.environ.copy()
        environment["HOME"] = str(home)
        return subprocess.run(
            [str(self.binary), *arguments],
            check=False,
            capture_output=True,
            text=True,
            timeout=20,
            env=environment,
        )

    def document(self, result: subprocess.CompletedProcess[str]) -> dict[str, Any]:
        self.assertTrue(result.stdout.strip(), result.stderr)
        value = json.loads(result.stdout)
        self.assertIsInstance(value, dict)
        return value

    def valid_batch(self, name: str = "batch-1") -> Path:
        path = self.root / f"{name}.json"
        path.write_text(
            json.dumps(
                {
                    "format_version": 1,
                    "batch_id": name,
                    "batch_type": "mining",
                    "scope": {"label": "CLI integration test"},
                }
            ),
            encoding="utf-8",
        )
        return path

    def intake(self, batch: Path) -> subprocess.CompletedProcess[str]:
        return self.run_cli(
            "intake",
            "--config",
            str(self.config_path),
            "--payload",
            str(batch),
            "--submission-ref",
            f"local:{batch.stem}",
            "--title",
            "CLI test batch",
        )

    def test_capabilities_advertise_complete_operations_surface(self) -> None:
        result = self.run_cli("--capabilities-json")
        self.assertEqual(result.returncode, 0, result.stderr)
        document = self.document(result)
        self.assertEqual(document["format_version"], 1)
        self.assertEqual(
            set(document["commands"]),
            {
                "contract-validate",
                "fetch",
                "intake",
                "cocoon-transition",
                "inbox-baseline",
                "inbox-verify",
                "product-integrate",
                "candidate-plan",
                "candidate-rebuild",
                "viewer-build",
            },
        )

    def test_contract_validate_reports_valid_and_invalid_documents(self) -> None:
        batch = self.valid_batch()
        valid = self.run_cli(
            "contract",
            "validate",
            "--config",
            str(self.config_path),
            "--contract",
            "mining_batch_v1",
            "--input",
            str(batch),
        )
        self.assertEqual(valid.returncode, 0, valid.stderr)
        self.assertTrue(self.document(valid)["valid"])

        batch.write_text("[]\n", encoding="utf-8")
        invalid = self.run_cli(
            "contract",
            "validate",
            "--config",
            str(self.config_path),
            "--contract",
            "mining_batch_v1",
            "--input",
            str(batch),
        )
        self.assertNotEqual(invalid.returncode, 0)
        invalid_document = self.document(invalid)
        self.assertFalse(invalid_document["valid"])
        self.assertTrue(invalid_document["diagnostics"])
        self.assertTrue(invalid.stderr.strip())

    def test_intake_returns_only_immediate_ok_and_preserves_source(self) -> None:
        batch = self.valid_batch()
        before = batch.read_bytes()
        result = self.intake(batch)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.document(result), {"status": "ok"})
        self.assertEqual(batch.read_bytes(), before)
        queued = list(self.queue.iterdir())
        self.assertEqual(len(queued), 1)
        self.assertEqual(queued[0].read_bytes(), before)

        failed = self.intake(self.root / "missing.json")
        self.assertNotEqual(failed.returncode, 0)
        self.assertEqual(self.document(failed), {"status": "fail"})
        self.assertTrue(failed.stderr.strip())

    def test_queue_must_be_disjoint_from_artifact_custody(self) -> None:
        document = json.loads(self.config_path.read_text(encoding="utf-8"))
        artifact_store = self.root / "overlapping-artifacts"
        nested_queue = artifact_store / "intake"
        nested_queue.mkdir(parents=True)
        document["paths"]["artifact_store"] = str(artifact_store)
        document["paths"]["queue"] = str(nested_queue)
        self.config_path.write_text(json.dumps(document), encoding="utf-8")

        source = self.root / "opaque.bin"
        source.write_bytes(b"opaque bytes")
        result = self.run_cli(
            "intake",
            "--config",
            str(self.config_path),
            "--payload",
            str(source),
            "--submission-ref",
            "local:overlap",
            "--title",
            "Unsafe overlapping storage",
        )

        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.document(result), {"status": "fail"})
        self.assertEqual(list(nested_queue.iterdir()), [])

    def test_intake_accepts_opaque_bytes_without_manifest_inference(self) -> None:
        source = self.root / "legacy-payload.zip"
        content = b"not parsed at receipt\x00\xff"
        source.write_bytes(content)

        result = self.run_cli(
            "intake",
            "--config",
            str(self.config_path),
            "--payload",
            str(source),
            "--submission-ref",
            "local:opaque",
            "--title",
            "Opaque legacy payload",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.document(result), {"status": "ok"})
        queued = list(self.queue.iterdir())
        self.assertEqual(len(queued), 1)
        self.assertEqual(queued[0].read_bytes(), content)

    def test_missing_optional_legacy_checkout_does_not_block_runtime(self) -> None:
        document = json.loads(self.config_path.read_text(encoding="utf-8"))
        document["paths"]["legacy_inbox"] = str(self.root / "not-installed")
        self.config_path.write_text(json.dumps(document), encoding="utf-8")
        source = self.root / "opaque.bin"
        source.write_bytes(b"runtime remains independent")

        result = self.run_cli(
            "intake",
            "--config",
            str(self.config_path),
            "--payload",
            str(source),
            "--submission-ref",
            "local:no-legacy",
            "--title",
            "No legacy checkout",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.document(result), {"status": "ok"})

    def test_conventional_legacy_inbox_is_protected_when_config_is_null(self) -> None:
        fake_home = self.root / "home"
        conventional = fake_home / "Projects" / "new" / "art-lineages" / "inbox"
        conventional.mkdir(parents=True)
        document = json.loads(self.config_path.read_text(encoding="utf-8"))
        document["paths"]["legacy_inbox"] = None
        document["paths"]["queue"] = str(conventional)
        self.config_path.write_text(json.dumps(document), encoding="utf-8")
        source = self.root / "opaque.bin"
        source.write_bytes(b"must not enter legacy")

        result = self.run_cli_with_home(
            fake_home,
            "intake",
            "--config",
            str(self.config_path),
            "--payload",
            str(source),
            "--submission-ref",
            "local:protected-legacy",
            "--title",
            "Protected legacy",
        )

        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.document(result), {"status": "fail"})
        self.assertEqual(list(conventional.iterdir()), [])

    def test_legacy_baseline_and_verify_never_write_legacy_inbox(self) -> None:
        legacy_file = self.legacy / "existing.json"
        legacy_file.write_bytes(b"immutable legacy bytes")
        before = legacy_file.read_bytes()

        baseline = self.run_cli(
            "inbox", "baseline", "--config", str(self.config_path)
        )
        self.assertEqual(baseline.returncode, 0, baseline.stderr)
        self.assertTrue(self.document(baseline)["ok"])
        verified = self.run_cli(
            "inbox", "verify", "--config", str(self.config_path)
        )
        self.assertEqual(verified.returncode, 0, verified.stderr)
        self.assertTrue(self.document(verified)["ok"])
        self.assertEqual(legacy_file.read_bytes(), before)

        legacy_file.write_bytes(b"changed")
        changed = self.run_cli(
            "inbox", "verify", "--config", str(self.config_path)
        )
        self.assertNotEqual(changed.returncode, 0)
        self.assertFalse(self.document(changed)["ok"])
        self.assertTrue(changed.stderr.strip())

    def test_fetch_persists_failure_contract_without_network_access(self) -> None:
        request = self.root / "unsafe-fetch.json"
        request.write_text(
            json.dumps(
                {
                    "contract": "fetch_request_v1",
                    "format_version": 1,
                    "request_id": "unsafe-fetch",
                    "locator": "https://example.com/data",
                    "method": "GET",
                    "redirect_policy": {
                        "follow": False,
                        "maximum_redirects": 0,
                        "allow_https_to_http": False,
                        "allowed_hosts": ["example.com"],
                    },
                    "output_ref": "../escape.bin",
                }
            ),
            encoding="utf-8",
        )
        control = self.root / "controls" / "unsafe-acquired.json"
        result = self.run_cli(
            "fetch",
            "--config",
            str(self.config_path),
            "--request",
            str(request),
            "--output-control",
            str(control),
        )
        self.assertNotEqual(result.returncode, 0)
        stdout_document = self.document(result)
        self.assertTrue(control.is_file())
        self.assertEqual(json.loads(control.read_text(encoding="utf-8")), stdout_document)
        self.assertEqual(stdout_document["transport"]["status"], "failed")
        self.assertEqual(
            stdout_document["transport"]["error_code"], "unsafe_artifact_ref"
        )

        request.write_text("{}\n", encoding="utf-8")
        invalid_control = self.root / "controls" / "invalid-acquired.json"
        invalid = self.run_cli(
            "fetch",
            "--config",
            str(self.config_path),
            "--request",
            str(request),
            "--output-control",
            str(invalid_control),
        )
        self.assertNotEqual(invalid.returncode, 0)
        invalid_document = self.document(invalid)
        self.assertEqual(invalid_document["transport"]["status"], "failed")
        self.assertEqual(
            invalid_document["transport"]["error_code"], "invalid_request"
        )
        self.assertEqual(
            json.loads(invalid_control.read_text(encoding="utf-8")),
            invalid_document,
        )

    def test_product_threshold_is_noop_and_force_processes_queue(self) -> None:
        batch = self.valid_batch("force-batch")
        intake = self.intake(batch)
        self.assertEqual(intake.returncode, 0, intake.stderr)
        queued = list(self.queue.iterdir())
        self.assertEqual(len(queued), 1)

        waiting = self.run_cli(
            "product",
            "integrate",
            "--config",
            str(self.config_path),
            "--logical-date",
            "2026-07-18",
            "--run-id",
            "threshold-noop",
        )
        self.assertEqual(waiting.returncode, 0, waiting.stderr)
        waiting_document = self.document(waiting)
        self.assertFalse(waiting_document["processed"])
        self.assertEqual(waiting_document["reason"], "queued_batch_threshold_not_met")
        self.assertTrue(queued[0].is_file())

        forced = self.run_cli(
            "product",
            "integrate",
            "--config",
            str(self.config_path),
            "--logical-date",
            "2026-07-19",
            "--run-id",
            "forced-run",
            "--force",
        )
        self.assertEqual(forced.returncode, 0, forced.stderr)
        forced_document = self.document(forced)
        self.assertTrue(forced_document["processed"])
        self.assertTrue(forced_document["forced"])
        self.assertEqual(forced_document["aggregate"]["successful"], 1)
        self.assertFalse(queued[0].exists())
        self.assertEqual(list(self.legacy.iterdir()), [])

    def test_candidate_plan_rebuild_and_viewer_build_end_to_end(self) -> None:
        batch = self.valid_batch("pipeline-batch")
        intake = self.intake(batch)
        self.assertEqual(intake.returncode, 0, intake.stderr)
        product = self.run_cli(
            "product",
            "integrate",
            "--config",
            str(self.config_path),
            "--logical-date",
            "2026-07-20",
            "--run-id",
            "pipeline-product",
            "--force",
        )
        self.assertEqual(product.returncode, 0, product.stderr)
        product_snapshot = self.document(product)["snapshot"]

        external_graph = self.root / "external-graph.json"
        external_graph.write_text(
            json.dumps(
                {
                    "artifact_type": "external_candidate_source_graph_v1",
                    "format_version": 1,
                    "source_snapshot": {
                        "snapshot_id": "source-1",
                        "storage_ref": "sources/source-1.json",
                        "sha256": "a" * 64,
                    },
                    "works": [
                        {"id": "Q1", "label": "One", "covered": True},
                        {"id": "Q2", "label": "Two", "covered": False},
                        {"id": "Q3", "label": "Three", "covered": False},
                        {"id": "Q4", "label": "Four", "covered": False},
                        {"id": "Q5", "label": "Five", "covered": False},
                    ],
                    "agents": [
                        {"id": "Q101", "label": "Alpha", "profile": {"year": 1950}},
                        {"id": "Q102", "label": "Beta", "profile": {"year": 1960}},
                        {"id": "Q103", "label": "Gamma", "profile": {"year": 1970}},
                        {"id": "Q104", "label": "Delta", "profile": {"year": 1980}},
                    ],
                    "edges": [
                        {"work_id": "Q1", "agent_id": "Q101"},
                        {"work_id": "Q2", "agent_id": "Q101"},
                        {"work_id": "Q3", "agent_id": "Q101"},
                        {"work_id": "Q1", "agent_id": "Q102"},
                        {"work_id": "Q3", "agent_id": "Q102"},
                        {"work_id": "Q4", "agent_id": "Q102"},
                        {"work_id": "Q2", "agent_id": "Q103"},
                        {"work_id": "Q4", "agent_id": "Q103"},
                        {"work_id": "Q1", "agent_id": "Q104"},
                        {"work_id": "Q5", "agent_id": "Q104"},
                    ],
                }
            ),
            encoding="utf-8",
        )
        artifact = self.root / "artifacts" / "plans" / "candidate.json"
        control = self.root / "controls" / "candidate-plan.json"
        planned = self.run_cli(
            "candidate",
            "plan",
            "--config",
            str(self.config_path),
            "--external-graph",
            str(external_graph),
            "--product-snapshot",
            product_snapshot["metadata_path"],
            "--output-artifact",
            str(artifact),
            "--output-control",
            str(control),
        )
        self.assertEqual(planned.returncode, 0, planned.stderr)
        plan_document = self.document(planned)
        self.assertEqual(plan_document["contract"], "research_candidate_graph_plan_v1")
        self.assertTrue(artifact.is_file())
        self.assertTrue(control.is_file())

        rebuilt = self.run_cli(
            "candidate",
            "rebuild",
            "--config",
            str(self.config_path),
            "--plan-control",
            str(control),
            "--run-id",
            "pipeline-candidate",
        )
        self.assertEqual(rebuilt.returncode, 0, rebuilt.stderr)
        candidate_snapshot = self.document(rebuilt)["snapshot"]

        viewer = self.run_cli(
            "viewer",
            "build",
            "--config",
            str(self.config_path),
            "--product-export",
            product_snapshot["export_path"],
            "--product-snapshot-id",
            product_snapshot["snapshot_id"],
            "--candidate-export",
            candidate_snapshot["export_path"],
            "--candidate-snapshot-id",
            candidate_snapshot["snapshot_id"],
        )
        self.assertEqual(viewer.returncode, 0, viewer.stderr)
        viewer_document = self.document(viewer)
        self.assertEqual(viewer_document["command"], "viewer-build")
        self.assertTrue((self.root / "site" / "active.json").is_file())
        bundle_ref = viewer_document["site_bundle"]["bundle"]["storage_ref"]
        self.assertTrue((self.root / "site" / bundle_ref / "index.html").is_file())


if __name__ == "__main__":
    unittest.main()
