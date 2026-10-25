# Runtime binding experiments

These tools build isolated diagnostic executables. They do not enable a new
production transformation or certify binding preservation across unknown calls.
The baseline must relink to exactly the supplied installed executable before
any candidate is accepted. Input sources, objects and installations stay intact.

`check_chain.py --prefix INSTALL --output NEW_DIRECTORY` runs twenty iterations
over two persistent socket sessions. The chain covers simple and extended
queries, commit, savepoint recovery, lock waits, statement timeout, cancel,
transactional notifications, and reuse of the same backend identities. Run the
same driver against the native PostgreSQL control. It does not test the embedded
C API, Python binding, or browser package.

`check_authentication.py --prefix INSTALL --output NEW_DIRECTORY` checks SCRAM
acceptance and rejection, HBA reload, and peer user-map reload on real socket
connections. Run it on both the candidate and the PostgreSQL control when
changing backend startup or authentication configuration inheritance.

`startup_failure.c` is a diagnostic link fixture. Compile it with the runtime
headers and link the server with that object and
`-Wl,--wrap=pthread_getattr_np`; keep the unchanged server as the control.
`check_startup_failure.py --prefix DIAGNOSTIC_INSTALL --output NEW_DIRECTORY`
then forces forty client-stack discovery failures with a connection limit of
sixteen. Every failure must reach the startup cleanup diagnostic; a subsequent
transaction must commit without restarting the postmaster. The fixture is not
linked into product binaries. Runtime unit tests separately hold startup at a
barrier to verify slot reservation, concurrent launches, pause and signal
delivery, and synchronous callers' original error contract.

`bootstrap_failure.c` checks diagnostics before any execution context is bound.
Compile it with the runtime headers and the matching generated GUC/state headers,
then link a diagnostic server with that object and these five linker options:

```text
-Wl,--wrap=postgamma_instance_runtime_create
-Wl,--wrap=postgamma_instance_context_init
-Wl,--wrap=postgamma_role_context_init
-Wl,--wrap=postgamma_execution_context_init
-Wl,--wrap=postgamma_backend_state_context_prepare
```

Run `check_bootstrap_failure.py --prefix DIAGNOSTIC_INSTALL --output NEW_DIRECTORY`.
Every injected ENOMEM must print the stage and numeric error and exit with status
1, rather than aborting while accessing unbound PostgreSQL diagnostics. The
fixture uses the ordinary heap-backed server and is never linked into products.

Build `explicit_view.cpp` against the same Clang LibTooling installation used by
the project's AST tools. Run `check_view.py --help` for the callback, recursion,
function pointer and nonlocal exit fixture. Both original and transformed
fixtures must compile with warnings as errors and produce identical output.
The runtime preparation tests cover real initialization, relocation, inheritance,
unbound access rejection and carrier migration separately.

`build_view.py --help` describes the captured compile/link records required for
one translation unit. Original ABI entry points remain wrappers; direct internal
calls can carry a borrowed state view. Functions containing explicit binding
operations are excluded. External and indirect edges remain in `view.json`:
passing the fixture does not prove their contracts throughout PostgreSQL or its
extensions. No borrowed view may span a binding change or scheduler yield.

`--validated-entry` tests a separate boundary representation. The original ABI
wrapper checks the view before calling a private implementation with a nonnull
parameter contract. An unbound call executes an unchanged copy of the original
body, including its original state access checks and ABI calls. Access-free
returns, effects before the first access, and callbacks that establish the first
binding therefore retain their ordering. The fallback is marked cold; it is not
replaced by an unconditional error. Functions containing static local objects,
string or compound literals, predefined function names, or label addresses are
excluded so cloning cannot change their object identities. These exclusions and
unqualified call edges are retained in the report. Code size and fallback costs
must be measured as well as the bound path. This is still a diagnostic, not a
production-safe inference that unknown callees preserve bindings.

Run `check_view.py` both with and without `--validated-entry`. The validated
fixture also exercises unbound callbacks and shared object identities. Both
modes require an unbound address-only access to fail, even with `-DNDEBUG`.
`--cc` accepts compiler flags for separate ASan/UBSan runs.
For a comparison over exactly the same selected functions, pass
`--clone-safe-only` to the checked control. It applies the same object identity
exclusions without introducing nonnull parameters or unbound fallback bodies.

