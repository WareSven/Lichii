#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Local task state and evidence gates for the NX2 development pipeline."""

from copy import deepcopy
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import tempfile


STAGES = ("triage", "upstream", "implementation", "review", "build", "tests", "package", "gameplay")
STATUSES = {"pending", "reported", "passed", "failed"}
_SHA1 = re.compile(r"[0-9a-fA-F]{40}\Z")
_SHA256 = re.compile(r"[0-9a-fA-F]{64}\Z")
_UNASSIGNED = {"", "unassigned", "unknown", "none", "non attribué", "non attribue"}
_LABELS = {
    "triage": "Qualification", "upstream": "Vérification upstream",
    "implementation": "Correctif", "review": "Revue indépendante", "build": "Compilation",
    "tests": "Tests", "package": "Paquet", "gameplay": "Validation en jeu",
}


class PipelineError(Exception):
    """Invalid state, missing evidence, or an unavailable state transaction."""


def utc_now():
    return datetime.now(timezone.utc).isoformat()


def _actor(value):
    return isinstance(value, str) and value.strip().casefold() not in _UNASSIGNED


def _commit(value):
    return isinstance(value, str) and _SHA1.fullmatch(value) is not None


def _pending():
    return {"status": "pending", "evidence": [], "actor": "", "commit": None, "details": ""}


def fingerprint(path):
    """Record an existing evidence file; directories and empty files are not evidence."""
    try:
        resolved = Path(path).resolve(strict=True)
        if not resolved.is_file() or resolved.stat().st_size == 0:
            raise PipelineError(f"Evidence must be a nonempty file: {resolved}")
        digest = hashlib.sha256()
        with resolved.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(chunk)
        return {"path": str(resolved), "sha256": digest.hexdigest()}
    except (OSError, ValueError, TypeError) as error:
        raise PipelineError(f"Cannot read evidence {path!r}: {error}") from error


def new_task(id, title, game_id=None, owner="unassigned", next_action=""):
    if not isinstance(id, str) or not id.strip():
        raise PipelineError("A task needs a nonempty ID.")
    if not isinstance(title, str) or not title.strip():
        raise PipelineError("A task needs a nonempty title.")
    if not isinstance(owner, str) or not isinstance(next_action, str):
        raise PipelineError("Owner and next action must be strings.")
    if game_id is not None and (not isinstance(game_id, str)
                                or re.fullmatch(r"[0-9a-fA-F]{16}", game_id) is None):
        raise PipelineError("A game ID must have 16 hexadecimal digits.")
    now = utc_now()
    return {
        "id": id.strip(), "title": title.strip(), "game_id": game_id,
        "owner": owner.strip(), "next_action": next_action.strip(),
        "candidate_commit": None, "stages": {stage: _pending() for stage in STAGES},
        "imported": False, "sources": [], "created_at": now, "updated_at": now,
    }


def set_candidate(task, commit, actor):
    if not _commit(commit):
        raise PipelineError("The candidate requires a full 40-digit Git commit.")
    if not _actor(actor):
        raise PipelineError("A candidate change requires an identified actor.")
    commit = commit.lower()
    old_commit = task.get("candidate_commit")
    if old_commit != commit:
        task["candidate_commit"] = commit
        task.setdefault("stages", {})
        for stage in STAGES[1:]:
            task["stages"][stage] = _pending()
        task["candidate_changed_by"] = actor.strip()
        task["updated_at"] = utc_now()
    return task


def record_stage(task, stage, status, evidence_paths, actor, details="", commit=None):
    if stage not in STAGES:
        raise PipelineError(f"Unknown stage: {stage}")
    if not isinstance(status, str) or status not in {"passed", "failed"}:
        raise PipelineError("An explicit stage result must be passed or failed.")
    if not _actor(actor):
        raise PipelineError("A stage result requires an identified actor.")
    if not _actor(task.get("owner")):
        raise PipelineError("Assign an owner before recording a stage result.")
    if not isinstance(task.get("next_action"), str) or not task["next_action"].strip():
        raise PipelineError("Set the next action before recording a stage result.")
    if not isinstance(details, str):
        raise PipelineError("Stage details must be text.")
    candidate = task.get("candidate_commit")
    commit = candidate if commit is None and stage != "triage" else commit
    if stage != "triage" and (not _commit(candidate) or not _commit(commit)
                               or commit.lower() != candidate.lower()):
        raise PipelineError("The stage must refer to the current full candidate commit.")
    if stage == "triage" and commit is not None and (not _commit(commit)
                                                    or commit.lower() != str(candidate).lower()):
        raise PipelineError("The triage commit must match the candidate when specified.")
    if stage == "review":
        author = task.get("stages", {}).get("implementation", {}).get("actor")
        if not _actor(author):
            raise PipelineError("Record the implementation author before the independent review.")
        if actor.strip().casefold() in {author.strip().casefold(), task["owner"].strip().casefold()}:
            raise PipelineError("The reviewer must differ from the owner and implementation author.")
    if isinstance(evidence_paths, (str, bytes, Path)):
        raise PipelineError("Evidence paths must be a list of files.")
    try:
        evidence = [fingerprint(path) for path in evidence_paths]
    except TypeError as error:
        raise PipelineError("Evidence paths must be a list of files.") from error
    if not evidence:
        raise PipelineError("A stage result needs at least one evidence file.")
    task.setdefault("stages", {})[stage] = {
        "status": status, "evidence": evidence, "actor": actor.strip(),
        "commit": commit.lower() if commit else None, "details": details,
        "recorded_at": utc_now(),
    }
    for dependent in STAGES[STAGES.index(stage) + 1:]:
        task["stages"][dependent] = _pending()
    task["updated_at"] = utc_now()
    return task


