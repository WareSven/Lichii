#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later

from contextlib import redirect_stdout
import copy
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import work_pipeline as pipeline
from work_pipeline_core import PipelineError, STAGES, StateStore, fingerprint, new_task, record_stage, set_candidate


class PipelineIntegrationTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.script = self.root / "check.py"
        self.script.write_text("print('Executed isolated fixture check')\n", encoding="utf-8")
        self.evidence = self.root / "evidence.txt"
        self.evidence.write_text("Observed result for exact candidate\n", encoding="utf-8")
        self.cfg = {
            "schema_version": 1, "state_dir": str(self.root / "state"),
            "report_dir": str(self.root / "reports"), "imports": {},
            "commands": {"fixture": {
                "argv": [sys.executable, "-I", str(self.script)], "cwd": str(self.root),
                "source": str(self.root), "stage": "tests", "task_ids": ["candidate"],
                "heavy": False, "timeout_seconds": 10,
            }},
        }
        self.config_path = self.root / "config.json"
        self.write_config()
        self.store = StateStore(self.cfg["state_dir"])
        self.task = new_task("candidate", "Pipeline fixture", owner="developer", next_action="Review fixture")
        set_candidate(self.task, "a" * 40, "developer")
        self.store.update(lambda state: state["tasks"].append(self.task), {"action": "fixture"})
        self.clean = {"commit": "a" * 40, "dirty": ""}

    def write_config(self):
        self.config_path.write_text(json.dumps(self.cfg), encoding="utf-8")

    def main(self, *args):
        output = io.StringIO()
        with redirect_stdout(output):
            result = pipeline.main(["--config", str(self.config_path), *args])
        return result, output.getvalue()

    def stored_task(self):
        return pipeline.find_task(self.store.read(), "candidate")

    def run_check(self):
        return pipeline.run_check(self.cfg, self.store, "candidate", "fixture", "developer")

    def complete(self, task):
        for stage in STAGES:
            record_stage(task, stage, "passed", [self.evidence],
                         "reviewer" if stage == "review" else "developer")

    def test_cli_add_assign_candidate_record_handoff_and_gate(self):
        code, _ = self.main("add", "new", "--title", "New investigation", "--owner", "developer",
                            "--next-action", "Collect exact log")
        self.assertEqual(code, 0)
        self.main("assign", "new", "--owner", "researcher", "--next-action", "Inspect upstream diff")
        self.main("candidate", "new", "--actor", "researcher", "--commit", "b" * 40)
        self.main("record", "new", "--stage", "triage", "--status", "passed", "--actor", "researcher",
                  "--evidence", str(self.evidence), "--details", "Reproduced with captured log")
        task = pipeline.find_task(self.store.read(), "new")
        self.assertEqual(task["owner"], "researcher")
        self.assertEqual(task["candidate_commit"], "b" * 40)
        self.assertEqual(task["stages"]["triage"]["status"], "passed")
        code, text = self.main("handoff", "new", "--role", "review")
        self.assertEqual(code, 0)
        self.assertIn("Revue indépendante", text)
        self.assertIn(str(self.evidence), text)
        code, text = self.main("gate", "new")
        self.assertEqual(code, 1)
        self.assertFalse(json.loads(text)["ready"])
        self.assertTrue((self.root / "reports" / "dashboard.html").is_file())

    def test_successful_gate_is_zero_and_changed_evidence_is_one(self):
        self.store.update(lambda state: self.complete(pipeline.find_task(state, "candidate")), {"action": "complete"})
        self.assertEqual(self.main("gate", "candidate")[0], 0)
        self.evidence.write_text("Changed after certification", encoding="utf-8")
        self.assertEqual(self.main("gate", "candidate")[0], 1)

    def test_cli_unknown_task_errors_with_exit_two(self):
        result = subprocess.run([sys.executable, "-I", str(Path(pipeline.__file__).resolve()),
                                 "--config", str(self.config_path), "gate", "missing"],
                                capture_output=True, text=True, encoding="utf-8", timeout=20)
        self.assertEqual(result.returncode, 2)
        self.assertIn("Unknown task", result.stderr)

    def test_duplicate_add_does_not_modify_state(self):
        before = self.store.path.read_bytes()
        with self.assertRaises(PipelineError):
            self.main("add", "candidate", "--title", "Duplicate", "--owner", "developer", "--next-action", "No")
        self.assertEqual(self.store.path.read_bytes(), before)

    def test_import_refresh_preserves_assignments_and_verified_stages(self):
        template = copy.deepcopy(self.task)
        template.update(imported=True, owner="unassigned", next_action="Imported action",
                        sources=[fingerprint(self.evidence)], import_notes=[])
        state = {"tasks": []}
        pipeline.merge_imports(state, [copy.deepcopy(template)])
        existing = state["tasks"][0]
        existing.update(owner="developer", next_action="Review fixture", overrides=["owner", "next_action"])
        record_stage(existing, "triage", "passed", [self.evidence], "developer")
        record_stage(existing, "build", "passed", [self.evidence], "developer")
        incoming = copy.deepcopy(template)
        incoming["updated_at"] = "2099-01-01T00:00:00Z"
        pipeline.merge_imports(state, [incoming])
        current = state["tasks"][0]
        self.assertEqual(current["title"], template["title"])
        self.assertEqual(current["owner"], "developer")
        self.assertEqual(current["next_action"], "Review fixture")
        self.assertEqual(current["stages"]["build"]["status"], "passed")

    def test_import_new_commit_does_not_reuse_old_build(self):
        existing = copy.deepcopy(self.task)
        existing["imported"] = True
        record_stage(existing, "triage", "passed", [self.evidence], "developer")
        record_stage(existing, "build", "passed", [self.evidence], "developer")
        incoming = copy.deepcopy(self.task)
        incoming.update(imported=True, candidate_commit="b" * 40, import_notes=[])
        state = {"tasks": [existing]}
        pipeline.merge_imports(state, [incoming])
        self.assertEqual(state["tasks"][0]["stages"]["triage"]["status"], "pending")
        self.assertEqual(state["tasks"][0]["stages"]["build"]["status"], "pending")

    def test_import_new_failure_on_same_commit_invalidates_approvals(self):
        template = copy.deepcopy(self.task)
        template.update(imported=True, sources=[fingerprint(self.evidence)], import_notes=[])
        state = {"tasks": []}
        pipeline.merge_imports(state, [copy.deepcopy(template)])
        self.complete(state["tasks"][0])
        self.assertTrue(pipeline.assessments(state)["candidate"]["ready"])
        incoming = copy.deepcopy(template)
        incoming["stages"]["tests"] = {
            "status": "reported", "reported_status": "failed", "commit": "a" * 40,
            "actor": "legacy", "details": "New regression detected", "evidence": [fingerprint(self.evidence)],
        }
        pipeline.merge_imports(state, [incoming])
        self.assertFalse(pipeline.assessments(state)["candidate"]["ready"])
        self.assertEqual(state["tasks"][0]["stages"]["tests"]["reported_status"], "failed")
        self.assertEqual(state["tasks"][0]["stages"]["review"]["status"], "passed")
        self.assertEqual(state["tasks"][0]["stages"]["package"]["status"], "pending")

    def test_import_new_gameplay_failure_preserves_code_review(self):
        template = copy.deepcopy(self.task)
        template.update(imported=True, sources=[fingerprint(self.evidence)], import_notes=[])
        state = {"tasks": []}
        pipeline.merge_imports(state, [copy.deepcopy(template)])
        self.complete(state["tasks"][0])
        incoming = copy.deepcopy(template)
        incoming["stages"]["gameplay"] = {
            "status": "reported", "reported_status": "failed", "commit": "a" * 40,
            "actor": "manual-run-record", "details": "Owner reproduced flickering", "evidence": [fingerprint(self.evidence)],
        }
        pipeline.merge_imports(state, [incoming])
        result = state["tasks"][0]
        self.assertFalse(pipeline.assessments(state)["candidate"]["ready"])
        self.assertEqual(result["stages"]["gameplay"]["reported_status"], "failed")
        for stage in STAGES[:-1]:
            self.assertEqual(result["stages"][stage]["status"], "passed")

    def test_import_changed_failure_identity_invalidates_test_approval(self):
        template = copy.deepcopy(self.task)
        template.update(imported=True, sources=[fingerprint(self.evidence)], import_notes=[],
                        test_results=[{"name": "video", "cases": 20, "reported_status": "failed",
                                       "failures": ["known-baseline-case"], "evidence": [fingerprint(self.evidence)]}])
        template["stages"]["tests"] = {
            "status": "reported", "reported_status": "failed", "commit": "a" * 40,
            "actor": "legacy", "details": "video: failed (20 cases)", "evidence": [fingerprint(self.evidence)],
        }
        state = {"tasks": []}
        pipeline.merge_imports(state, [copy.deepcopy(template)])
        self.complete(state["tasks"][0])
        incoming = copy.deepcopy(template)
        incoming["test_results"][0]["failures"] = ["new-regression-case"]
        pipeline.merge_imports(state, [incoming])
        result = state["tasks"][0]
        self.assertEqual(result["stages"]["tests"]["status"], "reported")
        self.assertEqual(result["stages"]["review"]["status"], "passed")
        self.assertFalse(pipeline.assessments(state)["candidate"]["ready"])

    def test_import_container_metadata_change_preserves_stage_approvals(self):
        manifest = self.root / "metadata.json"
        manifest.write_text('{"recorded_at":"earlier"}', encoding="utf-8")
        template = copy.deepcopy(self.task)
        template.update(imported=True, sources=[fingerprint(manifest)], import_notes=[])
        template["stages"]["triage"] = {"status": "reported", "reported_status": "unknown",
                                         "actor": "legacy", "commit": "a" * 40,
                                         "details": "Candidate record", "evidence": [fingerprint(manifest)]}
        state = {"tasks": []}
        pipeline.merge_imports(state, [copy.deepcopy(template)])
        self.complete(state["tasks"][0])
        manifest.write_text('{"recorded_at":"later"}', encoding="utf-8")
        self.assertFalse(pipeline.assessments(state)["candidate"]["ready"])
        incoming = copy.deepcopy(template)
        incoming["sources"] = [fingerprint(manifest)]
        incoming["stages"]["triage"]["evidence"] = [fingerprint(manifest)]
        pipeline.merge_imports(state, [incoming])
        self.assertTrue(pipeline.assessments(state)["candidate"]["ready"])

    def test_import_new_artifact_hash_invalidates_build_and_later_stages(self):
        manifest = self.root / "artifact-manifest.json"
        binary = self.root / "fixture.bin"
        binary.write_bytes(b"old fixture binary; never executed")
        manifest.write_text(json.dumps({"artifact": fingerprint(binary)}), encoding="utf-8")
        template = copy.deepcopy(self.task)
        template.update(imported=True, sources=[fingerprint(manifest)], import_notes=[])
        template["stages"]["build"] = {
            "status": "reported", "reported_status": "passed", "actor": "legacy", "commit": "a" * 40,
            "details": "Build artifact checked", "evidence": [fingerprint(manifest), fingerprint(binary)],
        }
        state = {"tasks": []}
        pipeline.merge_imports(state, [copy.deepcopy(template)])
        self.complete(state["tasks"][0])
        binary.write_bytes(b"new fixture binary; never executed")
        manifest.write_text(json.dumps({"artifact": fingerprint(binary)}), encoding="utf-8")
        incoming = copy.deepcopy(template)
        incoming["sources"] = [fingerprint(manifest)]
        incoming["stages"]["build"]["evidence"] = [fingerprint(manifest), fingerprint(binary)]
        pipeline.merge_imports(state, [incoming])
        result = state["tasks"][0]
        for stage in STAGES[:4]:
            self.assertEqual(result["stages"][stage]["status"], "passed")
        self.assertEqual(result["stages"]["build"]["status"], "reported")
        for stage in STAGES[5:]:
            self.assertEqual(result["stages"][stage]["status"], "pending")
        self.assertFalse(pipeline.assessments(state)["candidate"]["ready"])

    def test_real_import_run_build_refresh_and_sync_preserve_success(self):
        candidate_manifest = self.root / "live-pending.json"
        build_manifest = self.root / "build-manifest.json"
        artifact = self.root / "compiled-fixture.bin"
        candidate_manifest.write_text(json.dumps({
            "candidate_commit": "a" * 40, "objective": "Imported fixture candidate",
            "build_manifest": str(build_manifest), "upstream_nx2": "c" * 40,
        }), encoding="utf-8")
        build_manifest.write_text(json.dumps({"commit": "a" * 40, "artifacts": []}), encoding="utf-8")
        self.script.write_text(
            "from pathlib import Path\nimport hashlib, json\n"
            f"artifact = Path({str(artifact)!r})\n"
            "artifact.write_bytes(b'Fixture build output; never executed')\n"
            f"manifest = Path({str(build_manifest)!r})\n"
            "manifest.write_text(json.dumps({'commit': 'a' * 40, 'artifacts': ["
            "{'path': str(artifact), 'sha256': hashlib.sha256(artifact.read_bytes()).hexdigest()}]}), encoding='utf-8')\n"
            "print('Fixture build complete')\n", encoding="utf-8")
        imported_id = "candidate:live-pending"
        self.cfg["imports"] = {"candidate_manifests": [str(candidate_manifest)]}
        self.cfg["commands"]["fixture"].update(stage="build", task_ids=[imported_id])
        self.store.update(lambda state: state["tasks"].clear(), {"action": "fixture-reset"})
        pipeline.sync(self.cfg, self.store)
        def prepare(state):
            task = pipeline.find_task(state, imported_id)
            task.update(owner="developer", next_action="Compile fixture", overrides=["owner", "next_action"])
            for stage in STAGES[:4]:
                record_stage(task, stage, "passed", [self.evidence],
                             "reviewer" if stage == "review" else "developer")
        self.store.update(prepare, {"action": "fixture-prepare"})
        with mock.patch.object(pipeline, "git_snapshot", return_value=self.clean):
            result = pipeline.run_check(self.cfg, self.store, imported_id, "fixture", "developer")
        self.assertEqual(result, 0)
        before_sync = pipeline.find_task(self.store.read(), imported_id)
        self.assertEqual(before_sync["stages"]["build"]["status"], "passed")
        self.assertEqual(before_sync["stages"]["review"]["status"], "passed")
        self.assertTrue(pipeline.assessments(self.store.read())[imported_id]["stages"]["build"]["valid"])
        pipeline.sync(self.cfg, self.store)
        after_sync = pipeline.find_task(self.store.read(), imported_id)
        self.assertEqual(after_sync["stages"]["build"], before_sync["stages"]["build"])
        self.assertEqual(after_sync["stages"]["review"], before_sync["stages"]["review"])
        self.assertEqual(after_sync["last_run"], before_sync["last_run"])

    def test_report_ignores_stale_caller_snapshot(self):
        stale = self.store.read()
        self.store.update(lambda state: pipeline.find_task(state, "candidate").update(title="Latest task title"),
                          {"action": "updated-title"})
        pipeline.report(self.cfg, stale)
        report = json.loads((self.root / "reports" / "report.json").read_text(encoding="utf-8"))
        self.assertEqual(report["revision"], self.store.read()["revision"])
        self.assertEqual(report["tasks"][0]["title"], "Latest task title")
        self.assertFalse(self.store.lock_path.exists())

    def test_import_cannot_replace_manual_task(self):
        with self.assertRaises(PipelineError):
            pipeline.merge_imports({"tasks": [self.task]}, [dict(self.task, imported=True)])

    def test_missing_import_source_blocks_even_a_completed_task(self):
        self.complete(self.task)
        self.task["imported"] = True
        state = {"tasks": [self.task]}
        pipeline.merge_imports(state, [])
        result = pipeline.assessments(state)["candidate"]
        self.assertFalse(result["ready"])
        self.assertIn("Source", result["label"])

    def test_real_subprocess_success_has_log_manifest_and_audit_events(self):
        with mock.patch.object(pipeline, "git_snapshot", return_value=self.clean):
            self.assertEqual(self.run_check(), 0)
        task = self.stored_task()
        record = task["stages"]["tests"]
        self.assertEqual(record["status"], "passed")
        self.assertEqual(record["commit"], "a" * 40)
        manifest = Path(task["last_run"])
        result = json.loads(manifest.read_text(encoding="utf-8"))
        self.assertEqual(result["returncode"], 0)
        self.assertEqual(result["inputs_before"], result["inputs_after"])
        self.assertIn("Executed isolated fixture check", (manifest.parent / "output.log").read_text(encoding="utf-8"))
        self.assertEqual({Path(item["path"]).name for item in record["evidence"]}, {"result.json", "output.log"})
        self.assertEqual([event["action"] for event in self.store.read()["event_log"]][-2:],
                         ["check-started", "check-finished"])
        self.assertFalse((self.store.directory / ".run.lock").exists())

    def test_real_subprocess_nonzero_records_failure(self):
        self.script.write_text("import sys\nprint('Regression failed')\nsys.exit(7)\n", encoding="utf-8")
        with mock.patch.object(pipeline, "git_snapshot", return_value=self.clean):
            self.assertEqual(self.run_check(), 1)
        self.assertEqual(self.stored_task()["stages"]["tests"]["status"], "failed")
        result = json.loads(Path(self.stored_task()["last_run"]).read_text(encoding="utf-8"))
        self.assertEqual(result["returncode"], 7)

    def test_dirty_or_wrong_commit_never_executes(self):
        for snapshot in ({"commit": "a" * 40, "dirty": " M tracked.cpp"},
                         {"commit": "b" * 40, "dirty": ""}):
            with self.subTest(snapshot=snapshot), mock.patch.object(pipeline, "git_snapshot", return_value=snapshot), \
                    mock.patch.object(pipeline, "execute") as execute:
                with self.assertRaises(PipelineError):
                    self.run_check()
                execute.assert_not_called()
                self.assertFalse((self.store.directory / ".run.lock").exists())

    def test_source_changed_during_successful_check_records_failure(self):
        with mock.patch.object(pipeline, "git_snapshot", side_effect=[self.clean, {"commit": "b" * 40, "dirty": ""}]):
            self.assertEqual(self.run_check(), 1)
        self.assertEqual(self.stored_task()["stages"]["tests"]["status"], "failed")

    def test_command_script_changed_during_execution_records_failure(self):
        self.script.write_text("from pathlib import Path\nPath(__file__).write_text('print(123)')\n", encoding="utf-8")
        with mock.patch.object(pipeline, "git_snapshot", return_value=self.clean):
            self.assertEqual(self.run_check(), 1)
        self.assertEqual(self.stored_task()["stages"]["tests"]["status"], "failed")

    def test_command_scope_and_actor_restrictions(self):
        with mock.patch.object(pipeline, "execute") as execute:
            for task_id, command_id, actor in (("other", "fixture", "developer"),
                                               ("candidate", "unconfigured", "developer"),
                                               ("candidate", "fixture", "another-agent")):
                with self.subTest(task=task_id, command=command_id, actor=actor), self.assertRaises(PipelineError):
                    pipeline.run_check(self.cfg, self.store, task_id, command_id, actor)
            execute.assert_not_called()

    def test_run_lock_is_respected_and_not_deleted(self):
        lock = self.store.directory / ".run.lock"
        lock.write_text("another run", encoding="utf-8")
        with mock.patch.object(pipeline, "execute") as execute, self.assertRaises(PipelineError):
            self.run_check()
        execute.assert_not_called()
        self.assertEqual(lock.read_text(encoding="utf-8"), "another run")

    def test_heavy_check_is_deferred_if_process_state_unknown(self):
        self.cfg["commands"]["fixture"]["heavy"] = True
        with mock.patch.object(pipeline, "git_snapshot", return_value=self.clean), \
                mock.patch.object(pipeline, "activity", return_value={"state": "unknown"}), \
                mock.patch.object(pipeline, "execute") as execute, self.assertRaises(PipelineError):
            self.run_check()
        execute.assert_not_called()
        self.assertEqual(self.store.read()["event_log"][-1]["action"], "check-deferred")

    def test_timeout_records_failure_with_preserved_output(self):
        def timed_out(command, log):
            Path(log).write_text("Partial output before timeout\n", encoding="utf-8")
            return {"returncode": None, "timeout": True, "cleanup_error": None}
        with mock.patch.object(pipeline, "git_snapshot", return_value=self.clean), \
                mock.patch.object(pipeline, "execute", side_effect=timed_out):
            self.assertEqual(self.run_check(), 1)
        task = self.stored_task()
        self.assertEqual(task["stages"]["tests"]["status"], "failed")
        result = json.loads(Path(task["last_run"]).read_text(encoding="utf-8"))
        self.assertTrue(result["timeout"])

    def test_execute_timeout_cleans_its_process_tree(self):
        process = mock.Mock()
        process.pid = 43210
        process.wait.side_effect = [subprocess.TimeoutExpired("fixture", 1), 0]
        process.poll.return_value = None
        with mock.patch.object(pipeline.subprocess, "Popen", return_value=process), \
                mock.patch.object(pipeline.subprocess, "run") as kill_tree, \
                mock.patch.object(pipeline.os, "killpg", create=True) as kill_group:
            result = pipeline.execute(self.cfg["commands"]["fixture"], self.root / "timeout.log")
        self.assertTrue(result["timeout"])
        self.assertIsNone(result["returncode"])
        process.kill.assert_called_once()
        if os.name == "nt":
            self.assertIn("/T", kill_tree.call_args.args[0])
            self.assertIn(str(process.pid), kill_tree.call_args.args[0])
        else:
            self.assertEqual(kill_group.call_args.args[0], process.pid)

    def test_candidate_changed_during_check_is_not_certified(self):
        def changed(command, log):
            Path(log).write_text("Completed fixture\n", encoding="utf-8")
            self.store.update(lambda state: set_candidate(pipeline.find_task(state, "candidate"), "b" * 40, "developer"),
                              {"action": "concurrent-candidate-change"})
            return {"returncode": 0}
        with mock.patch.object(pipeline, "git_snapshot", return_value=self.clean), \
                mock.patch.object(pipeline, "execute", side_effect=changed), self.assertRaises(PipelineError):
            self.run_check()
        self.assertEqual(self.stored_task()["stages"]["tests"]["status"], "pending")
        self.assertEqual(len(list((self.store.directory / "runs").glob("*/result.json"))), 1)

    def test_assignment_changed_during_check_is_not_certified(self):
        def reassigned(command, log):
            Path(log).write_text("Completed fixture\n", encoding="utf-8")
            self.store.update(lambda state: pipeline.find_task(state, "candidate").update(owner="new-owner"),
                              {"action": "concurrent-reassignment"})
            return {"returncode": 0}
        with mock.patch.object(pipeline, "git_snapshot", return_value=self.clean), \
                mock.patch.object(pipeline, "execute", side_effect=reassigned), self.assertRaises(PipelineError):
            self.run_check()
        self.assertEqual(self.stored_task()["stages"]["tests"]["status"], "pending")

    def test_config_rejects_string_task_scope(self):
        self.cfg["commands"]["fixture"]["task_ids"] = "candidate"
        self.write_config()
        with self.assertRaises(PipelineError):
            pipeline.load_config(self.config_path)

    def test_config_rejects_commands_that_can_hide_each_others_failure(self):
        self.cfg["commands"]["other-suite"] = copy.deepcopy(self.cfg["commands"]["fixture"])
        self.write_config()
        with self.assertRaises(PipelineError):
            pipeline.load_config(self.config_path)

    def test_test_binary_must_match_candidate_manifest_and_fingerprint(self):
        binary = self.root / "tests.exe"
        binary.write_bytes(b"fixture binary bytes; never executed")
        manifest = self.root / "build-manifest.json"
        manifest.write_text(json.dumps({"commit": "a" * 40, "artifacts": [fingerprint(binary)]}), encoding="utf-8")
        command = {"build_manifest": str(manifest), "test_binaries": [str(binary)]}
        self.assertEqual(len(pipeline.verify_test_binaries(command, "a" * 40)), 2)
        with self.assertRaises(PipelineError):
            pipeline.verify_test_binaries(command, "b" * 40)
        binary.write_bytes(b"modified fixture bytes")
        with self.assertRaises(PipelineError):
            pipeline.verify_test_binaries(command, "a" * 40)

    def test_direct_emulator_command_is_rejected(self):
        self.cfg["commands"]["fixture"]["argv"] = [str(self.root / "lichii.exe")]
        self.write_config()
        with self.assertRaises(PipelineError):
            pipeline.load_config(self.config_path)


if __name__ == "__main__":
    unittest.main()
