#!/usr/bin/env python3
from __future__ import annotations

import argparse
from pathlib import Path


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected one match, found {count}")
    return text.replace(old, new, 1)


def patch_header(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    text = replace_once(
        text,
        "    [[nodiscard]] static nlohmann::ordered_json write_projection(\n",
        """    [[nodiscard]] static nlohmann::ordered_json catalog(
        const nlohmann::json& product_export,
        std::string product_snapshot_id
    );

    [[nodiscard]] static nlohmann::ordered_json write_projection(
""",
        "viewer.hpp catalog declaration",
    )
    text = replace_once(
        text,
        """    [[nodiscard]] static nlohmann::ordered_json build_site(
        const nlohmann::json& projection,
        const std::filesystem::path& template_root,
""",
        """    [[nodiscard]] static nlohmann::ordered_json build_site(
        const nlohmann::json& projection,
        const nlohmann::json& catalog_data,
        const std::filesystem::path& template_root,
""",
        "viewer.hpp build_site signature",
    )
    path.write_text(text, encoding="utf-8")


def patch_viewer_cpp(path: Path, templates: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if "#include <array>\n" not in text:
        text = replace_once(
            text,
            "#include <algorithm>\n",
            "#include <algorithm>\n#include <array>\n#include <utility>\n",
            "viewer.cpp standard includes",
        )
    catalog_method = (templates / "catalog_method.cpp.txt").read_text(
        encoding="utf-8"
    ).rstrip()
    build_site_method = (templates / "build_site_method.cpp.txt").read_text(
        encoding="utf-8"
    ).rstrip()

    marker = "nlohmann::ordered_json viewer_builder::write_projection(\n"
    if "viewer_builder::catalog(" not in text:
        text = replace_once(
            text,
            marker,
            catalog_method + "\n\n" + marker,
            "viewer.cpp catalog insertion",
        )

    start = text.index("nlohmann::ordered_json viewer_builder::build_site(\n")
    end = text.index("\n\n} // namespace arachne::ariadne", start)
    text = text[:start] + build_site_method + text[end:]
    path.write_text(text, encoding="utf-8")


def patch_main(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    projection = """    const ordered_json projection = arachne::ariadne::viewer_builder::project(
        product, candidate,
        product_snapshot.control.at("snapshot_id").get<std::string>(),
        candidate_id
    );
"""
    text = replace_once(
        text,
        projection,
        projection
        + """    const ordered_json catalog = arachne::ariadne::viewer_builder::catalog(
        product,
        product_snapshot.control.at("snapshot_id").get<std::string>()
    );
""",
        "main.cpp catalog creation",
    )
    text = replace_once(
        text,
        """        = arachne::ariadne::viewer_builder::build_site(
            projection, config.viewer_templates, config.site_output,
""",
        """        = arachne::ariadne::viewer_builder::build_site(
            projection, catalog, config.viewer_templates, config.site_output,
""",
        "main.cpp build_site call",
    )
    path.write_text(text, encoding="utf-8")


def patch_tests(path: Path, templates: Path) -> None:
    text = path.read_text(encoding="utf-8")
    marker = (
        "TEST(AriadneViewer, "
        "StaticBundleIsDeterministicAndIdentifiesSnapshots)"
    )
    start = text.index(marker)
    replacement = (templates / "static_bundle_test.cpp.txt").read_text(
        encoding="utf-8"
    ).rstrip()
    path.write_text(text[:start] + replacement + "\n", encoding="utf-8")


def patch_workflow(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    anchor = """      - name: Build Arachne operations binary
        env:
          ARACHNE_BUILD_TESTS: "OFF"
          ARACHNE_BUILD_LEGACY_CLIENT: "OFF"
        run: scripts/build.sh
"""
    text = replace_once(
        text,
        anchor,
        anchor
        + """      - uses: actions/setup-node@v4
        with:
          node-version: "22"
          cache: npm
          cache-dependency-path: viewer/package-lock.json
      - name: Build React viewer assets
        working-directory: viewer
        run: |
          npm ci
          npm run build
""",
        "publication workflow viewer build",
    )
    path.write_text(text, encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Patch Arachne's production viewer pipeline for the React UI."
    )
    parser.add_argument("repository", type=Path, nargs="?", default=Path("."))
    arguments = parser.parse_args()

    repository = arguments.repository.resolve(strict=True)
    templates = Path(__file__).resolve().parents[1] / "patches"

    patch_header(repository / "include/ariadne/viewer.hpp")
    patch_viewer_cpp(repository / "src/ariadne/viewer.cpp", templates)
    patch_main(repository / "src/main.cpp")
    patch_tests(repository / "tests/viewer_tests.cpp", templates)
    patch_workflow(repository / ".github/workflows/publication.yml")
    print("Applied viewer production integration patches.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
