# AGENTS.md

> **Purpose:** Define the repository-wide workflow, commit hygiene, source-naming and documentation
> rules that every human or automated contributor must follow.

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

## Function and method naming

Apply this standard to every new or renamed function, method and helper whenever the repository
controls its name, including local, test-fixture and benchmark helpers. Names fixed by the language
or a required interface, such as C++ constructors, destructors and overloaded operators, are
exempt. A declaration and each call site must communicate the primary action or returned fact
without requiring the reader to inspect the implementation or an adjacent comment. When changing
an existing operation's behavior, verify that its current name still describes its action, result
and caller-relevant failure policy; rename it when it no longer does.

- Name a command—an operation used primarily to cause an effect or enforce a requirement—with a
  specific imperative, or command-form, verb in its base form. Include the object or effect when
  the verb alone would be ambiguous; for example, use `insert_order`, `cancel_route` or
  `require_healthy`.
- Name a query—an operation used primarily to return information without a caller-visible side
  effect—after the fact it returns. A caller-visible side effect is a change beyond the returned
  value, such as mutating stored state or writing output. Use question-form prefixes such as `is_`,
  `has_`, `can_` or `should_` for Boolean (`true` or `false`) results. Use a precise result name,
  such as `active_order_count` or `order_id`, for an accessor that directly returns the named
  property. When the query performs a meaningful computation or search, name that operation
  explicitly; for example, use `calculate_worst_case_exposure`, `derive_digest`,
  `summarize_samples` or `find_route`. Do not add an artificial imperative verb when a directly
  returned fact is already unambiguous.
- Name a conversion, parser or factory after both its operation and the value it produces or its
  source; for example, use `to_wire_bytes`, `parse_identifier`, `route_from_config` or
  `create_market_runtime`. A factory is an operation that constructs and returns a value.
- Include deliberate, caller-relevant failure behavior in a local helper's name when its return type
  does not communicate that behavior. This means failure behavior defined by the operation's
  contract, not an incidental implementation failure such as memory allocation. For example, use
  `parse_identifier_or_throw<Identifier>(text)` for a helper that parses identifier text and
  converts invalid input into an exception; do not use `id<Identifier>(text)`, which hides both the
  parsing action and the failure policy.
- Do not use a bare noun, generic verb or unexplained abbreviation—such as `id`, `get`, `do`,
  `run`, `process` or `handle`—when it leaves the specific action or result unclear. An
  established technical abbreviation may remain as a qualified noun, such as `order_id`,
  `sha256_digest` or `utc_timestamp`, when the complete name still communicates the behavior or
  returned fact.

## Source documentation standard

Apply this standard to every file an agent creates or changes, including production
code, tests, build files, scripts and Markdown documentation.

### File purpose

- Start every file with a brief purpose statement using the file type's native comment syntax.
- Put the purpose after a required shebang, encoding declaration, license header or Markdown title;
  otherwise make it the first content in the file.
- Use `// Purpose:` for C and C++, `# Purpose:` for CMake or shell-like files, a module docstring for
  Python, and `> **Purpose:**` directly below the title for Markdown.
- If a format permits no native comment syntax, keep the file valid and record the purpose in a
  schema-supported descriptive, extension or vendor metadata field. Put that field as early as the
  format permits, after any required format or version discriminator, and state the same
  responsibility and boundary required of a comment. Do not invent an unsupported field.
- If the format permits neither comments nor schema-supported descriptive metadata, record the
  exact file path plus its responsibility and boundary in the nearest repository-owned Markdown
  document that governs that file. This path-specific entry is the only external-purpose fallback.
- This metadata exception applies only when native comments are illegal. Comment-capable formats
  must still use their native comment syntax in the first eligible position and cannot substitute
  metadata. `CMakePresets.json` complies through its top-level
  `vendor["aegis.dev/documentation/1.0"].purpose` field immediately after `version`.
- Describe the file's responsibility and boundary in one or two sentences. Do not merely repeat the
  filename.

### Logical-block comments

These requirements are a project-specific application of the C++ Core Guidelines on
[function specifications][cpp-functions], [class invariants][cpp-class-invariants],
[non-redundant comments][cpp-comment-code], [intent comments][cpp-comment-intent],
[crisp comments][cpp-comment-crisp], [preconditions][cpp-preconditions] and
[postconditions][cpp-postconditions], including the preference for
[checkable preconditions][cpp-expects] and [checkable postconditions][cpp-ensures]. Apply them to
every language used in this repository.

[cpp-functions]: https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines.html#s-functions
[cpp-class-invariants]: https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines.html#rc-struct
[cpp-comment-code]: https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines.html#rl-comments
[cpp-comment-intent]: https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines.html#rl-comments-intent
[cpp-comment-crisp]: https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines.html#rl-comments-crisp
[cpp-preconditions]: https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines.html#ri-pre
[cpp-postconditions]: https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines.html#ri-post
[cpp-expects]: https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines.html#ri-expects
[cpp-ensures]: https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines.html#ri-ensures

- Put a concise explanatory comment immediately above each meaningful logical block: a public type
  or contract group, a non-trivial function, an algorithm phase, a validation phase, a canonical
  encoding section, a test fixture, or a group of tests proving one behavior.
- Give every class-like type, function and method a description that states its purpose at the level
  of its user or caller. For a class-like type, state the responsibility it owns and its boundary.
  For a function or method, state the operation it performs and its intended outcome.
- When relevant, a class-like type's description must also state its invariant—a condition
  established by construction, assumed at entry to public operations and reestablished before
  those operations return—plus important ownership, lifetime, valid-state or thread-safety
  guarantees. State an invariant shared by all methods once in the class description; do not repeat
  it for every method.
