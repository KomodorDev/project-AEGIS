# AGENTS.md

## Git workflow

- `dev` is the integration branch.
- Always start new work from `dev`.
- Create a separate feature branch for each task.
- Open every pull request or merge request with `dev` as the base/target branch.
- Never open a pull request targeting `main`.
- Never commit or push directly to `main` or `dev`.
- Only create a pull request from `dev` into `main` when the user explicitly requests a release.
- Before opening a pull request, verify that its base branch is `dev`.

## Commit structure

- When a task has natural boundaries, split the pull request into coherent, single-purpose commits.
- Each commit should be independently understandable and reviewable; keep directly related code,
  tests, and documentation together where practical.
- Do not split changes mechanically when they are inseparable or when doing so would create broken or
  misleading intermediate commits.
- Before publishing, review the commit history and avoid one giant commit when a clearer decomposition
  makes sense.
