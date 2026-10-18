# Installation

Choose the artifact that matches the application boundary. Python applications
install one wheel. Native applications link the static C SDK and deploy its
matching resource pack. For a standalone database service, follow the
[threaded server quickstart](../server/quickstart.md); for web applications, use
the [browser preview guide](browser.md).

Binary users do not need the source-build packages below. They are required
only when compiling PostGamma itself.

Run source builds and tests as an unprivileged user because PostgreSQL `initdb`
refuses to run as root. Use `sudo` only for the package-installation commands.

## One-command clean-host validation

After cloning the repository, run the complete source gate from a regular
account. The account may use sudo for package and swap setup, but it remains the
owner of every source, build, and test process:

```bash
./scripts/full-test.sh
```

The provisioner recognizes Debian/Ubuntu, Fedora/RHEL/CentOS/Rocky/AlmaLinux/
Anolis/Amazon Linux, and openSUSE package families. It installs missing host
packages while protecting the versions of packages already installed, including
transitive dependencies. System Git 2.28 or newer is reused; older Git uses a
checksum-pinned local fallback. CPython 3.11, GNU Make 4.4.1, LLVM/Clang 22 and
the PostgreSQL TAP tools are installed under the managed build directory.
Product compilation uses the host's headers and libraries. AST and C++17 test
tools use the private toolchain's sysroot and library paths, so old hosts do not
need a newer system C++ runtime to run the build tools. It validates the current
checkout's clean, committed source directly, runs the environment preflight,
and starts `make full-test static-sdk-package`, followed by tests
of the packaged SDK and pip-installed wheel with build-library overrides removed.
The full test prepares the pinned Wasm SDK and Chromium, Firefox and Linux WebKit,
checks their dependencies, then runs native validation and the release Wasm suite
serially. Browser setup errors stop before the long native gate; either suite
failing makes the command fail.
The source gate also generates the C and Python API references and builds the
documentation website with warnings treated as errors. The provisioner installs
missing Doxygen packages and the pinned `docs/requirements.txt` dependencies;
Python documentation tools remain inside the managed toolchain. Documentation
failures fail the source gate; the provisioner runs this check before native
kernel validation starts.
The installed-wheel check uses a Python interpreter with the same ABI as the
wheel and host runtime libraries. A suitable system interpreter is reused;
otherwise a checksum-pinned CPython is built under
`build/full-test/state/runtime-python` and cached, using the existing system
headers and libraries. `POSTGAMMA_RUNTIME_PYTHON` selects an existing compatible interpreter,
and `POSTGAMMA_PYTHON_SOURCE_URL` selects a mirror of the pinned source archive.
The check also compares libraries actually loaded by Python with the host
loader's resolution, so a toolchain's preloaded zlib cannot stand in for the
system library.
A standalone copy of the script clones
the selected repository because no checkout is available. The script never
creates an account and refuses to run as root. Package installation, swap
activation, and a temporary process-limit adjustment are the only commands
invoked through sudo.

Package version constraints apply only to the provisioning transaction; the
script does not add persistent package holds. If the configured repositories
cannot supply missing prerequisites without replacing installed packages,
provisioning stops. Supply matching development packages or a repository for
that system version. The package inventories and transaction constraints are
saved under `build/full-test/run/packages/`.
On Rocky Linux 9, installing missing Doxygen temporarily enables the configured
CRB repository for that transaction; the repository configuration stays unchanged.

On a machine with less than 3 GiB of RAM the default is `JOBS=1`. On a machine
with less than 4 GiB of RAM, the script ensures that at least 4 GiB of swap is
active. If it must create swap, it uses a UID-specific file under `/var/tmp`
and does not add the file to `fstab`. While the test is running, it raises a
lower global and cgroup swap preference to 60, then restores the original
values on exit. `POSTGAMMA_SWAP_SWAPPINESS` selects a different temporary
value. When invoked from a checkout, outputs remain under
`build/full-test`, including `full-test.log` and the full build tree. A
standalone copy of the script also stores its cloned source below that directory
in the current working directory. The checksum-matched toolchain is stored under
`build/full-test/state` and reused on later runs. Managed build, input, and
runner directories are recreated for each run; standalone mode also recreates
its cloned source directory. `make clean` removes the entire repository `build/`
directory, including this toolchain.

