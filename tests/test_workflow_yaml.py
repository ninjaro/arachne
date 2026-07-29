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
        check = "build/arachne product check-inbox"
        apply = "build/arachne product apply-inbox"
        self.assertIn(check, text)
        self.assertIn(apply, text)
        self.assertLess(text.index(check), text.index(apply))
        for obsolete in (
            "product-integrate",
            "import-normalized",
            "cleanup_merged_inbox.py",
            "--database",
            "--manifest",
            "--apply",
        ):
            self.assertNotIn(obsolete, text)

    def test_issue_intake_proposes_only_a_validated_strict_batch(self) -> None:
        text = (WORKFLOWS / "intake.yml").read_text(encoding="utf-8")
        materialize = "scripts/materialize_product_batch.py"
        check = "build/arachne product check-inbox"
        propose = "scripts/propose_state_change.py"
        self.assertIn(materialize, text)
        self.assertIn(check, text)
        self.assertIn(propose, text)
        self.assertLess(text.index(materialize), text.index(check))
        self.assertLess(text.index(check), text.index(propose))
        self.assertNotIn("build/arachne product apply-inbox", text)
        self.assertNotIn("dispatch_intake_request.py", text)


if __name__ == "__main__":
    unittest.main()
