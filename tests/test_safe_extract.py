from __future__ import annotations

import io
import os
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest import mock

from scripts.safe_extract import Limits, UnsafeArchive, extract


class SafeExtractTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def archive(self, entries: dict[str, bytes]) -> Path:
        path = self.root / "input.zip"
        with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as archive:
            for name, content in entries.items():
                archive.writestr(name, content)
        return path

    def test_extracts_regular_files_to_new_directory(self) -> None:
        archive = self.archive({"batch/data.json": b'{"format_version":1}'})
        output = self.root / "output"
        extract(archive, output, Limits())
        self.assertEqual((output / "batch/data.json").read_bytes(), b'{"format_version":1}')

    def test_rejects_parent_traversal_without_partial_output(self) -> None:
        archive = self.archive({"../escape.json": b"bad"})
        output = self.root / "output"
        with self.assertRaises(UnsafeArchive):
            extract(archive, output, Limits())
        self.assertFalse(output.exists())
        self.assertFalse((self.root.parent / "escape.json").exists())

    def test_rejects_absolute_and_backslash_paths(self) -> None:
        for name in ("/absolute.json", "..\\escape.json", "C:/drive.json"):
            archive = self.archive({name: b"bad"})
            with self.assertRaises(UnsafeArchive, msg=name):
                extract(archive, self.root / f"out-{len(name)}", Limits())

    def test_rejects_declared_resource_limits(self) -> None:
        archive = self.archive({"large.bin": b"x" * 32})
        with self.assertRaises(UnsafeArchive):
            extract(archive, self.root / "output", Limits(max_file_bytes=16))

    def test_never_extracts_into_external_legacy_inbox(self) -> None:
        archive = self.archive({"batch.json": b"{}"})
        inbox = self.root / "legacy-inbox"
        inbox.mkdir()
        with self.assertRaises(UnsafeArchive):
            extract(archive, inbox / "expanded", Limits(), inbox)

    def test_conventional_legacy_inbox_is_protected_without_an_argument(self) -> None:
        archive = self.archive({"batch.json": b"{}"})
        fake_home = self.root / "home"
        inbox = fake_home / "Projects" / "new" / "art-lineages" / "inbox"
        inbox.mkdir(parents=True)
        with mock.patch.dict(os.environ, {"HOME": str(fake_home)}):
            with self.assertRaises(UnsafeArchive):
                extract(archive, inbox / "expanded", Limits())

    def test_rejects_symlink_entries(self) -> None:
        path = self.root / "input.zip"
        info = zipfile.ZipInfo("link")
        info.create_system = 3
        info.external_attr = 0o120777 << 16
        with zipfile.ZipFile(path, "w") as archive:
            archive.writestr(info, "target")
        with self.assertRaises(UnsafeArchive):
            extract(path, self.root / "output", Limits())


if __name__ == "__main__":
    unittest.main()
