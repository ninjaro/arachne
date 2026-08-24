from __future__ import annotations

import shutil
import subprocess
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
WORKFLOWS = ROOT / ".github" / "workflows"
WRITERS = (
    "product-integration.yml",
    "candidate-rebuild.yml",
    "source-refresh.yml",
    "manual-dispatch.yml",
)


class WorkflowYamlTests(unittest.TestCase):
    def test_all_workflows_parse_as_yaml(self) -> None:
        yamllint = shutil.which("yamllint")
        self.assertIsNotNone(yamllint, "yamllint is required")
        result = subprocess.run(
            [
                str(yamllint),
                "-d",
                "{extends: relaxed, rules: {line-length: disable, truthy: disable}}",
                *map(str, sorted(WORKFLOWS.glob("*.yml"))),
            ],
            check=False,
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_no_legacy_pages_or_viewer_workflow_remains(self) -> None:
        self.assertFalse((WORKFLOWS / "publication.yml").exists())
        text = "\n".join(
            path.read_text(encoding="utf-8")
            for path in sorted(WORKFLOWS.glob("*.yml"))
        )
        for obsolete in (
            "deploy-pages",
            "upload-pages-artifact",
            "viewer-build",
            "viewer/",
            ".arachne-state",
            "ARACHNE_DATA_WRITER_TOKEN",
        ):
            self.assertNotIn(obsolete, text)

    def test_canonical_writers_share_one_serialization_group(self) -> None:
        for name in WRITERS:
            text = (WORKFLOWS / name).read_text(encoding="utf-8")
            self.assertIn("group: arachne-data-write", text, name)
            self.assertIn("cancel-in-progress: false", text, name)

    def test_product_writer_rejects_stale_state_without_generated_mirrors(self) -> None:
        text = (WORKFLOWS / "product-integration.yml").read_text(encoding="utf-8")
        self.assertIn("scripts/state_manifest.py check", text)
        self.assertIn("scripts/state_manifest.py refresh", text)
        self.assertIn("scripts/publish_state_repository.py", text)
        self.assertIn('--expected-base "${ARACHNE_STATE_BASE}"', text)
        self.assertIn("--allow database", text)
        self.assertIn("--allow state-manifest.json", text)
        for generated in (
            "--allow graphs/product",
            "--allow derived",
            "derived/research.json",
            "derived/taste-index.json",
        ):
            self.assertNotIn(generated, text)

    def test_transient_product_snapshot_is_exact_and_not_proposed(self) -> None:
        for name in ("candidate-rebuild.yml", "source-refresh.yml"):
            text = (WORKFLOWS / name).read_text(encoding="utf-8")
            self.assertIn("scripts/materialize_local_product_snapshot.py", text)
            self.assertIn("${RUNNER_TEMP}/product-graph", text)
            self.assertIn("--product-snapshot", text)
            self.assertNotIn("graphs/product/active.json", text)
        source = (WORKFLOWS / "source-refresh.yml").read_text(encoding="utf-8")
        self.assertIn("scripts/stage_image_hints.py", source)
        self.assertIn(".arachne/ci-state/derived/wikidata-image-hints.json", source)

    def test_issue_intake_uses_read_only_state_credential(self) -> None:
        text = (WORKFLOWS / "intake.yml").read_text(encoding="utf-8")
        self.assertIn("ARACHNE_DATA_READ_TOKEN", text)
        self.assertIn("persist-credentials: false", text)
        self.assertNotIn("ARACHNE_DATA_WRITER_APP", text)
        self.assertIn("build/arachne product check-inbox", text)
        self.assertNotIn("build/arachne product apply-inbox", text)

    def test_writer_identity_is_a_scoped_github_app(self) -> None:
        for name in WRITERS:
            text = (WORKFLOWS / name).read_text(encoding="utf-8")
            self.assertIn("actions/create-github-app-token@", text, name)
            self.assertIn("ARACHNE_DATA_WRITER_APP_ID", text, name)
            self.assertIn("ARACHNE_DATA_WRITER_APP_PRIVATE_KEY", text, name)
            self.assertIn("repositories: arachne-data", text, name)


if __name__ == "__main__":
    unittest.main()
