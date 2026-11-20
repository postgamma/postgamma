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

`build_runtime.py --help` builds independent runtime experiments using a JSON
specification containing `baseline_prefix`, `link_record`, optional `headers`
(destination relative to the overlay include directory to input file), optional
compiler `flags`, and `units` (`compile_record` and replacement `source`). It
retains replacement sources, commands, hashes and build logs under the output.
Failed outputs are retained; use a new output directory for each attempt.
`--jobs` bounds concurrent object compilation. The header overlay must include
the original header tree as well as replacements: quote includes from old
neighboring headers otherwise bypass a partial overlay. Each replacement must
match one linked object.

Report instruction counts, cycles and whole-instance CPU for offloaded work;
do not equate a targeted diagnostic pass with a delivery or release gate.
