# State access profiling

These diagnostic tools measure a fixed read-only transaction stream through
ordinary libpq. They do not change the server's state-access implementation.
Record the server binary hash, build flags, seed hash, settings and commands
alongside each profile. Compare complete ordered results across variants.

Start the server under Callgrind with:

```text
--tool=callgrind --instr-atstart=no --separate-threads=yes
--dump-instr=yes --collect-jumps=yes
--compress-strings=no --compress-pos=no
--callgrind-out-file=/absolute/run/directory/callgrind.out.%p
```

Raise the process open-file limit before launching Valgrind when profiling the
thread server. Its runtime reserves descriptor capacity for all logical roles.
Use fresh data directories and the same resource limits for both variants.

Compile `state_access_probe.c` as a test-only PostgreSQL module, using the
matching server headers and Valgrind headers. A generated server also needs its
generated source include directory. Install the following SQL function in the
disposable test database, using an absolute module path:

```sql
CREATE FUNCTION postgamma_state_profile(int) RETURNS void
AS '/absolute/run/directory/state_access_probe.so'
LANGUAGE C STRICT;
```

`state_access_client.py` takes `--libpq`, `--conninfo` and `--script`. The script
is a JSON array with one SQL statement per item. It sends each statement as a
separate simple-protocol request. Include `BEGIN` and `COMMIT` in the array to
preserve transaction boundaries. On startup the client prints its backend ID
and full result oracle. A thread server's backend ID is logical, not an OS PID.

Drive the same client's stdin through these phases, waiting for each JSON
response before issuing the next command:

1. An integer warmup count.
2. `{"sql":"SELECT postgamma_state_profile(0)"}` to enable instrumentation.
3. An integer instrumentation warmup count.
4. `{"sql":"SELECT postgamma_state_profile(1)"}` to zero counters.
5. The integer measured transaction count.
6. `{"sql":"SELECT postgamma_state_profile(2)"}` to dump and stop instrumentation.
7. `0` to disconnect.

The client checks all ordered values, NULL flags, column types and command
statuses on every iteration. The controller must additionally compare the
initial oracle across variants, enforce timeouts, retain stderr and check
process exit codes. Timing this Python client is not a throughput benchmark.

Use only the dump whose trigger is `Client Request: measured`. With separate
threads, this client request dumps the executing backend thread; exclude all
other threads, parent processes, warmup and termination dumps. The interval
includes the end of the zero-control SQL and the start of the dump-control SQL.
Report that small boundary cost explicitly. A separate zero-transaction
control can measure its magnitude; do not silently subtract an estimate.

The probe avoids dependence on `callgrind_control` PID discovery: PostgreSQL
rewrites its process title, and some Valgrind builds do not initialize a child
control channel after fork. If external control is used instead, verify its
acknowledgment and a nonempty dump; a zero exit code alone is insufficient.

Analyze a dump using `analyze_state_access.py --help`. Supply its matching
binary, backend-state catalog, ownership policy and ownership report. For a
local-exec candidate, also pass the symbol's verified final TLS offset via
`--local-exec-tpoff`. The analyzer:

- sums instruction self costs and checks them against `totals`, excluding
  inclusive call-edge costs;
- matches executable instruction addresses to its disassembly;
- reports conservatively recognized TLS/table loads, constant slot accesses,
  checks, runtime calls and ordinary function costs;
- leaves unknown register flows and dynamic slot indexes unassigned.

Address matching does not replace the controller's binary-hash check. The
recognized slot distribution is not a claim of complete dynamic coverage.
Bridge calls supplement the slot-load counts and must not be added to them.

For hardware sampling, establish the persistent client before attaching to its
actual backend PID/TID. A thread-server connection created after `perf record
-p` attaches is not necessarily included. Check the sampled thread identity,
lost samples and throttle records; a successful command alone does not prove
workload coverage. Retain unusable captures with that qualification. Keep
unprofiled throughput rounds separate from counter and sampling diagnostics.
Aggregate callers and callees when comparing functions affected by inlining.
Use separate native benchmark and `perf stat` rounds to assess throughput and
hardware cycles; Callgrind instruction counts alone do not establish a speedup.

