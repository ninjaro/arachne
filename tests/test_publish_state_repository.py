from __future__ import annotations

import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts" / "publish_state_repository.py"


def git(root: Path, *arguments: str) -> str:
    result = subprocess.run(
        ["git", *arguments],
        cwd=root,
        check=True,
        capture_output=True,
        text=True,
    )
    return result.stdout.strip()


class StatePublisherTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="arachne-publisher-")
        self.root = Path(self.temporary.name)
        self.remote = self.root / "remote.git"
        self.seed = self.root / "seed"
        self.writer = self.root / "writer"
        git(self.root, "init", "--bare", "--initial-branch=main", str(self.remote))
        git(self.root, "init", "--initial-branch=main", str(self.seed))
        git(self.seed, "config", "user.name", "Test")
        git(self.seed, "config", "user.email", "test@example.invalid")
        (self.seed / "database").mkdir()
        (self.seed / "database" / "state.txt").write_text("one\n", encoding="utf-8")
        git(self.seed, "add", ".")
        git(self.seed, "commit", "-m", "seed")
        git(self.seed, "remote", "add", "origin", str(self.remote))
        git(self.seed, "push", "-u", "origin", "main")
        git(self.root, "clone", str(self.remote), str(self.writer))
        self.base = git(self.writer, "rev-parse", "HEAD")

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def invoke(self, expected: str | None = None) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [
                "python3",
                str(SCRIPT),
                "--state-root",
                str(self.writer),
                "--expected-base",
                expected or self.base,
                "--title",
                "update state",
                "--allow",
                "database",
            ],
            check=False,
            capture_output=True,
            text=True,
        )

    def test_publishes_allowed_state_from_exact_base(self) -> None:
        (self.writer / "database" / "state.txt").write_text("two\n", encoding="utf-8")
        result = self.invoke()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(
            git(self.remote, "show", "main:database/state.txt"),
            "two",
        )

    def test_rejects_unexpected_path(self) -> None:
        (self.writer / "outside.txt").write_text("no\n", encoding="utf-8")
        result = self.invoke()
        self.assertEqual(result.returncode, 2)
        self.assertIn("unexpected state change", result.stderr)

    def test_rejects_remote_advance_instead_of_rebasing(self) -> None:
        (self.writer / "database" / "state.txt").write_text("writer\n", encoding="utf-8")
        (self.seed / "database" / "state.txt").write_text("newer\n", encoding="utf-8")
        git(self.seed, "add", "database/state.txt")
        git(self.seed, "commit", "-m", "newer state")
        git(self.seed, "push", "origin", "main")

        result = self.invoke()

        self.assertEqual(result.returncode, 2)
        self.assertIn("stale state write rejected", result.stderr)
        self.assertEqual(git(self.remote, "show", "main:database/state.txt"), "newer")


if __name__ == "__main__":
    unittest.main()
