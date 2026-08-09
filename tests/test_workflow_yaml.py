from __future__ import annotations

import shutil
import subprocess
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
WORKFLOWS = ROOT / ".github" / "workflows"


class WorkflowYamlTests(unittest.TestCase):
    def test_all_workflows_parse_as_yaml(self) -> None:
        yamllint = shutil.which("yamllint")
        self.assertIsNotNone(
            yamllint,
            "yamllint is required for workflow syntax validation",
        )
        files = sorted(WORKFLOWS.glob("*.yml"))
        result = subprocess.run(
            [
                str(yamllint),
                "-d",
                "{extends: relaxed, rules: {line-length: disable, truthy: disable}}",
                *map(str, files),
            ],
            check=False,
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_upload_artifact_inputs_are_mapping_entries(self) -> None:
        allowed = {
            "name",
            "path",
            "if-no-files-found",
            "retention-days",
            "compression-level",
            "overwrite",
            "include-hidden-files",
        }
        for path in sorted(WORKFLOWS.glob("*.yml")):
            lines = path.read_text(encoding="utf-8").splitlines()
            for index, line in enumerate(lines):
                if "uses: actions/upload-artifact@" not in line:
                    continue
                step_indent = len(line) - len(line.lstrip())
                with_index = next(
                    (
                        candidate
                        for candidate in range(index + 1, len(lines))
                        if lines[candidate].strip() == "with:"
                    ),
                    None,
                )
                self.assertIsNotNone(with_index, f"{path}:{index + 1}: missing with")
                key_indent = (
                    len(lines[with_index]) - len(lines[with_index].lstrip()) + 2
                )
                for candidate in range(with_index + 1, len(lines)):
                    value = lines[candidate]
                    stripped = value.strip()
                    if not stripped or stripped.startswith("#"):
                        continue
                    indentation = len(value) - len(value.lstrip())
                    if indentation <= step_indent:
                        break
                    if indentation != key_indent:
                        continue
                    self.assertIn(
                        ":",
                        stripped,
                        f"{path}:{candidate + 1}: non-mapping token in action inputs",
                    )
                    key = stripped.split(":", 1)[0]
                    self.assertIn(
                        key,
                        allowed,
                        f"{path}:{candidate + 1}: unsupported upload-artifact input",
                    )

    def test_internal_queue_is_not_guarded_as_immutable(self) -> None:
        operational = (
            "intake.yml",
            "product-integration.yml",
            "candidate-rebuild.yml",
            "publication.yml",
        )
        for name in operational:
            text = (WORKFLOWS / name).read_text(encoding="utf-8")
            self.assertNotIn("inbox_manifest.py", text, name)
            self.assertNotIn("ARACHNE_INBOX", text, name)

    def test_product_workflow_uses_fixed_inbox_commands_in_order(self) -> None:
        text = (WORKFLOWS / "product-integration.yml").read_text(encoding="utf-8")
        product_tasks = "build/arachne product check-inbox apply-inbox"
        hint_tasks = (
            "build/arachne product rebuild-merge-hints export-merge-hints"
        )
        self.assertIn(product_tasks, text)
        self.assertIn(hint_tasks, text)
        self.assertLess(text.index(product_tasks), text.index(hint_tasks))
        for obsolete in (
            "product-integrate",
            "import-normalized",
            "cleanup_merged_inbox.py",
            "compact_merge_hints.py",
            "--minimum-score",
            "--per-type",
            "--per-entity",
            "--database",
            "--manifest",
            "--apply",
        ):
            self.assertNotIn(obsolete, text)

    def test_issue_intake_proposes_only_a_validated_strict_batch(self) -> None:
        text = (WORKFLOWS / "intake.yml").read_text(encoding="utf-8")
        materialize = "scripts/process_issue_batches.py"
        check = "build/arachne product check-inbox"
        propose = "scripts/propose_state_change.py"
        self.assertIn(materialize, text)
        self.assertIn(check, text)
        self.assertIn(propose, text)
        self.assertLess(text.index(materialize), text.index(check))
        self.assertLess(text.index(check), text.index(propose))
        self.assertNotIn("build/arachne product apply-inbox", text)
        self.assertNotIn("dispatch_intake_request.py", text)

    def test_publication_stages_optional_snapshot_bound_image_hints(self) -> None:
        text = (WORKFLOWS / "publication.yml").read_text(encoding="utf-8")
        resolve_product = "PRODUCT_SNAPSHOT_PATH="
        default_hints = 'image_hints="derived/wikidata-image-hints.json"'
        default_probe = (
            '[[ ! -f "${GITHUB_WORKSPACE}/.arachne-state/${image_hints}" ]]'
        )
        stage = "viewer/scripts/stage_image_hints.py"
        asset_build = "npm run build:assets"
        site_build = "viewer-build"
        self.assertIn("image_hints_artifact:", text)
        self.assertIn('image_hints="${IMAGE_HINTS_INPUT}"', text)
        self.assertIn(default_hints, text)
        self.assertIn(default_probe, text)
        self.assertIn("--state-root", text)
        self.assertIn("--product-snapshot-control", text)
        self.assertIn("viewer/public/data/wikidata-image-hints.json", text)
        self.assertLess(text.index(default_hints), text.index(stage))
        self.assertLess(text.index(resolve_product), text.index(stage))
        self.assertLess(text.index(stage), text.index(asset_build))
        self.assertLess(text.index(asset_build), text.index(site_build))
        self.assertNotIn("npm run build\n", text)

    def test_source_refresh_stages_image_hints_in_reviewed_state(self) -> None:
        text = (WORKFLOWS / "source-refresh.yml").read_text(encoding="utf-8")
        generated = '"${IMAGE_HINTS}"'
        reviewed = (
            '"${GITHUB_WORKSPACE}/.arachne-state/derived/'
            'wikidata-image-hints.json"'
        )
        stager = "viewer/scripts/stage_image_hints.py"
        cleanup = 'rm -f "${IMAGE_HINTS}"'
        proposal = "scripts/propose_state_change.py"

        self.assertIn(stager, text)
        self.assertIn(generated, text)
        self.assertIn(reviewed, text)
        self.assertIn('--product-snapshot-control "${PRODUCT_CONTROL}"', text)
        self.assertLess(text.index(reviewed), text.index(cleanup))
        self.assertLess(text.index(stager), text.index(cleanup))
        self.assertLess(text.index(stager), text.index(proposal))

    def test_source_refresh_uses_direct_grouped_operations_commands(self) -> None:
        text = (WORKFLOWS / "source-refresh.yml").read_text(encoding="utf-8")
        fetch_plan = '"${GITHUB_WORKSPACE}/build/arachne" fetch plan'
        fetch = '"${GITHUB_WORKSPACE}/build/arachne" fetch'
        worker = "hpc/wikidata/build_external_graph.py"
        candidate_plan = '"${GITHUB_WORKSPACE}/build/arachne" candidate plan'
        candidate_rebuild = (
            '"${GITHUB_WORKSPACE}/build/arachne" candidate rebuild'
        )

        for command in (
            fetch_plan,
            fetch,
            worker,
            candidate_plan,
            candidate_rebuild,
        ):
            self.assertIn(command, text)
        fetch_plan_index = text.index(fetch_plan)
        fetch_index = text.index(fetch, fetch_plan_index + len(fetch_plan))
        self.assertLess(fetch_plan_index, fetch_index)
        self.assertLess(fetch_index, text.index(worker))
        self.assertLess(text.index(worker), text.index(candidate_plan))
        self.assertLess(text.index(candidate_plan), text.index(candidate_rebuild))
        self.assertIn('--output-directory "${RESULT_DIRECTORY}"', text)
        self.assertIn("wikidata-results/wikidata-image-hints.json", text)
        self.assertNotIn("scripts/arachne_ops.py", text)
        for redundant in (
            "--artifact-store",
            "--graph-store",
            "--candidate-policy-config",
            "--image-hints-output",
        ):
            self.assertNotIn(redundant, text)

    def test_candidate_workflow_uses_direct_grouped_commands(self) -> None:
        text = (WORKFLOWS / "candidate-rebuild.yml").read_text(encoding="utf-8")
        plan = '"${GITHUB_WORKSPACE}/build/arachne" candidate plan'
        rebuild = '"${GITHUB_WORKSPACE}/build/arachne" candidate rebuild'
        self.assertIn(plan, text)
        self.assertIn(rebuild, text)
        self.assertLess(text.index(plan), text.index(rebuild))
        self.assertNotIn("scripts/arachne_ops.py", text)


if __name__ == "__main__":
    unittest.main()
