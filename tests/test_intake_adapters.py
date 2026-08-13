from __future__ import annotations

import hashlib
import importlib.util
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]


def load_product_materializer():
    path = ROOT / "scripts" / "materialize_product_batch.py"
    specification = importlib.util.spec_from_file_location(
        "materialize_product_batch", path
    )
    if specification is None or specification.loader is None:
        raise RuntimeError("could not load product batch materializer")
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    return module


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
                            "assets/7f3d7bde-6f85-4a9e-a350-92770d453bc2)"
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

    def test_provisional_issue_adapter_rejects_undefined_zip_packages(self) -> None:
        event = self.root / "zip-event.json"
        event.write_text(
            json.dumps(
                {
                    "repository": {"full_name": "example/arachne"},
                    "issue": {
                        "number": 18,
                        "title": "Undefined archive",
                        "body": (
                            "[batch.zip](https://github.com/user-attachments/"
                            "files/1234/batch.zip)"
                        ),
                    },
                }
            ),
            encoding="utf-8",
        )
        request = self.root / "zip-request.json"

        parsed = self.run_script(
            "issue_intake_request.py",
            "--event",
            str(event),
            "--output",
            str(request),
        )

        self.assertNotEqual(parsed.returncode, 0)
        self.assertFalse(request.exists())

    def test_verified_issue_attachment_materializes_in_fixed_inbox(self) -> None:
        repository = self.root / "repository"
        (repository / "inbox").mkdir(parents=True)
        payload = self.artifacts / "intake" / "batch.json"
        payload.parent.mkdir()
        content = (
            b'{"format":"arachne_batch_v2","batch_id":"issue-17",'
            b'"create":{},"update":{},"merge":{}}\n'
        )
        payload.write_bytes(content)
        locator = "https://github.com/user-attachments/assets/example"
        request = self.root / "request.json"
        request.write_text(
            json.dumps(
                {
                    "submission_ref": "github-issue:example/arachne#17",
                    "attachment_url": locator,
                    "attachment_name": "batch.json",
                }
            ),
            encoding="utf-8",
        )
        fetch = self.root / "fetch.json"
        fetch.write_text(
            json.dumps(
                {
                    "locator": locator,
                    "request_id": "issue-request-17",
                    "output_ref": "intake/batch.json",
                }
            ),
            encoding="utf-8",
        )
        acquired = self.root / "acquired.json"
        acquired.write_text(
            json.dumps(
                {
                    "contract": "acquired_artifact_v1",
                    "format_version": 1,
                    "request_id": "issue-request-17",
                    "source_locator": locator,
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
        module = load_product_materializer()

        result = module.main(
            [
                "--request",
                str(request),
                "--fetch-request",
                str(fetch),
                "--acquired-control",
                str(acquired),
                "--config",
                str(self.config),
            ],
            repository_root=repository,
        )

        self.assertEqual(result, 0)
        self.assertEqual((repository / "inbox" / "issue-17.json").read_bytes(), content)

    def test_product_materializer_rejects_tampered_transport_bytes(self) -> None:
        repository = self.root / "repository"
        (repository / "inbox").mkdir(parents=True)
        payload = self.artifacts / "intake" / "tampered.json"
        payload.parent.mkdir()
        payload.write_bytes(b"tampered")
        locator = "https://github.com/user-attachments/assets/tampered"
        request = self.root / "request-tampered.json"
        request.write_text(
            json.dumps(
                {
                    "submission_ref": "github-issue:example/arachne#18",
                    "attachment_url": locator,
                    "attachment_name": "batch.json",
                }
            ),
            encoding="utf-8",
        )
        fetch = self.root / "fetch-tampered.json"
        fetch.write_text(
            json.dumps(
                {
                    "locator": locator,
                    "request_id": "issue-request-18",
                    "output_ref": "intake/tampered.json",
                }
            ),
            encoding="utf-8",
        )
        acquired = self.root / "acquired-tampered.json"
        acquired.write_text(
            json.dumps(
                {
                    "contract": "acquired_artifact_v1",
                    "format_version": 1,
                    "request_id": "issue-request-18",
                    "source_locator": locator,
                    "transport": {"status": "delivered"},
                    "artifact": {
                        "storage_ref": "intake/tampered.json",
                        "sha256": hashlib.sha256(b"expected").hexdigest(),
                        "byte_length": len(b"tampered"),
                    },
                }
            ),
            encoding="utf-8",
        )
        module = load_product_materializer()

        result = module.main(
            [
                "--request",
                str(request),
                "--fetch-request",
                str(fetch),
                "--acquired-control",
                str(acquired),
                "--config",
                str(self.config),
            ],
            repository_root=repository,
        )

        self.assertNotEqual(result, 0)
        self.assertEqual(list((repository / "inbox").iterdir()), [])

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

    def test_refuses_symlinked_artifact_path_components(self) -> None:
        real = self.artifacts / "real"
        real.mkdir()
        payload = real / "keep.json"
        payload.write_bytes(b"keep")
        (self.artifacts / "alias").symlink_to(real, target_is_directory=True)
        request = self.root / "fetch.json"
        request.write_text(
            json.dumps(
                {"request_id": "request-symlink", "output_ref": "alias/keep.json"}
            ),
            encoding="utf-8",
        )
        control = self.root / "acquired.json"
        control.write_text(
            json.dumps(
                {
                    "contract": "acquired_artifact_v1",
                    "format_version": 1,
                    "request_id": "request-symlink",
                    "transport": {"status": "delivered"},
                    "artifact": {
                        "storage_ref": "alias/keep.json",
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

    def test_intake_dispatch_refuses_symlinked_artifact_components(self) -> None:
        real = self.artifacts / "dispatch-real"
        real.mkdir()
        payload = real / "batch.json"
        payload.write_bytes(b"keep")
        (self.artifacts / "dispatch-alias").symlink_to(
            real, target_is_directory=True
        )
        locator = "https://github.com/user-attachments/assets/test"
        request = self.root / "request.json"
        request.write_text(
            json.dumps(
                {
                    "attachment_url": locator,
                    "submission_ref": "github-issue:test:1",
                    "title": "Symlink test",
                }
            ),
            encoding="utf-8",
        )
        fetch = self.root / "fetch.json"
        fetch.write_text(
            json.dumps(
                {
                    "locator": locator,
                    "request_id": "dispatch-symlink",
                    "output_ref": "dispatch-alias/batch.json",
                }
            ),
            encoding="utf-8",
        )
        control = self.root / "acquired.json"
        control.write_text(
            json.dumps(
                {
                    "contract": "acquired_artifact_v1",
                    "format_version": 1,
                    "request_id": "dispatch-symlink",
                    "source_locator": locator,
                    "transport": {"status": "delivered"},
                    "artifact": {
                        "storage_ref": "dispatch-alias/batch.json",
                        "sha256": hashlib.sha256(b"keep").hexdigest(),
                        "byte_length": 4,
                    },
                }
            ),
            encoding="utf-8",
        )

        result = self.run_script(
            "dispatch_intake_request.py",
            "--request",
            str(request),
            "--fetch-request",
            str(fetch),
            "--acquired-control",
            str(control),
            "--config",
            str(self.config),
            "--binary",
            "/bin/true",
        )

        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(payload.read_bytes(), b"keep")

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
