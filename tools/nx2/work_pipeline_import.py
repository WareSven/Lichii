#!/usr/bin/env python3
"""Read historical evidence without approving any work-pipeline gate.

Only explicit case records in run.json supply gameplay observations. Historical
claims remain ``reported`` even when their current files and hashes agree.
"""

from __future__ import annotations

import hashlib
import json
import re
from datetime import datetime, timezone
from pathlib import Path, PureWindowsPath


STAGES = ("triage", "upstream", "implementation", "review", "build", "tests", "package", "gameplay")
COMMIT = re.compile(r"^[0-9a-fA-F]{40}$")
SHA256 = re.compile(r"^[0-9a-fA-F]{64}$")
GAME_ID = re.compile(r"^[0-9a-fA-F]{16}$")
MAX_JSON = 16 * 1024 * 1024
MAX_EVIDENCE = 1024 * 1024 * 1024


def _text(value):
    return value if isinstance(value, str) else ""


def _commit(value):
    return value.lower() if isinstance(value, str) and COMMIT.fullmatch(value) else ""


def _status(value):
    return {"pass": "passed", "passed": "passed", "fail": "failed", "failed": "failed",
            "known_baseline_failures": "failed"}.get(_text(value).lower(), "unknown")


def _combined(statuses):
    if "failed" in statuses:
        return "failed"
    return "passed" if statuses and all(s == "passed" for s in statuses) else "unknown"


def _unique(items):
    return list({item["path"]: item for item in items}.values())


def _task(identifier, title):
    return {
        "id": identifier, "title": title, "game_id": "", "owner": "unassigned",
        "next_action": "Examiner les preuves importées et attribuer le travail.",
        "candidate_commit": "", "stages": {
            name: {"status": "pending", "reported_status": "unknown", "evidence": [],
                   "actor": "", "commit": "", "details": ""} for name in STAGES},
        "imported": True, "sources": [], "import_notes": [],
        "updated_at": datetime.now(timezone.utc).isoformat(),
        "test_results": [], "packages": [], "gameplay_observations": [],
    }


def _report(task, name, details, evidence=(), status="unknown", actor="historical-import"):
    stage = task["stages"][name]
    stage.update(status="reported", reported_status=status, details=details,
                 evidence=_unique(list(evidence)), actor=actor, commit=task["candidate_commit"])