def assess_task(task):
    """Recheck gates from current files, without trusting cached readiness or import prose."""
    blockers = []
    if not _actor(task.get("owner")):
        blockers.append("Responsable à attribuer.")
    if not isinstance(task.get("next_action"), str) or not task["next_action"].strip():
        blockers.append("Prochaine action à renseigner.")
    candidate = task.get("candidate_commit")
    if not _commit(candidate):
        blockers.append("Révision candidate complète à renseigner.")
    stages = task.get("stages", {})
    if not isinstance(stages, dict):
        stages = {}
    results = {}
    fingerprints = {}

    def check_evidence(evidence):
        problems = []
        if not isinstance(evidence, list) or not evidence:
            return ["Preuves manquantes."]
        for item in evidence:
            if not isinstance(item, dict) or not isinstance(item.get("path"), str):
                problems.append("Référence de preuve invalide.")
                continue
            path, expected = item["path"], item.get("sha256")
            if not Path(path).is_absolute() or not isinstance(expected, str) or not _SHA256.fullmatch(expected):
                problems.append(f"Empreinte ou chemin de preuve invalide: {path}")
                continue
            if path not in fingerprints:
                try:
                    fingerprints[path] = fingerprint(path)["sha256"]
                except PipelineError:
                    fingerprints[path] = None
            actual = fingerprints[path]
            if actual is None:
                problems.append(f"Preuve absente ou illisible: {path}")
            elif actual != expected.lower():
                problems.append(f"Preuve modifiée depuis validation: {path}")
        return problems

    source_problems = []
    if task.get("imported") or task.get("sources"):
        source_problems = check_evidence(task.get("sources"))
        blockers.extend(f"Source importée: {problem}" for problem in source_problems)
    for stage in STAGES:
        record = stages.get(stage, {})
        if not isinstance(record, dict):
            record = {}
        status = record.get("status", "pending")
        if not isinstance(status, str):
            status = "invalid"
        problems = []
        if status == "reported":
            problems.append("Résultat importé à confirmer; aucune validation automatique.")
        elif status == "failed":
            problems.append("Échec enregistré.")
        elif status == "pending":
            problems.append("Étape en attente.")
        elif status != "passed":
            problems.append("État invalide.")
        if status == "passed":
            if not _actor(record.get("actor")):
                problems.append("Auteur du résultat manquant.")
            commit = record.get("commit")
            if stage != "triage" and (not _commit(commit) or not _commit(candidate)
                                       or commit.lower() != candidate.lower()):
                problems.append("Résultat associé à une autre révision ou sans révision.")
            if stage == "triage" and commit is not None and (not _commit(commit)
                                                               or commit.lower() != str(candidate).lower()):
                problems.append("Qualification associée à une autre révision.")
            if stage == "review":
                implementation = stages.get("implementation", {})
                author = implementation.get("actor") if isinstance(implementation, dict) else None
                reviewer = record.get("actor", "")
                excluded = {str(author).strip().casefold(), str(task.get("owner")).strip().casefold()}
                if not _actor(author) or str(reviewer).strip().casefold() in excluded:
                    problems.append("Revue indépendante manquante.")
        evidence = record.get("evidence", [])
        if status in {"passed", "failed", "reported"}:
            problems.extend(check_evidence(evidence))
        valid = status == "passed" and not problems
        results[stage] = {"status": status, "valid": valid, "problems": problems}
        blockers.extend(f"{_LABELS[stage]}: {problem}" for problem in problems)
    phase = next((stage for stage in STAGES if not results[stage]["valid"]), "ready")
    ready = not blockers
    if phase == "ready" and not ready:
        phase = "sources" if source_problems else "assignment"
    labels = dict(_LABELS, ready="Prêt à intégrer", assignment="Attribution / suivi", sources="Sources à actualiser")
    label = labels[phase]
    return {"phase": phase, "label": label, "ready": ready, "blockers": blockers, "stages": results}


