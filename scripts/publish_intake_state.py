#!/usr/bin/env python3
"""Commit one serialized intake directly to the official state branch."""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path


REF = re.compile(r"[A-Za-z0-9._/-]+\Z")


class IntakePublicationError(RuntimeError):
    """Intake state cannot be published without risking unrelated state."""


def command(
    argv: list[str], root: Path, capture: bool = False, check: bool = True
) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(
        argv, cwd=root, check=False, text=True, capture_output=capture
    )
    if check and result.returncode != 0:
        detail = result.stderr.strip() if capture else ""
        raise IntakePublicationError(
            f"command failed ({result.returncode}): {argv[0]} {detail}".rstrip()
        )
    return result


def relative_inside(path: Path, root: Path, label: str) -> Path:
    resolved = path.resolve(strict=False)
    try:
        return resolved.relative_to(root)
    except ValueError as error:
        raise IntakePublicationError(f"{label} must be inside state root") from error


def changed_paths(root: Path) -> list[tuple[str, Path]]:
    result = subprocess.run(
        ["git", "status", "--porcelain=v1", "-z"],
        cwd=root,
        check=False,
        capture_output=True,
    )
    if result.returncode != 0:
        raise IntakePublicationError("cannot inspect intake state changes")
    records: list[tuple[str, Path]] = []
    for raw in result.stdout.split(b"\0"):
        if not raw:
            continue
        if len(raw) < 4:
            raise IntakePublicationError("malformed git status record")
        status = raw[:2].decode("ascii")
        if "R" in status or "C" in status:
            raise IntakePublicationError("intake must not rename or copy state paths")
        records.append((status, Path(raw[3:].decode("utf-8"))))
    return records


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--state-root", type=Path, required=True)
    result.add_argument("--queue", type=Path, required=True)
    result.add_argument("--ledger", type=Path, required=True)
    result.add_argument("--base", required=True)
    result.add_argument("--title", required=True)
    return result


def main() -> int:
    arguments = parser().parse_args()
    try:
        state = arguments.state_root.resolve(strict=True)
        if not (state / ".git").exists():
            raise IntakePublicationError("state root is not a Git checkout")
        if not REF.fullmatch(arguments.base) or ".." in arguments.base:
            raise IntakePublicationError("unsafe base ref")
        queue = relative_inside(arguments.queue, state, "queue")
        ledger = relative_inside(arguments.ledger, state, "ledger")

        changes = changed_paths(state)
        for status, path in changes:
            allowed = path == ledger or path == queue or queue in path.parents
            if not allowed:
                raise IntakePublicationError(f"unexpected intake state change: {path}")
            if "D" in status:
                raise IntakePublicationError(f"intake must not delete state: {path}")
        if not changes:
            print("intake state already present")
            return 0

        command(["git", "config", "user.name", "github-actions[bot]"], state)
        command(
            [
                "git",
                "config",
                "user.email",
                "41898282+github-actions[bot]@users.noreply.github.com",
            ],
            state,
        )
        command(["git", "add", "--", str(queue), str(ledger)], state)
        if ledger.suffix.lower() in {".sqlite", ".sqlite3", ".db"}:
            attribute = command(
                ["git", "check-attr", "filter", "--", str(ledger)],
                state,
                capture=True,
            ).stdout.strip()
            if not attribute.endswith(": filter: lfs"):
                raise IntakePublicationError("operational SQLite must use Git LFS")
        command(["git", "commit", "-m", arguments.title], state)
        command(["git", "pull", "--rebase", "origin", arguments.base], state)
        pushed = command(
            ["git", "push", "origin", f"HEAD:refs/heads/{arguments.base}"],
            state,
            check=False,
        )
        if pushed.returncode != 0:
            command(["git", "pull", "--rebase", "origin", arguments.base], state)
            command(
                ["git", "push", "origin", f"HEAD:refs/heads/{arguments.base}"],
                state,
            )
    except (OSError, UnicodeDecodeError, IntakePublicationError) as error:
        print(f"publish_intake_state: {error}", file=sys.stderr)
        return 2
    print("official intake state published")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
