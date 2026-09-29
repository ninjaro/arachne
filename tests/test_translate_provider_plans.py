from __future__ import annotations

import json
import stat
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from scripts.translate_provider_plans import PlanTranslationError, translate


ROOT = Path(__file__).resolve().parents[1]

# A stand-in for the operations CLI: it advertises the translation capability
# and writes the controls `arachne fetch plan` would write, so the driver can
# be tested without building the binary.
STUB = '''#!/usr/bin/env python3
import json
import sys
from pathlib import Path

arguments = sys.argv[1:]
if arguments == ["--capabilities-json"]:
    print(json.dumps({"format_version": 1, "commands": ["fetch-plan-translate"]}))
    raise SystemExit(0)
options = dict(zip(arguments[2::2], arguments[3::2]))
plan = json.loads(Path(options["--plan"]).read_text(encoding="utf-8"))
if plan["source"] == "musicbrainz":
    print("MusicBrainz bulk fetch must name a dated official core JSON dump", file=sys.stderr)
    raise SystemExit(3)
directory = Path(options["--output-directory"])
directory.mkdir(parents=True)
for request in plan["requests"]:
    document = {
        "contract": "fetch_request_v1",
        "format_version": 1,
        "request_id": request["request_id"],
        "door_id": plan["source"],
        "endpoint_id": "official-data-dumps",
        "operation": "bulk_snapshot",
        "plan_id": plan["plan_id"],
        "locator": request["locator"],
        "method": "GET",
        "output_ref": "bulk/" + plan["plan_id"] + "/" + request["request_id"],
    }
    (directory / (request["request_id"] + ".json")).write_text(
        json.dumps(document), encoding="utf-8"
    )
'''


def plan_document(provider: str, requests: int = 1) -> dict[str, object]:
    return {
        "contract": "fetch_plan_v1",
        "format_version": 1,
        "plan_id": f"run-1-{provider}",
        "source": provider,
        "requests": [
            {
                "request_id": f"{provider}-file-{index}",
                "locator": f"https://example.invalid/{provider}/{index}",
                "purpose": "official dump",
                "follow_up": False,
            }
            for index in range(requests)
        ],
        "created_at": "2026-09-28T00:00:00Z",
    }


class TranslateProviderPlansTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="arachne-plan-translate-")
        self.root = Path(self.temporary.name)
        self.plans = self.root / "plans"
        self.plans.mkdir()
        self.config = self.root / "arachne.json"
        self.config.write_text("{}", encoding="utf-8")
        self.binary = self.root / "arachne-stub"
        self.binary.write_text(STUB.replace("#!/usr/bin/env python3", f"#!{sys.executable}"),
                               encoding="utf-8")
        self.binary.chmod(self.binary.stat().st_mode | stat.S_IXUSR)
        self.write_plans()

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def write_plans(self, statuses: dict[str, str] | None = None) -> None:
        statuses = statuses or {
            "imdb": "planned",
            "musicbrainz": "planned",
            "open-library": "skipped",
            "discogs": "unavailable",
        }
        providers = []
        for provider, status in statuses.items():
            entry: dict[str, object] = {"provider": provider, "status": status}
            if status == "planned":
                entry["plan_ref"] = f"{provider}-fetch-plan.json"
                (self.plans / f"{provider}-fetch-plan.json").write_text(
                    json.dumps(plan_document(provider, 2)), encoding="utf-8"
                )
            else:
                entry["reason"] = "provider is not configured"
            providers.append(entry)
        (self.plans / "optional-bulk-provider-plans.json").write_text(
            json.dumps(
                {
                    "command": "optional-bulk-provider-plans",
                    "format_version": 1,
                    "run_id": "run-1",
                    "created_at": "2026-09-28T00:00:00Z",
                    "providers": providers,
                }
            ),
            encoding="utf-8",
        )

    def test_every_planned_provider_is_translated_and_failure_is_not_fatal(self) -> None:
        output = self.root / "controls"
        report = translate(self.plans, self.config, output, self.binary)
        statuses = {item["provider"]: item["status"] for item in report["providers"]}
        self.assertEqual(
            statuses,
            {
                "imdb": "translated",
                "musicbrainz": "failed",
                "open-library": "skipped",
                "discogs": "unavailable",
            },
        )
        self.assertEqual((report["translated_providers"], report["failed_providers"]), (1, 1))
        imdb = next(item for item in report["providers"] if item["provider"] == "imdb")
        self.assertEqual(
            [request["request_id"] for request in imdb["requests"]],
            ["imdb-file-0", "imdb-file-1"],
        )
        self.assertEqual({request["door_id"] for request in imdb["requests"]}, {"imdb"})
        self.assertEqual(
            {request["operation"] for request in imdb["requests"]}, {"bulk_snapshot"}
        )
        failed = next(
            item for item in report["providers"] if item["provider"] == "musicbrainz"
        )
        self.assertIn("dated official core JSON dump", failed["reason"])
        self.assertTrue((output / "imdb" / "imdb-file-0.json").is_file())
        self.assertFalse((output / "musicbrainz").exists())

    def test_a_plan_that_does_not_match_its_provider_never_reaches_transport(self) -> None:
        (self.plans / "imdb-fetch-plan.json").write_text(
            json.dumps(plan_document("discogs")), encoding="utf-8"
        )
        report = translate(self.plans, self.config, self.root / "controls", self.binary)
        imdb = next(item for item in report["providers"] if item["provider"] == "imdb")
        self.assertEqual(imdb["status"], "failed")
        self.assertIn("names another source", imdb["reason"])

    def test_a_malformed_plan_report_fails_before_any_translation(self) -> None:
        (self.plans / "optional-bulk-provider-plans.json").write_text(
            json.dumps({"command": "other", "format_version": 1, "providers": []}),
            encoding="utf-8",
        )
        with self.assertRaises(PlanTranslationError):
            translate(self.plans, self.config, self.root / "controls", self.binary)

    def test_cli_writes_one_report_and_refuses_to_overwrite_it(self) -> None:
        report_path = self.root / "translation.json"
        arguments = [
            sys.executable,
            str(ROOT / "scripts/translate_provider_plans.py"),
            "--plans", str(self.plans),
            "--config", str(self.config),
            "--output-directory", str(self.root / "cli-controls"),
            "--report", str(report_path),
            "--binary", str(self.binary),
        ]
        first = subprocess.run(arguments, cwd=ROOT, text=True, capture_output=True, check=False)
        self.assertEqual(first.returncode, 0, first.stderr)
        self.assertEqual(json.loads(first.stdout)["translated_providers"], 1)
        self.assertEqual(
            json.loads(report_path.read_text(encoding="utf-8"))["command"],
            "translate-provider-plans",
        )
        second = subprocess.run(arguments, cwd=ROOT, text=True, capture_output=True, check=False)
        self.assertEqual(second.returncode, 2)
        self.assertIn("report already exists", second.stderr)


if __name__ == "__main__":
    unittest.main()
