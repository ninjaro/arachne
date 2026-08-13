#!/usr/bin/env python3
"""Propose persistent state as a reviewable pull request with Git LFS guards."""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

REPOSITORY = re.compile(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+\Z")
REF = re.compile(r"[A-Za-z0-9._/-]+\Z")


class PublicationError(RuntimeError):
    """Persistent state is unsafe or cannot be proposed for review."""


def run(argv: list[str], cwd: Path, capture: bool = False) -> str:
    result = subprocess.run(
        argv, cwd=cwd, check=False, text=True, capture_output=capture
    )
    if result.returncode != 0:
        detail = result.stderr.strip() if capture else ""
        raise PublicationError(
            f"command failed ({result.returncode}): {argv[0]} {detail}".rstrip()
        )
    return result.stdout.strip() if capture else ""


def write_output(path: str | None, values: dict[str, str]) -> None:
    if not path:
        return
    with open(path, "a", encoding="utf-8") as stream:
        for key, value in values.items():
            if "\n" in value or "\r" in value:
                raise PublicationError("workflow outputs must be single-line values")
            stream.write(f"{key}={value}\n")


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--state-root", type=Path, required=True)
    result.add_argument("--repository", required=True)
    result.add_argument("--base", required=True)
    result.add_argument("--branch", required=True)
    result.add_argument("--title", required=True)
    result.add_argument("--body", required=True)
    return result


def main() -> int:
    arguments = parser().parse_args()
    try:
        state = arguments.state_root.resolve(strict=True)
        if not (state / ".git").exists():
            raise PublicationError(f"state root is not a Git checkout: {state}")
        if not REPOSITORY.fullmatch(arguments.repository):
            raise PublicationError("repository must be an owner/name pair")
        for name, value in (("base", arguments.base), ("branch", arguments.branch)):
            if not REF.fullmatch(value) or ".." in value or value.startswith("/"):
                raise PublicationError(f"unsafe {name} ref")

        status = run(["git", "status", "--porcelain"], state, capture=True)
        if not status:
            values = {"changed": "false", "pull_request_url": ""}
            write_output(os.environ.get("GITHUB_OUTPUT"), values)
            print("persistent state is unchanged")
            return 0

        run(["git", "switch", "-c", arguments.branch], state)
        run(["git", "config", "user.name", "github-actions[bot]"], state)
        run(
            [
                "git",
                "config",
                "user.email",
                "41898282+github-actions[bot]@users.noreply.github.com",
            ],
            state,
        )
        run(["git", "add", "--all"], state)
        staged = subprocess.run(
            ["git", "diff", "--cached", "--quiet"], cwd=state, check=False
        )
        if staged.returncode == 0:
            values = {"changed": "false", "pull_request_url": ""}
            write_output(os.environ.get("GITHUB_OUTPUT"), values)
            print("persistent state is unchanged")
            return 0
        if staged.returncode != 1:
            raise PublicationError("cannot inspect staged state changes")
        changed_paths = run(
            ["git", "diff", "--cached", "--name-only"], state, capture=True
        ).splitlines()
        for changed_path in changed_paths:
            if Path(changed_path).suffix.lower() not in {".sqlite", ".sqlite3", ".db"}:
                continue
            attribute = run(
                ["git", "check-attr", "filter", "--", changed_path],
                state,
                capture=True,
            )
            if not attribute.endswith(": filter: lfs"):
                raise PublicationError(
                    f"SQLite state must be tracked through Git LFS: {changed_path}"
                )
        run(["git", "commit", "-m", arguments.title], state)
        run(
            ["git", "push", "origin", f"HEAD:refs/heads/{arguments.branch}"], state
        )
        pull_request_url = run(
            [
                "gh",
                "pr",
                "create",
                "--repo",
                arguments.repository,
                "--base",
                arguments.base,
                "--head",
                arguments.branch,
                "--title",
                arguments.title,
                "--body",
                arguments.body,
            ],
            state,
            capture=True,
        )
        values = {"changed": "true", "pull_request_url": pull_request_url}
        write_output(os.environ.get("GITHUB_OUTPUT"), values)
        print(pull_request_url)
        return 0
    except (OSError, PublicationError) as error:
        print(f"propose_state_change: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