Inspect the operating-system plan without root privileges or host changes:

```bash
./scripts/full-test.sh --plan
```

Common overrides are explicit environment variables:

```bash
POSTGAMMA_JOBS=2 \
  PG_REPOSITORY=https://github.com/postgres/postgres.git \
  ./scripts/full-test.sh
```

The repository variables accept HTTPS URLs, Git URLs, local repositories, and
Git bundle files. Local inputs must be readable by the current user and are
copied into its managed input directory. An optional fallback can be configured
for every source:

```bash
POSTGAMMA_REPOSITORY=https://mirror.example/postgamma.git \
  POSTGAMMA_REPOSITORY_FALLBACK=https://github.com/postgamma/postgamma.git \
  PG_REPOSITORY=https://mirror.example/postgres.git \
  PG_REPOSITORY_FALLBACK=https://git.postgresql.org/git/postgresql.git \
  PGVECTOR_REPOSITORY=https://mirror.example/pgvector.git \
  PGVECTOR_REPOSITORY_FALLBACK=https://github.com/pgvector/pgvector.git \
  ./scripts/full-test.sh
```

Fallbacks are tried only when the primary clone fails. Checked-out commits and
the required pgvector release tag are still verified against the repository
manifests, so changing a download endpoint cannot change the tested source.

`POSTGAMMA_WORK_ROOT`, `POSTGAMMA_STATE_ROOT`, and `POSTGAMMA_TOOLCHAIN` select
user-owned managed directories. `POSTGAMMA_REPOSITORY` and
`POSTGAMMA_REF` let a standalone copy of the script clone a different main
repository or revision. `POSTGAMMA_CREATE_SWAP=no` disables swap creation.
`POSTGAMMA_SWAP_SWAPPINESS` controls the temporary low-memory swap preference
and defaults to 60.
`SESSION_EXECUTOR_REQUEST_TIMEOUT_SECONDS` controls the per-operation wait in
the 1,000-session executor gate and defaults to 300 seconds while preserving
the full workload. `SESSION_EXECUTOR_TIMEOUT_SECONDS` sets the outer timeout;
the clean-host runner uses 3600 seconds for that gate.
`RUNTIME_EVENTS_REQUEST_TIMEOUT_SECONDS` controls the per-operation wait in
the 1,000-connection events gate. The clean-host runner uses 300 seconds so
swapped-out connection state can be reclaimed on low-memory hosts;
`RUNTIME_EVENTS_TIMEOUT_SECONDS` bounds its normal driver to 1200 seconds.
Direct `make runtime-events-check` retains the 30-second operation and
300-second normal-driver defaults. The separate 32-connection trace run keeps
its 180-second outer limit. Connection counts, event assertions, resource
checks, and the dedicated timeout/cancellation tests are unchanged.
`LOGICAL_MANAGEMENT_TIMEOUT_SECONDS` controls each traced logical-management
workload and defaults to 1800 seconds in the clean-host runner for older
`strace` implementations without seccomp filtering.
Use `--prepare-only` to stop after source initialization and the complete
environment preflight, including the Wasm SDK and browser matrix. Cached tools
and browsers are reused. Browser execution has additional host-library and OS
requirements beyond the native glibc floor; unsupported browser environments
fail preflight rather than silently skipping Wasm.

## Product build and test commands

After installing the host compiler prerequisites, run these commands from the
repository root. Each build initializes the pinned source dependencies itself:

| Product | Check installed prerequisites | Build | Product tests |
| --- | --- | --- | --- |
| Threaded PostgreSQL server | `make server-doctor` | `make server-build JOBS=2` | `make server-test JOBS=2` |
| Embedded C SDK archive | `make c-doctor` | `make c-build JOBS=2` | `make c-test JOBS=2` |
| Python wheel | `make python-doctor` | `make python-build JOBS=2` | `make python-test JOBS=2` |
| Browser Wasm SDK | `make wasm-doctor` | `make wasm-build JOBS=2` | `make wasm-test JOBS=2` |

