from __future__ import annotations

import hashlib
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
            "transport": {
                "format_version": 1,
                "defaults": {
                    "timeouts": {
                        "total_ms": 5000,
                        "connect_ms": 1000,
                        "pool_ms": 1000,
                    },
                    "retry": {
                        "maximum_attempts": 2,
                        "initial_delay_ms": 1,
                        "maximum_delay_ms": 5,
                        "total_delay_budget_ms": 10,
                        "respect_retry_after": True,
                    },
                    "admission": {
                        "maximum_concurrency": 2,
                        "minimum_interval_ms": 0,
                    },
                    "cache": {"ttl_seconds": 60},
                    "maximum_artifact_bytes": 1024 * 1024,
                    "redirect_policy": {
                        "follow": False,
                        "maximum_redirects": 0,
                        "allow_https_to_http": False,
                    },
                },
                "doors": [
                    {
                        "door_id": "test-door",
                        "endpoints": [
                            {
                                "endpoint_id": "public",
                                "protocol": "rest",
                                "base_url": "https://example.com/",
                                "allowed_methods": ["GET"],
                                "authentication": {"mode": "none"},
                                "bulk_capable": False,
                                "resumable_download": False,
                                "write_enabled": False,
                            }
                        ],
                    },
                    {
                        "door_id": "wikidata",
                        "endpoints": [
                            {
                                "endpoint_id": "entity-api",
                                "protocol": "rest",
                                "base_url": "https://www.wikidata.org/w/api.php",
                                "allowed_methods": ["POST"],
                                "authentication": {"mode": "none"},
                                "bulk_capable": False,
                                "resumable_download": False,
                                "write_enabled": False,
                            },
                            {
                                "endpoint_id": "official-dumps",
                                "protocol": "http_file",
                                "base_url": "https://dumps.wikimedia.org/wikidatawiki/entities/",
                                "allowed_methods": ["GET"],
                                "authentication": {"mode": "none"},
                                "bulk_capable": True,
                                "resumable_download": True,
                                "write_enabled": False,
                            },
                        ],
                    },
                ],
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
                    "format": "arachne_batch_v2",
                    "batch_id": name,
                    "create": {},
                    "update": {},
                    "merge": {},
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

    def approve(self, batch: Path) -> subprocess.CompletedProcess[str]:
        submission_ref = f"local:{batch.stem}"
        payload_sha256 = hashlib.sha256(batch.read_bytes()).hexdigest()
        envelope_id = "env_" + hashlib.sha256(
            (
                "batch_envelope_v1\n"
                + payload_sha256
                + "\n"
                + submission_ref
            ).encode("utf-8")
        ).hexdigest()[:32]
        return self.run_cli(
            "cocoon",
            "transition",
            "--config",
            str(self.config_path),
            "--envelope-id",
            envelope_id,
            "--to",
            "accepted",
            "--actor-ref",
            "maintainer:test",
            "--reason",
            "explicit test approval",
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
                "fetch-plan-translate",
                "intake",
                "cocoon-transition",
                "inbox-baseline",
                "inbox-verify",
                "product-check-inbox",
                "product-apply-inbox",
                "product-rebuild-merge-hints",
                "product-export-merge-hints",
                "product-research",
                "product-entity",
                "product-taste-index",
                "candidate-plan",
                "candidate-rebuild",
                "viewer-build",
            },
        )

    def product_snapshot(self) -> tuple[Path, Path, Path]:
        graph_store = self.root / "graphs"
        export = graph_store / "product" / "exports" / "product.jsonl"
        export.parent.mkdir(parents=True, exist_ok=True)
        records = [
            {
                "table": "entities",
                "row": {"id": "work-000001", "entity_type": "work"},
            },
            {
                "table": "entities",
                "row": {"id": "agent-000001", "entity_type": "person"},
            },
            {
                "table": "entities",
                "row": {"id": "concept-000001", "entity_type": "concept"},
            },
            {
                "table": "works",
                "row": {
                    "entity_id": "work-000001",
                    "medium": "film",
                    "year_start": 1950,
                    "year_end": None,
                    "date_start_text": None,
                    "production_info_json": None,
                },
            },
            {
                "table": "agents",
                "row": {
                    "entity_id": "agent-000001",
                    "agent_type": "person",
                },
            },
            {
                "table": "concepts",
                "row": {
                    "entity_id": "concept-000001",
                    "concept_type": "genre",
                    "slug": "test-genre",
                },
            },
            {
                "table": "names",
                "row": {
                    "id": 1,
                    "entity_id": "work-000001",
                    "name_type": "original",
                    "value": "Test Work",
                    "is_preferred": 1,
                },
            },
            {
                "table": "names",
                "row": {
                    "id": 2,
                    "entity_id": "agent-000001",
                    "name_type": "original",
                    "value": "Test Agent",
                    "is_preferred": 1,
                },
            },
            {
                "table": "names",
                "row": {
                    "id": 3,
                    "entity_id": "concept-000001",
                    "name_type": "original",
                    "value": "Test genre",
                    "is_preferred": 1,
                },
            },
            {
                "table": "credits",
                "row": {
                    "id": 1,
                    "work_id": "work-000001",
                    "agent_id": "agent-000001",
                    "role": "director",
                    "importance": "primary",
                    "credit_order": 1,
                    "credited_as": None,
                },
            },
            {
                "table": "work_concepts",
                "row": {
                    "id": 1,
                    "work_id": "work-000001",
                    "concept_id": "concept-000001",
                    "relation_type": "exemplifies",
                    "centrality": 90,
                    "historical_role": "canonical",
                    "confidence": 0.9,
                },
            },
            {
                "table": "ingest_issues",
                "row": {
                    "batch_id": "batch-test",
                    "code": "unknown_reference",
                    "json_path": "/create/credits/0",
                    "message": "Unknown agent.",
                    "value_json": '{"agent_id":"agent-999999"}',
                    "status": "open",
                },
            },
        ]
        export.write_text(
            "".join(json.dumps(record, separators=(",", ":")) + "\n" for record in records),
            encoding="utf-8",
        )
        database = graph_store / "product" / "product.sqlite"
        database.write_bytes(b"immutable product snapshot")
        report = graph_store / "product" / "reports" / "validation.json"
        report.parent.mkdir(parents=True, exist_ok=True)
        report.write_text('{"passed":true}\n', encoding="utf-8")
        digest = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
        control = self.root / "product-control.json"
        control.write_text(
            json.dumps(
                {
                    "contract": "product_graph_snapshot_v1",
                    "format_version": 1,
                    "snapshot_id": "product-cli-test",
                    "run_id": "run-cli-test",
                    "graph_version": "test-1",
                    "content_sha256": digest(database),
                    "database": {
                        "storage_ref": "product/product.sqlite",
                        "sha256": digest(database),
                        "byte_length": database.stat().st_size,
                    },
                    "exports": [
                        {
                            "kind": "product-jsonl",
                            "artifact": {
                                "storage_ref": "product/exports/product.jsonl",
                                "sha256": digest(export),
                                "byte_length": export.stat().st_size,
                            },
                        }
                    ],
                    "activated_at": "2026-08-09T12:00:00Z",
                    "structural_validation": {
                        "passed": True,
                        "report": {
                            "storage_ref": "product/reports/validation.json",
                            "sha256": digest(report),
                            "byte_length": report.stat().st_size,
                        },
                    },
                }
            )
            + "\n",
            encoding="utf-8",
        )
        decisions = self.root / "merge-hint-decisions.json"
        decisions.write_text(
            '{"artifact_type":"arachne_merge_hint_decisions_v1",'
            '"format_version":1,"ignored_pairs":[]}\n',
            encoding="utf-8",
        )
        review = self.root / "merge-hints-review.json"
        review.write_text(
            json.dumps(
                {
                    "artifactType": "arachne_merge_hint_review_v1",
                    "formatVersion": 1,
                    "source": {
                        "productSha256": digest(database),
                        "decisionsSha256": digest(decisions),
                        "ignoredPairCount": 0,
                    },
                    "items": [],
                }
            )
            + "\n",
            encoding="utf-8",
        )
        return control, review, decisions

    def test_human_help_is_available_at_root_product_and_subcommand_levels(self) -> None:
        for arguments, expected in (
            (("--help",), "Arachne\n"),
            (("-h",), "Usage:"),
            (("help",), "Commands:"),
            (("help", "product"), "Arachne product"),
            (("product", "--help"), "taste-index"),
            (("help", "product", "research"), "Required options:"),
            (("product", "research", "--help"), "--product-snapshot"),
            (("product", "entity", "--help"), "--id ID"),
            (("help", "fetch"), "Pheidippides"),
            (("fetch", "--help"), "fetch plan"),
            (("help", "fetch", "plan"), "--output-directory"),
            (("fetch", "plan", "--help"), "fetch_plan_v1"),
            (("help", "candidate"), "candidate snapshot"),
            (("candidate", "--help"), "candidate rebuild"),
            (("help", "candidate", "plan"), "--external-graph"),
            (("candidate", "plan", "--help"), "--output-artifact"),
            (("help", "candidate", "rebuild"), "--run-id"),
            (("candidate", "rebuild", "--help"), "plan-control"),
        ):
            with self.subTest(arguments=arguments):
                result = self.run_cli(*arguments)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn(expected, result.stdout)

        unknown = self.run_cli("unknown-command")
        self.assertNotEqual(unknown.returncode, 0)
        self.assertIn("arachne help", unknown.stderr)

    def test_product_research_entity_and_taste_index_are_snapshot_bound_json(self) -> None:
        control, review, decisions = self.product_snapshot()
        research_path = self.root / "research.json"
        research = self.run_cli(
            "product",
            "research",
            "--config",
            str(self.config_path),
            "--product-snapshot",
            str(control),
            "--merge-hints",
            str(review),
            "--merge-hint-decisions",
            str(decisions),
            "--output",
            str(research_path),
        )
        self.assertEqual(research.returncode, 0, research.stderr)
        self.assertFalse(research.stdout)
        report = json.loads(research_path.read_text(encoding="utf-8"))
        self.assertEqual(report["artifact_type"], "product_research_report_v1")
        self.assertEqual(report["product_snapshot"]["snapshot_id"], "product-cli-test")
        self.assertEqual(report["summary"]["ingestIssues"], 1)
        self.assertEqual(report["summary"]["qualityGaps"], 1)

        entity = self.run_cli(
            "product",
            "entity",
            "--config",
            str(self.config_path),
            "--product-snapshot",
            str(control),
            "--id",
            "agent-000001",
            "--compact",
        )
        self.assertEqual(entity.returncode, 0, entity.stderr)
        entity_document = self.document(entity)
        self.assertEqual(entity_document["family"], "agent")
        self.assertEqual(entity_document["credits"][0]["work_label"], "Test Work")
        self.assertNotIn("\n  ", entity.stdout)

        taste = self.run_cli(
            "product",
            "taste-index",
            "--config",
            str(self.config_path),
            "--product-snapshot",
            str(control),
        )
        self.assertEqual(taste.returncode, 0, taste.stderr)
        taste_document = self.document(taste)
        self.assertEqual(taste_document["artifact_type"], "taste_index_v1")
        self.assertIn("work-000001", taste_document["entities"])
        self.assertIn("agent-000001", taste_document["entities"])

        database = self.root / "graphs" / "product" / "product.sqlite"
        product_export = (
            self.root / "graphs" / "product" / "exports" / "product.jsonl"
        )
        local_export = self.root / "local-product.jsonl"
        database_hash = hashlib.sha256(database.read_bytes()).hexdigest()
        local_export.write_text(
            json.dumps(
                {
                    "table": "__local_product_identity",
                    "row": {
                        "database_sha256": database_hash,
                        "snapshot_id": "local-" + database_hash[:16],
                    },
                },
                separators=(",", ":"),
            )
            + "\n"
            + product_export.read_text(encoding="utf-8"),
            encoding="utf-8",
        )
        local = self.run_cli(
            "product",
            "research",
            "--database",
            str(database),
            "--product-export",
            str(local_export),
            "--merge-hints",
            str(review),
            "--merge-hint-decisions",
            str(decisions),
            "--compact",
        )
        self.assertEqual(local.returncode, 0, local.stderr)
        local_document = self.document(local)
        self.assertEqual(
            local_document["product_snapshot"]["snapshot_id"],
            "local-" + database_hash[:16],
        )

        database.write_bytes(b"changed local product snapshot")
        stale = self.run_cli(
            "product",
            "taste-index",
            "--database",
            str(database),
            "--product-export",
            str(local_export),
        )
        self.assertNotEqual(stale.returncode, 0)
        self.assertIn("does not match", stale.stderr)

    def test_fixed_product_inbox_commands_reject_arguments(self) -> None:
        for command, unexpected in (
            ("check-inbox", ("--database", str(self.root / "other.sqlite"))),
            ("apply-inbox", ("--apply",)),
        ):
            with self.subTest(command=command):
                result = self.run_cli("product", command, *unexpected)
                self.assertNotEqual(result.returncode, 0)
                self.assertTrue(result.stderr.strip())

    def test_product_queue_rejects_unknown_task_before_execution(self) -> None:
        result = self.run_cli(
            "product", "check-inbox", "not-a-product-task"
        )

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unknown product task", result.stderr)
        self.assertFalse(result.stdout.strip())

    def test_contract_validate_reports_valid_and_invalid_documents(self) -> None:
        batch = self.valid_batch()
        valid = self.run_cli(
            "contract",
            "validate",
            "--config",
            str(self.config_path),
            "--contract",
            "arachne_batch_v2",
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
            "arachne_batch_v2",
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

        approval = self.approve(batch)
        self.assertEqual(approval.returncode, 0, approval.stderr)
        approved = self.document(approval)["envelope"]
        self.assertEqual(approved["status"], "accepted")
        self.assertEqual(approved["accepted_by"], "maintainer:test")

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
                    "door_id": "test-door",
                    "endpoint_id": "public",
                    "operation": "point_lookup",
                    "freshness_policy": "fresh_required",
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

    def test_fetch_plan_translation_consumes_selectors_and_chunks_entities(self) -> None:
        plan = self.root / "fetch-plan.json"
        plan.write_text(
            json.dumps(
                {
                    "contract": "fetch_plan_v1",
                    "format_version": 1,
                    "plan_id": "profile-enrichment-plan",
                    "source": "wikidata",
                    "requests": [
                        {
                            "request_id": "profiles",
                            "locator": "https://www.wikidata.org/w/api.php",
                            "purpose": "bounded profile enrichment",
                            "entities": [f"Q{index}" for index in range(1, 56)],
                            "fields": ["labels", "gender", "occupation"],
                            "follow_up": True,
                        }
                    ],
                    "created_at": "2026-07-20T03:00:00Z",
                }
            ),
            encoding="utf-8",
        )
        controls = self.root / "fetch-controls"
        result = self.run_cli(
            "fetch",
            "plan",
            "--config",
            str(self.config_path),
            "--plan",
            str(plan),
            "--output-directory",
            str(controls),
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        document = self.document(result)
        self.assertEqual(document["request_count"], 2)
        self.assertEqual(len(list(controls.glob("*.json"))), 2)
        bodies = sorted((self.root / "artifacts" / "fetch-bodies").rglob("*.form"))
        self.assertEqual(len(bodies), 2)
        first_body = bodies[0].read_text(encoding="utf-8")
        self.assertIn("action=wbgetentities", first_body)
        self.assertIn("props=claims%7Cdescriptions%7Clabels", first_body)
        self.assertEqual(first_body.count("Q"), 50)
        for control in document["controls"]:
            request = control["request"]
            self.assertEqual(request["door_id"], "wikidata")
            self.assertEqual(request["endpoint_id"], "entity-api")
            self.assertEqual(request["freshness_policy"], "fresh_required")
            body_ref = request["body_artifact"]["storage_ref"]
            body_path = self.root / "artifacts" / body_ref
            self.assertEqual(
                hashlib.sha256(body_path.read_bytes()).hexdigest(),
                request["body_artifact"]["sha256"],
            )

    def test_bulk_fetch_translation_preserves_decompression_encoding(self) -> None:
        plan = self.root / "bulk-fetch-plan.json"
        plan.write_text(
            json.dumps(
                {
                    "contract": "fetch_plan_v1",
                    "format_version": 1,
                    "plan_id": "wikidata-bulk-plan",
                    "source": "wikidata",
                    "requests": [
                        {
                            "request_id": "wikidata-official-dump",
                            "locator": "https://dumps.wikimedia.org/wikidatawiki/entities/latest-all.json.bz2",
                            "purpose": "complete snapshot refresh",
                            "follow_up": False,
                        }
                    ],
                    "created_at": "2026-07-20T03:00:00Z",
                }
            ),
            encoding="utf-8",
        )
        controls = self.root / "bulk-fetch-controls"

        result = self.run_cli(
            "fetch",
            "plan",
            "--config",
            str(self.config_path),
            "--plan",
            str(plan),
            "--output-directory",
            str(controls),
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        request = self.document(result)["controls"][0]["request"]
        self.assertEqual(request["operation"], "bulk_snapshot")
        self.assertEqual(request["endpoint_id"], "official-dumps")
        self.assertTrue(request["output_ref"].endswith(".json.bz2"))

if __name__ == "__main__":
    unittest.main()