## Preparation candidate audit

`make backend-state-preparation-plan` writes
`build/plans/backend-state-preparation.json` (relative to `BUILD_DIR`). It joins
the AST initializer facts, ownership, exact source hashes and replacement
tokens. It distinguishes implicit zero storage, proven explicit zero images,
file-scope templates and fallback reasons. Function-local nonzero templates
remain lazy. A relocation component qualifies only if every member qualifies.
The existing descriptors, slot IDs, offsets and accessors are unchanged.

The report is **analysis only**. `preparation_candidate` describes the source
and replacement-plan evidence; `direct_access_eligible` stays false. Applied
artifacts, the full header/configuration inputs, references from untransformed
code (including non-AST references), provider linkage and preparation/publication
must still be checked. Template
stability must not be confused with an immutable live value.

To join a saved profile, use `buildsys/plan_backend_state_preparation.py` with
the same inputs as the Make target plus `--profile analysis.json`. The join
checks full declaration identities and layout. Its candidate fraction is over
recognized slot loads only, and is conditional on the remaining activation
gates. It is neither complete dynamic coverage nor an expected speedup.

For an isolated native experiment, `prepare_backend_state_templates.py` installs
explicit providers in a **transformed** source/build tree. Record the subsequent
compilation and link with `cc_capture.py`, including the runtime and extensions.
`qualify_backend_state_preparation.py --help` describes the required inputs. It
reparses that compilation domain with `postgamma-state-inventory
--audit-preparation`, rechecks initializer/layout facts against configured headers,
and classifies every surviving reference. Only type expressions, matching
resolver template arguments, closed static relocation references and generated
provider returns qualify. Unknown basic assembly keeps affected slots lazy.
Empty assembly is harmless; on x86 the audit also recognizes complete
operand-free `nop`, `pause` and `rep; nop` strings. Directives, labels and
unrecognized strings remain opaque, and extended-assembly operands still pass
through the AST reference checks. A provider must also exist in the linked server; unlinked extension
state cannot silently enter the core registry. The report retains input hashes,
compile/link identities, every fallback reason and the exact preparation set.
It qualifies that configuration, not other targets or arbitrary native modules.

`generate_backend_state_preparation.py --qualification qualified.json --output
<transformed-source>/src/include/postgamma/backend_state_preparation.inc`
emits the explicit registry after verifying the audit's input hashes. Recompile
the isolated server with `POSTGAMMA_PREPARED_ROLE_STATE=1`, then repeat the
artifact qualification and compare the emitted registry against the one compiled
into that server. The default build does not select a preparation set. Changing
compiler options requires rebuilding objects, including PostgreSQL's archive
members and `objfiles.txt` dependency caches.

Preparation runs after inheritance and before first binding. It stages defaults
and relocations without switching the current execution, preserves inherited
values, and publishes each slot's initialized flag before the set's readiness.
A failed preparation leaves live bytes and flags unchanged. Repeated preparation
of the same immutable set is a no-op; binding requires that set in enabled builds.
The existing accessors remain in use. `runtime-backend-state-preparation-test`
and its `-sanitizer-test` counterpart also belong to the full runtime gates. They
exercise direct writes followed by inheritance/named access, closed cycles and
self references, rollback/retry, nested bindings and carrier migration.

For direct access, also pass `--direct-header
<transformed-source>/src/include/postgamma/backend_state_direct.h` to the registry
generator and rebuild with both `POSTGAMMA_PREPARED_ROLE_STATE=1` and
`POSTGAMMA_DIRECT_ROLE_STATE=1`. Repeat artifact qualification and byte-compare
both generated files. Registry assertions and a set-specific TLS symbol prevent
mixing different preparation sets. Both switches remain off by default.

