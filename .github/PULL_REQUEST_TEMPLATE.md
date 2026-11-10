<!--
Mandatory destination:
Base repository: amarqs182/celosia
Base branch: main

Do not submit this work to HenryKun55/celosia. GitHub may preselect the
original repository because this project is still attached to its fork
network. Verify the base repository before creating the PR.
-->

# Pull Request

## Destination confirmation

- [ ] Base repository is `amarqs182/celosia`.
- [ ] Base branch is `main`.
- [ ] This PR is not targeting `HenryKun55/celosia`.

## Summary

<!-- What changed and why. Describe the problem being solved, not only the mechanics. -->

## Changes

<!-- Group by area: PS2Recomp (ps2xAnalyzer / ps2xRecomp / ps2xRuntime / ps2xIOP / ps2xTest / ps2xStudio), game/overrides, celosia script, docs, CI. -->

-

## Branch
<!-- Topic branch, never a direct commit to main. The pull request targets amarqs182/celosia:main. -->

- Branch: `<feat|fix|refactor|perf|docs|hotfix>/<name>`
- Base: `amarqs182/celosia:main`

## Validation checklist

- [ ] `PS2Recomp` builds (CMake configure + build, no new warnings)
- [ ] `ps2x_tests` passes - summary line pasted below
- [ ] `ctest` `gs_cache` suites pass (the `gs_cache.*` tests)
- [ ] `git diff --check` is clean (no whitespace errors, LF endings preserved)
- [ ] `git status` reviewed - nothing outside the task scope
- [ ] No secrets, credentials, tokens, keys or game data in the diff

`ps2x_tests` summary pasted here: `Passed: <n>` / `Failed: <n>`.

## Game-specific checklist
<!-- Only when touching game/overrides (native overrides for Shadow of the Colossus). -->

- [ ] `SOTC_DIFFTEST` reports 0 mismatches on the affected route (paste the run)
- [ ] `PS2X_PROFILE` before and after pasted in the description (sample counts, hot pcs)

## Known limitations

-

## Linked issues or worklist items

- Closes #
- Worklist:

<!-- Any validation that was not executed must be explained here. -->

- [ ] Any unrun validation is explained below.
