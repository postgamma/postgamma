# Contributing to PostGamma

Thank you for helping improve postgamma. Public bug reports and focused pull
requests are welcome through the repository's issue tracker and pull-request
interface.

## Before opening an issue

Search existing issues, reduce the problem to the smallest program possible,
and include the information listed in the
[troubleshooting guide](docs/troubleshooting.md#report-a-reproducible-problem).
Do not post security-sensitive details or production data in a public issue;
follow [SECURITY.md](SECURITY.md) instead.

## Development setup

PostGamma transforms an unmodified PostgreSQL submodule and has separate fast,
product, and upstream compatibility gates. Initialize the source input first:

```bash
make source-init
make doctor
```

Use `make doctor-threaded`, `make doctor-sdk`, `make doctor-python`, or
`make doctor-docs` before a focused build. Use `make doctor` for the complete
local release environment. These checks honor the selected compiler and
PostgreSQL configure arguments, and fail before parallel compilation when a
required header, library, host capability, or submodule is unavailable.

Run `make help-internal` for lower-level compatibility and maintainer targets.
Start with the narrowest gate that covers a change, then run the applicable
product gate before requesting review.

Common public-surface checks are:

```bash
make threaded-postgresql-check JOBS=4
make docs-check
make static-sdk-check JOBS=4
make python-check JOBS=4
```

## Source rules

- Keep the PostgreSQL submodule unmodified; changes belong in adapters,
  generated transformations, or PostGamma-owned runtime code.
- Keep generated and build artifacts out of Git.
- Use Python for project-owned build tooling and C for runtime code; the AST
  transformer uses C++ because Clang LibTooling exposes a C++ API.
- Keep code, comments, commit messages, public documentation, manifests, and
  test output in English.
- Add executable evidence for behavior claims and fail closed when a source
  inventory or public API changes.

## Pull requests

Describe the user-visible behavior, the invariant being protected, and the
commands used to validate the change. Keep unrelated cleanup in a separate
commit. Never commit cluster data, wheels, archives, generated PostgreSQL trees,
or local absolute paths.

## Contribution license

Unless explicitly stated otherwise, any contribution intentionally submitted
for inclusion in PostGamma is licensed under the Apache License 2.0 without
additional terms or conditions.