Only constant qualified slots use the checked role base plus a constant offset.
Dynamic slot arguments, named lookups and unqualified slots retain the existing
resolver contract. Generated relocated accesses carry their slot identity, so
a completely qualified relocation component uses the same prepared view.
Unqualified components retain their relocation bridge; frontend compilations
discard both the backend slot and bridge arguments. Initialization flags and
address tables remain necessary for inheritance, named/dynamic lookups and
fallback consumers. The generated offset switch is an always-inlined compile-time
selector: optimized constant accesses must contain no selector, address-table
lookup or returning resolver call. Binding/restoration validates and publishes
the role view; even an address-only access rejects an unbound view. No protection
page or PostgreSQL function-specific inline override is involved.

`runtime-backend-state-direct-test` and its sanitizer counterpart exercise the
same lifecycle suite with actual direct accesses. The buildsys tests additionally
check GCC/Clang machine code, single evaluation of dynamic arguments in the C
suite, and rejection of mixed-set objects. Runtime lookup/relocation entry points
remain available for fallback callers. Neither the switches nor these targeted
checks constitute SDK, wheel, Wasm or performance qualification.

The direct-access experiment also compensates for the small-function inlining
cost of checked state access in generated backend objects. With GCC at `-O2`,
the build raises `max-inline-insns-auto` to 20 only when the compiler's active
budget is smaller. Larger budgets, other optimization levels, unsupported
compilers (including Clang), and explicit `max-inline-insns-auto` or
`-finline-limit` settings are left unchanged. Unit-growth limits and all runtime
checks remain in force. This does not select individual PostgreSQL functions,
change installed extension flags, or affect the default resolver build. The
policy follows PostgreSQL's backend `common.mk` domain, including timezone
objects; it is not added to common/port libraries or libpq compilation.

Treat this as a compiler-policy change when comparing performance: rebuild all
backend objects, retain compiler inlining reports and final binary sizes, and
give the original PostgreSQL control the same budget. Include an isolated
object replacement only as an attribution experiment, not as the final build.
Verify actual call removal and instruction/cycle counts alongside unchanged
ordered SQL results; fewer calls alone do not prove an end-to-end speedup.

## Executable TLS experiment

A fresh generated native ELF server build can opt into
`POSTGAMMA_SERVER_LOCAL_EXEC_TLS=1` on its Make command. This selects local-exec
for both the core address-table and prepared-role-base TLS declarations while
retaining their exported symbols. Evaluate this combination separately from
preparation alone and direct access using the default TLS model.
The default is unchanged. The Make variable adds a compiler definition only to
backend objects and the server variants of common/port objects; it is not added
to installed `pg_config` flags or extension compilation flags.

Use a separate, clean build for this experiment. Make does not track changes to
compiler options on existing objects. Do not enable it for objects that will
also be linked into an SDK, shared library, Python wheel or Wasm module.
Verify the final executable's TLS instructions and dynamic symbols, and test a
late-loaded extension from existing threads before interpreting benchmark data.

## Connection and idle-memory diagnostic

Build `connection_churn.c` against the same ordinary libpq for every variant,
using C11, pthreads and `-O2`. The driver accepts:

```text
connection-churn churn CONNINFO CLIENTS ITERATIONS samples.csv
connection-churn idle CONNINFO CLIENTS
```

`churn` warms up 50 connections per client, synchronizes clients, and performs
the requested count of connect / `SELECT 1` / close cycles. It checks the exact
single int4 result on every cycle. The CSV separates connect latency from the
complete cycle, including `PQfinish`. JSON reports the measured transaction
count and the interval from the earliest worker start to the last finish.
Any connection, result or output error fails the run. Use a connection timeout
and a statement timeout in CONNINFO, and an external deadline for the process.

`idle` opens and checks all requested sessions, prints `ready`, and holds them
until a line arrives on stdin. A controller can then measure PSS for the entire
server process tree, including all fork children. Use a fresh server for each
sample, identical settings/data/CPU assignments, alternating variant order and
separate unprofiled repetitions. Keep all latency samples, identities and errors.
These diagnostics do not certify a TPC-C result or a release package.
