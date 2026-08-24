#!/usr/bin/env python3
"""Commit reviewed arachne-data changes and reject a stale remote write."""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path


REF = re.compile(r"[A-Za-z0-9._/-]+\Z")
COMMIT = re.compile(r"[0-9a-f]{40}\Z")


class StatePublicationError(RuntimeError):
    """Persistent state cannot be published without overwriting newer data."""


def run(
    argv: list[str], root: Path, *, capture: bool = False, check: bool = True
) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(
        argv,
        cwd=root,
        check=False,
        text=True,
        capture_output=capture,
    )
    if check and result.returncode != 0:
        detail = result.stderr.strip() if capture else ""
        raise StatePublicationError(
            f"command failed ({result.returncode}): {' '.join(argv[:2])} {detail}".rstrip()
        )
    return result


def status_paths(root: Path) -> list[Path]:
    result = subprocess.run(
        ["git", "status", "--porcelain=v1", "-z"],
        cwd=root,
        check=False,
        capture_output=True,
    )
    if result.returncode != 0:
        raise StatePublicationError("cannot inspect state repository changes")
    paths: list[Path] = []
    for record in result.stdout.split(b"\0"):
        if not record:
            continue
        if len(record) < 4:
            raise StatePublicationError("malformed Git status record")
        status = record[:2].decode("ascii")
        if "R" in status or "C" in status:
            raise StatePublicationError("state publication must not rename or copy paths")
        paths.append(Path(record[3:].decode("utf-8")))
    return paths


def inside_allowed(path: Path, allowed: tuple[Path, ...]) -> bool:
    return any(path == candidate or candidate in path.parents for candidate in allowed)


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--state-root", type=Path, required=True)
    result.add_argument("--base", default="main")
    result.add_argument("--expected-base", required=True)
    result.add_argument("--title", required=True)
    result.add_argument("--allow", action="append", type=Path, required=True)
    result.add_argument("--writer-name", default="arachne-data-writer[bot]")
    result.add_argument(
        "--writer-email", default="arachne-data-writer[bot]@users.noreply.github.com"
    )
    return result


def main() -> int:
    arguments = parser().parse_args()
    try:
        state = arguments.state_root.expanduser().resolve(strict=True)
        if not (state / ".git").exists():
            raise StatePublicationError("state root is not a Git checkout")
        if not REF.fullmatch(arguments.base) or ".." in arguments.base:
            raise StatePublicationError("unsafe base branch")
        if not COMMIT.fullmatch(arguments.expected_base):
            raise StatePublicationError("expected base must be a full Git commit")
        checkout_head = run(
            ["git", "rev-parse", "HEAD"], state, capture=True
        ).stdout.strip()
        if checkout_head != arguments.expected_base:
            raise StatePublicationError(
                "state checkout moved after the writer captured its base"
            )

        allowed = tuple(arguments.allow)
        for path in allowed:
            if path.is_absolute() or not path.parts or ".." in path.parts:
                raise StatePublicationError(f"allowed path is not repository-relative: {path}")
        changed = status_paths(state)
        unexpected = [path for path in changed if not inside_allowed(path, allowed)]
        if unexpected:
            raise StatePublicationError(
                "unexpected state change: " + ", ".join(map(str, unexpected))
            )
        if not changed:
            print("authoritative state is unchanged")
            return 0

        run(["git", "fetch", "--no-tags", "origin", arguments.base], state)
        remote = run(
            ["git", "rev-parse", "FETCH_HEAD"], state, capture=True
        ).stdout.strip()
        if remote != arguments.expected_base:
            raise StatePublicationError(
                "stale state write rejected: arachne-data/main advanced during the run"
            )

        run(["git", "config", "user.name", arguments.writer_name], state)
        run(["git", "config", "user.email", arguments.writer_email], state)
        run(["git", "add", "--", *map(str, allowed)], state)
        staged = run(["git", "diff", "--cached", "--quiet"], state, check=False)
        if staged.returncode == 0:
            print("authoritative state is unchanged")
            return 0
        if staged.returncode != 1:
            raise StatePublicationError("cannot inspect staged state changes")
        for changed_path in run(
            ["git", "diff", "--cached", "--name-only"], state, capture=True
        ).stdout.splitlines():
            if Path(changed_path).suffix.lower() not in {".sqlite", ".sqlite3", ".db"}:
                continue
            attribute = run(
                ["git", "check-attr", "filter", "--", changed_path],
                state,
                capture=True,
            ).stdout.strip()
            if not attribute.endswith(": filter: lfs"):
                raise StatePublicationError(
                    f"SQLite state must be tracked through Git LFS: {changed_path}"
                )
        run(["git", "commit", "-m", arguments.title], state)
        run(["git", "fetch", "--no-tags", "origin", arguments.base], state)
        remote_before_push = run(
            ["git", "rev-parse", "FETCH_HEAD"], state, capture=True
        ).stdout.strip()
        if remote_before_push != arguments.expected_base:
            raise StatePublicationError(
                "stale state write rejected: arachne-data/main advanced during the run"
            )
        pushed = run(
            [
                "git",
                "push",
                "--atomic",
                "origin",
                f"HEAD:refs/heads/{arguments.base}",
            ],
            state,
            check=False,
        )
        if pushed.returncode != 0:
            raise StatePublicationError(
                "state push was rejected; the remote may have advanced"
            )
    except (OSError, UnicodeDecodeError, StatePublicationError) as error:
        print(f"publish_state_repository: {error}", file=sys.stderr)
        return 2
    print("authoritative state published")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
