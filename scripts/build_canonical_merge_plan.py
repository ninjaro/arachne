#!/usr/bin/env python3
"""Build the reviewed merge plan represented by the July 2026 audits.

The Markdown reports remain human-readable audit evidence.  This command turns
only their explicitly accepted rows, plus the separately validated expansion
listed below, into a closed machine-readable plan.  Review/deferred rows are
copied into the plan's blocked-ID guard rather than inferred as merges.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import tempfile
from typing import Any


class PlanError(RuntimeError):
    pass


DEFERRED_CONCEPT_HEADINGS = {
    "lithograph",
    "anti-war satire",
    "claustrophobic interiors",
    "dream-reality convergence",
    "dream–reality instability",
    "political pink film",
    "shapeshifting",
    "taboo-family roleplay",
}

ACCEPTED_CONCEPT_HEADINGS = {
    "anti-novel",
    "antiwar film",
    "artist book",
    "artist's studio",
    "coldwave",
    "cutout animation",
    "dark wave",
    "dream–reality permeability",
    "fiction-documentary hybrid",
    "human-animal bond",
    "human-animal continuum",
    "human–alien alliance",
    "life-death boundary",
    "memoir/fiction hybrid",
    "nonconsensual human experimentation",
    "nonlinear storytelling",
    "post-minimalism",
    "pseudo-documentary",
    "pseudotranslation",
    "shapeshifting alien",
    "synth-pop",
    "synth-punk",
    "tradition-modernity conflict",
    "US military-base town",
    "alternate guitar tunings",
    "ambient interlude",
    "B-movie aesthetic",
    "cycle of violence",
    "family secrets",
    "fantastic creature",
    "gender stereotype",
    "long take",
    "low-budget aesthetic",
    "mask",
    "nested narrative",
    "social outsider",
    "wounded soldier",
    "analog synthesis",
    "archive reworking",
    "cyclic time",
    "elemental kaiju weaponry",
    "future observer in the present",
    "graphic body transformation",
    "hallucinatory imagery",
    "high–low cultural collision",
    "horror and comedy hybrid",
    "live-studio immediacy",
    "lock-groove construction",
    "masculinity crisis",
    "pornographic parody",
    "scientific report frame",
    "slow projection",
    "social decay",
    "Tropicalism",
    "ambient",
    "documentary film",
    "electronic",
    "erotic film",
    "erotic vampire film",
    "experimental electronic",
    "exploitation film",
    "giant monster film",
    "post-apocalyptic film",
    "post-industrial",
    "science fiction film",
    "sexploitation film",
    "deathrock",
    "hidden identity",
    "kosmische",
    "zombie film",
}


# These pairs were validated after the first concept report using exact
# translation, parallel hierarchy, identical assertions/evidence, spelling, or
# conservative word-order equivalence.  Each pair is source -> target.
EXPANDED_CONCEPT_PAIRS = (
    ("Neue Sachlichkeit", "New Objectivity"),
    ("Neue Sachlichkeit portraiture", "New Objectivity portraiture"),
    ("self-replicating perfect human", "perfect self-replicating human"),
    ("ageing", "aging"),
    ("documentary portrait", "portrait documentary"),
    ("mind–body conflict", "body–mind conflict"),
    ("anthology horror", "horror anthology"),
    ("New York downtown scene", "downtown New York scene"),
    ("noise–ambient continuum", "ambient-noise continuum"),
    ("adult narrative feature", "narrative adult feature"),
    ("adult horror-fantasy hybrid", "adult fantasy-horror hybrid"),
    ("erotic crime melodrama", "crime-erotic melodrama"),
    ("independent low-budget filmmaking", "low-budget independent filmmaking"),
    ("lead dramatic performance", "dramatic lead performance"),
    ("fiction-reportage boundary", "reportage-fiction boundary"),
)


PUBLISHER_NORMALIZATION = {
    "Cinenacional.com": "Cinenacional.com",
    "cinenacional.com": "Cinenacional.com",
    "DistribPix": "DistribPix",
    "Distribpix": "DistribPix",
    "Filmportal.de": "filmportal.de",
    "filmportal.de": "filmportal.de",
    "It's Psychedelic Baby Magazine": "It's Psychedelic Baby Magazine",
    "It’s Psychedelic Baby Magazine": "It's Psychedelic Baby Magazine",
    "Musée d'Orsay": "Musée d'Orsay",
    "Musée d’Orsay": "Musée d'Orsay",
    "The Film-Makers' Cooperative": "The Film-Makers' Cooperative",
    "The Film-Makers’ Cooperative": "The Film-Makers' Cooperative",
    "TIME": "Time",
    "Time": "Time",
    "UniFrance": "Unifrance",
    "Unifrance": "Unifrance",
    "VICE": "Vice",
    "Vice": "Vice",
    "WIRED": "Wired",
    "Wired": "Wired",
}


LABEL_NORMALIZATION = {
    "Serpent's Tail edition": "Serpent's Tail edition",
    "Serpent’s Tail edition": "Serpent's Tail edition",
}


def _load_object(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise PlanError(f"cannot read {path}: {error}") from error
    if not isinstance(value, dict):
        raise PlanError(f"{path} must contain an object")
    return value


def _atomic_write(path: Path, content: str) -> None:
    path = path.resolve()
    path.parent.mkdir(parents=True, exist_ok=True)
    try:
        mode = path.stat().st_mode & 0o777
    except FileNotFoundError:
        mode = 0o644
    descriptor, temporary = tempfile.mkstemp(
        prefix=f".{path.name}.", suffix=".tmp", dir=path.parent
    )
    temporary_path = Path(temporary)
    try:
        os.fchmod(descriptor, mode)
        with os.fdopen(descriptor, "w", encoding="utf-8", newline="\n") as output:
            output.write(content)
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary_path, path)
        directory_fd = os.open(path.parent, os.O_RDONLY)
        try:
            os.fsync(directory_fd)
        finally:
            os.close(directory_fd)
    except Exception:
        try:
            temporary_path.unlink()
        except FileNotFoundError:
            pass
        raise


def _table_groups(
    report: str, start: str, end: str, prefix: str
) -> list[dict[str, Any]]:
    try:
        section = report.split(start, 1)[1].split(end, 1)[0]
    except IndexError as error:
        raise PlanError(f"cannot find report section {start!r}") from error
    groups: list[dict[str, Any]] = []
    for line in section.splitlines():
        if not line.startswith(f"| `{prefix}"):
            continue
        cells = [cell.strip() for cell in line.strip().strip("|").split("|")]
        target_match = re.search(rf"`({re.escape(prefix)}[^`]+)`", cells[0])
        members = re.findall(rf"`({re.escape(prefix)}[^`]+)`", cells[1])
        if target_match is None or len(members) < 2:
            raise PlanError(f"cannot parse merge row: {line}")
        groups.append(
            {
                "target": target_match.group(1),
                "members": members,
                "reason": cells[3].replace("<br>", "; ") if len(cells) > 3 else "",
            }
        )
    return groups


def _review_queue_ids(report: str) -> list[str]:
    try:
        section = report.split(
            "# Appendix B: additional organization/group review queue", 1
        )[1].split("# Appendix C:", 1)[0]
    except IndexError as error:
        raise PlanError("cannot find organization/group review queue") from error
    return sorted(set(re.findall(r"`(agent-[0-9]+)`", section)))


def _concept_sections(report: str) -> list[tuple[str, str]]:
    parts = re.split(r"(?m)^### ", report)[1:]
    result: list[tuple[str, str]] = []
    for part in parts:
        heading, body = part.split("\n", 1)
        result.append((heading.strip(), body))
    return result


def _concept_groups(
    report: str, tags: list[dict[str, Any]]
) -> tuple[list[dict[str, Any]], list[str]]:
    by_identifier: dict[str, dict[str, Any]] = {}
    for record in tags:
        for candidate in (record.get("canonical_id"), record.get("local_id")):
            if isinstance(candidate, str) and candidate:
                by_identifier[candidate] = record
    # Reports written against v2 contain con_<sha256> identifiers.  Accept
    # those as input references while emitting only the matched readable
    # transport IDs in the new plan.
    report_identifier = r"(?:concept-[0-9]{6,}|con_[0-9a-f]{64})"
    accepted: list[dict[str, Any]] = []
    blocked: list[str] = []
    sections = _concept_sections(report)
    headings = {heading for heading, _ in sections}
    reviewed = ACCEPTED_CONCEPT_HEADINGS | DEFERRED_CONCEPT_HEADINGS
    unexpected = headings - reviewed
    missing = reviewed - headings
    if unexpected or missing:
        raise PlanError(
            "concept audit disposition is not closed; "
            f"unreviewed headings={sorted(unexpected)}, "
            f"missing reviewed headings={sorted(missing)}"
        )
    for heading, body in sections:
        member_line = re.search(r"(?m)^- Members: (.+)$", body)
        target_match = re.search(
            rf"(?m)^- Recommended target: `({report_identifier})`$", body
        )
        if member_line is None or target_match is None:
            raise PlanError(f"cannot parse concept section {heading!r}")
        canonical_members = re.findall(
            rf"`({report_identifier})`", member_line.group(1)
        )
        try:
            members = [by_identifier[value]["local_id"] for value in canonical_members]
            target_record = by_identifier[target_match.group(1)]
        except KeyError as error:
            raise PlanError(
                f"concept report ID is absent from the manifest: {error}"
            ) from error
        if heading in DEFERRED_CONCEPT_HEADINGS:
            blocked.extend(members)
            continue
        if heading not in ACCEPTED_CONCEPT_HEADINGS:
            raise PlanError(f"concept section lacks an accepted disposition: {heading}")
        canonical = {
            "name": target_record["name"],
            "type": target_record["type"],
            "slug": target_record["slug"],
        }
        group: dict[str, Any] = {
            "target": target_record["local_id"],
            "members": members,
            "canonical": canonical,
            "reason": re.search(r"(?m)^- Reason: (.+)$", body).group(1),
        }
        if heading == "lock-groove construction":
            group["canonical"]["name"] = "locked-groove construction"
        if heading == "Tropicalism":
            group["alias_languages"] = {"Tropicalismo": "pt"}
        if heading == "kosmische":
            group["extra_aliases"] = [
                {"value": "Kosmische Musik", "language": "de"}
            ]
        accepted.append(group)
    return accepted, sorted(set(blocked))


def _unique_tag_by_name(
    tags: list[dict[str, Any]], name: str
) -> dict[str, Any]:
    matches = [record for record in tags if record["name"] == name]
    if len(matches) != 1:
        raise PlanError(
            f"expected exactly one concept named {name!r}; found {len(matches)}"
        )
    return matches[0]


def _expanded_concepts(tags: list[dict[str, Any]]) -> list[dict[str, Any]]:
    result: list[dict[str, Any]] = []
    for source_name, target_name in EXPANDED_CONCEPT_PAIRS:
        source = _unique_tag_by_name(tags, source_name)
        target = _unique_tag_by_name(tags, target_name)
        if source["type"] != target["type"]:
            raise PlanError(
                f"expanded concept pair crosses types: {source_name} / {target_name}"
            )
        group: dict[str, Any] = {
            "target": target["local_id"],
            "members": [source["local_id"], target["local_id"]],
            "canonical": {
                "name": target["name"],
                "type": target["type"],
                "slug": target["slug"],
            },
            "reason": "additional high-confidence equivalence validated against usage, relations, and evidence",
        }
        if source_name.startswith("Neue Sachlichkeit"):
            group["alias_languages"] = {source_name: "de"}
        result.append(group)
    return result


def _validate_deep_report(report: str) -> None:
    required_findings = {
        "Wakamatsu identity merge": (
            "agent-002780",
            "agent-002781",
            "agent-002887",
            "Applied 2026-07-24",
            "was consolidated there",
        ),
        "known work split": (
            "work-000836",
            "split remains deferred",
        ),
        "version-sensitive non-merge": (
            "work-001858",
            "work-001859",
            "not a safe work key",
        ),
    }
    missing: dict[str, list[str]] = {}
    for finding, fragments in required_findings.items():
        absent = [fragment for fragment in fragments if fragment not in report]
        if absent:
            missing[finding] = absent
    if missing:
        raise PlanError(
            "deep audit no longer contains the reviewed safety findings: "
            f"{missing}"
        )


def build_plan(
    manifest: dict[str, Any],
    concept_report: str,
    deep_report: str,
    non_concept_report: str,
) -> dict[str, Any]:
    _validate_deep_report(deep_report)
    agent_groups = _table_groups(
        non_concept_report,
        "# Appendix A: automatic agent merge groups",
        "# Appendix B:",
        "agent-",
    )
    work_groups = _table_groups(
        non_concept_report,
        "## Automatic",
        "## Conditional and blocked",
        "work-",
    )
    concept_groups, blocked_concepts = _concept_groups(
        concept_report, manifest["tags"]
    )
    expanded = _expanded_concepts(manifest["tags"])
    existing_members = {
        member for group in concept_groups for member in group["members"]
    }
    for group in expanded:
        if existing_members.intersection(group["members"]):
            raise PlanError(
                f"expanded concept group overlaps the original report: {group}"
            )
        existing_members.update(group["members"])
    concept_groups.extend(expanded)

    # The two identical Onanie Bomb source records differ only by a trailing
    # slash.  The used slash-form survives; its other URL becomes an alternate.
    source_merges = [
        {
            "target": "source-007643",
            "members": ["source-007642", "source-007643"],
            "reason": "normalized-identical URL and metadata; used source retained",
        }
    ]

    return {
        "contract": "canonical_merge_plan_v1",
        "format_version": 1,
        "source_reports": [
            "concept-merge-report.md",
            "deep-backyard-audit.md",
            "non-concept-merge-audit.md",
        ],
        "agent_merges": agent_groups,
        "work_merges": work_groups,
        "concept_merges": concept_groups,
        "source_merges": source_merges,
        "assertion_updates": [
            {
                "match": {
                    "work": "work-007213",
                    "tag": "concept-005334",
                    "relation": "exemplifies",
                },
                "set": {"relation": "associated_with"},
                "reason": (
                    "death-rock wording describes the film's subject/music "
                    "association, not genre membership by the film"
                ),
            }
        ],
        "publisher_normalization": PUBLISHER_NORMALIZATION,
        "manifestation_label_normalization": LABEL_NORMALIZATION,
        "blocked": {
            "agents": sorted(
                set(
                    [
                        "agent-000462",
                        "agent-000688",
                        "agent-005134",
                        "agent-001129",
                        "agent-001308",
                    ]
                )
                | set(_review_queue_ids(non_concept_report))
            ),
            "works": [
                "work-000836",
                "work-001858",
                "work-001859",
                "work-004942",
                "work-004943",
                "work-007787",
                "work-007788",
            ],
            "concepts": blocked_concepts,
            "sources": [
                "source-000444",
                "source-000445",
                "source-012751",
                "source-012752",
            ],
        },
    }


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--concept-report", type=Path, required=True)
    parser.add_argument("--deep-report", type=Path, required=True)
    parser.add_argument("--non-concept-report", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    return parser


def main(argv: list[str] | None = None) -> int:
    arguments = _parser().parse_args(argv)
    protected_inputs = {
        arguments.manifest.resolve(),
        arguments.concept_report.resolve(),
        arguments.deep_report.resolve(),
        arguments.non_concept_report.resolve(),
    }
    if arguments.output.resolve() in protected_inputs:
        raise PlanError("merge-plan output must be distinct from every input")
    manifest = _load_object(arguments.manifest)
    try:
        concept_report = arguments.concept_report.read_text(encoding="utf-8")
        deep_report = arguments.deep_report.read_text(encoding="utf-8")
        non_concept_report = arguments.non_concept_report.read_text(
            encoding="utf-8"
        )
    except OSError as error:
        raise PlanError(f"cannot read audit report: {error}") from error
    plan = build_plan(
        manifest, concept_report, deep_report, non_concept_report
    )
    serialized = json.dumps(
        plan, ensure_ascii=False, sort_keys=True, indent=2
    ) + "\n"
    _atomic_write(arguments.output, serialized)
    summary = {
        "agent_groups": len(plan["agent_merges"]),
        "agent_losers": sum(
            len(group["members"]) - 1 for group in plan["agent_merges"]
        ),
        "work_groups": len(plan["work_merges"]),
        "work_losers": sum(
            len(group["members"]) - 1 for group in plan["work_merges"]
        ),
        "concept_groups": len(plan["concept_merges"]),
        "concept_losers": sum(
            len(group["members"]) - 1 for group in plan["concept_merges"]
        ),
        "source_groups": len(plan["source_merges"]),
    }
    print(json.dumps(summary, sort_keys=True))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except PlanError as error:
        raise SystemExit(f"error: {error}") from error
