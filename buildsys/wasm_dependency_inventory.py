#!/usr/bin/env python3
"""Inventory an existing native link closure for the WebAssembly port."""

from __future__ import annotations

import argparse
from collections import Counter, defaultdict
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import subprocess
from typing import Any

from build_static_library import StaticLibraryError, classify_response, closure_digest, load_response
from check_embedded_process_assumptions import ProcessAssumptionError, assumption_facts, write_json
from c_source_probe import function_span, sanitize_c
from derive_kernel_link import derive, response_content
from postgresql_embedded_adapter import load_adapter


KIND = "postgamma.wasm-native-dependency-inventory"

# These classify native references, not the availability of SDK providers.
SYMBOL_GROUPS = {
    "process": "fork vfork execv execve execvp execl execlp system popen pclose "
    "wait waitpid setsid setpgid posix_spawn posix_spawnp",
    "host_signal": "kill raise pthread_kill pthread_sigmask sigaction sigprocmask "
    "sigpending sigwait sigwaitinfo sigtimedwait sigqueue signal pause",
    "signal_set": "sigemptyset sigfillset sigaddset sigdelset sigismember",
    "process_timer": "setitimer getitimer alarm timer_create timer_settime timer_delete",
    "clock": "clock_gettime clock_getres gettimeofday time nanosleep clock_nanosleep "
    "sleep usleep",
    "wait_descriptor": "eventfd pipe pipe2 poll ppoll select pselect epoll_create "
    "epoll_create1 epoll_ctl epoll_wait epoll_pwait signalfd kqueue kevent",
    "shared_memory": "mmap munmap mprotect madvise shm_open shm_unlink shmget shmat "
    "shmdt shmctl memfd_create",
    "filesystem": "open openat close read write pread pwrite preadv pwritev readv "
    "writev lseek stat lstat fstat fstatat statfs fstatfs access faccessat chmod "
    "fchmod fchmodat chown fchown fchownat mkdir mkdirat rmdir unlink unlinkat "
    "rename renameat link linkat symlink symlinkat readlink readlinkat truncate "
    "ftruncate fsync fdatasync syncfs sync_file_range posix_fadvise posix_fallocate "
    "fallocate copy_file_range fcntl flock dup dup2 dup3 opendir fdopendir readdir "
    "closedir rewinddir realpath getcwd chdir fchdir umask utime utimes mkdtemp "
    "mkstemp mkostemp",
    "stdio": "fopen freopen fdopen fclose fflush fread fwrite fseek fseeko ftell "
    "ftello fileno fgets fputs fgetc fputc getc putc getchar putchar ungetc "
    "clearerr feof ferror fprintf vfprintf printf vprintf puts setvbuf setbuf "
    "tmpfile stdin stdout stderr",
    "memory_stream": "fopencookie open_memstream fmemopen funopen",
    "dynamic_loader": "dlopen dlclose dlsym dlerror dladdr",
    "network": "socket socketpair accept accept4 bind listen connect shutdown "
    "send recv sendto recvfrom sendmsg recvmsg getsockname getpeername "
    "getsockopt setsockopt getaddrinfo freeaddrinfo gai_strerror getnameinfo "
    "getifaddrs freeifaddrs gethostname gethostbyname inet_ntop inet_pton",
    "identity": "getpid getppid getuid geteuid getgid getegid getpwnam getpwuid "
    "getpwuid_r getgrnam getgrgid setuid seteuid setgid setegid",
    "environment": "getenv setenv unsetenv putenv environ",
    "locale": "setlocale newlocale duplocale freelocale uselocale nl_langinfo "
    "nl_langinfo_l strcoll strxfrm localeconv tzset localtime localtime_r "
    "gmtime gmtime_r mktime strftime mbstowcs wcstombs gnu_get_libc_version",
    "diagnostics": "backtrace backtrace_symbols backtrace_symbols_fd syslog "
    "openlog closelog",
    "resource_limits": "getrlimit setrlimit getrusage sysconf __sysconf prctl "
    "personality sched_getaffinity sched_setaffinity getpriority setpriority",
    "terminal": "isatty tcgetattr tcsetattr getopt getopt_long optarg optind opterr",
    "error_recovery": "setjmp _setjmp __sigsetjmp sigsetjmp longjmp _longjmp "
    "siglongjmp abort exit _exit _Exit atexit __assert_fail",
    "random": "getrandom getentropy arc4random arc4random_buf RAND_bytes",
    "compression": "deflate deflateEnd deflateInit_ deflateInit2_ inflate "
    "inflateEnd inflateInit_ inflateInit2_ compress compress2 uncompress crc32 "
    "gzopen gzdopen gzclose gzread gzwrite gzgets gzgetc gzerror gzeof",
    "c_library": "acos acosh asin asinh atan atan2 atanh atof atoi atol bsearch "
    "calloc cbrt ceil cos cosh erf erfc exp exp2 explicit_bzero floor fmod free "
    "frexp hypot ldexp lgamma log log10 log2 malloc memchr memcmp memcpy memmove "
    "memset pow realloc rint round sin sinh snprintf sqrt srandom strcasecmp "
    "strcat strchr strchrnul strcmp strcpy strcspn strdup strerror strerror_r "
    "strlcat strlcpy strlen strncasecmp strncat strncmp strncpy strnlen strpbrk "
    "strrchr strsep strsignal strspn strstr strtod strtof strtok_r strtol "
    "strtoll strtoul strtoull tan tanh tgamma tolower toupper trunc vsnprintf "
    "qsort random rand srand sscanf fscanf vsscanf",
}
EXACT_GROUP = {
    symbol: group for group, names in SYMBOL_GROUPS.items() for symbol in names.split()
}
PREFIX_GROUPS = (
    ("posix_spawn", "process"),
    ("pthread_", "thread"),
    ("sem_", "semaphore"),
    ("sched_yield", "thread"),
    ("LZ4_", "compression"),
    ("ZSTD_", "compression"),
    ("__", "native_abi"),
    ("_GLOBAL_OFFSET_TABLE_", "native_abi"),
)
EXCLUDED_GROUPS = {"process", "process_timer", "host_signal", "network", "dynamic_loader"}
PROVIDERS = {
    "process": "Exclude native process/shell paths from the embedded target link.",
    "host_signal": "Retain logical interrupts; replace required host signal coordination.",
    "signal_set": "Validate the SDK signal-set representation used by logical masks.",
    "process_timer": "Keep virtual session deadlines; remove the native timer fallback.",
    "clock": "Supply consistent clock domains and deadline-aware waits.",
    "wait_descriptor": "Adapt PostgreSQL WaitEventSet and carrier wakeups together.",
    "thread": "Validate target pthread, TLS, stack, and lifecycle providers.",
    "semaphore": "Provide process-local counting/wakeup semantics on shared linear memory.",
    "shared_memory": "Replace OS segment identity/allocation with instance-owned target memory.",
    "filesystem": "Implement the selected target filesystem and database ownership contract.",
    "stdio": "Validate target stdio against the selected filesystem and diagnostics sinks.",
    "memory_stream": "Validate or replace GNU memory streams used by target initdb capture.",
    "dynamic_loader": "Keep static module lookup; exclude native dynamic loading.",
    "network": "Keep memory transport; exclude socket/DNS branches from the target link.",
    "identity": "Define logical role/database identity without host PID or user databases.",
    "environment": "Preserve context ownership of configuration and environment access.",
    "locale": "Choose and test target locale/collation support explicitly.",
    "diagnostics": "Route diagnostics to the host; review native backtrace/syslog fallbacks.",
    "resource_limits": "Provide target resource budgets; review native limits and host probes.",
    "terminal": "Review retained frontend/interactive paths against the initial API scope.",
    "error_recovery": "Validate same-thread error recovery and instance-fatal cleanup.",
    "random": "Validate an entropy provider; an ordinary persistent file is not sufficient.",
    "compression": "Choose exclusion or target-built optional compression libraries.",
    "native_abi": "Recompile source for the target ABI; do not retain native compiler imports.",
    "c_library": "Recompile and validate with the selected target C/math libraries.",
    "unclassified": "Review the source/caller before selecting a target provider or exclusion.",
}

