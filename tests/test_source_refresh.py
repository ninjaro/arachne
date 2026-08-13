from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PLAN = ROOT / "scripts" / "wikidata_bulk_fetch_plan.py"
GATE = ROOT / "scripts" / "source_refresh_gate.py"


class SourceRefreshTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="arachne-refresh-test-")
        self.root = Path(self.temporary.name)
        self.config = self.root / "config.json"
        self.config.write_text(
            json.dumps(
                {
                    "format_version": 1,
                    "candidate_rebuild": {
                        "sources": {"wikidata": {"refresh_days": 60}}
                    }
                }
            ),
            encoding="utf-8",
        )
        self.marker = self.root / "state" / "wikidata.json"

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def run_script(self, *arguments: object) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, *map(str, arguments)],
            check=False,
            capture_output=True,
            text=True,
            timeout=10,
        )

    def test_bulk_plan_accepts_only_official_dump_url(self) -> None:
        output = self.root / "plan.json"
        valid = self.run_script(
            PLAN,
            "--url",
            "https://dumps.wikimedia.org/wikidatawiki/entities/latest-all.json.bz2",
            "--plan-id",
            "wikidata-refresh-20260720",
            "--created-at",
            "2026-07-20T03:00:00Z",
            "--output",
            output,
        )
        self.assertEqual(valid.returncode, 0, valid.stderr)
        plan = json.loads(output.read_text(encoding="utf-8"))
        self.assertEqual(plan["contract"], "fetch_plan_v1")
        self.assertEqual(plan["source"], "wikidata")
        self.assertNotIn("entities", plan["requests"][0])

        rejected = self.run_script(
            PLAN,
            "--url",
            "https://example.org/easier-point-api",
            "--plan-id",
            "bad-plan",
            "--created-at",
            "2026-07-20T03:00:00Z",
            "--output",
            self.root / "bad.json",
        )
        self.assertNotEqual(rejected.returncode, 0)

    def test_configured_cadence_and_success_marker(self) -> None:
        never = self.run_script(
            GATE,
            "gate",
            "--config",
            self.config,
            "--marker",
            self.marker,
            "--now",
            "2026-07-20T03:00:00Z",
        )
        self.assertEqual(never.returncode, 0, never.stderr)
        self.assertTrue(json.loads(never.stdout)["due"])

        recorded = self.run_script(
            GATE,
            "record",
            "--marker",
            self.marker,
            "--snapshot-id",
            "wikidata-20260720",
            "--source-sha256",
            "a" * 64,
            "--completed-at",
            "2026-07-20T03:00:00Z",
        )
        self.assertEqual(recorded.returncode, 0, recorded.stderr)

        fresh = self.run_script(
            GATE,
            "gate",
            "--config",
            self.config,
            "--marker",
            self.marker,
            "--now",
            "2026-08-01T03:00:00Z",
        )
        self.assertEqual(fresh.returncode, 0, fresh.stderr)
        self.assertFalse(json.loads(fresh.stdout)["due"])

        due = self.run_script(
            GATE,
            "gate",
            "--config",
            self.config,
            "--marker",
            self.marker,
            "--now",
            "2026-09-20T03:00:00Z",
        )
        self.assertEqual(due.returncode, 0, due.stderr)
        self.assertTrue(json.loads(due.stdout)["due"])


if __name__ == "__main__":
    unittest.main()
