# Contributing to amarqs182/celosia

This repository is the maintained destination for the R36S adaptation of Celosia. Contributions for this work belong here, not in the original repository.

## Repository boundary — mandatory

- Submit all R36S changes to [amarqs182/celosia](https://github.com/amarqs182/celosia).
- The target branch is `main`.
- Never push commits to, or open pull requests against, [HenryKun55/celosia](https://github.com/HenryKun55/celosia).
- Do not configure the Henry repository as a push remote. It is a read-only reference to the original project.
- Do not use GitHub's **Compare & pull request** shortcut until you have checked the base repository. Because this repository is still connected to the original fork network, GitHub may preselect `HenryKun55/celosia`.

A pull request is correctly targeted only when its destination URL is under:

`https://github.com/amarqs182/celosia/pull/`

A PR URL under `HenryKun55/celosia` is the wrong destination for this project.

## Clone and remote setup

For maintainers with write access:

```sh
git clone https://github.com/amarqs182/celosia.git
cd celosia
git remote -v
```

The fetch and push URLs for `origin` must point to `amarqs182/celosia`.

For external contributors, fork **this repository** and clone their own copy. Their `origin` should point to their own fork; the pull request must still target `amarqs182/celosia:main`.

Check remotes before the first push:

```sh
git remote -v
```

Do not push if any push URL points to `HenryKun55/celosia`. If an old `upstream` remote points to Henry, remove it:

```sh
git remote remove upstream
```

Do not add Henry as a writable remote.

## Install the local push guard (required)

Enable the repository hook in each clone:

```sh
git config --local core.hooksPath .githooks
git config --get core.hooksPath
```

The `.githooks/pre-push` hook blocks pushes to `HenryKun55/celosia` and direct pushes to `main`. It applies to normal Git pushes after installation. Do not bypass it with `--no-verify`.

The hook is a client-side safeguard, not a GitHub permission boundary. The pull-request destination must still be checked explicitly. The `Contribution policy` GitHub Action checks every PR that is opened against this fork and requires base `main`.

## Branch and push workflow

Do not commit directly to `main`. Create a topic branch for each change:

```sh
git switch main
git pull --ff-only origin main
git switch -c fix/short-description
# make and test the change
git add <files>
git commit -m "fix: describe the change"
git push -u origin HEAD
```

Use a suitable prefix such as `fix/`, `feat/`, `docs/`, or `test/`.

## Create the pull request

In GitHub's pull request form, check both fields before submitting:

- **Base repository:** `amarqs182/celosia`
- **Base branch:** `main`

The compare/head repository is the contributor's branch. The base/destination repository must remain `amarqs182/celosia`.

For GitHub CLI, specify the destination repository explicitly:

```sh
gh pr create --repo amarqs182/celosia --base main
```

If GitHub shows `HenryKun55/celosia` as the base repository, stop. Do not submit that PR. Close it if it was already created, then create a new PR with the correct base repository.

## Before submitting

- [ ] The change is for the R36S adaptation or this fork's maintenance.
- [ ] The branch is not `main`.
- [ ] No push remote points to `HenryKun55/celosia`.
- [ ] The pull request base repository is `amarqs182/celosia`.
- [ ] The pull request base branch is `main`.
- [ ] Tests relevant to the change have been run, or the PR explains why they could not be run.
