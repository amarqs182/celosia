# Pull Request

<!-- AGENTS.md lifecycle: INSPECT -> PLAN -> BRANCH -> IMPLEMENT -> TEST -> REVIEW -> COMMIT -> PUSH -> PR -> MERGE -> RELEASE -->

## Summary

<!-- What changed and why. Describe the problem being solved, not only the mechanics. -->

## Changes

<!-- Group by area: PS2Recomp (ps2xAnalyzer / ps2xRecomp / ps2xRuntime / ps2xIOP / ps2xTest / ps2xStudio), game/overrides, celosia script, docs, CI. -->

-

## Branch
<!-- Name per the conventions: feat/* fix/* docs/* refactor/* perf/* (hotfix/* for urgent production fixes). -->

- Branch: `<feat|fix|docs|refactor|perf>/<name>`
- Base: `origin/main`

## Validation

- [ ] `PS2Recomp` builds (CMake configure + build, no new warnings)
- [ ] `ps2x_tests` passes — summary line pasted below
- [ ] `ctest` `gs_cache` suites pass (the `gs_cache.*` tests)
- [ ] `git diff --check` is clean (no whitespace errors, LF endings preserved)
- [ ] `git status` reviewed — nothing outside the task scope
- [ ] No secrets, credentials, tokens, keys or game data in the diff

`ps2x_tests` summary pasted here: `Passed: <n>` / `Failed: <n>`.

## Game-specific checklist
<!-- Only when touching game/overrides (native overrides for Shadow of the Colossus). -->

- [ ] `SOTC_DIFFTEST` reports 0 mismatches on the affected route (paste the run)
- [ ] `PS2X_PROFILE` before and after pasted in the description (sample counts, hot pcs)

## Known limitations

-

## Linked issues / worklist items

- Closes #
- Worklist:
