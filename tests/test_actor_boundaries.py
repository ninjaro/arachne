from __future__ import annotations

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class ActorBoundaryTests(unittest.TestCase):
    def text(self, relative: str) -> str:
        return (ROOT / relative).read_text(encoding="utf-8")

    def test_pheidippides_transport_has_no_graph_or_algorithm_dependency(self) -> None:
        source = self.text("src/pheidippides/transport.cpp")
        header = self.text("include/pheidippides/transport.hpp")
        combined = source + header
        for forbidden in ("sqlite3", "penelope/", "ariadne/", "coordinator.hpp"):
            self.assertNotIn(forbidden, combined)
        self.assertNotRegex(source, r"nlohmann::json::parse|\.parse\(")

    def test_ariadne_has_no_writable_store_dependency(self) -> None:
        combined = "\n".join(
            path.read_text(encoding="utf-8")
            for path in sorted((ROOT / "src/ariadne").glob("*.cpp"))
        )
        self.assertNotIn("sqlite3", combined)
        self.assertNotIn("penelope/", combined)
        self.assertNotIn("pheidippides/", combined)

    def test_penelope_has_no_transport_or_algorithm_dependency(self) -> None:
        combined = "\n".join(
            path.read_text(encoding="utf-8")
            for path in sorted((ROOT / "src/penelope").glob("*.cpp"))
        )
        self.assertNotIn("pheidippides/", combined)
        self.assertNotIn("ariadne/", combined)
        self.assertNotIn("http_client", combined)

    def test_cmake_keeps_transport_and_graph_targets_separate(self) -> None:
        cmake = self.text("CMakeLists.txt")
        transport_block = re.search(
            r"add_library\(pheidippides_transport.*?target_compile_features\(pheidippides_transport.*?\)",
            cmake,
            re.DOTALL,
        )
        self.assertIsNotNone(transport_block)
        self.assertNotIn("SQLite", transport_block.group(0))
        self.assertNotIn("penelope_store", transport_block.group(0))

    def test_required_actor_and_contract_areas_exist(self) -> None:
        for relative in (
            "contracts/schemas",
            "contracts/examples",
            "src/arachne",
            "src/pheidippides",
            "src/ariadne",
            "src/penelope",
            "viewer",
        ):
            self.assertTrue((ROOT / relative).is_dir(), relative)


if __name__ == "__main__":
    unittest.main()