`wasm-build` prepares/reuses Emscripten and Node, generates target code, then
builds the npm tarball without installing browsers. It defaults to release; `WASM_MODE=debug` selects debug
for both generation and packaging. The C SDK build retains its static-consumer
and host-safety packaging gates. Other builds do not run full acceptance.
Existing lower-level target names remain available.
Doctor commands inspect installed dependencies without downloading tools or
initializing submodules. For a fresh Wasm checkout, run `make source-init` and
`make wasm-toolchain-setup` before a standalone `make wasm-doctor`.

On a prepared host, `make full-test JOBS=2` runs the complete native gate followed
by Wasm acceptance. Native-only CI can keep using `make complete-source-check`.
`make full-test-prepare` checks both environments without kernel compilation or
full acceptance and installs missing Wasm tools/browsers. Use
`make full-test-doctor` to only check an already prepared environment.
Multi-hour Wasm alpha qualification is explicit:
`make wasm-alpha-test JOBS=2`.

After a successful full test, `build/full-test/artifacts/` contains the exact
SDK archive, wheel, wheel receipt, and a manifest binding their hashes to the
tested source commit. `build/full-test/artifact-check.json` records the host,
resolved runtime library paths, required glibc symbol versions, and package
test results. It does not claim compatibility with systems on which the
artifacts have not been exercised.

To test those same bytes on another system, copy the artifact directory, check
out the source commit in its manifest, and run:

```bash
POSTGAMMA_TEST_ARTIFACTS="$PWD/build/transferred-artifacts" \
  ./scripts/full-test.sh --artifacts-only
```

This mode provisions the consumer test tools, links the packaged C SDK's
quickstart and pgvector consumer, and runs the existing installed-wheel unit,
integration and documentation checks. It does not fetch PostgreSQL or rebuild
the kernel. It supplements the complete source run; it does not replace it.
The default managed interpreter tests CPython 3.11. The release workflow's
other CPython versions remain separate release gates.

## Source-build prerequisites

Native source builds are supported on x86-64 Linux with glibc 2.17 or newer. Official
Linux binaries are compiled and tested at that floor with the generic x86-64
CPU baseline. A full local release build needs a C/C++ toolchain, matching LLVM
and Clang development files, PostgreSQL's generator and native-library
dependencies, Python development headers, tracing tools, and the documentation
toolchain.
LLVM and Clang releases 16 through 22 are validated; both tools must come from
the same major release.
GNU Make 4.3 or newer is required because the build graph uses grouped targets;
PostgreSQL's lower standalone Make requirement is not sufficient for this
superproject.

On Ubuntu or Debian, first install the distribution-independent prerequisites:

```bash
sudo apt-get update
sudo apt-get install -y \
  bison build-essential ca-certificates curl doxygen findutils flex git libicu-dev \
  libipc-run-perl libjson-pp-perl libmime-base64-perl libreadline-dev perl \
  pkg-config python3-dev python3-venv strace tar util-linux zlib1g-dev
```