- When relevant, a function or method's description must also explain the meaning and constraints
  of its inputs; its observable behavior, side effects, state changes or ordering; its return value,
  other outputs or resulting state; its preconditions and postconditions; and its caller-relevant
  failure behavior, including how an error is reported. A precondition is a requirement that must
  hold before a call; a postcondition is a guarantee that must hold after the documented form of
  completion.
- Prefer precise names, types, qualifiers and machine-checkable contracts over prose when they can
  express a guarantee completely. Information is relevant to the description when it is needed to
  use or review the construct correctly and is not already clear from its name, types, qualifiers,
  return type or formal contracts. A concise sentence can cover several items; do not add empty
  `Inputs:` or `Outputs:` sections when there is nothing useful to say. State intent and guarantees,
  not obvious syntax, a copy of the declaration or incidental implementation steps.
- Split long algorithms into named phases with comments wherever a reviewer must otherwise infer
  why the steps occur in that order.
- In tests, explain the production contract or regression risk being proved, not the mechanics of
  `REQUIRE` or `CHECK`.
- In Markdown, an informative heading plus its introductory prose counts as the explanation for that
  logical block; add prose when a heading alone does not establish purpose or scope.

### C, C++ and Python divider hierarchy

Use matching visual boundaries around every class-like type and function-level block in C/C++
headers and sources and in Python modules, including tests, support code, tools and benchmarks. The
divider hierarchy is fixed from strongest to weakest; use the language's native line-comment prefix:

- Class/type boundary: `// ########################################################################` in C/C++ or
  `# ########################################################################` in Python.
- Method/function boundary: `// --------------------------------------------------------` in C/C++ or
  `# --------------------------------------------------------` in Python.
- Internal algorithm-step boundary: `// ++++++++++++++++++++++++++++++++++++++++` in C/C++ or
  `# ++++++++++++++++++++++++++++++++++++++++` in Python.

Apply the boundaries as follows:

- In C/C++, leave exactly one blank line immediately before every divider, including opening,
  closing, shared and internal-step dividers. This spacing rule also applies to the final divider
  before a namespace close or end of file.
- In Python, leave at least one blank line immediately before every divider. Follow Ruff's
  formatter-native two blank lines around top-level definitions rather than forcing exactly one
  there; nested dividers normally retain one blank line.
- Put the opening divider first and the explanatory comment immediately after it. For a C or C++
  class-like type, function or method, this comment is the required description and must satisfy the
  logical-block comment rules above.
- In Python, a divider comment and a normal docstring may satisfy the description rules together.
  Put the caller-facing contract in the docstring and use the divider comment for a complementary
  responsibility, invariant or architectural reason; do not repeat the same sentences in both. If
  the construct has no docstring, the divider comment alone must satisfy all applicable rules.
- Close every governed block with the same divider, separated from the governed code by the required
  blank line. The final class, function or algorithm phase in a file must also have an explicit
  closing divider; reaching a C++ namespace close, a Python dedent or end of file is not an implicit
  close.
- Use the class/type divider for `class`, `struct`, `union` and `enum class` definitions or forward
  declarations, Python classes, and named concept/type-alias/protocol contract groups. Continue to
  use method/function dividers for functions declared or defined inside class-like types.
- Use the method/function divider for constructors, methods, free functions, operators, factories,
  Python `def` and `async def` blocks, test cases and benchmark callbacks. Closely related overloads
  may form one documented block.
- Use the step divider only inside longer function bodies where validation, transformation,
  encoding, commit or other meaningful phases need to be distinguished. Close the final phase
  before the C/C++ function's closing brace or the Python function's closing dedent.
- Adjacent blocks at the same level may share one divider as the previous block's close and the next
  block's open. Put the required blank line before that shared divider, then put the next block's
  description immediately after it.
- In Python, put an opening divider and its description before any decorators without inserting a
  blank line between the description, decorators and declaration.
- Start a Python class or function that contains nested dividers with its normal docstring, then put
  the first nested divider after that docstring. Ruff removes empty lines placed directly after a
  suite header, so the docstring makes the required separation formatter-stable.
- Keep C++ namespace-closing comments outside the class/function boundary they contain. In Python,
  indent dividers to the level of the class, function or phase they govern.

Example:

```cpp

// ########################################################################
// Owns validated orders and preserves canonical insertion order.
class OrderBook final {
public:

  // --------------------------------------------------------
  // Inserts the order when valid; otherwise returns a validation error without modifying the book.
  [[nodiscard]] Result<void> add(Order order);

  // --------------------------------------------------------
};

// ########################################################################

// --------------------------------------------------------
// Inserts the order when valid; otherwise returns a validation error without modifying the book.
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

Python example:

```python
"""Demonstrate the repository's Python divider hierarchy."""


# ########################################################################
# Keeps invalid orders outside the owned sequence by centralizing validation.
class OrderBook:
    """Own validated orders in canonical insertion order."""

    # --------------------------------------------------------
    # Prevents partial book mutation by validating before insertion.
    def add(self, order: Order) -> None:
        """Insert the order when valid; otherwise raise without modifying the book."""

        # ++++++++++++++++++++++++++++++++++++++++
        # Reject invalid input before changing owned state.
        self._validate(order)

        # ++++++++++++++++++++++++++++++++++++++++
        # Commit only after every fallible operation has succeeded.
        self._orders.append(order)

        # ++++++++++++++++++++++++++++++++++++++++

    # --------------------------------------------------------


# ########################################################################
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