class _Reader:
    def __init__(self):
        self.cache = {}

    def path(self, value, base=None, within=None):
        if not isinstance(value, (str, Path)) or not str(value):
            raise ValueError("missing path")
        raw = str(value).replace("\\", "/")
        windows = PureWindowsPath(raw)
        if raw.startswith("//") or "://" in raw or "\x00" in raw:
            raise ValueError("network/device paths are not allowed")
        if ".." in raw.split("/"):
            raise ValueError("parent traversal is not allowed")
        if windows.drive and not windows.root:
            raise ValueError("drive-relative paths are not allowed")
        # Colons are permitted only in a Windows drive prefix, never in ADS names.
        remainder = raw[2:] if re.match(r"^[A-Za-z]:", raw) else raw
        if ":" in remainder:
            raise ValueError("alternate streams are not allowed")
        path = Path(raw)
        if not path.is_absolute():
            path = (Path(base) if base else Path.cwd()) / path
        path = path.resolve()
        if within is not None and not path.is_relative_to(Path(within).resolve()):
            raise ValueError("path escapes its evidence directory")
        return path

    def evidence(self, value, notes, base=None, within=None, expected=None):
        try:
            path = self.path(value, base, within)
            info = path.stat()
            if not path.is_file():
                raise ValueError("not a regular file")
            if info.st_size > MAX_EVIDENCE:
                raise ValueError("evidence exceeds the 1 GiB import limit")
            key = (str(path), info.st_size, info.st_mtime_ns)
            digest = self.cache.get(key)
            if digest is None:
                hasher = hashlib.sha256()
                with path.open("rb") as stream:
                    for block in iter(lambda: stream.read(1024 * 1024), b""):
                        hasher.update(block)
                after = path.stat()
                if (info.st_size, info.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
                    raise ValueError("file changed during import")
                digest = hasher.hexdigest()
                self.cache[key] = digest
            result = {"path": str(path), "sha256": digest}
            if expected is not None:
                if not isinstance(expected, str) or not SHA256.fullmatch(expected):
                    notes.append(f"Invalid expected SHA256: {path}")
                    return result, "unknown"
                if digest != expected.lower():
                    notes.append(f"SHA256 mismatch: {path}; expected {expected.lower()}, read {digest}")
                    return result, "failed"
            return result, "passed" if expected is not None else "unknown"
        except (OSError, ValueError, RuntimeError) as error:
            notes.append(f"Unavailable evidence {value!s}: {error}")
            return None, "unknown"

    def document(self, value, notes, base=None, within=None):
        try:
            path = self.path(value, base, within)
            if path.stat().st_size > MAX_JSON:
                raise ValueError("JSON exceeds the 16 MiB import limit")
            raw = path.read_bytes()
            data = json.loads(raw.decode("utf-8-sig"))
            if not isinstance(data, dict):
                raise ValueError("JSON root must be an object")
            return path, data, {"path": str(path), "sha256": hashlib.sha256(raw).hexdigest()}
        except (OSError, ValueError, UnicodeError, RuntimeError) as error:
            notes.append(f"Cannot read JSON {value!s}: {error}")
            return None

    def records(self, entries, notes, base, within=None):
        evidence, statuses = [], []
        if not isinstance(entries, list):
            notes.append("Malformed evidence list.")
            return evidence, ["unknown"]
        for entry in entries:
            if isinstance(entry, str):
                path, expected = entry, None
            elif isinstance(entry, dict):
                path, expected = entry.get("path") or entry.get("run"), entry.get("sha256")
            else:
                notes.append("Malformed evidence entry.")
                statuses.append("unknown")
                continue
            item, status = self.evidence(path, notes, base, within, expected)
            if item:
                evidence.append(item)
            statuses.append(status)
        return evidence, statuses


def _observations(reader, task, runs):
    evidence, statuses, game_ids = [], [], set()
    for path, run, source in runs:
        game = run.get("game")
        if isinstance(game, dict) and GAME_ID.fullmatch(_text(game.get("program_id"))):
            game_ids.add(game["program_id"].upper())
        cases = run.get("cases")
        if not isinstance(cases, dict):
            task["import_notes"].append(f"Missing manual cases object: {path}")
            continue
        task["sources"].append(source)
        for name, case in cases.items():
            if not isinstance(case, dict) or _status(case.get("status")) == "unknown":
                continue
            reported = _status(case["status"])
            attachments = []
            history = case.get("history", [])
            if isinstance(history, list):
                matching = [h for h in history if isinstance(h, dict)
                            and h.get("status") == case.get("status")
                            and h.get("note", "") == case.get("note", "")]
                if matching:
                    attachments = matching[-1].get("evidence", [])
            files, checks = reader.records(attachments, task["import_notes"], path.parent,
                                          within=path.parent)
            if not files:
                task["import_notes"].append(f"No attached manual evidence for {name}: {path}")
            if reported == "passed" and (not files or _combined(checks) != "passed"):
                effective = "unknown"
            else:
                effective = reported
            observation = {"run": str(path), "case": name, "reported_status": reported,
                           "note": _text(case.get("note")), "evidence": [source, *files],
                           "evidence_status": _combined(checks),
                           "commit": _commit((run.get("source") or {}).get("commit"))
                           if isinstance(run.get("source"), dict) else ""}
            task["gameplay_observations"].append(observation)
            if name == "gameplay":
                statuses.append(effective)
                evidence.extend([source, *files])
    if len(game_ids) == 1:
        task["game_id"] = game_ids.pop()
    elif len(game_ids) > 1:
        task["import_notes"].append("Several games share this candidate; observations retain their run paths.")
    if statuses:
        details = "\n".join(f"{Path(o['run']).parent.name}: {o['reported_status']} — {o['note']}"
                            for o in task["gameplay_observations"] if o["case"] == "gameplay")
        _report(task, "gameplay", details, evidence, _combined(statuses), "manual-run-record")
    else:
        task["import_notes"].append("No explicit manual gameplay result in matching run.json files.")


def _tests(reader, task, manifest):
    path, data, source = manifest
    records = data.get("validation", [])
    if not isinstance(records, list) or not records:
        task["import_notes"].append(f"No structured test results: {path}")
        return
    evidence, statuses = [source], []
    for record in records:
        if not isinstance(record, dict):
            task["import_notes"].append(f"Malformed test result: {path}")
            statuses.append("unknown")
            continue
        files, checks = reader.records([{"path": record.get("report"),
                                         "sha256": record.get("report_sha256")}],
                                       task["import_notes"], path.parent)
        status = _status(record.get("status"))
        failures = record.get("failures", [])
        if isinstance(failures, list) and failures:
            status = "failed"
        if status == "passed" and not files:
            status = "unknown"
        if "failed" in checks:
            status = "failed"
        statuses.append(status)
        evidence.extend(files)
        task["test_results"].append({"name": _text(record.get("name")),
                                     "cases": record.get("cases"), "reported_status": status,
                                     "source_status": record.get("status"),
                                     "failures": failures, "evidence": files})
    summary = "\n".join(f"{r['name']}: {r['reported_status']} ({r['cases']} cases; "
                        f"source: {r['source_status']})" for r in task["test_results"])
    existing = task["stages"]["tests"]
    if existing["status"] == "reported":
        statuses.append(existing["reported_status"])
    _report(task, "tests", summary, [*existing["evidence"], *evidence], _combined(statuses))


def _build(reader, task, manifest):
    path, data, source = manifest
    task["sources"].append(source)
    files, checks = reader.records(data.get("artifacts", []), task["import_notes"], path.parent)
    if not files:
        task["import_notes"].append(f"No currently readable build artifacts: {path}")
    existing = task["stages"]["build"]
    if existing["status"] == "reported":
        checks.append(existing["reported_status"])
    details = (existing["details"] + "\n") if existing["details"] else ""
    _report(task, "build", details + f"Historical build manifest: {path}. "
            f"{len(files)} artifact(s) read; hashes checked when supplied.",
            [*existing["evidence"], source, *files], _combined(checks))
    _tests(reader, task, manifest)


def _package(reader, task, setup):
    path, data, source = setup
    task["sources"].append(source)
    try:
        package = reader.path(data.get("package"), path.parent)
        if not package.is_dir():
            raise ValueError("package directory is missing")
    except (OSError, ValueError, RuntimeError) as error:
        task["import_notes"].append(f"Unavailable package from {path}: {error}")
        _report(task, "package", "Package unavailable; no validation imported.", [source])
        return
    files, checks = reader.records(data.get("runtime", []), task["import_notes"], package,
                                  within=package)
    executable = any(Path(item["path"]).name.lower() == "lichii.exe" for item in files)
    if not executable:
        task["import_notes"].append(f"No verified lichii.exe in package inventory: {package}")
        checks.append("unknown")
    result = _combined(checks)
    task["packages"].append({"path": str(package), "files": len(files),
                              "reported_status": result, "evidence": [source, *files]})
    existing = task["stages"]["package"]
    statuses = [result] + ([existing["reported_status"]] if existing["status"] == "reported" else [])
    _report(task, "package", "\n".join(f"{p['path']}: {p['reported_status']} ({p['files']} files)"
                                       for p in task["packages"]),
            [*existing["evidence"], source, *files], _combined(statuses))


def import_tasks(config: dict) -> list[dict]:
    """Import configured local sources, never execute commands or approve gates.

    ``candidate_manifests`` accepts pending records or build manifests. Pending
    records are enriched by exact-commit build manifests beside legacy_config.
    Setup manifests are discovered under ``run_root.parent / 'variants'``.
    Unreadable configured sources become visible diagnostic tasks.
    """
    if not isinstance(config, dict):
        raise ValueError("Import config must be an object")
    reader, tasks, problems = _Reader(), [], []
    legacy_config = reader.document(config["legacy_config"], problems) if config.get("legacy_config") else None
    legacy_state = reader.document(config["legacy_state"], problems) if config.get("legacy_state") else None
    runs, setups, manifests = {}, {}, {}
    run_roots = config.get("run_roots", [])
    if not isinstance(run_roots, list):
        problems.append("run_roots must be a list")
        run_roots = []
    for root in run_roots:
        try:
            directory = reader.path(root)
            if not directory.is_dir():
                raise ValueError("run root directory is missing")
            for path in sorted(directory.glob("*/run.json")):
                document = reader.document(path, problems, within=directory)
                if document:
                    runs[str(document[0])] = document
            variants = directory.parent / "variants"
            if variants.is_dir():
                for path in sorted(variants.glob("*/setup-validation.json")):
                    document = reader.document(path, problems, within=variants)
                    if document:
                        setups[str(document[0])] = document
        except (OSError, ValueError, RuntimeError) as error:
            problems.append(f"Cannot inspect run root {root!s}: {error}")
    if legacy_config:
        build_root = legacy_config[0].parent / "builds"
        if build_root.is_dir():
            for path in sorted(build_root.glob("*/build-manifest.json")):
                document = reader.document(path, problems, within=build_root)
                if document:
                    manifests[str(document[0])] = document

    backlog = {}
    for document, key in ((legacy_state, "backlog"), (legacy_config, "hypotheses")):
        if not document:
            continue
        path, data, source = document
        entries = data.get(key, [])
        if not isinstance(entries, list):
            problems.append(f"Malformed {key} list: {path}")
            continue
        for record in entries:
            if not isinstance(record, dict) or not _text(record.get("id")):
                problems.append(f"Missing task id in {path}")
                continue
            identifier = record["id"]
            if key == "hypotheses" and not identifier.startswith("hypothesis:"):
                identifier = "hypothesis:" + identifier
            old_sources = backlog.get(identifier, (None, None, []))[2]
            backlog[identifier] = (record, path, [*old_sources, source])
    for identifier, (record, path, sources) in backlog.items():
        task = _task("legacy:" + identifier, _text(record.get("title")) or identifier)
        task["sources"] = sources
        files, _ = reader.records(record.get("evidence", []), task["import_notes"], path.parent)
        _report(task, "triage", "Historical work item; current configuration takes precedence over state titles.",
                [*sources, *files])
        case = re.fullmatch(r"case:([0-9a-fA-F]{16}):(.+)", identifier)
        if case:
            task["game_id"] = case[1].upper()
            referenced_runs = []
            for entry in record.get("evidence", []):
                if isinstance(entry, dict) and entry.get("run"):
                    run = reader.document(entry["run"], task["import_notes"], path.parent)
                    if run:
                        referenced_runs.append(run)
            _observations(reader, task, referenced_runs)
        if not files:
            task["import_notes"].append("No readable supporting evidence for this legacy work item.")
        tasks.append(task)

    candidates = config.get("candidate_manifests", [])
    if not isinstance(candidates, list):
        problems.append("candidate_manifests must be a list")
        candidates = []
    by_commit = {}
    for value in candidates:
        notes = []
        document = reader.document(value, notes)
        if not document:
            problems.extend(notes)
            continue
        path, data, source = document
        commit = _commit(data.get("candidate_commit")) or _commit(data.get("commit"))
        identifier = path.parent.name if path.stem == "build-manifest" else path.stem
        title = next((_text(data.get(k)) for k in ("objective", "candidate_change", "mesh_fix", "intended_change", "request", "title")
                      if _text(data.get(k))), identifier)
        if commit and commit in by_commit:
            task = by_commit[commit]
            task["sources"].append(source)
            task["import_notes"].append(f"Additional record for the same commit: {path}; {title}")
            continue
        task = _task("candidate:" + identifier, title)
        task["candidate_commit"] = commit
        task["sources"].append(source)
        task["import_notes"].extend(notes)
        if not commit:
            task["import_notes"].append("Missing full 40-character candidate commit; automatic evidence association disabled.")
        else:
            by_commit[commit] = task
        task["next_action"] = _text(data.get("next")) or task["next_action"]
        _report(task, "triage", "Imported candidate record; its prose and state do not approve gates.", [source])
        _report(task, "implementation", f"Declared candidate commit: {commit or 'unknown'}", [source])
        upstream = {k: data[k] for k in ("upstream_nx2", "upstream_merge", "ported_pr", "ported_head", "retention_pr") if k in data}
        if upstream:
            _report(task, "upstream", json.dumps(upstream, ensure_ascii=False), [source])
        if data.get("source_reviews") or data.get("independent_review"):
            _report(task, "review", "Historical review mentioned; reviewer identity and scope require explicit review evidence.", [source])
        if data.get("build_manifest"):
            linked = reader.document(data["build_manifest"], task["import_notes"], path.parent)
            if linked:
                manifests[str(linked[0])] = linked
        if path.stem == "build-manifest" or isinstance(data.get("artifacts"), list):
            manifests[str(path)] = document
        if data.get("setup_validation"):
            linked = reader.document(data["setup_validation"], task["import_notes"], path.parent)
            if linked:
                setups[str(linked[0])] = linked
        matching_builds = [m for m in manifests.values() if commit and _commit(m[1].get("commit")) == commit]
        if matching_builds:
            for manifest in matching_builds:
                _build(reader, task, manifest)
        else:
            task["import_notes"].append("No build manifest with the exact candidate commit.")
        matching_setups = [s for s in setups.values() if commit and _commit(s[1].get("commit")) == commit]
        if matching_setups:
            for setup in matching_setups:
                _package(reader, task, setup)
        else:
            task["import_notes"].append("No setup manifest with the exact candidate commit; package is unverified.")
        matching_runs = [r for r in runs.values() if commit and isinstance(r[1].get("source"), dict)
                         and _commit(r[1]["source"].get("commit")) == commit]
        _observations(reader, task, matching_runs)
        tasks.append(task)
    if problems:
        task = _task("legacy:import-diagnostics", "Sources d’import à vérifier")
        task["import_notes"] = problems
        task["next_action"] = "Corriger les sources manquantes ou mal formées, puis relancer la synchronisation."
        tasks.append(task)
    for task in tasks:
        task["sources"] = _unique(task["sources"])
        task["import_notes"] = list(dict.fromkeys(task["import_notes"]))
    return tasks
