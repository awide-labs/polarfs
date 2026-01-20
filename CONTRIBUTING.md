# Contributing to PolarFS

Thank you for your interest in contributing to PolarFS! This document outlines the guidelines and requirements for contributing to this project.

## Getting Started

1. Fork the repository
2. Clone your fork locally
3. Create a new branch for your changes
4. Make your changes following the guidelines below
5. Submit a pull request

## Commit Message Format

All commits must follow the [Conventional Commits](https://www.conventionalcommits.org/) specification. This is enforced by CI checks on all pull requests.

### Format

```
<type>[optional scope]: <description>

[optional body]

Refs: <reference>[, <reference>...]
```

### Rules

- **Header (first line)**: Must be 72 characters or less
- **Body lines**: Each line must be 72 characters or less
- **Refs footer**: Mandatory - must contain a comma-separated list of issue references (e.g., `Refs: PROJ-1234` or `Refs: PROJ-1234, PROJ-5678`). Only one `Refs:` footer is allowed per commit

### Valid Types

| Type | Description |
|------|-------------|
| `feat` | A new feature |
| `fix` | A bug fix |
| `docs` | Documentation only changes |
| `style` | Changes that do not affect the meaning of the code (formatting, etc.) |
| `refactor` | A code change that neither fixes a bug nor adds a feature |
| `perf` | A code change that improves performance |
| `test` | Adding missing tests or correcting existing tests |
| `build` | Changes that affect the build system or external dependencies |
| `ci` | Changes to CI configuration files and scripts |
| `chore` | Other changes that don't modify src or test files |
| `revert` | Reverts a previous commit |

### Optional Footers

In addition to the mandatory `Refs:` footer, you may include other optional
footers following the Conventional Commits specification. Common examples
include:
- `Skip-changelog: true` - Skip changelog requirement (see Changelog section)
- `See: <URL>` - Reference to external documentation, RFCs, or related
  resources
- `Discussion: <URL>` - Link to discussion related to this particular commit, e.g. mailing list discussion

All footers must follow the `token: value` format and are exempt from the
72-character line length limit.

### Exemptions

- **Merge commits**: Automatically generated merge commits are exempt from
  Conventional Commits validation
- **Commits brought by merges**: All commits that are brought in by merge
  commits (e.g., when merging from upstream) are also exempt from validation,
  as we have no control over their commit message format

### Examples

```
feat(api): add user authentication endpoint

Implement JWT-based authentication for the REST API.

Refs: PROJ-1234
```

```
fix: resolve memory leak in cache handler

The cache was not properly releasing resources on cleanup.

Refs: PROJ-5678, PROJ-5679
```

```
docs: update installation instructions

Refs: PROJ-9012
```

```
perf: optimize index scan for large tables

Use bitmap index scans instead of sequential scans for queries with
large result sets. This improves query performance by reducing I/O
operations and memory usage.

Refs: PROJ-3456
See: https://www.postgresql.org/docs/current/indexes-bitmap-scans.html
```

**Example with Skip-changelog footer:**

```
fix: resolve internal deadlock in test suite

This fixes a deadlock condition that only occurs in the test
environment and does not affect production. The issue was caused by
improper lock ordering during parallel test execution.

Skip-changelog: true
Refs: PROJ-7890
```

## Changelog Requirements

Commits that introduce user-visible changes must include an update to `CHANGELOG.md`. This is enforced by CI checks on all pull requests.

### Changes Requiring Changelog Updates

- `feat` - New features
- `fix` - Bug fixes
- `perf` - Performance improvements
- Breaking changes (indicated by `!` after type/scope or `BREAKING CHANGE:` in body)

### Changelog Format

We follow the [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) format. Add your changes under the `[Unreleased]` section in the appropriate category:

- **Added** - for new features
- **Changed** - for changes in existing functionality
- **Deprecated** - for soon-to-be removed features
- **Removed** - for now removed features
- **Fixed** - for any bug fixes
- **Security** - in case of vulnerabilities
- **Performance**: performance improvements (our extension to Keep a Changelog)

### Example

```markdown
## [Unreleased]

### Added
- New configuration parameter `polar_enable_parallel_ddl`

### Performance
- Optimized index creation for large tables
- Reduced memory usage in query planner
```

### Skipping Changelog Updates

If a commit with user-visible changes intentionally does not require a changelog update, add the following footer to the commit message:

```
Skip-changelog: true
```

## Code Style

- Follow the existing code style in the project
- Ensure your code compiles without warnings
- Add tests for new functionality

## Pull Request Process

1. Ensure all CI checks pass
2. Update documentation if needed
3. Request review from maintainers
4. Address any feedback
5. Once approved, your PR will be merged

## Questions?

If you have questions about contributing, please open an issue or contact the maintainers.
