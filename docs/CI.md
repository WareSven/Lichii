# Lichii validation and release pipeline

Validation and binary releases use two GitHub Actions workflows. Local edits take effect on
GitHub only after they are pushed to the relevant branch. This guide describes
the workflow files, not the status of any particular remote run.

## Events and outputs

| Event | Validation | Packaged binaries | GitHub release |
| --- | --- | --- | --- |
| PR targeting `nx2` or `nx2-experimental` | `build-check` | No | No |
| Push to `nx2` | `build-check` | No | No |
| Push to `nx2-experimental` touching source, tests, build inputs, tools or the validation/release workflows | `build-check`, also required inside `release-binaries` | All platforms | Pre-release after validation and builds succeed |
| Other push to `nx2-experimental` | `build-check` | No | No |
| Manual `build-check` | Selected ref | No | No |
| Manual `release-binaries` | Required for the selected ref | All platforms | Only if `publish_release` is selected; default is **false** |

There is no scheduled build in these files. The existing automatic publication
on qualifying `nx2-experimental` pushes remains enabled.

## Required validation

`build-check.yml` accepts `workflow_call`. The release workflow calls it through
`./.github/workflows/build-check.yml`, so validation uses the same workflow
revision and source commit as the release run. GitHub documents this behavior
for [local reusable workflows](https://docs.github.com/en/actions/how-tos/reuse-automations/reuse-workflows).

The release dependency chain is:

```text
validation → prepare-release → build (all platforms) → release
```

An unsuccessful or cancelled validation prevents tag preparation and packaging.
An unsuccessful platform build prevents publication. No `continue-on-error`
exception bypasses this chain. The packaged configurations can disable tests
because the separate, mandatory validation configuration builds and runs them.

Validation runs on Ubuntu 24.04 and includes:

- Pipeline and shader parser Python tests, before dependency setup and C++ compilation.
- GCC 13 production and test targets, with warnings treated as errors.
- CTest with `--output-on-failure --no-tests=error`.
- Debug adapter, ThreadWorker wake-up and Linux switch-discovery checks.

The directly triggered validation and the validation called from a release use
separate concurrency groups. A newer direct validation can replace an older one
on the same ref; it cannot cancel the release's validation. Running release
workflows are not cancelled by subsequent pushes.

The workflows do not change repository branch-protection settings. Maintainers
must configure required status checks in GitHub separately. A green CI result
establishes the listed code checks; it does not establish correct game rendering,
performance or save/load behavior. GPU probes and manual gameplay evidence remain
separate, and belong in the [local development pipeline](nx2/work-pipeline.md).

## Manual build and publication

Run **Actions → release-binaries → Run workflow**, select the branch, and leave
**Publish the binaries as a GitHub release** disabled for an artifact-only build.
The validation chain runs in both modes. Select publication only when a public
pre-release is intended.

Artifacts are retained for seven days and their names include the source SHA.
The workflow builds Linux x86_64 and ARM64 archives/AppImages, Windows x86_64
archives, and a macOS ARM64 app archive and disk image. Linux and Windows also
have the PGO variants defined by the matrix. Published pre-releases use the tag
`build-<run number>`; the tag is checked against `github.sha` before publication.

Validation and compilation use read-only repository contents permissions.
Only tag preparation and publication request `contents: write`. Release builds
are not triggered by pull requests.

## Reviewing a change

Use the PR template to link the task, the reproduction or regression evidence,
the relevant upstream diffs, the exact candidate commit and independent review.
Record build/test results and gameplay results separately. A known failing test
needs its identity and baseline comparison; CI does not silently ignore it.

If validation fails, inspect its failing command before attempting another
release. If the tested source changes, validate the new commit. A local package
or historical test report is not evidence that a newer remote SHA passed.
