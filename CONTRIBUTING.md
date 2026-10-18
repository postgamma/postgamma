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
make doctor-native
```

Use `make server-doctor`, `make c-doctor`, `make python-doctor`, `make wasm-doctor`,
or `make docs-doctor` before a focused build. Wasm contributors can prepare build
tools with `make wasm-toolchain-setup` and test browsers with
`make wasm-browser-setup`. Use `make doctor` to check every installed environment,
including Wasm and all three browser engines, or `make full-test-doctor` for the
native/Wasm test environments without docs. Doctors do not install tools.
These checks honor the selected compiler and
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
