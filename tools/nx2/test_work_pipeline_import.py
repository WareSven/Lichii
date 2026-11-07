#!/usr/bin/env python3
"""Regression checks for historical evidence import boundaries."""

import hashlib
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from work_pipeline_import import import_tasks


class ImportTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(dir=os.environ.get("WORK_PIPELINE_TEST_TMP"))
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.commit = "a" * 40
        self.config_path = self.write_json("improvement/config.json", {"hypotheses": []})
        self.state_path = self.write_json("improvement/state/state.json", {"backlog": []})
        self.runs = self.root / "compatibility/runs"
        self.runs.mkdir(parents=True)
        self.config = {"legacy_config": str(self.config_path), "legacy_state": str(self.state_path),
                       "candidate_manifests": [], "run_roots": [str(self.runs)]}

    def write(self, name, content=b"evidence"):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
        return path

    def write_json(self, name, data):
        return self.write(name, json.dumps(data).encode("utf-8"))

    def evidence(self, path):
        return {"path": str(path), "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}

    def candidate(self, data=None, name="change-pending.json"):
        path = self.write_json("improvement/" + name, data or {"candidate_commit": self.commit})
        self.config["candidate_manifests"].append(str(path))
        return path

    def task(self):
        return next(t for t in import_tasks(self.config) if t["id"].startswith("candidate:"))

    def make_run(self, status="pass", commit=None, evidence=True, name="trial"):
        attachment = self.write(f"compatibility/runs/{name}/evidence/owner.txt", b"owner observation")
        case = {"status": status, "note": "Manual observation", "history": [{
            "status": status, "note": "Manual observation",
            "evidence": [self.evidence(attachment)] if evidence else []}]}
        return self.write_json(f"compatibility/runs/{name}/run.json", {
            "source": {"commit": commit or self.commit}, "game": {"program_id": "0400486024DEC000"},
            "cases": {"gameplay": case}, "fps": 120})

    def build(self, missing_report=False, status="passed", failures=None):
        artifact = self.write("build/bin/lichii.exe")
        report = self.write("improvement/builds/change/tests.xml", b"<testsuite/>")
        data = {"commit": self.commit, "artifacts": [self.evidence(artifact)],
                "validation": [{"name": "suite", "status": status, "cases": 3,
                                "failures": failures or [], "report": str(report)}]}
        path = self.write_json("improvement/builds/change/build-manifest.json", data)
        if missing_report:
            report.unlink()
        return path

    def setup(self, runtime=None):
        artifact = self.write("compatibility/packages/change/lichii.exe")
        entry = self.evidence(artifact)
        entry["path"] = "lichii.exe"
        return self.write_json("compatibility/variants/change/setup-validation.json", {
            "commit": self.commit, "package": str(artifact.parent),
            "runtime": [entry] if runtime is None else runtime})

    def test_current_hypothesis_overrides_stale_state(self):
        self.write_json("improvement/state/state.json", {"backlog": [{
            "id": "hypothesis:cache", "title": "Stale", "evidence": []}]})
        evidence = self.write("evidence.txt")
        self.write_json("improvement/config.json", {"hypotheses": [{
            "id": "cache", "title": "Fresh", "evidence": [str(evidence)]}]})
        tasks = import_tasks(self.config)
        self.assertEqual(len(tasks), 1)
        self.assertEqual(tasks[0]["id"], "legacy:hypothesis:cache")
        self.assertEqual(tasks[0]["title"], "Fresh")
        self.assertEqual(len(tasks[0]["sources"]), 2)

    def test_verified_files_never_promote_gates(self):
        self.candidate()
        self.build()
        self.setup()
        self.make_run()
        task = self.task()
        self.assertEqual(task["candidate_commit"], self.commit)
        self.assertEqual(task["game_id"], "0400486024DEC000")
        for name in ("build", "tests", "package", "gameplay"):
            self.assertEqual(task["stages"][name]["status"], "reported")
            self.assertEqual(task["stages"][name]["reported_status"], "passed")
        self.assertTrue(all(s["status"] in ("pending", "reported") for s in task["stages"].values()))

    def test_state_prose_boolean_and_fps_cannot_pass_gameplay(self):
        self.candidate({"candidate_commit": self.commit, "gameplay_validated": True,
                        "state": "owner satisfied all passed", "fps": 120})
        self.make_run(status="not_tested")
        self.assertEqual(self.task()["stages"]["gameplay"]["status"], "pending")

    def test_latest_manual_failure_is_not_hidden_by_earlier_pass(self):
        self.candidate()
        path = self.make_run(status="fail")
        run = json.loads(path.read_text())
        run["cases"]["gameplay"]["history"].insert(0, {"status": "pass", "note": "Earlier pass", "evidence": []})
        path.write_text(json.dumps(run))
        task = self.task()
        self.assertEqual(task["stages"]["gameplay"]["reported_status"], "failed")
        self.assertEqual(task["gameplay_observations"][0]["reported_status"], "failed")

    def test_other_commit_run_does_not_validate_candidate(self):
        self.candidate()
        self.make_run(commit="b" * 40)
        self.assertEqual(self.task()["stages"]["gameplay"]["status"], "pending")

    def test_missing_manual_attachment_downgrades_pass(self):
        self.candidate()
        self.make_run(evidence=False)
        task = self.task()
        self.assertEqual(task["stages"]["gameplay"]["reported_status"], "unknown")
        self.assertEqual(task["gameplay_observations"][0]["reported_status"], "passed")
        self.assertTrue(any("No attached manual evidence" in n for n in task["import_notes"]))

    def test_missing_test_report_does_not_report_pass(self):
        self.candidate()
        self.build(missing_report=True)
        self.assertEqual(self.task()["stages"]["tests"]["reported_status"], "unknown")

    def test_known_baseline_failures_remain_failures(self):
        self.candidate()
        self.build(status="known_baseline_failures", failures=["existing NV12 failure"])
        task = self.task()
        self.assertEqual(task["stages"]["tests"]["reported_status"], "failed")
        self.assertEqual(task["test_results"][0]["failures"], ["existing NV12 failure"])

    def test_package_hash_mismatch_is_visible(self):
        self.candidate()
        self.setup()
        self.write("compatibility/packages/change/lichii.exe", b"changed")
        task = self.task()
        self.assertEqual(task["stages"]["package"]["reported_status"], "failed")
        self.assertTrue(any("SHA256 mismatch" in n for n in task["import_notes"]))

    def test_package_relative_and_absolute_escapes_are_rejected(self):
        self.candidate()
        outside = self.write("secret.txt")
        self.setup(runtime=[{"path": "../../secret.txt"}, self.evidence(outside)])
        task = self.task()
        self.assertEqual(task["stages"]["package"]["reported_status"], "unknown")
        paths = [e["path"] for e in task["stages"]["package"]["evidence"]]
        self.assertNotIn(str(outside), paths)
        self.assertTrue(any("parent traversal" in n for n in task["import_notes"]))
        self.assertTrue(any("escapes" in n for n in task["import_notes"]))

    def test_missing_and_malformed_json_are_visible(self):
        malformed = self.write("malformed.json", b"{bad")
        self.config["candidate_manifests"] = [str(malformed), str(self.root / "missing.json")]
        tasks = import_tasks(self.config)
        self.assertEqual(tasks[0]["id"], "legacy:import-diagnostics")
        self.assertEqual(len(tasks[0]["import_notes"]), 2)
        self.assertTrue(all(s["status"] == "pending" for s in tasks[0]["stages"].values()))

    def test_same_commit_records_do_not_duplicate_candidate(self):
        self.candidate(name="cpu-pending.json")
        self.candidate(name="scale-pending.json")
        tasks = import_tasks(self.config)
        candidates = [t for t in tasks if t["id"].startswith("candidate:")]
        self.assertEqual(len(candidates), 1)
        self.assertEqual(len(candidates[0]["sources"]), 2)

    def test_partial_commit_does_not_associate_build_or_run(self):
        self.candidate({"candidate_commit": self.commit[:12]})
        self.build()
        self.make_run()
        task = self.task()
        self.assertEqual(task["candidate_commit"], "")
        self.assertEqual(task["stages"]["build"]["status"], "pending")
        self.assertEqual(task["stages"]["gameplay"]["status"], "pending")

    def test_later_manifest_cannot_hide_previous_artifact_mismatch(self):
        self.candidate()
        first = self.build()
        data = json.loads(first.read_text())
        data["artifacts"][0]["sha256"] = "0" * 64
        self.write_json("improvement/builds/aaa-failed/build-manifest.json", data)
        self.assertEqual(self.task()["stages"]["build"]["reported_status"], "failed")

    def test_embedded_commands_remain_inert_data(self):
        marker = self.root / "must-not-exist.txt"
        self.candidate({"candidate_commit": self.commit,
                        "configure_command": "python -c \"open('" + str(marker) + "','w')\"",
                        "validation_commands": [{"argv": ["invalid-program"]}]})
        task = self.task()
        self.assertFalse(marker.exists())
        self.assertEqual(task["stages"]["tests"]["status"], "pending")


if __name__ == "__main__":
    unittest.main()
