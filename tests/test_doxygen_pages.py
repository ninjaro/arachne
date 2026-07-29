from __future__ import annotations

from pathlib import Path
import tempfile
import unittest

from scripts.patch_doxygen_pages import PatchError, patch_site


class PatchDoxygenPagesTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.site = Path(self.temporary.name)
        (self.site / "viewer").mkdir()
        (self.site / "viewer" / "index.html").write_text(
            "viewer", encoding="utf-8"
        )
        (self.site / "cov").mkdir()
        (self.site / "cov" / "index.html").write_text(
            "coverage", encoding="utf-8"
        )
        (self.site / "d1" / "d2").mkdir(parents=True)
        (self.site / "d3" / "d4").mkdir(parents=True)
        (self.site / "d1" / "d2" / "viewer.html").write_text(
            "old viewer page", encoding="utf-8"
        )
        (self.site / "d3" / "d4" / "coverage.html").write_text(
            "old coverage frame", encoding="utf-8"
        )
        (self.site / "pages.html").write_text(
            """
            <html><body>
              <a class="el" href="d1/d2/viewer.html">Arachne Viewer</a>
              <a class="el" href="d3/d4/coverage.html">
                <span>Code Coverage Report</span>
              </a>
            </body></html>
            """,
            encoding="utf-8",
        )

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def test_rewrites_index_and_preserves_old_urls_as_redirects(self) -> None:
        redirects = patch_site(self.site)

        pages = (self.site / "pages.html").read_text(encoding="utf-8")
        self.assertIn('href="viewer/"', pages)
        self.assertIn('href="cov/"', pages)
        self.assertEqual(pages.count('target="_top"'), 2)

        viewer = (self.site / redirects["Arachne Viewer"]).read_text(
            encoding="utf-8"
        )
        coverage = (self.site / redirects["Code Coverage Report"]).read_text(
            encoding="utf-8"
        )
        self.assertIn("../../viewer/", viewer)
        self.assertIn("../../cov/", coverage)
        self.assertNotIn("iframe", coverage)
        self.assertIn("window.top.location.replace", coverage)

    def test_rejects_missing_standalone_destination(self) -> None:
        (self.site / "cov" / "index.html").unlink()
        with self.assertRaises(PatchError):
            patch_site(self.site)


if __name__ == "__main__":
    unittest.main()
