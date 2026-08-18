# AGENTS.md

> **Purpose:** Define the repository-wide workflow, commit hygiene and source-documentation rules
> that every human or automated contributor must follow.

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

## Source documentation standard

Apply this standard to every file an agent creates or changes, including production
code, tests, build files, scripts and Markdown documentation.

### File purpose

- Start every file with a brief purpose statement using the file type's native comment syntax.
- Put the purpose after a required shebang, encoding declaration, license header or Markdown title;
  otherwise make it the first content in the file.
- Use `// Purpose:` for C and C++, `# Purpose:` for CMake or shell-like files, a module docstring for
  Python, and `> **Purpose:**` directly below the title for Markdown.
- Describe the file's responsibility and boundary in one or two sentences. Do not merely repeat the
  filename.

### Logical-block comments

- Put a concise explanatory comment immediately above each meaningful logical block: a public type
  or contract group, a non-trivial function, an algorithm phase, a validation phase, a canonical
  encoding section, a test fixture, or a group of tests proving one behavior.
- Explain intent, invariants, ordering, failure policy or architectural reason. Do not narrate
  obvious syntax or restate the following line.
- Split long algorithms into named phases with comments wherever a reviewer must otherwise infer
  why the steps occur in that order.
- In tests, explain the production contract or regression risk being proved, not the mechanics of
  `REQUIRE` or `CHECK`.
- In Markdown, an informative heading plus its introductory prose counts as the explanation for that
  logical block; add prose when a heading alone does not establish purpose or scope.

### Interesting syntax

- When a language or build construct is important and likely unfamiliar, add a nearby comment that
  begins with `Interesting syntax:` and explains what the construct guarantees or why it is used.
- Reserve this label for genuinely non-obvious constructs such as constrained templates, hidden
  friends, CMake generator expressions or deliberate move-only semantics. Do not flag routine syntax.

### Review requirements

- Keep comments concise, accurate and updated in the same commit as the behavior they describe.
- Remove or revise comments made stale by a change; never leave commented-out code as documentation.
- Prefer clear names and small functions over compensating for unclear code with large comments.
- Before committing, verify that each changed file has its purpose statement and that every
  non-trivial logical block can be understood without reverse-engineering its intent.
