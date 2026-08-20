from __future__ import annotations

import importlib.util
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def load_module(name: str, relative_path: str):
    path = ROOT / relative_path
    specification = importlib.util.spec_from_file_location(name, path)
    if specification is None or specification.loader is None:
        raise RuntimeError(f"could not load {path}")
    module = importlib.util.module_from_spec(specification)
    sys.modules[name] = module
    specification.loader.exec_module(module)
    return module


class MultiIssueIntakeTests(unittest.TestCase):
    def run_script(self, name: str, *arguments: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, str(ROOT / "scripts" / name), *arguments],
            check=False,
            capture_output=True,
            text=True,
        )

    def test_issue_parser_accepts_arbitrary_json_attachment_count(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            event = root / "event.json"
            output = root / "request.json"
            links = "\n".join(
                f"[batch-{index}.json](https://github.com/user-attachments/files/"
                f"{1000 + index}/batch-{index}.json)"
                for index in range(1, 101)
            )
            event.write_text(
                json.dumps(
                    {
                        "repository": {"full_name": "example/arachne"},
                        "issue": {
                            "number": 24,
                            "title": "Many batches",
                            "body": links,
                        },
                    }
                ),
                encoding="utf-8",
            )

            completed = self.run_script(
                "issue_intake_request.py",
                "--event",
                str(event),
                "--output",
                str(output),
            )

            self.assertEqual(completed.returncode, 0, completed.stderr)
            request = json.loads(output.read_text(encoding="utf-8"))
            self.assertEqual(request["format_version"], 2)
            self.assertEqual(len(request["attachments"]), 100)
            self.assertNotIn("attachment_url", request)
            self.assertEqual(request["attachments"][0]["name"], "batch-1.json")
            self.assertEqual(request["attachments"][-1]["name"], "batch-100.json")

    def test_single_attachment_keeps_legacy_projection(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            event = root / "event.json"
            output = root / "request.json"
            url = "https://github.com/user-attachments/files/1234/batch.json"
            event.write_text(
                json.dumps(
                    {
                        "repository": {"full_name": "example/arachne"},
                        "issue": {
                            "number": 25,
                            "title": "One batch",
                            "body": f"[batch.json]({url})",
                        },
                    }
                ),
                encoding="utf-8",
            )

            completed = self.run_script(
                "issue_intake_request.py",
                "--event",
                str(event),
                "--output",
                str(output),
            )

            self.assertEqual(completed.returncode, 0, completed.stderr)
            request = json.loads(output.read_text(encoding="utf-8"))
            self.assertEqual(len(request["attachments"]), 1)
            self.assertEqual(request["attachment_url"], url)
            self.assertEqual(request["attachment_name"], "batch.json")

    def test_adaptive_workers_are_bounded_but_file_count_is_not(self) -> None:
        module = load_module(
            "process_issue_batches_workers",
            "scripts/process_issue_batches.py",
        )
        self.assertEqual(module.adaptive_worker_count(1, 4), 1)
        self.assertEqual(module.adaptive_worker_count(2, 4), 2)
        self.assertEqual(module.adaptive_worker_count(10, 4), 4)
        self.assertEqual(module.adaptive_worker_count(100000, 4), 4)

        attachments = [
            {
                "url": f"https://github.com/user-attachments/files/{index}/batch.json",
                "host": "github.com",
                "name": "batch.json",
            }
            for index in range(1, 5001)
        ]
        parsed = module.issue_attachments({"attachments": attachments})
        self.assertEqual(len(parsed), 5000)

    def test_submission_uses_total_byte_budget(self) -> None:
        module = load_module(
            "process_issue_batches_budget",
            "scripts/process_issue_batches.py",
        )
        self.assertEqual(
            module.total_byte_budget(
                {
                    "security": {
                        "submission_max_total_bytes": 12345,
                        "archive_max_total_bytes": 54321,
                    }
                }
            ),
            12345,
        )
        self.assertEqual(
            module.total_byte_budget(
                {"security": {"archive_max_total_bytes": 54321}}
            ),
            54321,
        )

    def test_final_inbox_name_is_unique_and_safe(self) -> None:
        module = load_module(
            "process_issue_batches_target",
            "scripts/process_issue_batches.py",
        )
        attachment = module.FetchedAttachment(
            index=7,
            name="../../A batch (final).json",
            child_request=Path("request.json"),
            fetch_request=Path("fetch.json"),
            acquired_control=Path("control.json"),
            byte_length=100,
            sha256="a" * 64,
        )
        target = module.final_target(Path("/repo"), 24, attachment)
        self.assertEqual(
            target,
            Path("/repo/inbox/issue-24-" + "a" * 64 + "-A_batch_final_.json"),
        )

    def test_processor_materializes_many_attachments_atomically(self) -> None:
        module = load_module(
            "process_issue_batches_integration",
            "scripts/process_issue_batches.py",
        )
        with tempfile.TemporaryDirectory() as temporary:
            repository = Path(temporary) / "repository"
            scripts = repository / "scripts"
            inbox = repository / "inbox"
            artifacts = repository / "artifacts"
            scripts.mkdir(parents=True)
            inbox.mkdir()
            artifacts.mkdir()

            (scripts / "issue_fetch_request.py").write_text(
                """#!/usr/bin/env python3
import argparse, json
from pathlib import Path
p=argparse.ArgumentParser(); p.add_argument('--request',type=Path); p.add_argument('--config'); p.add_argument('--output',type=Path); a=p.parse_args()
r=json.loads(a.request.read_text()); i=r['attachment_index']; n=r['attachment_name']
a.output.write_text(json.dumps({'locator':r['attachment_url'],'request_id':f'req-{i}','output_ref':f'intake/{i:04d}-{n}'}))
""",
                encoding="utf-8",
            )
            (scripts / "arachne_ops.py").write_text(
                """#!/usr/bin/env python3
import argparse, hashlib, json
from pathlib import Path
p=argparse.ArgumentParser(); p.add_argument('--config',type=Path); p.add_argument('--binary'); sub=p.add_subparsers(dest='op'); f=sub.add_parser('fetch'); f.add_argument('--request',type=Path); f.add_argument('--output-control',type=Path); a=p.parse_args()
c=json.loads(a.config.read_text()); r=json.loads(a.request.read_text()); root=Path(c['paths']['artifact_store']); target=root/r['output_ref']; target.parent.mkdir(parents=True,exist_ok=True); content=json.dumps({'format':'arachne_batch','batch_id':r['request_id'],'create':{},'update':{},'merge':{}}).encode()+b'\\n'; target.write_bytes(content); a.output_control.write_text(json.dumps({'contract':'acquired_artifact_v1','format_version':1,'request_id':r['request_id'],'source_locator':r['locator'],'transport':{'status':'delivered'},'artifact':{'storage_ref':r['output_ref'],'sha256':hashlib.sha256(content).hexdigest(),'byte_length':len(content)}}))
""",
                encoding="utf-8",
            )
            (scripts / "materialize_product_batch.py").write_text(
                """#!/usr/bin/env python3
import argparse, json, shutil
from pathlib import Path
p=argparse.ArgumentParser(); p.add_argument('--request',type=Path); p.add_argument('--fetch-request',type=Path); p.add_argument('--acquired-control'); p.add_argument('--config',type=Path); a=p.parse_args()
r=json.loads(a.request.read_text()); f=json.loads(a.fetch_request.read_text()); c=json.loads(a.config.read_text()); issue=r['submission_ref'].rsplit('#',1)[1]; source=Path(c['paths']['artifact_store'])/f['output_ref']; shutil.copyfile(source, Path.cwd()/'inbox'/f'issue-{issue}.json')
""",
                encoding="utf-8",
            )
            (scripts / "discard_acquired_artifact.py").write_text(
                """#!/usr/bin/env python3
import argparse, json
from pathlib import Path
p=argparse.ArgumentParser(); p.add_argument('--fetch-request',type=Path); p.add_argument('--acquired-control'); p.add_argument('--config',type=Path); a=p.parse_args()
f=json.loads(a.fetch_request.read_text()); c=json.loads(a.config.read_text()); (Path(c['paths']['artifact_store'])/f['output_ref']).unlink()
""",
                encoding="utf-8",
            )

            request = repository / "request.json"
            request.write_text(
                json.dumps(
                    {
                        "format_version": 2,
                        "submission_ref": "github-issue:example/arachne#24",
                        "title": "Many",
                        "attachments": [
                            {
                                "url": f"https://github.com/user-attachments/files/{index}/batch-{index}.json",
                                "host": "github.com",
                                "name": f"batch-{index}.json",
                            }
                            for index in range(1, 6)
                        ],
                    }
                ),
                encoding="utf-8",
            )
            config = repository / "config.json"
            config.write_text(
                json.dumps(
                    {
                        "paths": {"artifact_store": str(artifacts)},
                        "security": {"submission_max_total_bytes": 1048576},
                        "transport": {
                            "defaults": {"admission": {"maximum_concurrency": 3}}
                        },
                    }
                ),
                encoding="utf-8",
            )
            output = repository / "result.json"
            old_cwd = Path.cwd()
            try:
                import os
                os.chdir(repository)
                result = module.main(
                    [
                        "--request", str(request),
                        "--config", str(config),
                        "--binary", "/bin/true",
                        "--work-dir", str(repository / "work"),
                        "--output", str(output),
                    ],
                    repository_root=repository,
                )
            finally:
                os.chdir(old_cwd)

            self.assertEqual(result, 0)
            files = sorted(inbox.glob("issue-24-*.json"))
            self.assertEqual(len(files), 5)
            self.assertFalse((inbox / "issue-24.json").exists())
            summary = json.loads(output.read_text(encoding="utf-8"))
            self.assertEqual(summary["attachment_count"], 5)
            self.assertEqual(summary["fetch_workers"], 3)
            self.assertEqual(list(artifacts.rglob("*.json")), [])


if __name__ == "__main__":
    unittest.main()