# Focused current-source anchors supplement the existing bootstrap assumptions.
# Function scope is evidence of location, not a control-flow/reachability proof.
SEAMS = (
    ("thread-stack", "runtime/src/thread_runtime.c",
     ("postgamma_thread_create", "query_current_stack"), "all role/carrier threads",
     "required-adaptation", "Native pthread attributes and Linux stack inspection."),
    ("carrier-wakeup", "runtime/src/thread_runtime.c", ("postgamma_wake_target_create",),
     "role/carrier notification", "required-adaptation", "Linux eventfd or non-Linux pipe."),
    ("postgres-wait", "runtime/include/postgamma/postgres_waiteventset_runtime_impl.h",
     ("postgamma_backend_attach_wait_event",), "blocked PostgreSQL role",
     "required-adaptation", "Wake target is registered as WL_SOCKET_READABLE."),
    ("virtual-deadline", "runtime/src/postgres_backend_runtime.c",
     ("postgamma_backend_setitimer", "postgamma_backend_adjust_wait_timeout",
      "postgamma_dispatch_pending_signals"), "bound session or standalone execution",
     "already-virtualized", "Virtual timer and pending SIGALRM; clocks and waits still need porting."),
    ("native-shell-fallback", "runtime/src/postgres_backend_runtime.c",
     ("postgamma_system", "postgamma_popen"), "native compatibility path",
     "excluded", "Embedded profile returns ENOTSUP; compiled fallback imports remain."),
    ("shared-segment", "postgres/src/backend/port/sysv_shmem.c",
     ("PGSharedMemoryCreate", "CreateAnonymousSegment", "InternalIpcMemoryCreate"),
     "postmaster shared-memory initialization", "required-adaptation",
     "The mmap branch still allocates a minimal System V segment and uses inode identity."),
    ("dynamic-shared-memory", "postgres/src/backend/storage/ipc/dsm_impl.c",
     ("dsm_impl_op",), "shared-memory consumers", "required-adaptation",
     "OS shared-memory providers remain; no inference that all DSM use requires parallel query."),
    ("semaphores", "postgres/src/backend/port/posix_sema.c", ("PGSemaphoreInit", "PGSemaphoreLock"),
     "shared-memory initialization and backend waits", "required-adaptation",
     "Selected native unnamed POSIX semaphore provider needs target validation."),
    ("cluster-owner", "embedded-c/src/data_directory_lock.c",
     ("postgamma_data_directory_lock_acquire",), "instance owner",
     "required-adaptation", "Host file identity and fcntl/OFD locking are native contracts."),
    ("relative-filesystem", "runtime/src/path_runtime.c", ("postgamma_path_open",),
     "bound instance filesystem access", "required-adaptation",
     "Instance-relative paths already exist; descriptor/openat semantics remain native."),
    ("initdb-memory-stream", "embedded-c/src/postgres_initdb_host.c",
     ("postgamma_initdb_popen", "open_capture_stream"), "in-process initdb",
     "required-adaptation", "Initialization uses open_memstream and fopencookie, not a shell."),
    ("kernel-entry", "runtime/src/postgres_embedded_kernel.c", ("embedded_instance_main",),
     "embedded postmaster control", "already-virtualized",
     "In-process PostmasterMain and sigsetjmp cleanup still require target recovery tests."),
    ("logical-tool-stub", "embedded-c/src/postgres_logical_tool_host.c", ("fork",),
     "native logical management tools", "excluded",
     "The local fork definition rejects process creation; logical tools are outside initial Wasm scope."),
    ("entropy-source", "postgres/src/port/pg_strong_random.c", (),
     "initialization and random-value consumers", "required-adaptation",
     "The non-OpenSSL, non-Windows provider reads /dev/urandom; validate target entropy independently of OPFS."),
    ("libc-collation-version", "postgres/src/backend/utils/adt/pg_locale_libc.c",
     ("get_collation_actual_version_libc",), "SQL collation metadata",
     "required-adaptation", "The __GLIBC__ branch uses gnu_get_libc_version for non-C locales."),
)
SEAM_LITERALS = {"entropy-source": ('"/dev/urandom"',)}


