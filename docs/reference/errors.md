# Status and error reference

Every failing C operation returns a `pgm_status`. When PostgreSQL or the host
boundary has diagnostic detail, the operation also returns a `pgm_error`.
Python translates the same status and SQLSTATE into the public DB-API exception
hierarchy.

## Host status mapping

| C status | Stable name | Python exception |
| --- | --- | --- |
| `PGM_STATUS_OK` | `ok` | No exception |
| `PGM_STATUS_INVALID_ARGUMENT` | `invalid-argument` | `InterfaceError` |
| `PGM_STATUS_BUSY` | `busy` | `OperationalError` |
| `PGM_STATUS_TIMEOUT` | `timeout` | `QueryTimeoutError` |
| `PGM_STATUS_CANCELED` | `canceled` | `QueryCanceledError` |
| `PGM_STATUS_IO_ERROR` | `io-error` | `OperationalError` |
| `PGM_STATUS_POSTGRES_ERROR` | `postgres-error` | Classified by SQLSTATE |
| `PGM_STATUS_CONNECTION_FAILED` | `connection-failed` | `OperationalError` |
| `PGM_STATUS_INSTANCE_FAILED` | `instance-failed` | `OperationalError` |
| `PGM_STATUS_FORKED_PROCESS` | `forked-process` | `ForkedProcessError` |
| `PGM_STATUS_VERSION_MISMATCH` | `version-mismatch` | `InterfaceError` |
| `PGM_STATUS_UNSUPPORTED` | `unsupported` | `NotSupportedError` |
| `PGM_STATUS_OUT_OF_MEMORY` | `out-of-memory` | `OperationalError` |
| `PGM_STATUS_INTERNAL_ERROR` | `internal-error` | `InternalError` |
| `PGM_STATUS_REENTRANT_CALL` | `reentrant-call` | `InternalError` |

`pgm_status_name()` returns the stable name. Do not assume every failure has a
SQLSTATE: path validation, lifecycle, allocation, and fork errors originate at
the host boundary.

## SQLSTATE mapping in Python

For `PGM_STATUS_POSTGRES_ERROR`, PostGamma uses the first two SQLSTATE
characters:

| SQLSTATE class | Python exception |
| --- | --- |
| `22` data exception | `DataError` |
| `23` integrity constraint violation | `IntegrityError` |
| `08`, `40`, `53`, `54`, `55`, `57`, `58` | `OperationalError` |
| `0A` feature not supported | `NotSupportedError` |
| `2D`, `2F`, `34`, `3D`, `3F`, `42` | `ProgrammingError` |
| `XX` internal error | `InternalError` |
| Any other class | `DatabaseError` |

SQLSTATE `57014` maps to `QueryCanceledError`. A host-side timeout maps to
`QueryTimeoutError` even when PostgreSQL cancellation is used to retire the
request.

## Diagnostic fields

Python `postgamma.Error` exposes `status`, `status_name`, `sqlstate`,
`severity`, `detail`, and `hint`. C callers can inspect the corresponding
`pgm_error_*` accessors and all named PostgreSQL diagnostic fields through
`pgm_error_field()`.

Log the structured fields and free the C error with `pgm_error_free()`. Do not
branch on the human-readable message when a status or SQLSTATE is available.
