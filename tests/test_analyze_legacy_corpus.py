from __future__ import annotations

import hashlib
import json
import os
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest import mock

from scripts.analyze_legacy_corpus import CorpusError, analyze, write_report


class AnalyzeLegacyCorpusTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.legacy = self.root / "legacy"
        self.legacy.mkdir()

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def test_inventories_hashes_and_observed_key_types(self) -> None:
        payload = b'{"works":[],"review_note":"later"}'
        (self.legacy / "batch.json").write_bytes(payload)

        report = analyze(self.legacy)

        self.assertTrue(report["observations_only"])
        self.assertFalse(report["manifest_inferred"])
        self.assertEqual(report["summary"]["file_count"], 1)
        self.assertEqual(report["files"][0]["sha256"], hashlib.sha256(payload).hexdigest())
        self.assertEqual(report["top_level_key_types"]["works"], {"array": 1})
        self.assertEqual(report["top_level_key_types"]["review_note"], {"string": 1})

    def test_observes_safe_zip_json_without_extracting(self) -> None:
        archive_path = self.legacy / "batches.zip"
        with zipfile.ZipFile(archive_path, "w") as archive:
            archive.writestr("nested/batch.json", '{"evidence":[]}')

        report = analyze(self.legacy)
        member = report["files"][0]["members"][0]

        self.assertTrue(member["safe"])
        self.assertEqual(member["json_status"], "parsed")
        self.assertFalse((self.legacy / "nested").exists())

    def test_reports_unsafe_zip_member_without_reading_it(self) -> None:
        archive_path = self.legacy / "unsafe.zip"
        with zipfile.ZipFile(archive_path, "w") as archive:
            archive.writestr("../escape.json", "{}")

        report = analyze(self.legacy)

        self.assertFalse(report["files"][0]["members"][0]["safe"])
        self.assertEqual(report["summary"]["problem_count"], 1)
        self.assertFalse((self.root / "escape.json").exists())

    def test_refuses_to_write_report_inside_legacy_inbox(self) -> None:
        (self.legacy / "batch.json").write_text("{}", encoding="utf-8")
        report = analyze(self.legacy)

        with self.assertRaises(CorpusError):
            write_report(report, self.legacy / "analysis.json")

        output = write_report(report, self.root / "analysis.json")
        self.assertEqual(json.loads(output.read_text())["report_type"],
                         "legacy_corpus_observation")

    def test_conventional_legacy_inbox_is_always_protected(self) -> None:
        (self.legacy / "batch.json").write_text("{}", encoding="utf-8")
        report = analyze(self.legacy)
        fake_home = self.root / "home"
        conventional = fake_home / "Projects" / "new" / "art-lineages" / "inbox"
        conventional.mkdir(parents=True)

        with mock.patch.dict(os.environ, {"HOME": str(fake_home)}):
            with self.assertRaises(CorpusError):
                write_report(report, conventional / "analysis.json")


if __name__ == "__main__":
    unittest.main()