def _empty_state():
    return {"schema_version": 1, "revision": 0, "tasks": [], "event_log": [], "updated_at": utc_now()}


def _validate_state(state):
    if (not isinstance(state, dict) or isinstance(state.get("schema_version"), bool)
            or state.get("schema_version") != 1):
        raise PipelineError("Unsupported or corrupt pipeline state schema.")
    if not isinstance(state.get("tasks"), list) or not isinstance(state.get("event_log"), list):
        raise PipelineError("The state requires tasks and event_log lists.")
    revision = state.get("revision", 0)
    if not isinstance(revision, int) or isinstance(revision, bool) or revision < 0:
        raise PipelineError("Invalid state revision.")
    identifiers = set()
    for task in state["tasks"]:
        if not isinstance(task, dict) or not isinstance(task.get("id"), str) or not task["id"].strip():
            raise PipelineError("Invalid task in pipeline state.")
        if task["id"] in identifiers:
            raise PipelineError(f"Duplicate task ID: {task['id']}")
        identifiers.add(task["id"])
        if not isinstance(task.get("title"), str) or not task["title"].strip():
            raise PipelineError(f"Missing task title: {task['id']}")
        if not isinstance(task.get("stages"), dict):
            raise PipelineError(f"Invalid stages: {task['id']}")
        for stage, record in task["stages"].items():
            if (stage not in STAGES or not isinstance(record, dict)
                    or not isinstance(record.get("status"), str) or record["status"] not in STATUSES):
                raise PipelineError(f"Invalid stage record: {task['id']}/{stage}")
    if any(not isinstance(event, dict) for event in state["event_log"]):
        raise PipelineError("Invalid event log.")


class StateStore:
    def __init__(self, directory):
        self.directory = Path(directory).resolve()
        self.path = self.directory / "state.json"
        self.lock_path = self.directory / ".state.lock"

    def read(self):
        try:
            with self.path.open("r", encoding="utf-8-sig") as stream:
                state = json.load(stream)
        except FileNotFoundError:
            return _empty_state()
        except (OSError, ValueError) as error:
            raise PipelineError(f"Cannot read pipeline state {self.path}: {error}") from error
        _validate_state(state)
        return state

    def update(self, mutator, event):
        """Apply one mutation and its audit event in the same atomic state replacement."""
        if not isinstance(event, dict) or not event:
            raise PipelineError("A state mutation requires an audit event.")
        try:
            self.directory.mkdir(parents=True, exist_ok=True)
            lock_fd = os.open(self.lock_path, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
        except FileExistsError as error:
            raise PipelineError(f"Another writer owns {self.lock_path}; inspect it before retrying.") from error
        except OSError as error:
            raise PipelineError(f"Cannot lock pipeline state: {error}") from error
        temporary = None
        try:
            with os.fdopen(lock_fd, "w", encoding="utf-8") as lock:
                json.dump({"pid": os.getpid(), "created_at": utc_now()}, lock)
                lock.flush()
                os.fsync(lock.fileno())
            state = self.read()
            previous_revision = state.get("revision", 0)
            previous_events = deepcopy(state["event_log"])
            mutator(state)
            # Mutators cannot rewrite historical audit events.
            state["event_log"] = previous_events
            state["revision"] = previous_revision + 1
            state["updated_at"] = utc_now()
            logged = deepcopy(event)
            logged["recorded_at"] = state["updated_at"]
            logged["revision"] = state["revision"]
            state["event_log"].append(logged)
            _validate_state(state)
            serialized = json.dumps(state, ensure_ascii=False, indent=2, allow_nan=False) + "\n"
            with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", prefix=".state-", suffix=".tmp",
                                             dir=self.directory, delete=False) as stream:
                temporary = Path(stream.name)
                stream.write(serialized)
                stream.flush()
                os.fsync(stream.fileno())
            os.replace(temporary, self.path)
            return state
        except (OSError, ValueError, TypeError) as error:
            raise PipelineError(f"Pipeline state transaction failed: {error}") from error
        finally:
            if temporary is not None:
                temporary.unlink(missing_ok=True)
            self.lock_path.unlink(missing_ok=True)
