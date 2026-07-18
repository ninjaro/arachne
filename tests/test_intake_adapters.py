from __future__ import annotations

import hashlib
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]


class IntakeAdapterTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.queue = self.root / "queue"
        self.artifacts = self.root / "artifacts"
        self.queue.mkdir()
        self.artifacts.mkdir()
        config = json.loads(
            (ROOT / "config" / "arachne.example.json").read_text(encoding="utf-8")
        )
        config["paths"]["queue"] = str(self.queue)
        config["paths"]["artifact_store"] = str(self.artifacts)
        self.config = self.root / "config.json"
        self.config.write_text(json.dumps(config), encoding="utf-8")

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def run_script(self, name: str, *arguments: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, str(ROOT / "scripts" / name), *arguments],
            check=False,
            capture_output=True,
            text=True,
        )

    def test_issue_request_becomes_policy_bounded_transport_contract(self) -> None:
        event = self.root / "event.json"
        event.write_text(
            json.dumps(
                {
                    "repository": {"full_name": "example/arachne"},
                    "issue": {
                        "number": 17,
                        "title": "Observed batch",
                        "body": (
                            "[batch.json](https://github.com/user-attachments/"
                            "files/1234/batch.json)"
                        ),
                    },
                }
            ),
            encoding="utf-8",
        )
        request = self.root / "issue-request.json"
        fetch = self.root / "fetch-request.json"

        parsed = self.run_script(
            "issue_intake_request.py",
            "--event",
            str(event),
            "--output",
            str(request),
        )
        self.assertEqual(parsed.returncode, 0, parsed.stderr)
        built = self.run_script(
            "issue_fetch_request.py",
            "--request",
            str(request),
            "--config",
            str(self.config),
            "--output",
            str(fetch),
        )
        self.assertEqual(built.returncode, 0, built.stderr)
        document = json.loads(fetch.read_text(encoding="utf-8"))
        self.assertEqual(document["contract"], "fetch_request_v1")
        self.assertEqual(document["expected"]["maximum_bytes"], 67108864)
        self.assertEqual(document["redirect_policy"]["allowed_hosts"],
                         json.loads(self.config.read_text())["security"]["attachment_allowed_hosts"])
        self.assertTrue(document["output_ref"].endswith("-batch.json"))

    def test_discards_only_matching_verified_acquisition(self) -> None:
        payload = self.artifacts / "intake" / "batch.json"
        payload.parent.mkdir()
        content = b'{"opaque":"batch"}'
        payload.write_bytes(content)
        fetch_request = self.root / "fetch.json"
        fetch_request.write_text(
            json.dumps({"request_id": "request-1", "output_ref": "intake/batch.json"}),
            encoding="utf-8",
        )
        control = self.root / "acquired.json"
        control.write_text(
            json.dumps(
                {
                    "contract": "acquired_artifact_v1",
                    "format_version": 1,
                    "request_id": "request-1",
                    "transport": {"status": "delivered"},
                    "artifact": {
                        "storage_ref": "intake/batch.json",
                        "sha256": hashlib.sha256(content).hexdigest(),
                        "byte_length": len(content),
                    },
                }
            ),
            encoding="utf-8",
        )

        result = self.run_script(
            "discard_acquired_artifact.py",
            "--fetch-request",
            str(fetch_request),
            "--acquired-control",
            str(control),
            "--config",
            str(self.config),
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(payload.exists())
        self.assertTrue(control.exists())

    def test_refuses_artifact_reference_outside_store(self) -> None:
        outside = self.root / "outside.json"
        outside.write_bytes(b"keep")
        request = self.root / "fetch.json"
        request.write_text(
            json.dumps({"request_id": "request-2", "output_ref": "../outside.json"}),
            encoding="utf-8",
        )
        control = self.root / "acquired.json"
        control.write_text(
            json.dumps(
                {
                    "contract": "acquired_artifact_v1",
                    "format_version": 1,
                    "request_id": "request-2",
                    "transport": {"status": "delivered"},
                    "artifact": {
                        "storage_ref": "../outside.json",
                        "sha256": hashlib.sha256(b"keep").hexdigest(),
                        "byte_length": 4,
                    },
                }
            ),
            encoding="utf-8",
        )

        result = self.run_script(
            "discard_acquired_artifact.py",
            "--fetch-request",
            str(request),
            "--acquired-control",
            str(control),
            "--config",
            str(self.config),
        )

        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(outside.read_bytes(), b"keep")

    def test_refuses_overlapping_queue_and_artifact_store(self) -> None:
        overlap_queue = self.artifacts / "queue"
        overlap_queue.mkdir()
        config = json.loads(self.config.read_text(encoding="utf-8"))
        config["paths"]["queue"] = str(overlap_queue)
        self.config.write_text(json.dumps(config), encoding="utf-8")
        payload = self.artifacts / "batch.json"
        payload.write_bytes(b"keep")
        request = self.root / "fetch.json"
        request.write_text(
            json.dumps({"request_id": "request-3", "output_ref": "batch.json"}),
            encoding="utf-8",
        )
        control = self.root / "acquired.json"
        control.write_text(
            json.dumps(
                {
                    "contract": "acquired_artifact_v1",
                    "format_version": 1,
                    "request_id": "request-3",
                    "transport": {"status": "delivered"},
                    "artifact": {
                        "storage_ref": "batch.json",
                        "sha256": hashlib.sha256(b"keep").hexdigest(),
                        "byte_length": 4,
                    },
                }
            ),
            encoding="utf-8",
        )

        result = self.run_script(
            "discard_acquired_artifact.py",
            "--fetch-request",
            str(request),
            "--acquired-control",
            str(control),
            "--config",
            str(self.config),
        )

        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(payload.read_bytes(), b"keep")

    def test_refuses_artifact_store_nested_in_legacy_inbox(self) -> None:
        config = json.loads(self.config.read_text(encoding="utf-8"))
        config["paths"]["legacy_inbox"] = str(self.root)
        self.config.write_text(json.dumps(config), encoding="utf-8")
        payload = self.artifacts / "legacy-risk.json"
        payload.write_bytes(b"keep")
        request = self.root / "fetch.json"
        request.write_text(
            json.dumps(
                {"request_id": "request-4", "output_ref": "legacy-risk.json"}
            ),
            encoding="utf-8",
        )
        control = self.root / "acquired.json"
        control.write_text(
            json.dumps(
                {
                    "contract": "acquired_artifact_v1",
                    "format_version": 1,
                    "request_id": "request-4",
                    "transport": {"status": "delivered"},
                    "artifact": {
                        "storage_ref": "legacy-risk.json",
                        "sha256": hashlib.sha256(b"keep").hexdigest(),
                        "byte_length": 4,
                    },
                }
            ),
            encoding="utf-8",
        )

        result = self.run_script(
            "discard_acquired_artifact.py",
            "--fetch-request",
            str(request),
            "--acquired-control",
            str(control),
            "--config",
            str(self.config),
        )

        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(payload.read_bytes(), b"keep")

    def test_conventional_legacy_inbox_is_protected_when_config_is_null(self) -> None:
        fake_home = self.root / "home"
        inbox = fake_home / "Projects" / "new" / "art-lineages" / "inbox"
        artifact_store = inbox / "artifacts"
        artifact_store.mkdir(parents=True)
        payload = artifact_store / "keep.json"
        payload.write_bytes(b"keep")
        config = json.loads(self.config.read_text(encoding="utf-8"))
        config["paths"]["legacy_inbox"] = None
        config["paths"]["artifact_store"] = str(artifact_store)
        self.config.write_text(json.dumps(config), encoding="utf-8")
        request = self.root / "fetch.json"
        request.write_text(
            json.dumps({"request_id": "request-5", "output_ref": "keep.json"}),
            encoding="utf-8",
        )
        control = self.root / "acquired.json"
        control.write_text(
            json.dumps(
                {
                    "contract": "acquired_artifact_v1",
                    "format_version": 1,
                    "request_id": "request-5",
                    "transport": {"status": "delivered"},
                    "artifact": {
                        "storage_ref": "keep.json",
                        "sha256": hashlib.sha256(b"keep").hexdigest(),
                        "byte_length": 4,
                    },
                }
            ),
            encoding="utf-8",
        )

        with mock.patch.dict(os.environ, {"HOME": str(fake_home)}):
            result = self.run_script(
                "discard_acquired_artifact.py",
                "--fetch-request",
                str(request),
                "--acquired-control",
                str(control),
                "--config",
                str(self.config),
            )

        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(payload.read_bytes(), b"keep")

    def test_operations_preflight_rejects_queue_storage_overlap(self) -> None:
        config = json.loads(self.config.read_text(encoding="utf-8"))
        config["paths"]["artifact_store"] = str(self.queue / "artifacts")
        self.config.write_text(json.dumps(config), encoding="utf-8")

        result = self.run_script(
            "arachne_ops.py",
            "--config",
            str(self.config),
            "preflight",
        )

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("disjoint from the queue", result.stderr)


if __name__ == "__main__":
    unittest.main()
