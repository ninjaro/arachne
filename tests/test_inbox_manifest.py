from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class InboxManifestTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.observed = self.root / "observed"
        self.observed.mkdir()
        (self.observed / "batch.json").write_text("{}", encoding="utf-8")

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def test_conventional_legacy_output_is_detected_when_observing_elsewhere(self) -> None:
        fake_home = self.root / "home"
        conventional = fake_home / "Projects" / "new" / "art-lineages" / "inbox"
        conventional.mkdir(parents=True)
        output = conventional / "manifest.json"

        environment = os.environ.copy()
        environment["HOME"] = str(fake_home)
        result = subprocess.run(
            [
                sys.executable,
                str(ROOT / "scripts" / "inbox_manifest.py"),
                "snapshot",
                "--legacy-inbox",
                str(self.observed),
                "--manifest",
                str(output),
            ],
            check=False,
            capture_output=True,
            text=True,
            env=environment,
        )

        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(list(conventional.iterdir()), [])


if __name__ == "__main__":
    unittest.main()
