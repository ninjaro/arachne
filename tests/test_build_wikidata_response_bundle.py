from __future__ import annotations

import hashlib
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts" / "build_wikidata_response_bundle.py"


class WikidataResponseBundleTests(unittest.TestCase):
    def test_correlates_context_and_rejects_a_missing_acquisition(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            requests = root / "requests"
            acquired = root / "acquired"
            artifacts = root / "artifacts"
            requests.mkdir()
            acquired.mkdir()
            artifacts.mkdir()

            def add(
                request_id: str,
                door_id: str,
                locator: str,
                extensions: dict[str, object],
                body: dict[str, object],
                acquired_at: str,
            ) -> Path:
                storage_ref = f"payloads/{request_id}.json"
                payload = artifacts / storage_ref
                payload.parent.mkdir(parents=True, exist_ok=True)
                payload_bytes = json.dumps(body).encode("utf-8")
                payload.write_bytes(payload_bytes)
                request = {
                    "contract": "fetch_request_v1",
                    "format_version": 1,
                    "request_id": request_id,
                    "door_id": door_id,
                    "endpoint_id": "entity-api",
                    "operation": "point_lookup",
                    "plan_id": "wikidata-enrichment-run",
                    "locator": locator,
                    "method": "POST",
                    "expected": {"maximum_bytes": 1024 * 1024},
                    "output_ref": storage_ref,
                    "extensions": extensions,
                }
                (requests / f"{request_id}.json").write_text(
                    json.dumps(request), encoding="utf-8"
                )
                control = {
                    "contract": "acquired_artifact_v1",
                    "format_version": 1,
                    "artifact_id": f"artifact-{request_id}",
                    "request_id": request_id,
                    "door_id": door_id,
                    "operation": "point_lookup",
                    "source_locator": locator,
                    "artifact": {
                        "storage_ref": storage_ref,
                        "sha256": hashlib.sha256(payload_bytes).hexdigest(),
                        "byte_length": len(payload_bytes),
                        "media_type": "application/json",
                    },
                    "transport": {
                        "status": "delivered",
                        "attempts": 1,
                        "delivery_mode": "fetched",
                    },
                    "response_metadata": {
                        "status_code": 200,
                        "headers": [],
                        "redirect_chain": [],
                        "started_at": acquired_at,
                        "completed_at": acquired_at,
                    },
                    "acquired_at": acquired_at,
                }
                path = acquired / f"{request_id}.json"
                path.write_text(json.dumps(control), encoding="utf-8")
                return path

            identity_control = add(
                "identity-ja",
                "wikidata",
                "https://www.wikidata.org/w/api.php",
                {
                    "org.ninjaro.arachne.identity_query": {
                        "query_id": "identity-ja",
                        "canonical_entity_ids": ["agent-012095"],
                        "kind": "name",
                        "value": "深井国",
                        "language": "ja",
                    }
                },
                {"search": [{"id": "Q300", "label": "深井国", "language": "ja"}]},
                "2026-08-25T12:00:00Z",
            )
            media_control = add(
                "commons-media",
                "wikimedia-commons",
                "https://commons.wikimedia.org/w/api.php",
                {
                    "org.ninjaro.arachne.media_files": [
                        {
                            "remote_key": "File:First.jpg",
                            "contexts": [
                                {
                                    "canonical_entity_id": "work-000001",
                                    "wikidata_qid": "Q100",
                                    "provider_property": "P18",
                                    "media_kind": "image",
                                }
                            ],
                        },
                        {
                            "remote_key": "File:Second.jpg",
                            "contexts": [
                                {
                                    "canonical_entity_id": "agent-000001",
                                    "wikidata_qid": "Q200",
                                    "provider_property": "P154",
                                    "media_kind": "logo",
                                }
                            ],
                        },
                    ]
                },
                {
                    "query": {
                        "pages": [
                            {"title": "File:Second.jpg", "imageinfo": [{"mime": "image/jpeg"}]},
                            {"title": "File:First.jpg", "imageinfo": [{"mime": "image/jpeg"}]},
                        ]
                    }
                },
                "2026-08-25T12:01:00Z",
            )
            output = root / "bundle.json"
            command = [
                sys.executable,
                str(SCRIPT),
                "--request-controls",
                str(requests),
                "--acquired-controls",
                str(acquired),
                "--artifact-root",
                str(artifacts),
                "--output",
                str(output),
            ]
            result = subprocess.run(command, capture_output=True, text=True, check=False)
            self.assertEqual(result.returncode, 0, result.stderr)
            bundle = json.loads(output.read_text(encoding="utf-8"))
            self.assertEqual(bundle["artifact_type"], "wikidata_response_bundle_v1")
            self.assertEqual(bundle["snapshot_id"], "wikidata-enrichment-run")
            self.assertEqual(bundle["fetched_at"], "2026-08-25T12:01:00Z")
            self.assertEqual(len(bundle["acquisitions"]), 2)
            self.assertEqual(len(bundle["responses"]), 3)
            identity = next(row for row in bundle["responses"] if "query_id" in row)
            self.assertEqual(identity["canonical_entity_ids"], ["agent-012095"])
            media = {row["remote_key"]: row for row in bundle["responses"] if "remote_key" in row}
            self.assertEqual(media["File:Second.jpg"]["media_kind"], "logo")
            self.assertEqual(
                media["File:First.jpg"]["body"]["query"]["pages"][0]["title"],
                "File:First.jpg",
            )
            self.assertEqual(
                bundle["acquisitions"][0]["control"]["transport"]["delivery_mode"],
                "fetched",
            )

            identity_control.unlink()
            missing = subprocess.run(command, capture_output=True, text=True, check=False)
            self.assertEqual(missing.returncode, 2)
            self.assertIn("missing acquired controls for identity-ja", missing.stderr)
            self.assertTrue(media_control.is_file())


if __name__ == "__main__":
    unittest.main()
