# Repository instructions

## Repository identity and contribution boundary

This repository is `amarqs182/celosia`, the maintained fork for the R36S adaptation.

- The only pull-request destination for this project is `amarqs182/celosia:main`.
- `HenryKun55/celosia` is the original upstream and is read-only reference material.
- Never push commits, branches, tags, or refs to `HenryKun55/celosia`.
- Never open, retarget, or recommend a pull request whose base repository is `HenryKun55/celosia`.
- Never use `git push --mirror`, and never push to a remote merely because it is named `upstream`.
- Do not assume GitHub's default PR base is correct. The fork is still linked to the upstream network, so the UI may preselect the wrong destination.

Before every push:
1. Run `git remote -v` and inspect push URLs.
2. Ensure no push URL points to `HenryKun55/celosia`.
3. Ensure the local branch is not `main`.
4. Use the repository's `.githooks/pre-push` hook (see `CONTRIBUTING.md`).

Before every PR creation or update:
1. Explicitly set the target repository to `amarqs182/celosia`.
2. Explicitly set the target branch to `main`.
3. Verify the resulting PR's base repository and branch after creation.
4. With GitHub CLI, use `gh pr create --repo amarqs182/celosia --base main`.

External contributors may push their working branch to their own fork. Their PR must still target `amarqs182/celosia:main`. Contributors with write access may push topic branches to this repository. Nobody should push directly to `main`.

Do not automatically synchronize `main` from `HenryKun55/celosia` or merge upstream changes into it. If upstream changes are needed, fetch and inspect them first, then propose a deliberate integration PR to this fork. Do not send R36S changes upstream.

## Engineering workflow

- Inspect the current branch, working tree, remotes, recent commits, and relevant code before editing.
- Preserve unrelated changes and existing user work.
- Keep changes minimal and focused; follow KISS, YAGNI, and existing project conventions.
- Build and run the narrowest relevant tests. Do not claim a test passed unless it was actually run.
- Use a dedicated topic branch for each change. Never commit directly to `main`.
- Review the final diff and check for accidental credentials, machine-specific paths, generated files, and unrelated edits.
- Report exactly what changed, what was tested, and what remains unverified.

Read `CONTRIBUTING.md` before pushing or opening a pull request.