class InventoryError(ValueError):
    """Native evidence cannot be inventoried unambiguously."""


def run(arguments: list[str], *, cwd: Path, input_text: str | None = None) -> str:
    result = subprocess.run(arguments, cwd=cwd, input=input_text, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
    if result.returncode:
        raise InventoryError(f"{arguments[0]} failed ({result.returncode}): {result.stderr.strip()}")
    return result.stdout


def load_json(path: Path) -> dict[str, Any]:
    document = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(document, dict):
        raise InventoryError(f"expected JSON object: {path}")
    return document


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def display(path: Path, root: Path) -> str:
    try:
        return path.relative_to(root).as_posix()
    except ValueError:
        return path.as_posix()


def classify_symbol(symbol: str) -> dict[str, str]:
    name = symbol.split("@", 1)[0]
    group = EXACT_GROUP.get(name)
    if group is None and (re.match(r"^(u_|ucasemap_|ucnv_|ucol_|uiter_|uloc_)", name)
                          or re.match(r"^(?:str|isw|tow|to)[a-z]+_l$", name)):
        group = "locale"
    if group is None:
        group = next((group for prefix, group in PREFIX_GROUPS if name.startswith(prefix)),
                     "unclassified")
    disposition = ("excluded" if group in EXCLUDED_GROUPS else
                   "unresolved" if group in {"unclassified", "compression", "terminal"} else
                   "required-adaptation")
    return {"category": group, "target_disposition": disposition,
            "provider_action": PROVIDERS[group]}


def direct_input_variables(makefile: str) -> list[str]:
    start = makefile.find("$(EMBEDDED_KERNEL_PUBLIC_API_STATIC_LIBRARY) \\\n")
    end = makefile.find("\nkernel-static-library:", start)
    if start < 0 or end < 0:
        raise InventoryError("native static-library rule was not found")
    names = re.findall(r'--input "\$\((\w+)\)"', makefile[start:end])
    if not names or len(names) != len(set(names)):
        raise InventoryError("native direct-input list is empty or ambiguous")
    return names


def native_inputs(root: Path, build: Path) -> dict[str, Path]:
    names = direct_input_variables((root / "Makefile").read_text(encoding="utf-8"))
    query = [".PHONY: _postgamma_wasm_inventory_values", "_postgamma_wasm_inventory_values:"]
    query.extend(f"\t$(info WASM_AUDIT:{name}=$({name}))" for name in names)
    query.append("\t@:")
    output = run(["make", "--no-print-directory", "-s", "-f", "Makefile", "-f", "-",
                  f"BUILD_DIR={build}", "_postgamma_wasm_inventory_values"],
                 cwd=root, input_text="\n".join(query) + "\n")
    values = {}
    for line in output.splitlines():
        if line.startswith("WASM_AUDIT:"):
            key, value = line.removeprefix("WASM_AUDIT:").split("=", 1)
            if key in values or not value:
                raise InventoryError(f"invalid native input: {key}")
            values[key] = Path(value).resolve(strict=True)
    if list(values) != names:
        raise InventoryError("native input variable expansion is incomplete")
    return values


def parse_nm(output: str) -> list[dict[str, str]]:
    records = []
    for line in output.splitlines():
        if not line.strip():
            continue
        match = re.fullmatch(r"(.+?): (\S+) ([Uwv])\s*(?:\t(.*))?", line)
        if match is None:
            raise InventoryError(f"unrecognized undefined-symbol record: {line}")
        container, symbol, kind, location = match.groups()
        records.append({"container": container, "symbol": symbol, "binding": kind,
                        "location": location or ""})
    return records


def nm_references(paths: list[Path], root: Path, nm: str, *, locations: bool) -> list[dict[str, str]]:
    records = []
    for start in range(0, len(paths), 48):
        arguments = [nm, "-A", "-P", "-u"]
        if locations:
            arguments.append("-l")
        output = run([*arguments, *map(str, paths[start:start + 48])], cwd=root)
        records.extend(parse_nm(output))
    return records


def resolve_location(location: str, root: Path, build: Path,
                     basenames: dict[str, set[Path]]) -> dict[str, Any]:
    if not location:
        return {"status": "unavailable"}
    match = re.fullmatch(r"(.+):(\d+)(?: \(discriminator \d+\))?", location)
    if match is None:
        return {"status": "unparsed", "raw": location}
    name, line = match.groups()
    candidates: set[Path] = set()
    prefixes = {
        "postgamma-source/": root, "postgamma-build/": build,
        "postgresql-build/": build / "embedded-bootstrap",
        "embedded-kernel-build/": build / "embedded-kernel",
        "logical-management-build/": build / "embedded-logical",
        "extension-runtime-build/": build / "embedded-release",
    }
    for prefix, base in prefixes.items():
        for occurrence in re.finditer(re.escape(prefix), name):
            path = base / name[occurrence.end():]
            if path.is_file():
                candidates.add(path.resolve())
    direct = Path(name)
    if direct.is_absolute() and direct.is_file():
        candidates.add(direct.resolve())
    if not candidates and "/" not in name:
        candidates.update(basenames.get(name, set()))
    if len(candidates) == 1:
        path = next(iter(candidates))
        return {"status": "mapped" if int(line) > 0 else "file-only",
                "source": display(path, root), "line": int(line), "raw": location}
    return {"status": "ambiguous" if candidates else "unmapped", "raw": location,
            "candidates": sorted(display(path, root) for path in candidates)}


def compare_hash(path: Path, expected: str, root: Path, label: str) -> dict[str, Any]:
    actual = sha256(path) if path.is_file() else None
    return {"check": label, "path": display(path, root), "expected": expected,
            "actual": actual, "status": "matched" if expected == actual else "mismatch"}


def function_scopes(source: str) -> list[dict[str, Any]]:
    """Find PostgreSQL-style function bodies without treating comments as code."""
    sanitized = sanitize_c(source)
    stack = []
    ends = {}
    for offset, character in enumerate(sanitized):
        if character == "{":
            stack.append(offset)
        elif character == "}" and stack:
            ends[stack.pop()] = offset + 1
    scopes = []
    for match in re.finditer(r"^([A-Za-z_]\w*)\s*\([^;{}]*\)\s*\{", sanitized, re.M):
        start = match.end() - 1
        if start in ends:
            scopes.append({"function": match.group(1), "start": start, "end": ends[start],
                           "line": source.count("\n", 0, start) + 1,
                           "end_line": source.count("\n", 0, ends[start]) + 1})
    return scopes


def source_callers(source: str, symbol: str, line: int,
                   scopes: list[dict[str, Any]]) -> dict[str, Any]:
    if line > 0:
        return {"kind": "debug-line-scope", "functions": [scope["function"] for scope in scopes
                if scope["line"] <= line <= scope["end_line"]]}
    spelling = re.sub(r"^__isoc(?:23|99)_", "", symbol)
    if spelling == "__sigsetjmp":
        spelling = "sigsetjmp"
    offsets = [match.start() for match in re.finditer(rf"\b{re.escape(spelling)}\s*\(", source)]
    candidates = sorted({scope["function"] for scope in scopes for offset in offsets
                         if scope["start"] < offset < scope["end"]})
    return {"kind": "current-source-candidates", "functions": candidates}


def source_seams(root: Path) -> list[dict[str, Any]]:
    records = []
    for identity, filename, functions, role, disposition, condition in SEAMS:
        path = root / filename
        raw = path.read_text(encoding="utf-8")
        sanitized = sanitize_c(raw)
        anchors = []
        for function in functions:
            start, end = function_span(sanitized, function, 1)
            anchors.append({"function": function, "line": raw.count("\n", 0, start) + 1,
                            "end_line": raw.count("\n", 0, end) + 1})
        for literal in SEAM_LITERALS.get(identity, ()):
            if raw.count(literal) != 1:
                raise InventoryError(f"source literal is missing or ambiguous: {filename}: {literal}")
            anchors.append({"literal": literal, "line": raw.count("\n", 0, raw.index(literal)) + 1})
        records.append({"id": identity, "source": filename, "source_sha256": sha256(path),
                        "role": role, "native_condition": condition,
                        "target_disposition": disposition, "anchors": anchors})
    return records


def dependency_records(external: set[str], references: dict[str, list[dict[str, Any]]]) -> list[dict[str, Any]]:
    return [{"symbol": symbol, **classify_symbol(symbol),
             "at_native_library_boundary": symbol in external,
             "references": references.get(symbol, [])}
            for symbol in sorted(external | set(references))]


def collect(root: Path, build: Path, nm: str) -> dict[str, Any]:
    seams = source_seams(root)
    bootstrap = build / "embedded-bootstrap"
    pg_build = bootstrap / "postgres-pic"
    generated = bootstrap / "generated/postgres"
    link_path = bootstrap / "reports/kernel-link-report.json"
    link = load_json(link_path)
    db_path = bootstrap / "link-db/postgresql.json"
    adapter_path = root / "manifests/postgresql/embedded.json"
    upstream_path = root / "manifests/upstream.json"
    config_path = pg_build / ".postgamma-configure.json"
    log_path = pg_build / "config.log"
    adapter = load_adapter(adapter_path)
    closure, derived = derive(load_json(db_path), pg_build, config_path, log_path,
                              upstream_path, adapter)
    response_path = bootstrap / "kernel/kernel-link.rsp"
    if response_content(closure) != response_path.read_text(encoding="utf-8"):
        raise InventoryError("captured backend command and response file disagree")
    if derived["included"] != link.get("included"):
        raise InventoryError("captured backend inputs and kernel report disagree")
    direct = native_inputs(root, build)
    _arguments, libraries, backend = classify_response(load_response(response_path),
                                                       pg_build / "src/backend")
    ordered = [*direct.values(), *backend]
    receipt_path = build / "embedded-kernel/reports/static-library-link.json"
    receipt = load_json(receipt_path)
    library = Path(receipt["output"]).resolve(strict=True)
    freshness = [compare_hash(library, receipt["output_sha256"], root, "native-static-library"),
                 compare_hash(response_path, link["response_sha256"], root, "kernel-response")]
    digest = closure_digest(ordered)
    freshness.append({"check": "native-static-inputs", "expected": receipt["closure"],
                      "actual": {"input_count": len(ordered), "input_digest": digest},
                      "status": "matched" if digest == receipt["closure"]["input_digest"]
                      and len(ordered) == receipt["closure"]["input_count"] else "mismatch"})
    for key, path in (("embedded_adapter_sha256", adapter_path),
                      ("configure_state_sha256", config_path),
                      ("toolchain_config_log_sha256", log_path),
                      ("link_database_sha256", db_path)):
        freshness.append(compare_hash(path, link["identity"][key], root, key))
    configuration = load_json(config_path)
    freshness.append(compare_hash(generated / "configure", configuration["configure_sha256"],
                                  root, "configured-source"))
    for name, expected in configuration.get("inputs", {}).items():
        freshness.append(compare_hash(Path(name), expected, root, "configure-input"))

    inputs: dict[Path, set[str]] = defaultdict(set)
    for name, path in direct.items():
        inputs[path].add(name)
    for path in backend:
        inputs[path].add("captured-postgres-backend")
    bundle_reports = (
        bootstrap / "reports/private-libpq-link.json",
        build / "embedded-kernel/reports/private-initdb-link.json",
        build / "embedded-logical/reports/private-logical-tool-link.json",
        build / "embedded-kernel/reports/embedded-static-modules.json",
    )
    bundled_modules = []
    for path in bundle_reports:
        bundle = load_json(path)
        freshness.append(compare_hash(Path(bundle["output"]), bundle["output_sha256"], root,
                                      f"bundle-output:{path.name}"))
        for name, expected in bundle["inputs"].items():
            item = Path(name).resolve(strict=True)
            freshness.append(compare_hash(item, expected, root, f"bundle-input:{path.name}"))
            if item.suffix in {".o", ".a"}:
                inputs[item].add(f"expanded-bundle:{path.name}")
        for module in bundle.get("modules", []):
            bundled_modules.append({"id": module["id"], "logical_name": module["logical_name"],
                                    "build": module["build"],
                                    "capabilities": module["capabilities"]})
            for name in module["object_inputs"]:
                inputs[Path(name).resolve(strict=True)].add(f"static-module:{module['id']}")

    support_path = generated / ".postgamma-support.json"
    support = load_json(support_path)
    for name, expected in support["sha256"].items():
        freshness.append(compare_hash(generated / name, expected, root, "generated-support"))
    postgres_adapter_path = root / "manifests/postgresql/adapter.json"
    postgres_adapter = load_json(postgres_adapter_path)
    freshness.append(compare_hash(postgres_adapter_path, support["adapter"]["sha256"], root,
                                  "generated-support-adapter"))
    for copy in postgres_adapter["generated_support"]["copies"]:
        if copy["source_root"] == "project":
            freshness.append(compare_hash(generated / copy["target"],
                                          sha256(root / copy["source"]), root,
                                          "current-runtime-copy"))

    native_assumptions_path = bootstrap / "reports/process-assumptions.json"
    native_assumptions = load_json(native_assumptions_path)
    old_records = {record["id"]: record for record in native_assumptions["records"]}
    current_assumptions = []
    for assumption in adapter["process_assumptions"]:
        current, _keys = assumption_facts(assumption, root, root / "postgres")
        current_assumptions.append(current)
        freshness.append({"check": "process-assumption-source", "id": current["id"],
                          "expected": old_records.get(current["id"], {}).get("source_sha256"),
                          "actual": current["source_sha256"],
                          "status": "matched" if old_records.get(current["id"], {}).get(
                              "source_sha256") == current["source_sha256"] else "mismatch"})

    boundary = nm_references([library], root, nm, locations=False)
    external = {record["symbol"] for record in boundary}
    scanned = nm_references(sorted(inputs), root, nm, locations=True)
    basenames: dict[str, set[Path]] = defaultdict(set)
    for directory in (root / "runtime", root / "embedded-c", root / "extensions"):
        for path in directory.rglob("*.c"):
            basenames[path.name].add(path.resolve())
    by_symbol: dict[str, list[dict[str, Any]]] = defaultdict(list)
    input_counts: Counter[str] = Counter()
    mapping_counts: Counter[str] = Counter()
    source_cache: dict[str, tuple[str, list[dict[str, Any]]]] = {}
    for record in scanned:
        symbol = record["symbol"]
        classification = classify_symbol(symbol)
        if symbol not in external and classification["category"] == "unclassified":
            continue
        container = record["container"]
        relative_container = container.removeprefix(str(root) + "/")
        location = resolve_location(record["location"], root, build, basenames)
        filename = location.get("source")
        if filename and classification["category"] not in {"c_library", "native_abi", "stdio"}:
            if filename not in source_cache:
                content = (root / filename).read_text(encoding="utf-8")
                source_cache[filename] = (sanitize_c(content), function_scopes(content))
            content, scopes = source_cache[filename]
            location["caller"] = source_callers(content, symbol, location["line"], scopes)
        mapping_counts[location["status"]] += 1
        by_symbol[symbol].append({"object": relative_container, "binding": record["binding"],
                                  "source_location": location})
        input_counts[relative_container.split("[", 1)[0]] += 1
    dependencies = dependency_records(external, by_symbol)

    source_hashes = {}
    for dependency in dependencies:
        for reference in dependency["references"]:
            filename = reference["source_location"].get("source")
            if filename and filename not in source_hashes:
                source_hashes[filename] = sha256(root / filename)
    modules = {}
    for module in ("postgres", "third_party/pgvector"):
        modules[module] = {"commit": run(["git", "rev-parse", "HEAD"], cwd=root / module).strip(),
                           "status": run(["git", "status", "--porcelain"], cwd=root / module)}
    expected_commit = load_json(upstream_path)["commit"]
    source_marker = load_json(generated / ".postgamma-source.json")
    for label, actual in (("captured-upstream", link["identity"]["upstream_commit"]),
                          ("materialized-upstream", source_marker["source_commit"]),
                          ("postgres-checkout", modules["postgres"]["commit"])):
        freshness.append({"check": label, "actual": actual, "expected": expected_commit,
                          "status": "matched" if actual == expected_commit else "mismatch"})
    checks = Counter(check["status"] for check in freshness)
    return {
        "schema_version": 1, "kind": KIND, "status": "inventoried",
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "wasm_validated": False,
        "scope": "existing native public C API superset, plus expanded bundle inputs",
        "source_identity": {"commit": run(["git", "rev-parse", "HEAD"], cwd=root).strip(),
                            "working_tree": run(["git", "status", "--porcelain"], cwd=root),
                            "submodules": modules, "mapped_source_sha256": source_hashes},
        "tool_identity": {"script_sha256": sha256(Path(__file__)),
                          "nm": run([nm, "--version"], cwd=root).splitlines()[0]},
        "native_evidence_consistency": {"status": "matched" if not checks["mismatch"] else "mismatch",
                                        "counts": dict(checks), "checks": freshness},
        "inputs": [{"path": display(path, root), "sha256": sha256(path),
                    "kind": "archive" if path.suffix == ".a" else "object",
                    "origins": sorted(origins),
                    "dependency_reference_count": input_counts[display(path, root)]}
                   for path, origins in sorted(inputs.items())],
        "native_library": {"path": display(library, root), "sha256": sha256(library),
                           "direct_input_count": len(direct), "backend_input_count": len(backend),
                           "external_symbol_count": len(external), "consumer_link_options": libraries,
                           "configure_arguments": configuration["arguments"],
                           "support_profile": support["adapter"]["support_profile"],
                           "bundled_modules": bundled_modules},
        "dependencies": dependencies,
        "summary": {"input_count": len(inputs), "dependency_count": len(dependencies),
                    "reference_count": sum(map(len, by_symbol.values())),
                    "source_mapping": dict(sorted(mapping_counts.items())),
                    "native_external_symbols_without_input_reference": sorted(
                        symbol for symbol in external if not by_symbol.get(symbol)),
                    "categories": dict(sorted(Counter(d["category"] for d in dependencies).items())),
                    "dispositions": dict(sorted(Counter(d["target_disposition"] for d in dependencies).items()))},
        "reviewed_source_seams": seams,
        "native_bootstrap_assumptions": current_assumptions,
        "captured_exclusions": link["excluded"],
        "catalog_registration_roots": link["catalog_registration_roots"],
        "limitations": [
            "This is a native dependency inventory, not a WebAssembly compile or runtime result.",
            "Target dispositions are porting actions, not evidence that the target already implements them.",
            "The native public API is a superset including logical tools and optional native libraries.",
            "Expanded archives include unselected members; object references are not call-graph reachability.",
            "Debug locations may name only a file or one reference; unmapped locations remain explicit.",
            "Caller hints describe current source scopes/candidates, not a preprocessed call graph or all references.",
            "Receipt hashes establish correspondence with recorded artifacts, not a fresh rebuild of every source.",
            "Bootstrap assumptions retain their original scope; historical executable passes are not rerun here.",
            "Runtime profile guards alone do not eliminate imports; target link/import validation is still required.",
            "Indirect calls, inline instructions, platform headers and uncompiled branches require target review.",
        ],
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", required=True, type=Path)
    parser.add_argument("--build-root", type=Path)
    parser.add_argument("--nm", default="nm")
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    try:
        if args.output.exists():
            raise InventoryError("output already exists; select a new run path to retain prior evidence")
        root = args.root.resolve(strict=True)
        build = (args.build_root or root / "build").resolve(strict=True)
        report = collect(root, build, args.nm)
        report["command"] = ["python3", display(Path(__file__).resolve(), root),
                             "--root", str(root), "--build-root", str(build),
                             "--nm", args.nm, "--output", str(args.output.resolve())]
        write_json(args.output.resolve(), report)
    except (OSError, ValueError, KeyError, StaticLibraryError, ProcessAssumptionError) as error:
        parser.error(str(error))
    print(f"Wasm native inventory: {report['summary']['input_count']} inputs, "
          f"{report['native_library']['external_symbol_count']} native external symbols; "
          f"evidence={report['native_evidence_consistency']['status']}; Wasm validation pending")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