Then install one matching LLVM/Clang development toolchain in the supported
range. If the distribution's unversioned packages are version 16 through 22,
use `sudo apt-get install clang libclang-dev llvm-dev`. Ubuntu 22.04 and Debian
12 instead default to LLVM 14. For those releases, this versioned LLVM 22 setup
uses the [official LLVM apt repository](https://apt.llvm.org/):

```bash
LLVM_MAJOR=22
. /etc/os-release
curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key |
  sudo tee /etc/apt/trusted.gpg.d/apt.llvm.org.asc >/dev/null
echo "deb [signed-by=/etc/apt/trusted.gpg.d/apt.llvm.org.asc] \
https://apt.llvm.org/${VERSION_CODENAME}/ \
llvm-toolchain-${VERSION_CODENAME}-${LLVM_MAJOR} main" |
  sudo tee /etc/apt/sources.list.d/apt.llvm.org.list >/dev/null
sudo apt-get update
sudo apt-get install -y \
  clang-${LLVM_MAJOR} libclang-${LLVM_MAJOR}-dev llvm-${LLVM_MAJOR}-dev
```

The build automatically prefers matching versioned commands such as
`llvm-config-22` and `clang++-22`; changing the system alternatives is not
required.

On Fedora, RHEL, Rocky Linux, or another dnf-based system whose default LLVM
is in the supported range, use:

```bash
sudo dnf install -y \
  bison clang clang-devel doxygen findutils flex gcc gcc-c++ git libicu-devel \
  libasan libtsan libubsan llvm-devel make perl perl-IPC-Run perl-Test-Harness \
  perl-JSON-PP perl-MIME-Base64 perl-Test-Simple perl-Time-HiRes \
  pkgconf-pkg-config python3-devel python3-pip readline-devel strace tar \
  util-linux zlib-devel
```

Amazon Linux 2023 needs versioned packages because its unversioned LLVM 15 and
Python 3.9 defaults are below the supported floors. The following LLVM 18 and
Python 3.11 combination is validated:

```bash
sudo dnf install -y \
  bison clang18 clang18-devel findutils flex gcc gcc-c++ git libicu-devel \
  libasan libtsan libubsan llvm18-devel make perl perl-IPC-Run perl-Test-Harness \
  perl-JSON-PP perl-MIME-Base64 perl-Test-Simple perl-Time-HiRes \
  pkgconf-pkg-config python3.11 python3.11-devel readline-devel strace tar \
  util-linux zlib-devel
make PYTHON=python3.11 doctor-python
```

Keep `PYTHON=python3.11` on subsequent make invocations. Versioned LLVM/Clang
executables are discovered automatically.

On openSUSE Leap 16, the validated package set is:

```bash
sudo zypper install -y \
  bison clang clang-devel doxygen findutils flex gcc gcc-c++ git libicu-devel \
  llvm-devel make perl perl-IPC-Run perl-JSON-PP perl-MIME-Base64 \
  perl-Test-Harness perl-Test-Simple perl-Time-HiRes pkgconf-pkg-config python3-devel \
  readline-devel strace tar util-linux zlib-devel
```

Enterprise Linux 8 currently supplies GNU Make 4.2.1. It can bootstrap a newer
Make, but it cannot evaluate this project's grouped targets. Install GNU Make
4.3 or newer in a separate prefix, put that prefix first on `PATH`, and confirm
the selected executable with `make --version` before running the preflight. The
release workflow builds the checksum-pinned GNU Make 4.4.1 source for this
reason.

Install the pinned documentation packages into the location recognized by the
Makefile:

```bash
python3 -m venv build/docs-tools
build/docs-tools/bin/python -m pip install -r docs/requirements.txt
```

Then initialize the exact source inputs and run the preflight that matches the
intended build:

```bash
make source-init
make doctor-native
```

Both PostgreSQL and pgvector are pinned Git submodules. This command populates
`postgres/` and `third_party/pgvector/` together. PostgreSQL uses its official
Git server by default; a checkout can select its GitHub mirror with:

```bash
make source-init PG_REPOSITORY=https://github.com/postgres/postgres.git
```

The choice is saved locally for initialization and subsequent fetches, while
the pinned commits and `.gitmodules` remain unchanged. `PGVECTOR_REPOSITORY`
configures the pgvector source in the same way. Run `make upstream-weekly JOBS=4`
to fetch and test the latest PostgreSQL `master` in an isolated worktree, or
select another upstream branch with `PG_BRANCH=REL_19_STABLE`.

The available preflights are:

| Command | Validates |
| --- | --- |
| `make doctor` | Native/Python/docs/Wasm tools, plus libraries and startup of all three browsers |
| `make server-doctor` | PostgreSQL source transformation and threaded build dependencies |
| `make c-doctor` | Native kernel, static C SDK, and runtime dependencies |
| `make python-doctor` | Native build plus the active CPython ABI and `Python.h` |
| `make wasm-doctor` | Host build tools, pinned Emscripten/Node/npm, SDK contracts and target-header compatibility; no browser required |
| `make wasm-browser-doctor` | Installed Chromium, Firefox and Linux WebKit libraries and actual page startup; no kernel build |
| `make docs-doctor` | Doxygen and the pinned MkDocs environment |
| `make full-test-doctor` | Native, documentation and Wasm test prerequisites, including all three browsers |
| `make doctor-native` / `make doctor-release` | Native/Python/docs release prerequisites; no Wasm tools or browsers required |

`make doctor` is the complete environment check. The product-specific commands
check only their own prerequisites. Doctors run small probes and write reports;
they do not install tools, modify source inputs or run kernel acceptance. Missing
dependencies fail with a remedy. The complete doctor continues independent
checks and returns nonzero if any fail. Browser startup is a preflight, not SQL,
OPFS or release qualification. Legacy `doctor-threaded`, `doctor-sdk`,
`doctor-python` and `doctor-docs` remain valid. The older `doctor-wasm` is the
internal SDK/Chromium probe check; use `wasm-doctor` for the SDK build environment.

The checks use the actual `CC`, `CXX`, `CFLAGS`, `LDFLAGS`, `LLVM_CONFIG`,
`CLANGXX`, and `PG_CONFIGURE_ARGS` selections. For example,
`PG_CONFIGURE_ARGS=--without-icu` disables the ICU probe, while
`--with-ssl=openssl` enables an OpenSSL compile/link probe. Optional PostgreSQL
features need their corresponding development packages. The full-test profile
also links small ASan/UBSan and TSan programs so missing compiler runtime
libraries fail during preflight.

The complete graph builds several PostgreSQL trees. Keep at least 8 GiB free,
prefer 4 GiB or more of memory, and set `JOBS` no higher than the useful CPU and
memory capacity. Source paths containing whitespace are unsupported. Runtime
evidence also requires `/proc`, `strace`/ptrace, and, for sanitizer gates,
permission to disable ASLR with `setarch -R`; restricted containers may need a
different security policy or a host build.

### Threaded server process limits

The generated threaded server shares one process-wide file-descriptor limit
across PostgreSQL roles. Complete source validation therefore needs a hard
`RLIMIT_NOFILE` of at least 300000. Check it with `ulimit -Hn`; when the hard
limit already permits it, run `ulimit -n 300000` before the tests. For a
systemd-managed runner, use `LimitNOFILE=1048576`.

## Threaded PostgreSQL developer build

For an installed server, cluster initialization, and a psql connection, follow
the [server quickstart](../server/quickstart.md). This section covers building
and validating the generated tree during development.

Build the transformed PostgreSQL tree without running the complete validation
suite:

```bash
make server-doctor JOBS=4
make server-build JOBS=4
```

The generated source is written to `build/generated/postgres/`, and its
out-of-tree build is written to `build/generated-build/`. These directories are
recreated from the pristine `postgres/` submodule and PostGamma-owned adapters,
manifests, transformer, and runtime sources; do not edit the generated files.

Run the full reference-versus-threaded PostgreSQL and isolation gate before
submitting changes to the transformation:

```bash
make server-test JOBS=4
```

Both builds keep the pinned source checkout clean. The reference server uses
the upstream backend unchanged. For upstream versions whose `pg_upgrade` passes
`SSIZE_MAX` to `copy_file_range`, configure writes a build-local `file.c` with
requests bounded by `PG_INT32_MAX`. This prevents offset-plus-length overflow
on older Linux kernels while retaining kernel-side copies, the short-copy loop,
error handling, and every upstream upgrade test. The same correction applies
to generated builds; its content hash is recorded in `.postgamma-configure.json`.

## Python wheel

The first release publishes platform-specific CPython wheels and no source
distribution. Once published, install the wheel matching the Python interpreter
and platform:

```bash
python -m pip install "postgamma=={{ POSTGAMMA_VERSION }}"
```

For an artifact downloaded from a release page:

```bash
python -m pip install ./postgamma-{{ POSTGAMMA_VERSION }}-*.whl
```

The wheel filename records the exact CPython ABI and may carry more than one
compatible manylinux tag. Let `pip` select the published artifact when a
package index is available; do not rename a developer `linux_x86_64` wheel to
claim manylinux compatibility.

Verify the package before creating data:

```bash
python -c "import postgamma; print(postgamma.__version__)"
python -c "import postgamma; print(postgamma.library_info())"
```

A wheel contains:

- the `postgamma` Python package;
- one CPython native extension with the private kernel linked into it;
- the matching PostgreSQL 19 resource pack;
- PostGamma, PostgreSQL, and pgvector license notices.

It does not require an installed PostgreSQL server or a separate
`libpostgamma.so`. Continue with the [Python quickstart](index.md).

### Build a wheel from source

After `make python-doctor` succeeds, run the product target:

```bash
make python-build JOBS=8
```

The wheel is written below `build/python-product/wheel/`. A developer build is
valid for testing on its build host; it does not automatically inherit the
release wheel's `manylinux_2_17` compatibility claim.

## Static C SDK

The static C product consists of a compile-time SDK and a runtime resource
pack. They must come from the same build.

The SDK prefix contains:

```text
include/postgamma/
├── postgamma.h
├── postgamma_arrow.h
└── postgamma_extension.h
lib/
├── libpostgamma.a
├── postgamma-static-libs.txt
└── pkgconfig/postgamma.pc
```

The resource pack contains PostgreSQL catalog bootstrap data, SQL support
files, time-zone data, and `bin/postgres` as an executable identity. The
embedded library does not start that executable.

Release assets package both parts as one archive with a stable layout:

```text
postgamma-sdk-{{ POSTGAMMA_VERSION }}-linux-x86_64/
├── include/postgamma/
├── lib/
├── resource-pack/
├── examples/quickstart.c
└── licenses/
```

Download `postgamma-sdk-{{ POSTGAMMA_VERSION }}-linux-x86_64.tar.gz` from the matching release,
verify its published SHA-256 digest, and unpack it. The directory itself is the
SDK prefix; its `resource-pack/` child is the runtime resource root.

### Build and validate from source

```bash
make c-doctor
make c-build JOBS=8
```

This runs the static consumer and host-safety gates, then creates the same
release archive layout below `build/dist/`. Packaging fails unless the
PostGamma, PostgreSQL, and pgvector license files are all present. Continue
with the [static C quickstart](c-static.md) for the exact compile and run
commands.

## Supported environment

The initial binary contract is:

| Component | Supported release target |
| --- | --- |
| Operating system | Linux |
| Architecture | x86-64 |
| CPU baseline | x86-64-v1; no AVX requirement |
| libc | glibc 2.17 or newer |
| Python | CPython 3.10 through 3.14 |
| Cluster format | PostgreSQL 19 |
| C language boundary | Versioned PostGamma C ABI |

Developer artifacts built on a newer distribution may require a newer glibc.
Only artifacts built and tested against the minimum release sysroot carry the
glibc 2.17 promise. Read [Platforms and glibc](../compatibility/platforms.md)
for static-link details.

## Build every local release artifact

From a complete source checkout, one target builds and tests the static SDK,
the wheel for the active CPython interpreter, and this documentation website:

```sh
make doctor-release
make release-candidate JOBS=8
```

The command runs the embedded release graph, compares a versioned 54-case SQL
corpus between PostgreSQL 19 over socket libpq and the in-process public API,
executes the isolated wheel tests, packages the tested static SDK, builds the
site in strict mode, and writes an aggregate receipt below `build/dist/`.

This is a local release candidate, not a publication command. Official wheels
must additionally pass the declared manylinux CPython matrix before upload.

## Verify before deployment

For Python, record `postgamma.library_info()` with the application build. For C,
check `pgm_abi_version()`, `pgm_postgresql_version()`, and required capability
bits before opening an instance.

Do not deploy a database directory created by a different PostgreSQL major.
Use logical dump and restore for major-version migration.

!!! note "Publication status"

    PostGamma-owned source is licensed under Apache-2.0. The commands above
    describe the candidate artifact layout. Upload to a package index or
    release service remains an explicit release-owner action and requires the
    complete manylinux execution evidence.
