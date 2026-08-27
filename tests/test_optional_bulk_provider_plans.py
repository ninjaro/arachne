from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PLANNER = ROOT / "scripts" / "optional_bulk_provider_plans.py"


class OptionalBulkProviderPlanTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="arachne-provider-plan-")
        self.root = Path(self.temporary.name)
        self.config = self.root / "arachne.json"
        self.output = self.root / "plans"

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def invoke(self) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [
                sys.executable,
                str(PLANNER),
                "--config",
                str(self.config),
                "--run-id",
                "provider-test",
                "--created-at",
                "2026-08-27T00:00:00Z",
                "--output-directory",
                str(self.output),
            ],
            check=False,
            capture_output=True,
            text=True,
        )

    @staticmethod
    def door(provider: str, endpoint: str) -> dict[str, object]:
        return {
            "door_id": provider,
            "endpoints": [{"endpoint_id": endpoint}],
        }

    def test_unconfigured_providers_are_explicitly_skipped(self) -> None:
        self.config.write_text(
            json.dumps({"format_version": 1, "transport": {"doors": []}}),
            encoding="utf-8",
        )

        result = self.invoke()

        self.assertEqual(result.returncode, 0, result.stderr)
        report = json.loads(
            (self.output / "optional-bulk-provider-plans.json").read_text(
                encoding="utf-8"
            )
        )
        self.assertEqual(
            {row["status"] for row in report["providers"]}, {"skipped"}
        )
        self.assertEqual(len(list(self.output.glob("*-fetch-plan.json"))), 0)

    def test_enabled_official_providers_preserve_policy_and_unavailability(
        self,
    ) -> None:
        self.config.write_text(
            json.dumps(
                {
                    "format_version": 1,
                    "external_enrichment": {
                        "optional_bulk_providers": {
                            "imdb": {"enabled": True},
                            "musicbrainz": {
                                "enabled": True,
                                "snapshot_id": "20260826-020001",
                            },
                            "open-library": {"enabled": True},
                            "discogs": {
                                "enabled": True,
                                "snapshot_date": None,
                            },
                        }
                    },
                    "transport": {
                        "doors": [
                            self.door("imdb", "official-datasets"),
                            self.door("musicbrainz", "official-json-dumps"),
                            self.door("open-library", "official-data-dumps"),
                            self.door("discogs", "official-data-dumps"),
                        ]
                    },
                }
            ),
            encoding="utf-8",
        )

        result = self.invoke()

        self.assertEqual(result.returncode, 0, result.stderr)
        report = json.loads(
            (self.output / "optional-bulk-provider-plans.json").read_text(
                encoding="utf-8"
            )
        )
        statuses = {row["provider"]: row["status"] for row in report["providers"]}
        self.assertEqual(
            statuses,
            {
                "imdb": "planned",
                "musicbrainz": "planned",
                "open-library": "planned",
                "discogs": "unavailable",
            },
        )
        imdb = json.loads(
            (self.output / "imdb-fetch-plan.json").read_text(encoding="utf-8")
        )
        policy = imdb["extensions"]["org.ninjaro.arachne.provider_policy"]
        self.assertTrue(policy["optional"])
        self.assertEqual(policy["license_id"], "IMDb-NonCommercial")
        self.assertIn("do not republish", policy["redistribution"])
        self.assertEqual(len(imdb["requests"]), 7)
        open_library = json.loads(
            (self.output / "open-library-fetch-plan.json").read_text(
                encoding="utf-8"
            )
        )
        self.assertEqual(len(open_library["requests"]), 4)
        self.assertTrue(
            any(
                request["locator"].endswith("ol_dump_wikidata_latest.txt.gz")
                for request in open_library["requests"]
            )
        )


if __name__ == "__main__":
    unittest.main()
