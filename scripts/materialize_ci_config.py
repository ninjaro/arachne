#!/usr/bin/env python3
"""Create a runner-local operations config backed by a persistent state checkout."""

from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path
from typing import Any


class ConfigurationError(RuntimeError):
    """The CI configuration cannot be materialized safely."""


def absolute_directory(path: Path, name: str, must_exist: bool) -> Path:
    result = path.resolve(strict=must_exist)
    if must_exist and not result.is_dir():
        raise ConfigurationError(f"{name} is not a directory: {result}")
    return result


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--template", type=Path, required=True)
    result.add_argument("--state-root", type=Path, required=True)
    result.add_argument("--runner-temp", type=Path, required=True)
    result.add_argument("--output", type=Path, required=True)
    return result


def main() -> int:
    arguments = parser().parse_args()
    try:
        with arguments.template.open(encoding="utf-8") as stream:
            config: dict[str, Any] = json.load(stream)
        if config.get("format_version") != 1 or not isinstance(
            config.get("paths"), dict
        ):
            raise ConfigurationError("template must be operations config version 1")

        state = absolute_directory(arguments.state_root, "state root", True)
        temporary = absolute_directory(arguments.runner_temp, "runner temp", True)
        paths = config["paths"]
        paths.update(
            {
                "queue": str(state / "queue"),
                "remainders": str(state / "remainders"),
                "ledger": str(state / "operations" / "ledger.sqlite3"),
                "graph_store": str(state / "graphs"),
                "artifact_store": str(state / "artifacts"),
                "lock_root": str(temporary / "arachne-locks"),
                "legacy_inbox_baseline": str(
                    state / "operations" / "legacy-inbox-baseline.json"
                ),
            }
        )
        (state / "queue").mkdir(parents=True, exist_ok=True)
        (state / "remainders").mkdir(parents=True, exist_ok=True)

        output = arguments.output.resolve(strict=False)
        output.parent.mkdir(parents=True, exist_ok=True)
        descriptor = os.open(output, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
            json.dump(config, stream, indent=2, sort_keys=True)
            stream.write("\n")
    except (OSError, json.JSONDecodeError, KeyError, TypeError,
            ConfigurationError) as error:
        print(f"materialize_ci_config: {error}", file=sys.stderr)
        return 2
    print(output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
