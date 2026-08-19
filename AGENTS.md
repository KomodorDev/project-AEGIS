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

### C and C++ divider hierarchy

Use matching visual boundaries around every class-like type and function-level block in C and C++
headers, sources, tests, support code and benchmarks. The divider hierarchy is fixed from strongest
to weakest:

- Class/type boundary: `// ########################################################################`
- Method/function boundary: `// --------------------------------------------------------`
- Internal algorithm-step boundary: `// ++++++++++++++++++++++++++++++++++++++++`

Apply the boundaries as follows:

- Put the opening divider first and the explanatory comment immediately after it. The description
  must explain responsibility, invariant or intent before the declaration or code it governs.
- Close every governed block with the same divider immediately after it. The final class, function
  or algorithm phase in a file must also have an explicit closing divider; reaching a namespace
  close or end of file is not an implicit close.
- Use the class/type divider for `class`, `struct`, `union` and `enum class` definitions or forward
  declarations, and for named concept/type-alias contract groups. Continue to use method/function
  dividers for functions declared or defined inside class-like types.
- Use the method/function divider for constructors, methods, free functions, operators, factories,
  test cases and benchmark callbacks. Closely related overloads may form one documented block.
- Use the step divider only inside longer function bodies where validation, transformation,
  encoding, commit or other meaningful phases need to be distinguished. Close the final phase
  before the function's closing brace.
- Adjacent blocks at the same level may share one divider as the previous block's close and the next
  block's open. In that case, put the next block's description immediately after the shared divider.
- Keep namespace-closing comments outside the class/function boundary they contain.

Example:

```cpp
// ########################################################################
// Owns validated orders and preserves canonical insertion order.
class OrderBook final {
public:
  // --------------------------------------------------------
  // Validates and inserts one order without partial mutation.
  [[nodiscard]] Result<void> add(Order order);
  // --------------------------------------------------------
};
// ########################################################################

// --------------------------------------------------------
// Validates and inserts one order without partial mutation.
Result<void> OrderBook::add(Order order) {
  // ++++++++++++++++++++++++++++++++++++++++
  // Reject invalid input before changing owned state.
  auto validation = validate(order);
  if (!validation) {
    return validation;
  }
  // ++++++++++++++++++++++++++++++++++++++++
  // Commit only after every fallible operation has succeeded.
  orders_.push_back(std::move(order));
  return Result<void>::success();
  // ++++++++++++++++++++++++++++++++++++++++
}
// --------------------------------------------------------
```

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