`--borrowed-scope` captures a bound view once per function while retaining the
original function and body. It does not clone static objects or alter function
pointers. A NULL entry view preserves lazy first access, including callbacks
that establish the first binding. Explicit binding and nonlocal-exit functions
are excluded. Returning callees still need to preserve binding; the report
retains unqualified direct and indirect calls. This mode is mutually exclusive
with the cloning and validated-entry modes.

Borrowed scopes override the state access macros only while preprocessing their
function bodies, then restore the previous definitions. This includes nested
PostgreSQL macros; recognizing only literal state macro calls misses these uses.
Writes to the binding itself, including writes hidden in a macro, are boundaries.
Known binding and nonlocal-exit boundaries propagate through local direct call
chains, including calls hidden inside macros. A raw TLS reader retains its own
original body without classifying its callers as binding writers. Unknown calls
remain unqualified.

`--borrowed-scope --inline-headers` also passes that view through static inline
callees in headers and the current translation unit. It emits private copies
using the same object-identity exclusions as validated-entry cloning. Originals
remain available for address-taken and unconverted callers. Forward declarations
stay beside their original declarations; file-local implementations stay beside
their original bodies. Late header includes and functions with unrecognized or
parameter-indexed attributes stay on the original path. No SQL workload or
PostgreSQL function name selects this path. Function identities and unqualified
call edges remain diagnostic evidence, not a proof that arbitrary extensions
preserve bindings. Run the fixture with both options, including its ASan/UBSan
variant: it checks inline call chains, late binding, address-only unbound failures,
retained static, function and source-position identities, attributes and late
header declarations.

`build_runtime.py --help` builds independent runtime experiments using a JSON
specification containing `baseline_prefix`, `link_record`, optional `headers`
(destination relative to the overlay include directory to input file), optional
compiler `flags`, and `units` (`compile_record` and replacement `source`). It
retains replacement sources, commands, hashes and build logs under the output.
Failed outputs are retained; use a new output directory for each attempt.
`--jobs` bounds concurrent object compilation. Optional `view_tool` and
`clang_resource_dir` fields apply the borrowed-scope experiment before compiling
each selected translation unit. The header overlay must include the original
header tree as well as replacements: quote includes from old neighboring headers
otherwise bypass a partial overlay. The tool snapshots sources and transformed
outputs and requires each replacement to match one linked object.
`view_flags: ["--inline-headers"]` enables header call propagation. Every selected
translation unit is parsed, even when all its accesses come from included macros.

Report instruction counts, cycles and whole-instance CPU for offloaded work;
do not equate a targeted diagnostic pass with a delivery or release gate.

`POSTGAMMA_PRIVATE_MEMORY_BLOCKS` is an opt-in backing-storage experiment.
It redirects raw block allocation only in PostgreSQL's four MemoryContext
implementations, and separately backs role images and the cloned GUC table.
Each logical backend owns 4 MiB context-block segments. Live blocks can grow
beyond one segment; only one completely idle segment is retained for reuse.
Partially live segments can contain free slots, so this is not a bound on all
unused bytes. Allocations above 256 KiB use independent blocks. Linux uses
private mappings for large backing blocks; other targets use the allocator
fallback. The arena follows the session across
carrier changes and is destroyed after PostgreSQL's normal cleanup. It does
not change the host allocator or retain arenas between logical sessions.
External client backends lease a pool owned by their instance. The pool reuses
only backing mappings, retaining at most 64 MiB and 256 idle mappings while
clients overlap. Role images still receive fresh zero initialization, and each
arena starts with empty allocation metadata. Ordinary context blocks keep
malloc's uninitialized-payload contract. The last client releases every cached
mapping and its physical pages, without adding a reclamation thread. Startup
failures release the same lease after destroying any partial role and execution.
Compare active-session PSS, closed-session PSS and connection latency together.
The switch is not enabled by default and does not imply performance acceptance.
