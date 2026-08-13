#!/usr/bin/env python3
"""Turn Doxygen related-page entries into standalone Pages links."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from html import escape, unescape
import json
import os
from pathlib import Path
import re
import sys
from urllib.parse import unquote, urlsplit


ANCHOR_RE = re.compile(
    r"<a\b(?P<attrs>[^>]*)>(?P<body>.*?)</a>",
    re.IGNORECASE | re.DOTALL,
)
HREF_RE = re.compile(
    r"(?P<prefix>\bhref\s*=\s*)(?P<quote>['\"])(?P<value>.*?)(?P=quote)",
    re.IGNORECASE | re.DOTALL,
)
TAG_RE = re.compile(r"<[^>]+>")
TARGET_RE = re.compile(r"\btarget\s*=", re.IGNORECASE)


@dataclass(frozen=True)
class StandalonePage:
    title: str
    destination: str


STANDALONE_PAGES = (
    StandalonePage("Arachne Viewer", "viewer/"),
    StandalonePage("Code Coverage Report", "cov/"),
)


class PatchError(RuntimeError):
    """The generated Doxygen site does not match the expected structure."""


def anchor_text(body: str) -> str:
    return " ".join(unescape(TAG_RE.sub(" ", body)).split())


def local_target(site_root: Path, href: str) -> Path | None:
    parsed = urlsplit(href)
    if parsed.scheme or parsed.netloc or not parsed.path:
        return None
    candidate = (site_root / unquote(parsed.path)).resolve()
    root = site_root.resolve()
    try:
        candidate.relative_to(root)
    except ValueError as error:
        raise PatchError(f"Doxygen page link escapes the site root: {href}") from error
    return candidate


def redirect_document(title: str, destination: str) -> str:
    escaped_title = escape(title)
    escaped_destination = escape(destination, quote=True)
    javascript_destination = json.dumps(destination)
    return f"""<!doctype html>
<html lang="en">
  <head>
    <meta charset="utf-8">
    <meta name="viewport" content="width=device-width,initial-scale=1">
    <meta http-equiv="refresh" content="0; url={escaped_destination}">
    <title>{escaped_title}</title>
    <link rel="canonical" href="{escaped_destination}">
  </head>
  <body>
    <p><a href="{escaped_destination}" target="_top">Open {escaped_title}</a></p>
    <script>
      window.top.location.replace(new URL({javascript_destination}, window.location.href).href);
    </script>
  </body>
</html>
"""


def patch_site(site_root: Path) -> dict[str, str]:
    site_root = site_root.resolve(strict=True)
    pages_path = site_root / "pages.html"
    if not pages_path.is_file():
        raise PatchError(f"missing Doxygen related-pages index: {pages_path}")

    for page in STANDALONE_PAGES:
        destination = site_root / page.destination
        if not destination.is_dir() or not (destination / "index.html").is_file():
            raise PatchError(
                f"standalone destination for {page.title!r} is incomplete: "
                f"{destination}"
            )

    source = pages_path.read_text(encoding="utf-8")
    original_targets: dict[str, Path] = {}
    replacements: dict[str, int] = {page.title: 0 for page in STANDALONE_PAGES}
    pages_by_title = {page.title: page for page in STANDALONE_PAGES}

    def replace_anchor(match: re.Match[str]) -> str:
        title = anchor_text(match.group("body"))
        page = pages_by_title.get(title)
        if page is None:
            return match.group(0)

        href_match = HREF_RE.search(match.group("attrs"))
        if href_match is None:
            raise PatchError(f"related-page entry has no href: {title}")
        target = local_target(site_root, href_match.group("value"))
        if target is None or target.suffix.lower() != ".html":
            raise PatchError(
                f"related-page entry is not a generated local HTML page: {title}"
            )
        original_targets.setdefault(title, target)

        attrs = HREF_RE.sub(
            lambda value: (
                f"{value.group('prefix')}{value.group('quote')}"
                f"{page.destination}{value.group('quote')}"
            ),
            match.group("attrs"),
            count=1,
        )
        if not TARGET_RE.search(attrs):
            attrs += ' target="_top"'
        replacements[title] += 1
        return f"<a{attrs}>{match.group('body')}</a>"

    patched = ANCHOR_RE.sub(replace_anchor, source)
    missing = [title for title, count in replacements.items() if count == 0]
    if missing:
        raise PatchError(f"missing related-page entries: {', '.join(missing)}")
    pages_path.write_text(patched, encoding="utf-8")

    redirect_paths: dict[str, str] = {}
    for page in STANDALONE_PAGES:
        target = original_targets[page.title]
        if not target.is_file():
            raise PatchError(f"missing generated page for {page.title!r}: {target}")
        relative = os.path.relpath(site_root / page.destination, target.parent)
        relative = relative.replace(os.sep, "/").rstrip("/") + "/"
        target.write_text(
            redirect_document(page.title, relative),
            encoding="utf-8",
        )
        redirect_paths[page.title] = str(target.relative_to(site_root))

    return redirect_paths


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("site_root", type=Path)
    arguments = parser.parse_args()
    try:
        redirects = patch_site(arguments.site_root)
    except (PatchError, OSError, UnicodeError) as error:
        print(f"patch_doxygen_pages: {error}", file=sys.stderr)
        return 2
    for title, path in redirects.items():
        print(f"{title}: {path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
