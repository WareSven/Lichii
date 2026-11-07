#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later

import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import work_pipeline_core as core


class PipelineCoreTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.evidence = self.root / "test-result.txt"
        self.evidence.write_text("Test passed; observed output recorded.\n", encoding="utf-8")
        self.task = core.new_task("test:one", "Candidate one", owner="author", next_action="Review result")
        core.set_candidate(self.task, "a" * 40, "author")

    def record(self, stage, status="passed", actor=None):
        return core.record_stage(self.task, stage, status, [self.evidence],
                                 actor or ("reviewer" if stage == "review" else "author"))

    def complete(self):
        for stage in core.STAGES:
            self.record(stage)

    def test_ready_requires_all_gates_and_independent_review(self):
        self.assertFalse(core.assess_task(self.task)["ready"])
        self.complete()
        assessment = core.assess_task(self.task)
        self.assertTrue(assessment["ready"])
        self.assertEqual(assessment["phase"], "ready")

    def test_changed_evidence_invalidates_gate(self):
        self.complete()
        self.evidence.write_text("Result changed", encoding="utf-8")
        assessment = core.assess_task(self.task)
        self.assertFalse(assessment["ready"])
        self.assertTrue(any("modifiée" in text for text in assessment["blockers"]))

    def test_missing_evidence_invalidates_gate(self):
        self.complete()
        self.evidence.unlink()
        self.assertFalse(core.assess_task(self.task)["ready"])

    def test_import_source_changed_without_sync_invalidates_gate(self):
        source = self.root / "import-manifest.json"
        source.write_text('{"result": "passed"}', encoding="utf-8")
        self.task.update(imported=True, sources=[core.fingerprint(source)])
        self.complete()
        self.assertTrue(core.assess_task(self.task)["ready"])
        source.write_text('{"result": "failed"}', encoding="utf-8")
        assessment = core.assess_task(self.task)
        self.assertFalse(assessment["ready"])
        self.assertEqual(assessment["phase"], "sources")
        self.assertTrue(all(result["valid"] for result in assessment["stages"].values()))

    def test_import_source_missing_invalidates_gate(self):
        source = self.root / "import-manifest.json"
        source.write_text('{"result": "passed"}', encoding="utf-8")
        self.task.update(imported=True, sources=[core.fingerprint(source)])
        self.complete()
        source.unlink()
        self.assertFalse(core.assess_task(self.task)["ready"])

    def test_import_without_source_never_passes(self):
        self.task["imported"] = True
        self.complete()
        self.assertFalse(core.assess_task(self.task)["ready"])

    def test_candidate_change_invalidates_commit_bound_stages(self):
        self.complete()
        core.set_candidate(self.task, "b" * 40, "author")
        self.assertEqual(self.task["stages"]["triage"]["status"], "passed")
        for stage in core.STAGES[1:]:
            self.assertEqual(self.task["stages"][stage]["status"], "pending")
        self.assertFalse(core.assess_task(self.task)["ready"])

    def test_unchanged_candidate_preserves_results(self):
        self.complete()
        core.set_candidate(self.task, "A" * 40, "author")
        self.assertTrue(core.assess_task(self.task)["ready"])

    def test_mismatched_commit_cannot_be_recorded(self):
        with self.assertRaises(core.PipelineError):
            core.record_stage(self.task, "build", "passed", [self.evidence], "author", commit="b" * 40)

    def test_mismatched_persisted_commit_invalidates_gate(self):
        self.complete()
        self.task["stages"]["tests"]["commit"] = "b" * 40
        self.assertFalse(core.assess_task(self.task)["ready"])

    def test_reviewer_cannot_be_owner_or_author(self):
        self.record("implementation", actor="developer")
        for actor in ("author", "developer", " AUTHOR ", "DEVELOPER"):
            with self.subTest(actor=actor), self.assertRaises(core.PipelineError):
                self.record("review", actor=actor)

    def test_persisted_self_review_fails_assessment(self):
        self.complete()
        self.task["stages"]["review"]["actor"] = "author"
        self.assertFalse(core.assess_task(self.task)["ready"])

    def test_reported_import_does_not_pass(self):
        self.complete()
        self.task["stages"]["tests"]["status"] = "reported"
        self.task["stages"]["tests"]["reported_status"] = "passed"
        self.assertFalse(core.assess_task(self.task)["ready"])

    def test_failure_blocks_readiness(self):
        self.complete()
        self.record("tests", status="failed")
        self.assertFalse(core.assess_task(self.task)["ready"])

    def test_new_build_invalidates_tests_and_package_even_on_same_commit(self):
        self.complete()
        self.record("build")
        self.assertTrue(core.assess_task(self.task)["stages"]["review"]["valid"])
        for stage in ("tests", "package", "gameplay"):
            self.assertEqual(self.task["stages"][stage]["status"], "pending")
        self.assertFalse(core.assess_task(self.task)["ready"])

    def test_new_implementation_invalidates_independent_review(self):
        self.complete()
        self.record("implementation")
        self.assertEqual(self.task["stages"]["review"]["status"], "pending")

    def test_owner_and_next_action_are_required(self):
        self.complete()
        self.task["owner"] = "unassigned"
        self.assertFalse(core.assess_task(self.task)["ready"])
        with self.assertRaises(core.PipelineError):
            self.record("triage")
        self.task["owner"] = "author"
        self.task["next_action"] = " "
        with self.assertRaises(core.PipelineError):
            self.record("triage")

    def test_invalid_evidence_or_status_rejected(self):
        empty = self.root / "empty.txt"
        empty.touch()
        for paths in ([], [empty], [self.root], [self.root / "absent.txt"], str(self.evidence)):
            with self.subTest(paths=paths), self.assertRaises(core.PipelineError):
                core.record_stage(self.task, "build", "passed", paths, "author")
        with self.assertRaises(core.PipelineError):
            core.record_stage(self.task, "build", "reported", [self.evidence], "author")

    def test_short_commit_and_bad_game_id_rejected(self):
        with self.assertRaises(core.PipelineError):
            core.set_candidate(self.task, "abcdef", "author")
        with self.assertRaises(core.PipelineError):
            core.new_task("id", "Title", game_id="short")

    def test_transaction_updates_state_and_event_together(self):
        store = core.StateStore(self.root / "state")
        self.assertEqual(store.read()["tasks"], [])
        self.assertFalse(store.directory.exists())
        state = store.update(lambda value: value["tasks"].append(self.task), {"action": "create"})
        self.assertEqual(state["revision"], 1)
        self.assertEqual(store.read()["tasks"][0]["id"], self.task["id"])
        self.assertEqual(store.read()["event_log"][0]["action"], "create")
        self.assertFalse(store.lock_path.exists())

    def test_failed_mutation_does_not_change_saved_state(self):
        store = core.StateStore(self.root / "state")
        store.update(lambda value: value["tasks"].append(self.task), {"action": "create"})
        before = store.path.read_bytes()
        def fail(value):
            value["tasks"].clear()
            raise core.PipelineError("Deliberate error")
        with self.assertRaises(core.PipelineError):
            store.update(fail, {"action": "bad"})
        self.assertEqual(before, store.path.read_bytes())
        self.assertFalse(store.lock_path.exists())

    def test_lock_excludes_writer_and_is_not_deleted(self):
        store = core.StateStore(self.root)
        store.lock_path.write_text("existing writer", encoding="utf-8")
        with self.assertRaises(core.PipelineError):
            store.update(lambda _: None, {"action": "noop"})
        self.assertEqual(store.lock_path.read_text(encoding="utf-8"), "existing writer")

    def test_corrupt_state_is_not_replaced(self):
        store = core.StateStore(self.root)
        store.path.write_text("{broken", encoding="utf-8")
        with self.assertRaises(core.PipelineError):
            store.read()
        with self.assertRaises(core.PipelineError):
            store.update(lambda _: None, {"action": "noop"})
        self.assertEqual(store.path.read_text(encoding="utf-8"), "{broken")

    def test_duplicate_tasks_and_corrupt_schema_rejected(self):
        store = core.StateStore(self.root)
        with self.assertRaises(core.PipelineError):
            store.update(lambda value: value["tasks"].extend([self.task, self.task]), {"action": "bad"})
        store.path.write_text(json.dumps({"schema_version": 2}), encoding="utf-8")
        with self.assertRaises(core.PipelineError):
            store.read()

    def test_mutator_cannot_rewrite_event_log(self):
        store = core.StateStore(self.root)
        store.update(lambda _: None, {"action": "first"})
        store.update(lambda value: value["event_log"].clear(), {"action": "second"})
        self.assertEqual([event["action"] for event in store.read()["event_log"]], ["first", "second"])

    def test_malformed_status_fails_closed(self):
        self.task["stages"]["tests"]["status"] = []
        self.assertFalse(core.assess_task(self.task)["ready"])
        store = core.StateStore(self.root)
        with self.assertRaises(core.PipelineError):
            store.update(lambda value: value["tasks"].append(self.task), {"action": "bad"})


if __name__ == "__main__":
    unittest.main()
