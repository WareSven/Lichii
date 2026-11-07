# Development pipeline

The local pipeline connects task ownership, evidence, independent review, builds,
tests, packages and manual gameplay observations. It uses Python 3.11+ and the
standard library. Configuration and state stay outside the source tree.

On this PC the dedicated source checkout is `C:/lx/pipeline-src`, configuration is
`C:/lx/pipeline/config.json`, and the dashboard is `C:/lx/pipeline/dashboard.html`.
The desktop shortcut **Lichii - Pipeline** refreshes imports before opening the
dashboard. The existing improvement-cycle launcher also refreshes this view after
its run. The dashboard is a dated snapshot; opening an old HTML file does not refresh
it. The pipeline does not install a schedule, publish, merge or control the emulator.

A fresh installation can start with this configuration, adjusting absolute paths
to the local checkouts. The first `sync` creates its state and reports. Missing
legacy sources are shown as import errors rather than a clean result.

```json
{
  "schema_version": 1,
  "state_dir": "C:/lx/pipeline/state",
  "report_dir": "C:/lx/pipeline",
  "imports": {
    "legacy_state": "C:/lx/improvement/state/state.json",
    "legacy_config": "C:/lx/improvement/config.json",
    "candidate_globs": ["C:/lx/improvement/*-pending.json"],
    "run_roots": ["C:/lx/compatibility/runs"]
  },
  "commands": {}
}
```

## Daily commands

```powershell
$pipeline='C:/lx/pipeline-src/tools/nx2/work_pipeline.py'
$config='C:/lx/pipeline/config.json'
python -I $pipeline --config $config sync
python -I $pipeline --config $config status
python -I $pipeline --config $config assign candidate:mesh-key-pending `
  --owner coordinator --next-action 'Review the exact candidate and complete the same-scene comparison'
python -I $pipeline --config $config handoff candidate:mesh-key-pending --role review
```

Task IDs are shown by `status`. Imports can deduplicate manifests for one commit;
use the displayed ID instead of assuming it from a filename. The importer reads
the existing backlog, current hypothesis configuration, candidate manifests and
compatibility runs. It checks referenced hashes without rewriting those sources.
Missing, changed and malformed evidence remains visible. Imported results are
labelled **reported**; they never satisfy a validation gate by themselves.

Assignments and explicit validation records survive a refresh. When an imported
candidate changes commit, previous candidate-specific approvals are removed.
Changed stage facts invalidate that stage and its successors; changing a build
manifest's unrelated metadata does not erase a review of the same code. A completed
configured build reconciles its new evidence before recording its result.
Conflicting manual tasks cannot be silently replaced by imports.

## New work and subagents

```powershell
python -I $pipeline --config $config add oot-example `
  --title 'Reproduce the reported rendering error' --game-id 0400486024DEC000 `
  --owner game-agent --next-action 'Read the session log and review upstream diffs'
python -I $pipeline --config $config candidate oot-example --commit FULL_40_DIGIT_COMMIT --actor game-agent
python -I $pipeline --config $config handoff oot-example --role research
```

The coordinating agent delegates one investigation per game, with a separate reviewer.
`handoff` supplies a task brief with the candidate, next action and hashed evidence;
the coordinator sends it through the available subagent tools. It does not pretend
to start a model or mark an agent active. A returning agent provides its commit,
test results, evidence paths and remaining uncertainty. The coordinator records
that result before handing off the next step. Follow `AGENTS.md` for upstream
research and live-game restrictions.

Record stages in this order: `triage`, `upstream`, `implementation`, `review`,
`build`, `tests`, `package`, `gameplay`. The reviewer must differ from both the
owner and implementation author. Actor names are accountable labels, not an
authentication mechanism. Do not approve a review on someone else's behalf.

```powershell
python -I $pipeline --config $config record oot-example --stage review --status passed `
  --actor independent-reviewer --commit FULL_40_DIGIT_COMMIT `
  --evidence C:/lx/evidence/review.txt --details 'Reviewed this diff and its regression test; limitations in the report'
python -I $pipeline --config $config gate oot-example
```

Explicit records are operator attestations: hashing proves evidence integrity,
not the truth of arbitrary prose. Every gate requires nonempty evidence and the
candidate's full commit. Changing an earlier stage invalidates its downstream
stages, including same-commit rebuilds. `gate` rereads hashes and returns 1 until
all stages pass. Known baseline failures remain failures until an explicit review
establishes the accepted comparison; they are never silently converted to success.

## Executing configured checks

`commands` in config.json is trusted operator configuration. Each entry specifies
an argument vector, absolute executable/source/working-directory paths, allowed
task IDs, stage (`build`, `tests` or `package`), timeout and `heavy` flag.
External scripts can be fingerprinted through `inputs`. Commands and scripts must
be reviewed: this is not a sandbox. Evidence documents never supply commands.
Only one aggregate command can own a task/stage, so a second suite's success cannot
overwrite the first suite's failure. Non-Python test commands additionally require
`build_manifest` and `test_binaries`: the manifest must carry the candidate commit
and an `artifacts` list of file `path`/`sha256` records matching every listed binary.

```json
{
  "stage": "tests",
  "task_ids": ["oot-example"],
  "argv": ["C:/tools/cmake/bin/ctest.exe", "--test-dir", "C:/lx/candidate-build", "--output-on-failure", "--no-tests=error"],
  "source": "C:/lx/candidate-src",
  "cwd": "C:/lx/candidate-build",
  "inputs": [],
  "build_manifest": "C:/lx/candidate-build/build-manifest.json",
  "test_binaries": ["C:/lx/candidate-build/bin/tests.exe"],
  "timeout_seconds": 300,
  "heavy": true
}
```

```powershell
python -I $pipeline --config $config run oot-example --command candidate-tests --actor game-agent
```

The source must be clean and match the selected commit. Command exit status,
source state, script fingerprints and task state are checked again after execution.
Zero exit status only proves the configured command succeeded; configure test
commands to fail for empty suites, crashes and unexpected failures. No gameplay
success is inferred. The runner stores immutable per-run logs and result manifests.
Failures return 1; invalid configuration, a changed task or a deferred heavy check
returns 2. Timeouts terminate only the command's process tree.

Heavy checks defer when an emulator is running or process detection is unavailable.
The initial installation leaves heavy candidate builds as explicit operator actions.
The existing candidate build scripts retain their 12-job builds and baseline rules.

`state.json` and its event journal are replaced atomically under an exclusive lock.
An interrupted writer can leave `.state.lock` or `.run.lock`; verify the recorded
process has exited before removing that exact lock. Locks are never stolen.
Concurrent source/task edits invalidate an executing check rather than certifying
results against a different candidate.

## Validation and CI

```powershell
python -m unittest discover -s tools/nx2 -p 'test_work_pipeline*.py' -v
```

Tests cover integrity gates, import conflicts, malicious report text, concurrent
updates and real subprocess success/failure. The repository CI runs these checks.
The release workflow reuses `build-check.yml` from the same commit and requires it
before creating a release tag or building release binaries; see [CI](../CI.md).
These checks do not replace game-specific validation or repository branch rules.
