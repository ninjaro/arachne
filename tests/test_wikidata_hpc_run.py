from __future__ import annotations

import hashlib
import json
import os
import sqlite3
import subprocess
import tempfile
import time
import unittest
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
RUN = ROOT / "hpc" / "wikidata" / "run"


class WikidataHpcRunTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="arachne-hpc-run-")
        self.root = Path(self.temporary.name)
        self.run_root = self.root / "hpcwork" / "arachne" / "wikidata"
        self.state = self.root / "state"
        (self.state / "config").mkdir(parents=True)
        (self.state / "graphs" / "product").mkdir(parents=True)
        self.product_control = self.state / "graphs" / "product" / "active.json"
        self.product_control.write_text('{"contract":"product_graph_snapshot_v1"}\n')
        (self.state / "config" / "arachne.json").write_text(
            json.dumps(
                {
                    "format_version": 1,
                    "project_timezone": "Europe/Berlin",
                    "paths": {
                        "legacy_inbox": None,
                        "queue": "queue",
                        "remainders": "remainders",
                        "ledger": "operations/ledger.sqlite3",
                        "graph_store": "graphs",
                        "artifact_store": "artifacts",
                        "lock_root": "locks",
                        "viewer_templates": "viewer",
                        "site_output": "site",
                        "legacy_inbox_baseline": "operations/baseline.json",
                    },
                    "candidate_rebuild": {
                        "sources": {
                            "wikidata": {
                                "candidate_pool_size": 4,
                                "gray_bonus_basis_points": 2000,
                            }
                        }
                    },
                }
            )
            + "\n",
            encoding="utf-8",
        )
        self.command_log = self.root / "commands.jsonl"
        self.binary = self.root / "fake-arachne"
        self.binary.write_text(
            """#!/usr/bin/env python3
import hashlib
import json
import os
import pathlib
import sys

argv = sys.argv[1:]
log = pathlib.Path(os.environ["ARACHNE_HPC_TEST_LOG"])
with log.open("a", encoding="utf-8") as stream:
    stream.write(json.dumps(argv) + "\\n")

def option(name):
    return pathlib.Path(argv[argv.index(name) + 1])

if argv[:2] == ["fetch", "plan"]:
    output = option("--output-directory")
    output.mkdir(parents=True, exist_ok=True)
    (output / "wikidata-official-dump.json").write_text(json.dumps({
        "request_id": "wikidata-official-dump",
        "output_ref": "wikidata/raw.bin"
    }) + "\\n")
elif argv[:1] == ["fetch"]:
    config = json.loads(option("--config").read_text())
    artifact_root = pathlib.Path(config["paths"]["artifact_store"])
    payload = artifact_root / "wikidata" / "raw.bin"
    payload.parent.mkdir(parents=True, exist_ok=True)
    payload.write_bytes(b"verified dump")
    digest = hashlib.sha256(payload.read_bytes()).hexdigest()
    option("--output-control").write_text(json.dumps({
        "contract": "acquired_artifact_v1",
        "format_version": 1,
        "request_id": "wikidata-official-dump",
        "transport": {"status": "delivered"},
        "artifact": {
            "storage_ref": "wikidata/raw.bin",
            "sha256": digest,
            "byte_length": payload.stat().st_size
        }
    }) + "\\n")
elif argv[:2] == ["candidate", "plan"]:
    option("--output-artifact").write_text('{"plan":true}\\n')
    option("--output-control").write_text('{"control":true}\\n')
elif argv[:2] == ["candidate", "rebuild"]:
    pass
else:
    raise SystemExit("unexpected fake command: " + repr(argv))
""",
            encoding="utf-8",
        )
        self.binary.chmod(0o755)
        self.sbatch_log = self.root / "sbatch.json"
        self.sbatch = self.root / "fake-sbatch"
        self.sbatch.write_text(
            """#!/usr/bin/env python3
import json
import os
import pathlib
import sys
pathlib.Path(os.environ["ARACHNE_SBATCH_TEST_LOG"]).write_text(json.dumps(sys.argv[1:]))
print("12345678;claix")
""",
            encoding="utf-8",
        )
        self.sbatch.chmod(0o755)
        self.environment = {
            **os.environ,
            "ARACHNE_HPC_TEST_LOG": str(self.command_log),
            "ARACHNE_SBATCH_TEST_LOG": str(self.sbatch_log),
        }

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def invoke(self, *arguments: object) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [str(RUN), *map(str, arguments)],
            check=False,
            capture_output=True,
            text=True,
            env=self.environment,
            timeout=20,
        )

    def prepare(self) -> subprocess.CompletedProcess[str]:
        return self.invoke(
            "prepare",
            "--run-root",
            self.run_root,
            "--state-root",
            self.state,
            "--binary",
            self.binary,
            "--run-id",
            "wikidata-test-run",
        )

    def acquire(self) -> subprocess.CompletedProcess[str]:
        return self.invoke(
            "acquire",
            "--run-root",
            self.run_root,
            "--allow-non-transfer-node",
        )

    def metadata_path(self) -> Path:
        return (self.run_root / "current" / "run.json").resolve(strict=True)

    def metadata(self) -> dict[str, Any]:
        return json.loads(self.metadata_path().read_text(encoding="utf-8"))

    def write_metadata(self, metadata: dict[str, Any]) -> None:
        self.metadata_path().write_text(
            json.dumps(metadata, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )

    def commands(self) -> list[list[str]]:
        return [
            json.loads(line)
            for line in self.command_log.read_text(encoding="utf-8").splitlines()
        ]

    def finish_extraction(self) -> dict[str, Any]:
        metadata = self.metadata()
        graph = Path(metadata["external_graph"])
        graph.write_text(
            '{"artifact_type":"external_candidate_source_graph_v1"}\n',
            encoding="utf-8",
        )
        hints = Path(metadata["image_hints"])
        hints.write_text(
            json.dumps(
                {
                    "artifact_type": "wikidata_image_hints_v1",
                    "entities": [
                        {
                            "entity_id": "work-1",
                            "family": "work",
                            "images": [{"file": "Work.jpg"}, {"file": "Poster.jpg"}],
                        },
                        {
                            "entity_id": "agent-1",
                            "family": "agent",
                            "images": [{"file": "Agent.jpg"}],
                        },
                    ],
                }
            )
            + "\n",
            encoding="utf-8",
        )
        def custody(path: Path, artifact_type: str) -> dict[str, object]:
            return {
                "artifact_type": artifact_type,
                "path": str(path),
                "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                "byte_length": path.stat().st_size,
            }

        Path(metadata["report"]).write_text(
            json.dumps(
                {
                    "status": "succeeded",
                    "output": custody(
                        graph, "external_candidate_source_graph_v1"
                    ),
                    "image_hints_output": custody(
                        hints, "wikidata_image_hints_v1"
                    ),
                }
            )
            + "\n",
            encoding="utf-8",
        )
        metadata["steps"]["extract"] = "complete"
        metadata["status"] = "extracted"
        self.write_metadata(metadata)
        return metadata

    def test_help_is_short_and_command_oriented(self) -> None:
        root = self.invoke("help")
        prepare = self.invoke("prepare", "--help")

        self.assertEqual(root.returncode, 0, root.stderr)
        self.assertIn("rebuild-candidates", root.stdout)
        self.assertIn("hpc/wikidata/run prepare", root.stdout)
        self.assertNotIn("_compute", root.stdout)
        self.assertEqual(prepare.returncode, 0, prepare.stderr)
        self.assertIn("--state-root", prepare.stdout)

    def test_prepare_materializes_one_discoverable_run(self) -> None:
        result = self.prepare()

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Prepared Wikidata run", result.stdout)
        self.assertTrue((self.run_root / "current").is_symlink())
        metadata = self.metadata()
        self.assertEqual(metadata["status"], "prepared")
        self.assertEqual(metadata["steps"]["prepare"], "complete")
        self.assertTrue(Path(metadata["operations_config"]).is_file())
        self.assertTrue(Path(metadata["fetch_request"]).is_file())
        self.assertTrue(Path(metadata["result_directory"]).is_dir())
        self.assertEqual(self.commands()[0][:2], ["fetch", "plan"])

    def test_prepare_rejects_an_unrelated_default_state_checkout(self) -> None:
        state = self.run_root / ".arachne-state"
        state.mkdir(parents=True)
        subprocess.run(
            ["git", "init", "--quiet", state], check=True, capture_output=True
        )
        subprocess.run(
            [
                "git",
                "-C",
                state,
                "remote",
                "add",
                "origin",
                "https://example.invalid/unrelated/state.git",
            ],
            check=True,
            capture_output=True,
        )

        result = self.invoke(
            "prepare",
            "--run-root",
            self.run_root,
            "--binary",
            self.binary,
            "--run-id",
            "wrong-state",
        )

        self.assertEqual(result.returncode, 2)
        self.assertIn("origin is not ninjaro/arachne", result.stderr)
        self.assertFalse((self.run_root / "runs").exists())

    def test_prepare_materializes_canonical_database_without_active_control(
        self,
    ) -> None:
        self.product_control.unlink()
        database = self.state / "database" / "art-islands.sqlite"
        database.parent.mkdir(parents=True)
        with sqlite3.connect(database) as connection:
            connection.executescript(
                (ROOT / "schema" / "product.sql").read_text(encoding="utf-8")
            )
            connection.executescript(
                """
                INSERT INTO entities(id, entity_type)
                VALUES('work-000001', 'work');
                INSERT INTO works(entity_id, medium)
                VALUES('work-000001', 'film');
                """
            )

        result = self.invoke(
            "prepare",
            "--run-root",
            self.run_root,
            "--state-root",
            self.state,
            "--binary",
            self.binary,
            "--run-id",
            "wikidata-test-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        metadata = self.metadata()
        control_path = Path(metadata["product_snapshot_control"])
        control = json.loads(control_path.read_text(encoding="utf-8"))
        self.assertEqual(control["contract"], "product_graph_snapshot_v1")
        self.assertTrue(control["snapshot_id"].startswith("product-local-"))
        self.assertEqual(
            control["content_sha256"],
            hashlib.sha256(database.read_bytes()).hexdigest(),
        )
        graph_store = Path(
            json.loads(
                Path(metadata["operations_config"]).read_text(encoding="utf-8")
            )["paths"]["graph_store"]
        )
        export = graph_store / control["exports"][0]["artifact"]["storage_ref"]
        self.assertTrue(export.is_file())
        self.assertIn("__local_product_identity", export.read_text(encoding="utf-8"))

    def test_acquire_uses_native_fetch_and_is_idempotent(self) -> None:
        self.assertEqual(self.prepare().returncode, 0)

        first = self.acquire()
        second = self.invoke("acquire", "--run-root", self.run_root)

        self.assertEqual(first.returncode, 0, first.stderr)
        self.assertEqual(second.returncode, 0, second.stderr)
        self.assertIn("already acquired", second.stdout)
        self.assertEqual(self.commands()[-1][0], "fetch")
        self.assertEqual(sum(command[0] == "fetch" for command in self.commands()), 2)
        self.assertEqual(self.metadata()["steps"]["acquire"], "complete")

    def test_acquire_refuses_a_non_transfer_node_by_default(self) -> None:
        self.assertEqual(self.prepare().returncode, 0)

        result = self.invoke("acquire", "--run-root", self.run_root)

        self.assertEqual(result.returncode, 2)
        self.assertIn("copy23-1 or copy23-2", result.stderr)
        self.assertEqual(self.metadata()["steps"]["acquire"], "pending")

    def test_submit_passes_only_metadata_and_resource_overrides(self) -> None:
        self.assertEqual(self.prepare().returncode, 0)
        self.assertEqual(self.acquire().returncode, 0)

        result = self.invoke(
            "submit",
            "--run-root",
            self.run_root,
            "--sbatch",
            self.sbatch,
            "--",
            "--time=36:00:00",
            "--mem=96G",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Job: 12345678", result.stdout)
        arguments = json.loads(self.sbatch_log.read_text(encoding="utf-8"))
        self.assertIn("--time=36:00:00", arguments)
        self.assertIn("--mem=96G", arguments)
        self.assertEqual(Path(arguments[-1]), self.metadata_path())
        self.assertTrue(arguments[-2].endswith("build_external_graph.sbatch"))
        slurm = self.metadata()["slurm"]
        self.assertEqual(slurm["job_id"], "12345678")
        self.assertTrue(slurm["stdout"].endswith("12345678.out"))

        repeated = self.invoke(
            "submit",
            "--run-root",
            self.run_root,
            "--sbatch",
            self.sbatch,
        )
        self.assertEqual(repeated.returncode, 2)
        self.assertIn("already submitted", repeated.stderr)

    def test_submit_and_fast_compute_metadata_updates_do_not_clobber(self) -> None:
        self.assertEqual(self.prepare().returncode, 0)
        self.assertEqual(self.acquire().returncode, 0)
        racing_sbatch = self.root / "racing-sbatch"
        racing_sbatch.write_text(
            """#!/usr/bin/env python3
import subprocess
import sys

probe = r'''import fcntl
import json
import os
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
lock = path.with_name(path.name + ".lock")
with lock.open("a+") as lock_stream:
    fcntl.flock(lock_stream.fileno(), fcntl.LOCK_EX)
    document = json.loads(path.read_text(encoding="utf-8"))
    document["steps"]["race_probe"] = "preserved"
    temporary = path.with_name(".race-probe.json")
    temporary.write_text(json.dumps(document, indent=2, sort_keys=True) + "\\n")
    os.replace(temporary, path)
'''
subprocess.Popen(
    [sys.executable, "-c", probe, sys.argv[-1]],
    stdout=subprocess.DEVNULL,
    stderr=subprocess.DEVNULL,
    start_new_session=True,
)
print("87654321;claix")
""",
            encoding="utf-8",
        )
        racing_sbatch.chmod(0o755)

        result = self.invoke(
            "submit",
            "--run-root",
            self.run_root,
            "--sbatch",
            racing_sbatch,
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            metadata = self.metadata()
            if metadata["steps"].get("race_probe") == "preserved":
                break
            time.sleep(0.02)
        self.assertEqual(metadata["steps"].get("race_probe"), "preserved")
        self.assertEqual(metadata["slurm"]["job_id"], "87654321")

    def test_internal_compute_refuses_a_login_node(self) -> None:
        self.assertEqual(self.prepare().returncode, 0)

        result = self.invoke(
            "_compute", "--metadata", self.metadata_path(), "--threads", "16"
        )

        self.assertEqual(result.returncode, 2)
        self.assertIn("must be launched by Slurm", result.stderr)

    def test_result_reports_fixed_paths_and_compact_counts(self) -> None:
        self.assertEqual(self.prepare().returncode, 0)
        metadata = self.finish_extraction()

        result = self.invoke("result", "--run-root", self.run_root)

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Status:   complete", result.stdout)
        self.assertIn("works:      1", result.stdout)
        self.assertIn("agents:     1", result.stdout)
        self.assertIn("images:     3", result.stdout)
        self.assertIn(metadata["image_hints"], result.stdout)
        self.assertIn(metadata["report"], result.stdout)

    def test_result_preserves_a_failed_follow_up_step(self) -> None:
        self.assertEqual(self.prepare().returncode, 0)
        metadata = self.finish_extraction()
        metadata["status"] = "failed"
        metadata["steps"]["candidates"] = "failed"
        metadata["steps"]["failed_step"] = "candidates"
        metadata["steps"]["failure"] = "candidate rebuild failed"
        self.write_metadata(metadata)

        result = self.invoke("result", "--run-root", self.run_root)

        self.assertEqual(result.returncode, 1)
        self.assertIn("Status:   failed", result.stdout)
        self.assertIn("Failed step: candidates", result.stdout)

    def test_result_reconciles_a_slurm_oom_failure(self) -> None:
        self.assertEqual(self.prepare().returncode, 0)
        self.assertEqual(self.acquire().returncode, 0)
        self.assertEqual(
            self.invoke(
                "submit",
                "--run-root",
                self.run_root,
                "--sbatch",
                self.sbatch,
            ).returncode,
            0,
        )
        sacct = self.root / "fake-sacct"
        sacct.write_text("#!/bin/sh\nprintf 'OUT_OF_MEMORY|0:125\\n'\n")
        sacct.chmod(0o755)

        result = self.invoke(
            "result", "--run-root", self.run_root, "--sacct", sacct
        )

        self.assertEqual(result.returncode, 1)
        self.assertIn("12345678 (OUT_OF_MEMORY)", result.stdout)
        self.assertIn("Failed step: slurm", result.stdout)
        self.assertIn("12345678.err", result.stdout)
        self.assertEqual(self.metadata()["status"], "failed")

    def test_result_does_not_overwrite_a_concurrent_compute_completion(self) -> None:
        self.assertEqual(self.prepare().returncode, 0)
        self.assertEqual(self.acquire().returncode, 0)
        self.assertEqual(
            self.invoke(
                "submit",
                "--run-root",
                self.run_root,
                "--sbatch",
                self.sbatch,
            ).returncode,
            0,
        )
        metadata = self.finish_extraction()
        metadata["steps"]["extract"] = "running"
        metadata["status"] = "running"
        self.write_metadata(metadata)
        sacct = self.root / "completing-sacct"
        sacct.write_text(
            """#!/usr/bin/env python3
import fcntl
import json
import os
import pathlib

path = pathlib.Path(os.environ["ARACHNE_RACE_METADATA"])
lock = path.with_name(path.name + ".lock")
with lock.open("a+") as lock_stream:
    fcntl.flock(lock_stream.fileno(), fcntl.LOCK_EX)
    document = json.loads(path.read_text(encoding="utf-8"))
    document["steps"]["extract"] = "complete"
    document["status"] = "extracted"
    temporary = path.with_name(".compute-complete.json")
    temporary.write_text(json.dumps(document, indent=2, sort_keys=True) + "\\n")
    os.replace(temporary, path)
print("COMPLETED|0:0")
""",
            encoding="utf-8",
        )
        sacct.chmod(0o755)
        self.environment["ARACHNE_RACE_METADATA"] = str(self.metadata_path())

        result = self.invoke(
            "result", "--run-root", self.run_root, "--sacct", sacct
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Status:   complete", result.stdout)
        self.assertNotIn("Failed step", result.stdout)
        self.assertEqual(self.metadata()["status"], "extracted")

    def test_result_does_not_claim_complete_for_missing_or_active_results(self) -> None:
        self.assertEqual(self.prepare().returncode, 0)
        metadata = self.finish_extraction()
        metadata["steps"]["candidates"] = "running"
        metadata["status"] = "rebuilding_candidates"
        self.write_metadata(metadata)

        active = self.invoke("result", "--run-root", self.run_root)

        self.assertEqual(active.returncode, 0, active.stderr)
        self.assertIn("Status:   rebuilding_candidates", active.stdout)
        self.assertNotIn("Wikidata run complete", active.stdout)

        Path(metadata["external_graph"]).unlink()
        missing = self.invoke("result", "--run-root", self.run_root)

        self.assertEqual(missing.returncode, 1)
        self.assertIn("Status:   failed", missing.stdout)
        self.assertIn("missing external graph", missing.stdout)

    def test_candidate_rebuild_uses_native_plan_then_rebuild(self) -> None:
        self.assertEqual(self.prepare().returncode, 0)
        self.finish_extraction()

        result = self.invoke(
            "rebuild-candidates", "--run-root", self.run_root
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        commands = self.commands()
        self.assertEqual(commands[-2][:2], ["candidate", "plan"])
        self.assertEqual(commands[-1][:2], ["candidate", "rebuild"])
        metadata = self.metadata()
        self.assertEqual(metadata["steps"]["candidates"], "complete")
        self.assertTrue(Path(metadata["candidate_plan_control"]).is_file())

    def test_clean_verifies_dump_and_keeps_results(self) -> None:
        self.assertEqual(self.prepare().returncode, 0)
        self.assertEqual(self.acquire().returncode, 0)
        metadata = self.finish_extraction()
        config = json.loads(
            Path(metadata["operations_config"]).read_text(encoding="utf-8")
        )
        payload = Path(config["paths"]["artifact_store"]) / "wikidata" / "raw.bin"
        self.assertTrue(payload.is_file())

        result = self.invoke("clean", "--run-root", self.run_root)

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(payload.exists())
        self.assertFalse(Path(metadata["work_directory"]).exists())
        self.assertTrue(Path(metadata["external_graph"]).is_file())
        self.assertTrue(Path(metadata["image_hints"]).is_file())
        self.assertTrue(Path(metadata["report"]).is_file())
        self.assertEqual(self.metadata()["steps"]["clean"], "complete")

        candidates = self.invoke(
            "rebuild-candidates", "--run-root", self.run_root
        )
        self.assertEqual(candidates.returncode, 2)
        self.assertIn("was cleaned", candidates.stderr)

    def test_clean_refuses_before_results_are_complete(self) -> None:
        self.assertEqual(self.prepare().returncode, 0)
        self.assertEqual(self.acquire().returncode, 0)
        metadata = self.metadata()
        config = json.loads(
            Path(metadata["operations_config"]).read_text(encoding="utf-8")
        )
        payload = Path(config["paths"]["artifact_store"]) / "wikidata" / "raw.bin"

        result = self.invoke("clean", "--run-root", self.run_root)

        self.assertEqual(result.returncode, 2)
        self.assertTrue(payload.is_file())
        self.assertIn("run report", result.stderr)

    def test_clean_refuses_tampered_results_without_deleting_source(self) -> None:
        self.assertEqual(self.prepare().returncode, 0)
        self.assertEqual(self.acquire().returncode, 0)
        metadata = self.finish_extraction()
        config = json.loads(
            Path(metadata["operations_config"]).read_text(encoding="utf-8")
        )
        payload = Path(config["paths"]["artifact_store"]) / "wikidata" / "raw.bin"
        Path(metadata["external_graph"]).write_text(
            '{"artifact_type":"external_candidate_source_graph_v1","tampered":true}\n',
            encoding="utf-8",
        )

        result = self.invoke("clean", "--run-root", self.run_root)

        self.assertEqual(result.returncode, 2)
        self.assertTrue(payload.is_file())
        self.assertIn("changed after extraction", result.stderr)
        self.assertEqual(self.metadata()["steps"]["clean"], "pending")

    def test_clean_and_candidate_rebuild_are_mutually_exclusive(self) -> None:
        self.assertEqual(self.prepare().returncode, 0)
        self.assertEqual(self.acquire().returncode, 0)
        metadata = self.finish_extraction()
        config = json.loads(
            Path(metadata["operations_config"]).read_text(encoding="utf-8")
        )
        payload = Path(config["paths"]["artifact_store"]) / "wikidata" / "raw.bin"
        metadata["steps"]["candidates"] = "running"
        metadata["status"] = "rebuilding_candidates"
        self.write_metadata(metadata)

        clean = self.invoke("clean", "--run-root", self.run_root)

        self.assertEqual(clean.returncode, 2)
        self.assertIn("wait for candidate rebuild", clean.stderr)
        self.assertTrue(payload.is_file())

        metadata = self.metadata()
        metadata["steps"]["candidates"] = "not_requested"
        metadata["steps"]["clean"] = "running"
        metadata["status"] = "cleaning"
        self.write_metadata(metadata)
        candidates = self.invoke(
            "rebuild-candidates", "--run-root", self.run_root
        )

        self.assertEqual(candidates.returncode, 2)
        self.assertIn("cleanup is running", candidates.stderr)

    def test_clean_preserves_a_prior_candidate_failure(self) -> None:
        self.assertEqual(self.prepare().returncode, 0)
        self.assertEqual(self.acquire().returncode, 0)
        metadata = self.finish_extraction()
        metadata["steps"]["candidates"] = "failed"
        metadata["steps"]["failed_step"] = "candidates"
        metadata["steps"]["failure"] = "candidate rebuild failed"
        metadata["status"] = "failed"
        self.write_metadata(metadata)

        clean = self.invoke("clean", "--run-root", self.run_root)
        result = self.invoke("result", "--run-root", self.run_root)

        self.assertEqual(clean.returncode, 0, clean.stderr)
        self.assertEqual(result.returncode, 1)
        self.assertIn("Status:   failed", result.stdout)
        self.assertIn("Failed step: candidates", result.stdout)
        self.assertEqual(self.metadata()["steps"]["clean"], "complete")


if __name__ == "__main__":
    unittest.main()
