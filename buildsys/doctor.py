#!/usr/bin/env python3
"""Validate the host before starting an expensive PostGamma build."""

from __future__ import annotations

import argparse
import locale
import os
import platform
import re
import shlex
import shutil
import subprocess
import sys
import sysconfig
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Mapping, Sequence

from toolchain import (
    LLVM_MAJORS,
    find_clang_cpp_library,
    find_clang_resource_dir,
    find_clangxx,
    find_llvm_config,
)


LLVM_VERSION_RANGE = f"{min(LLVM_MAJORS)} through {max(LLVM_MAJORS)}"
LLVM_VERSION_SPAN = f"{min(LLVM_MAJORS)}-{max(LLVM_MAJORS)}"
# Parsing LibTooling headers can exceed the small compiler probes' time budget.
AST_COMPILE_TIMEOUT = 120.0
READLINE_PROBE_SOURCE = (
    "#include <stdio.h>\n"
    "#include <readline/readline.h>\n"
    "int main(void) { rl_initialize(); return 0; }\n"
)

try:
    import resource
except ImportError:  # pragma: no cover - permits a useful unsupported-host report
    resource = None  # type: ignore[assignment]


MINIMUM_GLIBC = (2, 28)
MINIMUM_MAKE = (4, 3)
MINIMUM_PYTHON = (3, 10)
MAXIMUM_WHEEL_PYTHON = (3, 14)
AMAZON_LINUX_LLVM_MAJOR = 18
AMAZON_LINUX_PYTHON = "python3.11"
RECOMMENDED_DISK_BYTES = 8 * 1024**3
RECOMMENDED_MEMORY_BYTES = 4 * 1024**3
MINIMUM_TOOL_VERSIONS = {
    "bison": (2, 3),
    "flex": (2, 5, 35),
    "perl": (5, 14),
    "pkg-config": (0, 9),
}

# Descriptive profiles let each Make target check only the tools it consumes.
PROFILE_GROUPS = {
    "foundation": {"host", "checkout", "base"},
    "ast": {"host", "checkout", "base", "ast"},
    "full-test": {
        "host",
        "checkout",
        "base",
        "ast",
        "native",
        "runtime",
        "resources",
        "environment",
        "test",
    },
    "build": {
        "host",
        "checkout",
        "base",
        "ast",
        "native",
        "artifact",
        "runtime",
        "resources",
        "environment",
    },
    "threaded": {
        "host",
        "checkout",
        "base",
        "ast",
        "native",
        "resources",
        "environment",
    },
    "python": {
        "host",
        "checkout",
        "base",
        "ast",
        "native",
        "artifact",
        "runtime",
        "resources",
        "environment",
        "python",
    },
    "docs": {"docs", "resources", "environment"},
    "release": {
        "host",
        "checkout",
        "base",
        "ast",
        "native",
        "artifact",
        "runtime",
        "resources",
        "environment",
        "python",
        "test",
        "docs",
    },
}

# Public profile names describe the artifact boundary.  The older names remain
# available for internal build targets and existing automation.
PROFILE_GROUPS["sdk"] = PROFILE_GROUPS["build"]
PROFILE_GROUPS["all"] = PROFILE_GROUPS["release"]

GROUP_TITLES = {
    "host": "Supported host",
    "checkout": "Source checkout",
    "base": "Base toolchain",
    "ast": "Clang AST toolchain",
    "native": "PostgreSQL native dependencies",
    "artifact": "ELF and packaging tools",
    "runtime": "Runtime test capabilities",
    "python": "Python wheel toolchain",
    "test": "Complete test toolchain",
    "docs": "Documentation toolchain",
    "resources": "Host resources",
    "environment": "Environment overrides",
}

# key, display name, environment override, default, group, version arguments
TOOL_SPECS = (
    ("git", "git", None, "git", "base", ("--version",)),
    ("make", "GNU make", "MAKE", "make", "base", ("--version",)),
    ("bash", "bash", None, "/bin/bash", "base", ("--version",)),
    ("cc", "C compiler", "CC", "cc", "base", ("--version",)),
    ("cxx", "C++ compiler", "CXX", "c++", "base", ("--version",)),
    ("ar", "archiver", "AR", "ar", "base", ("--version",)),
    ("nm", "symbol reader", "NM", "nm", "base", ("--version",)),
    ("bison", "bison", "BISON", "bison", "base", ("--version",)),
    ("flex", "flex", "FLEX", "flex", "base", ("--version",)),
    ("perl", "Perl", "PERL", "perl", "base", ("-v",)),
    ("pkg-config", "pkg-config", "PKG_CONFIG", "pkg-config", "base", ("--version",)),
    ("find", "find", None, "find", "base", ("--version",)),
    ("tar", "tar", "TAR", "tar", "base", ("--version",)),
    ("readelf", "readelf", "READELF", "readelf", "artifact", ("--version",)),
    ("objcopy", "objcopy", "OBJCOPY", "objcopy", "artifact", ("--version",)),
    ("strace", "strace", "STRACE", "strace", "runtime", ("--version",)),
    ("setarch", "setarch", "SETARCH", "setarch", "test", ("--version",)),
    ("doxygen", "Doxygen", "DOXYGEN", "doxygen", "docs", ("--version",)),
    ("mkdocs", "MkDocs", "MKDOCS", "mkdocs", "docs", ("--version",)),
)

APT_PACKAGES = {
    "git": "git",
    "make": "build-essential",
    "bash": "bash",
    "cc": "build-essential",
    "cxx": "build-essential",
    "ar": "binutils",
    "nm": "binutils",
    "bison": "bison",
    "flex": "flex",
    "perl": "perl",
    "pkg-config": "pkg-config",
    "find": "findutils",
    "tar": "tar",
    "readelf": "binutils",
    "objcopy": "binutils",
    "strace": "strace",
    "setarch": "util-linux",
    "llvm-config": "llvm-dev",
    "clang++": "clang libclang-dev",
    "clang-probe": "clang libclang-dev llvm-dev",
    "perl-ipc-run": "libipc-run-perl",
    "readline": "libreadline-dev",
    "zlib": "zlib1g-dev",
    "icu": "libicu-dev",
    "openssl": "libssl-dev",
    "lz4": "liblz4-dev",
    "zstd": "libzstd-dev",
    "libxml": "libxml2-dev",
    "python-headers": "python3-dev",
    "python": "python3",
    "doxygen": "doxygen",
}

DNF_PACKAGES = {
    "git": "git",
    "make": "make",
    "bash": "bash",
    "cc": "gcc",
    "cxx": "gcc-c++",
    "ar": "binutils",
    "nm": "binutils",
    "bison": "bison",
    "flex": "flex",
    "perl": "perl",
    "pkg-config": "pkgconf-pkg-config",
    "find": "findutils",
    "tar": "tar",
    "readelf": "binutils",
    "objcopy": "binutils",
    "strace": "strace",
    "setarch": "util-linux",
    "llvm-config": "llvm-devel",
    "clang++": "clang clang-devel",
    "clang-probe": "clang clang-devel llvm-devel",
    "perl-ipc-run": "perl-IPC-Run",
    "readline": "readline-devel",
    "zlib": "zlib-devel",
    "icu": "libicu-devel",
    "openssl": "openssl-devel",
    "lz4": "lz4-devel",
    "zstd": "libzstd-devel",
    "libxml": "libxml2-devel",
    "python-headers": "python3-devel",
    "python": "python3",
    "doxygen": "doxygen",
}

AMAZON_DNF_PACKAGES = {
    **DNF_PACKAGES,
    "llvm-config": f"llvm{AMAZON_LINUX_LLVM_MAJOR}-devel",
    "clang++": (
        f"clang{AMAZON_LINUX_LLVM_MAJOR} "
        f"clang{AMAZON_LINUX_LLVM_MAJOR}-devel"
    ),
    "clang-probe": (
        f"clang{AMAZON_LINUX_LLVM_MAJOR} "
        f"clang{AMAZON_LINUX_LLVM_MAJOR}-devel "
        f"llvm{AMAZON_LINUX_LLVM_MAJOR}-devel"
    ),
    "python-headers": f"{AMAZON_LINUX_PYTHON}-devel",
    "python": AMAZON_LINUX_PYTHON,
}

ZYPPER_PACKAGES = {
    "git": "git",
    "make": "make",
    "bash": "bash",
    "cc": "gcc",
    "cxx": "gcc-c++",
    "ar": "binutils",
    "nm": "binutils",
    "bison": "bison",
    "flex": "flex",
    "perl": "perl",
    "pkg-config": "pkgconf-pkg-config",
    "find": "findutils",
    "tar": "tar",
    "readelf": "binutils",
    "objcopy": "binutils",
    "strace": "strace",
    "setarch": "util-linux",
    "llvm-config": "llvm-devel",
    "clang++": "clang clang-devel",
    "clang-probe": "clang clang-devel llvm-devel",
    "perl-ipc-run": "perl-IPC-Run",
    "readline": "readline-devel",
    "zlib": "zlib-devel",
    "icu": "libicu-devel",
    "openssl": "libopenssl-devel",
    "lz4": "liblz4-devel",
    "zstd": "libzstd-devel",
    "libxml": "libxml2-devel",
    "python-headers": "python3-devel",
    "python": "python3",
    "doxygen": "doxygen",
}

LLVM_FINDING_KEYS = {
    "llvm-config",
    "clang++",
    "clang-resource-dir",
    "clang-probe",
    "llvm-clang-pair",
}


@dataclass(frozen=True)
class Finding:
    group: str
    key: str
    status: str
    label: str
    detail: str

    @property
    def failed(self) -> bool:
        return self.status in {"missing", "error"}


class Reporter:
    def __init__(self) -> None:
        self.findings: list[Finding] = []

    def add(
        self, group: str, key: str, status: str, label: str, detail: str
    ) -> None:
        self.findings.append(Finding(group, key, status, label, detail))

    def failures(self) -> list[Finding]:
        return [finding for finding in self.findings if finding.failed]

    def render(self, profile: str | None) -> None:
        print(f"PostGamma build environment (profile: {profile or 'report-only'})")
        for group, title in GROUP_TITLES.items():
            findings = [item for item in self.findings if item.group == group]
            if not findings:
                continue
            print(f"{title}:")
            for finding in findings:
                print(f"  {finding.status:<7} {finding.label:<20} {finding.detail}")
        warnings = sum(item.status == "warning" for item in self.findings)
        print(
            f"Summary: {len(self.failures())} error(s), {warnings} warning(s), "
            f"{len(self.findings)} check(s)"
        )


def run(
    command: Sequence[str], timeout: float = 20.0
) -> subprocess.CompletedProcess[str]:
    try:
        return subprocess.run(
            list(command),
            check=False,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=timeout,
        )
    except subprocess.TimeoutExpired:
        return subprocess.CompletedProcess(
            list(command),
            124,
            f"command timed out after {timeout:g} seconds: {command[0]}",
            None,
        )
    except OSError as error:
        return subprocess.CompletedProcess(list(command), 127, str(error), None)


def command_tokens(value: str) -> tuple[str, ...]:
    """Parse a make-style command override without invoking a shell."""

    try:
        return tuple(shlex.split(value, posix=True))
    except ValueError:
        return ()


def resolve_command(value: str) -> tuple[str, ...] | None:
    tokens = command_tokens(value)
    if not tokens:
        return None
    executable = tokens[0]
    if "/" in executable:
        path = Path(executable).expanduser()
        found = (
            str(path.absolute())
            if path.is_file() and os.access(path, os.X_OK)
            else None
        )
    else:
        found = shutil.which(executable)
    return (found, *tokens[1:]) if found else None


def first_line(output: str) -> str:
    lines = [line.strip() for line in output.splitlines() if line.strip()]
    return lines[0] if lines else "version unavailable"


def concise_failure(output: str) -> str:
    lines = [line.strip() for line in output.splitlines() if line.strip()]
    return (lines[-1] if lines else "command failed without diagnostics")[:240]


def numeric_version(value: str) -> tuple[int, ...] | None:
    match = re.search(r"(?<![0-9])([0-9]+(?:\.[0-9]+)+)", value)
    return tuple(map(int, match.group(1).split("."))) if match else None


def parse_shell_words(value: str, name: str) -> tuple[list[str], str | None]:
    try:
        return shlex.split(value, posix=True), None
    except ValueError as error:
        return [], f"cannot parse {name}: {error}"


def configure_feature_enabled(
    arguments: Sequence[str], name: str, *, default: bool
) -> bool:
    """Apply the last Autoconf --with/--without switch for one feature."""

    enabled = default
    aliases = {name, "openssl"} if name == "ssl" else {name}
    for argument in arguments:
        for alias in aliases:
            if argument in {f"--without-{alias}", f"--with-{alias}=no"}:
                enabled = False
            elif argument == f"--with-{alias}" or argument.startswith(
                f"--with-{alias}="
            ):
                enabled = True
    return enabled


def required_configure_features(
    environment: Mapping[str, str],
) -> tuple[dict[str, bool], list[str]]:
    base, error = parse_shell_words(
        environment.get("PG_CONFIGURE_ARGS", ""), "PG_CONFIGURE_ARGS"
    )
    errors = [error] if error else []
    argument_sets = [base]
    for name in (
        "PG_REFERENCE_CONFIGURE_ARGS",
        "PG_GENERATED_CONFIGURE_ARGS",
        "PG_EMBEDDED_CONFIGURE_ARGS",
    ):
        extra, extra_error = parse_shell_words(environment.get(name, ""), name)
        if extra_error:
            errors.append(extra_error)
        argument_sets.append([*base, *extra])
    defaults = {
        "icu": True,
        "readline": True,
        "zlib": True,
        "ssl": False,
        "lz4": False,
        "zstd": False,
        "libxml": False,
    }
    features = {
        feature: any(
            configure_feature_enabled(arguments, feature, default=default)
            for arguments in argument_sets
        )
        for feature, default in defaults.items()
    }
    return features, errors


def glibc_version() -> tuple[str, tuple[int, ...] | None]:
    try:
        value = os.confstr("CS_GNU_LIBC_VERSION")
    except (AttributeError, OSError, ValueError):
        value = None
    if value:
        return value, numeric_version(value)
    name, version = platform.libc_ver()
    label = " ".join(part for part in (name, version) if part) or "unknown libc"
    return label, numeric_version(version)


def inspect_host(reporter: Reporter) -> None:
    system = platform.system()
    reporter.add(
        "host",
        "linux",
        "ok" if system == "Linux" else "error",
        "operating system",
        f"{system or 'unknown'} (Linux is required)",
    )
    machine = platform.machine().lower()
    reporter.add(
        "host",
        "architecture",
        "ok" if machine in {"x86_64", "amd64"} else "error",
        "architecture",
        f"{machine or 'unknown'} (x86-64 is required)",
    )
    libc_label, version = glibc_version()
    supported = version is not None and version[:2] >= MINIMUM_GLIBC
    reporter.add(
        "host",
        "glibc",
        "ok" if supported else "error",
        "libc",
        f"{libc_label} (glibc 2.28+ is required)",
    )


def inspect_tools(
    groups: set[str],
    environment: Mapping[str, str],
    build_dir: Path,
    reporter: Reporter,
) -> dict[str, tuple[str, ...]]:
    tools: dict[str, tuple[str, ...]] = {}
    for key, label, variable, default, group, version_arguments in TOOL_SPECS:
        if group not in groups:
            continue
        if key == "mkdocs" and (build_dir / "docs-tools/bin/mkdocs").is_file():
            default = str(build_dir / "docs-tools/bin/mkdocs")
        value = environment.get(variable, default) if variable else default
        command = resolve_command(value)
        if command is None:
            reporter.add(
                group,
                key,
                "missing",
                label,
                f"selected command is unavailable or invalid: {value!r}",
            )
            continue
        tools[key] = command
        version = run([*command, *version_arguments], timeout=10.0)
        detail = first_line(version.stdout) if version.returncode == 0 else command[0]
        minimum = MINIMUM_TOOL_VERSIONS.get(key)
        actual = numeric_version(version.stdout) if minimum else None
        compatible = (
            version.returncode == 0
            and (minimum is None or (actual is not None and actual >= minimum))
        )
        if minimum and not compatible:
            required = ".".join(map(str, minimum))
            detail += f" ({required}+ is required)"
        reporter.add(group, key, "ok" if compatible else "error", label, detail)

    if "base" in groups or "docs" in groups:
        supported = sys.version_info[:2] >= MINIMUM_PYTHON
        group = "base" if "base" in groups else "docs"
        reporter.add(
            group,
            "python",
            "ok" if supported else "error",
            "Python",
            f"{platform.python_version()} ({sys.executable}; Python 3.10+ is required)",
        )
    if "base" in groups:
        if make := tools.get("make"):
            output = run([*make, "--version"], timeout=10.0).stdout
            version = numeric_version(output)
            if (
                not output.startswith("GNU Make")
                or version is None
                or version < MINIMUM_MAKE
            ):
                reporter.add(
                    "base",
                    "make-version",
                    "error",
                    "make compatibility",
                    "GNU make 4.3 or newer is required by grouped targets",
                )
    return tools


def inspect_checkout(
    root: Path, git: tuple[str, ...] | None, reporter: Reporter
) -> None:
    directories = (root / "postgres", root / "third_party/pgvector")
    missing = [
        str(path.relative_to(root))
        for path in directories
        if not (path / ".git").exists()
    ]
    if missing:
        reporter.add(
            "checkout",
            "submodules",
            "missing",
            "git submodules",
            "not initialized: " + ", ".join(missing),
        )
        return
    if git is None:
        reporter.add("checkout", "submodules", "ok", "git submodules", "initialized")
        return
    result = run([*git, "-C", str(root), "submodule", "status", "--recursive"])
    bad = [line for line in result.stdout.splitlines() if line[:1] in {"-", "+", "U"}]
    if result.returncode or bad:
        detail = (
            concise_failure(result.stdout)
            if result.returncode
            else "not at recorded commits; run git submodule update --init --recursive"
        )
        reporter.add("checkout", "submodules", "error", "git submodules", detail)
    else:
        reporter.add(
            "checkout", "submodules", "ok", "git submodules", "initialized and pinned"
        )


def locate_probe_directory(root: Path, build_dir: Path) -> Path:
    candidate = build_dir if build_dir.is_dir() else build_dir.parent
    while not candidate.exists() and candidate != candidate.parent:
        candidate = candidate.parent
    return candidate if candidate.is_dir() else root


def environment_flags(
    environment: Mapping[str, str], names: Sequence[str]
) -> tuple[list[str], str | None]:
    flags: list[str] = []
    for name in names:
        values, error = parse_shell_words(environment.get(name, ""), name)
        if error:
            return [], error
        flags.extend(values)
    return flags, None


def compile_probe(
    command: Sequence[str],
    source: str,
    suffix: str,
    arguments: Sequence[str],
    probe_directory: Path,
    *,
    link_arguments: Sequence[str] = (),
    execute: bool = False,
    compile_timeout: float = 20.0,
) -> tuple[bool, str]:
    try:
        with tempfile.TemporaryDirectory(
            prefix=".postgamma-doctor-", dir=probe_directory
        ) as temporary:
            directory = Path(temporary)
            source_path = directory / f"probe{suffix}"
            output = directory / "probe"
            source_path.write_text(source, encoding="utf-8")
            result = run(
                [
                    *command,
                    *arguments,
                    str(source_path),
                    *link_arguments,
                    "-o",
                    str(output),
                ],
                timeout=compile_timeout,
            )
            if result.returncode:
                return False, concise_failure(result.stdout)
            if execute:
                result = run([str(output)], timeout=10.0)
                if result.returncode:
                    return False, concise_failure(result.stdout)
    except OSError as error:
        return False, str(error)
    action = "compile, link, and execution" if execute else "compile and link"
    return True, f"{action} probe passed"


def inspect_compilers(
    groups: set[str],
    tools: Mapping[str, tuple[str, ...]],
    environment: Mapping[str, str],
    root: Path,
    probe_directory: Path,
    reporter: Reporter,
) -> None:
    if "base" not in groups:
        return
    flags, error = environment_flags(environment, ("CPPFLAGS", "CFLAGS"))
    if error:
        reporter.add("base", "compiler-flags", "error", "compiler flags", error)
    elif cc := tools.get("cc"):
        source = """\
#include <pthread.h>
#include <stdint.h>
static void *worker(void *argument) { return argument; }
int main(void) {
    pthread_t thread;
    void *result = 0;
    if (pthread_create(&thread, 0, worker, (void *)(uintptr_t)1) != 0) return 1;
    if (pthread_join(thread, &result) != 0) return 2;
    return result == (void *)(uintptr_t)1 ? 0 : 3;
}
"""
        arguments = [
            *flags,
            "-std=c11",
            "-fPIC",
            f"-ffile-prefix-map={root}=postgamma-source",
            f"-fmacro-prefix-map={root}=postgamma-source",
            "-pthread",
        ]
        ok, detail = compile_probe(
            cc, source, ".c", arguments, probe_directory, execute=True
        )
        reporter.add(
            "base", "cc-probe", "ok" if ok else "error", "C11/PIC/pthread", detail
        )

    flags, error = environment_flags(environment, ("CPPFLAGS", "CXXFLAGS"))
    if error:
        reporter.add("base", "cxx-flags", "error", "C++ flags", error)
    elif cxx := tools.get("cxx"):
        source = (
            "#include <memory>\nint main() { auto value = std::make_unique<int>(19); "
            "return *value == 19 ? 0 : 1; }\n"
        )
        ok, detail = compile_probe(
            cxx, source, ".cpp", [*flags, "-std=c++17"], probe_directory, execute=True
        )
        reporter.add("base", "cxx-probe", "ok" if ok else "error", "C++17", detail)


def llvm_arguments(command: str, *arguments: str) -> tuple[list[str], str | None]:
    result = run([command, *arguments], timeout=10.0)
    if result.returncode:
        return [], concise_failure(result.stdout)
    values = command_tokens(result.stdout.strip())
    if result.stdout.strip() and not values:
        return [], "llvm-config returned arguments that cannot be parsed"
    return list(values), None


def inspect_ast(
    groups: set[str], root: Path, probe_directory: Path, reporter: Reporter
) -> None:
    if "ast" not in groups:
        return
    llvm = find_llvm_config(root)
    clang = find_clangxx(root, llvm)
    if llvm is None:
        reporter.add(
            "ast",
            "llvm-config",
            "missing",
            "llvm-config",
            f"LLVM {LLVM_VERSION_RANGE} is required",
        )
    if clang is None:
        reporter.add(
            "ast",
            "clang++",
            "missing",
            "clang++",
            f"Clang {LLVM_VERSION_RANGE} is required",
        )
    if llvm is None or clang is None:
        return
    llvm_result = run([llvm, "--version"], timeout=10.0)
    clang_result = run([clang, "--version"], timeout=10.0)
    llvm_version = numeric_version(llvm_result.stdout)
    clang_version = numeric_version(clang_result.stdout)
    llvm_major = llvm_version[0] if llvm_version else None
    clang_major = clang_version[0] if clang_version else None
    reporter.add(
        "ast",
        "llvm-config",
        "ok" if llvm_major in LLVM_MAJORS else "error",
        "llvm-config",
        f"{llvm} ({first_line(llvm_result.stdout)})",
    )
    reporter.add(
        "ast",
        "clang++",
        "ok" if clang_major in LLVM_MAJORS else "error",
        "clang++",
        f"{clang} ({first_line(clang_result.stdout)})",
    )
    if llvm_major not in LLVM_MAJORS or clang_major != llvm_major:
        reporter.add(
            "ast",
            "llvm-clang-pair",
            "error",
            "LLVM/Clang pairing",
            f"llvm-config major {llvm_major!r}, clang++ major {clang_major!r}; "
            f"matching {LLVM_VERSION_SPAN} versions are required",
        )
        return
    resource_dir = find_clang_resource_dir(clang)
    reporter.add(
        "ast",
        "clang-resource-dir",
        "ok" if resource_dir else "error",
        "Clang resource directory",
        resource_dir or "clang++ did not report an existing builtin-header directory",
    )
    cxxflags, first_error = llvm_arguments(llvm, "--cxxflags")
    ldflags, second_error = llvm_arguments(llvm, "--ldflags")
    libraries, third_error = llvm_arguments(
        llvm, "--link-shared", "--libs", "--system-libs"
    )
    if error := first_error or second_error or third_error:
        reporter.add("ast", "clang-probe", "error", "Clang LibTooling", error)
        return
    source = """\
#include <clang/Tooling/Tooling.h>
int main() {
    auto unit = clang::tooling::buildASTFromCode("int postgamma_probe;");
    return unit ? 0 : 1;
}
"""
    clang_cpp_library = find_clang_cpp_library(llvm) or "-lclang-cpp"
    ok, detail = compile_probe(
        (clang,),
        source,
        ".cpp",
        [
            *cxxflags,
            "-std=c++17",
            *([f"-resource-dir={resource_dir}"] if resource_dir else []),
            *ldflags,
        ],
        probe_directory,
        link_arguments=[clang_cpp_library, *libraries],
        execute=True,
        compile_timeout=AST_COMPILE_TIMEOUT,
    )
    reporter.add(
        "ast", "clang-probe", "ok" if ok else "error", "Clang LibTooling", detail
    )


def inspect_native(
    groups: set[str],
    tools: Mapping[str, tuple[str, ...]],
    environment: Mapping[str, str],
    probe_directory: Path,
    reporter: Reporter,
) -> None:
    if "native" not in groups:
        return
    features, errors = required_configure_features(environment)
    for error in errors:
        reporter.add(
            "native", "configure-arguments", "error", "configure arguments", error
        )
    enabled = ", ".join(sorted(name for name, value in features.items() if value))
    reporter.add("native", "features", "ok", "selected features", enabled or "none")
    cc = tools.get("cc")
    if cc is None:
        return
    flags, error = environment_flags(environment, ("CPPFLAGS", "CFLAGS", "LDFLAGS"))
    if error:
        return
    environment_libraries, error = environment_flags(environment, ("LIBS",))
    if error:
        return
    probes = {
        "readline": (
            READLINE_PROBE_SOURCE,
            ("-lreadline",),
            "Readline headers/library",
        ),
        "zlib": (
            "#include <zlib.h>\nint main(void) { return zlibVersion() ? 0 : 1; }\n",
            ("-lz",),
            "zlib headers/library",
        ),
        "ssl": (
            "#include <openssl/ssl.h>\n"
            "int main(void) { return OPENSSL_init_ssl(0, 0) ? 0 : 1; }\n",
            ("-lssl", "-lcrypto"),
            "OpenSSL headers/library",
        ),
        "lz4": (
            "#include <lz4.h>\n"
            "int main(void) { return LZ4_versionNumber() > 0 ? 0 : 1; }\n",
            ("-llz4",),
            "LZ4 headers/library",
        ),
        "zstd": (
            "#include <zstd.h>\n"
            "int main(void) { return ZSTD_versionNumber() > 0 ? 0 : 1; }\n",
            ("-lzstd",),
            "Zstandard headers/library",
        ),
    }
    override_prefixes = {"lz4": "LZ4", "zstd": "ZSTD"}
    for feature, (source, libraries, label) in probes.items():
        if not features[feature]:
            continue
        dependency_flags: list[str] = []
        dependency_libraries = list(libraries)
        if prefix := override_prefixes.get(feature):
            dependency_flags, flag_error = environment_flags(
                environment, (f"{prefix}_CFLAGS",)
            )
            overridden_libraries, library_error = environment_flags(
                environment, (f"{prefix}_LIBS",)
            )
            if flag_error or library_error:
                reporter.add(
                    "native",
                    feature,
                    "error",
                    label,
                    flag_error or library_error or "invalid dependency flags",
                )
                continue
            if overridden_libraries:
                dependency_libraries = overridden_libraries
        ok, detail = compile_probe(
            cc,
            source,
            ".c",
            [*flags, *dependency_flags],
            probe_directory,
            link_arguments=[*dependency_libraries, *environment_libraries],
        )
        key = "openssl" if feature == "ssl" else feature
        reporter.add("native", key, "ok" if ok else "missing", label, detail)

    pkg_config = tools.get("pkg-config")
    modules: dict[str, tuple[tuple[str, ...], str, str]] = {}
    if features["icu"]:
        modules["icu"] = (
            ("icu-uc", "icu-i18n"),
            "ICU",
            """\
#include <unicode/ucol.h>
int main(void) {
    UErrorCode status = U_ZERO_ERROR;
    UCollator *collator = ucol_open("", &status);
    if (collator) ucol_close(collator);
    return U_FAILURE(status);
}
""",
        )
    if features["libxml"]:
        modules["libxml"] = (
            ("libxml-2.0",),
            "XML2",
            "#include <libxml/parser.h>\n"
            "int main(void) { xmlInitParser(); xmlCleanupParser(); return 0; }\n",
        )
    for feature, (names, prefix, source) in modules.items():
        custom = bool(
            environment.get(f"{prefix}_CFLAGS")
            or environment.get(f"{prefix}_LIBS")
        )
        dependency_flags, flag_error = environment_flags(
            environment, (f"{prefix}_CFLAGS",)
        )
        dependency_libraries, library_error = environment_flags(
            environment, (f"{prefix}_LIBS",)
        )
        lookup_error = flag_error or library_error
        if not custom and pkg_config is not None:
            cflags_result = run([*pkg_config, "--cflags", *names], timeout=10.0)
            libs_result = run([*pkg_config, "--libs", *names], timeout=10.0)
            if cflags_result.returncode or libs_result.returncode:
                lookup_error = f"missing pkg-config modules: {' '.join(names)}"
            else:
                dependency_flags = list(command_tokens(cflags_result.stdout.strip()))
                dependency_libraries = list(command_tokens(libs_result.stdout.strip()))
        elif not custom:
            lookup_error = (
                "pkg-config is unavailable and no explicit flags were supplied"
            )
        if lookup_error:
            reporter.add("native", feature, "missing", feature, lookup_error)
            continue
        ok, detail = compile_probe(
            cc,
            source,
            ".c",
            [*flags, *dependency_flags],
            probe_directory,
            link_arguments=[*dependency_libraries, *environment_libraries],
        )
        reporter.add(
            "native",
            feature,
            "ok" if ok else "missing",
            feature,
            detail,
        )


def inspect_runtime(
    groups: set[str], tools: Mapping[str, tuple[str, ...]], reporter: Reporter
) -> None:
    if "runtime" in groups:
        procfs = Path("/proc/self/status").is_file()
        reporter.add(
            "runtime",
            "procfs",
            "ok" if procfs else "error",
            "procfs",
            "/proc is available" if procfs else "/proc is required by runtime checks",
        )
        if strace := tools.get("strace"):
            result = run(
                [
                    *strace,
                    "-qq",
                    "-f",
                    "-e",
                    "trace=none",
                    sys.executable,
                    "-c",
                    "pass",
                ],
                timeout=10.0,
            )
            reporter.add(
                "runtime",
                "ptrace",
                "ok" if result.returncode == 0 else "error",
                "ptrace/strace",
                "trace probe passed"
                if result.returncode == 0
                else concise_failure(result.stdout),
            )
    if "test" in groups:
        perl = tools.get("perl")
        result = run([*perl, "-MIPC::Run", "-e", "exit 0"]) if perl else None
        available = result is not None and result.returncode == 0
        reporter.add(
            "test",
            "perl-ipc-run",
            "ok" if available else "missing",
            "Perl IPC::Run",
            "module is available" if available else "required by PostgreSQL TAP tests",
        )
        if setarch := tools.get("setarch"):
            result = run(
                [*setarch, platform.machine(), "-R", sys.executable, "-c", "pass"],
                timeout=10.0,
            )
            reporter.add(
                "test",
                "setarch-capability",
                "ok" if result.returncode == 0 else "error",
                "ASLR control",
                "setarch -R probe passed"
                if result.returncode == 0
                else concise_failure(result.stdout),
            )


def inspect_python(groups: set[str], probe_directory: Path, reporter: Reporter) -> None:
    if "python" not in groups:
        return
    version = sys.version_info[:2]
    supported = (
        sys.implementation.name == "cpython"
        and MINIMUM_PYTHON <= version <= MAXIMUM_WHEEL_PYTHON
    )
    reporter.add(
        "python",
        "python-abi",
        "ok" if supported else "error",
        "wheel interpreter",
        f"{sys.implementation.name} {platform.python_version()} "
        "(CPython 3.10-3.14 is supported)",
    )
    include = sysconfig.get_config_var("INCLUDEPY")
    suffix = sysconfig.get_config_var("EXT_SUFFIX")
    header = Path(include) / "Python.h" if include else None
    if header is None or not header.is_file() or not suffix:
        detail = f"missing {header}" if header else "extension metadata is incomplete"
        reporter.add(
            "python", "python-headers", "missing", "Python development", detail
        )
        return
    command = resolve_command(sysconfig.get_config_var("CC") or "cc")
    if command is None:
        reporter.add(
            "python",
            "python-headers",
            "missing",
            "Python development",
            "Python's configured compiler is unavailable",
        )
        return
    source = """\
#include <Python.h>
static struct PyModuleDef module = {PyModuleDef_HEAD_INIT, "_doctor", 0, -1, 0};
PyMODINIT_FUNC PyInit__doctor(void) { return PyModule_Create(&module); }
"""
    ok, detail = compile_probe(
        command,
        source,
        ".c",
        ("-std=c11", "-fPIC", "-shared", f"-I{include}"),
        probe_directory,
    )
    reporter.add(
        "python",
        "python-headers",
        "ok" if ok else "missing",
        "Python development",
        detail,
    )


def total_memory_bytes() -> int | None:
    values: list[int] = []
    try:
        for line in Path("/proc/meminfo").read_text(encoding="ascii").splitlines():
            if line.startswith("MemTotal:"):
                values.append(int(line.split()[1]) * 1024)
                break
    except (OSError, ValueError, IndexError):
        pass
    for path in (
        Path("/sys/fs/cgroup/memory.max"),
        Path("/sys/fs/cgroup/memory/memory.limit_in_bytes"),
    ):
        try:
            raw = path.read_text(encoding="ascii").strip()
            if raw != "max":
                limit = int(raw)
                if limit > 0:
                    values.append(limit)
        except (OSError, ValueError):
            continue
    return min(values) if values else None


def usable_cpu_count() -> int:
    try:
        return len(os.sched_getaffinity(0))
    except (AttributeError, OSError):
        return os.cpu_count() or 1


def human_gib(value: int) -> str:
    return f"{value / 1024**3:.1f} GiB"


def inspect_resources(
    groups: set[str], root: Path, build_dir: Path, jobs: int, reporter: Reporter
) -> None:
    if "resources" not in groups:
        return
    location = locate_probe_directory(root, build_dir)
    try:
        with tempfile.TemporaryDirectory(
            prefix=".postgamma-doctor-write-", dir=location
        ) as temporary:
            marker = Path(temporary) / "probe"
            marker.write_text("postgamma\n", encoding="ascii")
        reporter.add(
            "resources", "build-write", "ok", "build filesystem", "writable"
        )
    except OSError as error:
        reporter.add(
            "resources", "build-write", "error", "build filesystem", str(error)
        )
    try:
        free = shutil.disk_usage(location).free
        reporter.add(
            "resources",
            "disk",
            "ok" if free >= RECOMMENDED_DISK_BYTES else "warning",
            "free disk",
            f"{human_gib(free)} at {location} (8 GiB recommended)",
        )
    except OSError as error:
        reporter.add("resources", "disk", "warning", "free disk", str(error))
    if memory := total_memory_bytes():
        recommended_memory = max(
            RECOMMENDED_MEMORY_BYTES, jobs * 512 * 1024**2
        )
        reporter.add(
            "resources",
            "memory",
            "ok" if memory >= recommended_memory else "warning",
            "memory",
            f"{human_gib(memory)} ({human_gib(recommended_memory)} recommended "
            f"for JOBS={jobs})",
        )
    processors = usable_cpu_count()
    reporter.add(
        "resources",
        "jobs",
        "ok" if jobs <= processors else "warning",
        "parallel jobs",
        f"JOBS={jobs}, logical CPUs={processors}",
    )
    if resource is not None:
        soft_limit, _ = resource.getrlimit(resource.RLIMIT_NOFILE)
        reporter.add(
            "resources",
            "open-files",
            "ok" if soft_limit >= 1024 else "warning",
            "open-file limit",
            str(soft_limit),
        )


def inspect_environment(
    groups: set[str], root: Path, environment: Mapping[str, str], reporter: Reporter
) -> None:
    if "environment" not in groups:
        return
    whitespace = any(character.isspace() for character in str(root))
    long_path = len(os.fsencode(root)) > 60
    path_status = "error" if whitespace else "warning" if long_path else "ok"
    if whitespace:
        path_detail = f"{root} (whitespace is unsupported)"
    elif long_path:
        path_detail = f"{root} (a shorter path is recommended for PostgreSQL tests)"
    else:
        path_detail = str(root)
    reporter.add(
        "environment",
        "source-path",
        path_status,
        "source path",
        path_detail,
    )
    try:
        current_locale = locale.setlocale(locale.LC_CTYPE)
        reporter.add("environment", "locale", "ok", "locale", current_locale or "C")
    except locale.Error as error:
        reporter.add("environment", "locale", "error", "locale", str(error))
    override_names = (
        "CONFIG_SITE",
        "CPATH",
        "C_INCLUDE_PATH",
        "CPLUS_INCLUDE_PATH",
        "LIBRARY_PATH",
        "LD_LIBRARY_PATH",
        "PKG_CONFIG_PATH",
        "PKG_CONFIG_LIBDIR",
        "PKG_CONFIG_SYSROOT_DIR",
        "LLVM_CONFIG",
        "CLANGXX",
        "ICU_CFLAGS",
        "ICU_LIBS",
        "LZ4_CFLAGS",
        "LZ4_LIBS",
        "ZSTD_CFLAGS",
        "ZSTD_LIBS",
        "XML2_CFLAGS",
        "XML2_LIBS",
        "PG_CONFIGURE_ARGS",
        "PG_REFERENCE_CONFIGURE_ARGS",
        "PG_GENERATED_CONFIGURE_ARGS",
        "PG_EMBEDDED_CONFIGURE_ARGS",
    )
    active_overrides = [name for name in override_names if environment.get(name)]
    reporter.add(
        "environment",
        "active-overrides",
        "info" if active_overrides else "ok",
        "active variables",
        ", ".join(active_overrides) if active_overrides else "none",
    )
    flags = " ".join(
        environment.get(name, "")
        for name in ("CPPFLAGS", "CFLAGS", "CXXFLAGS", "LDFLAGS", "LIBS")
    )
    risks: list[str] = []
    if environment.get("CONDA_PREFIX"):
        risks.append("CONDA_PREFIX can mix Conda and system headers/libraries")
    if "-march=native" in flags:
        risks.append("-march=native makes artifacts host-CPU-specific")
    if "-Werror" in flags:
        risks.append("-Werror can turn upstream warnings into failures")
    if re.search(r"(?:^|\s)-static(?:\s|$)", flags):
        risks.append("-static conflicts with shared and Python build products")
    reporter.add(
        "environment",
        "risky-overrides",
        "warning" if risks else "ok",
        "build overrides",
        "; ".join(risks) if risks else "no common contamination detected",
    )


def read_os_release() -> dict[str, str]:
    values: dict[str, str] = {}
    try:
        lines = Path("/etc/os-release").read_text(encoding="utf-8").splitlines()
    except OSError:
        return values
    for line in lines:
        if "=" in line and not line.startswith("#"):
            key, value = line.split("=", 1)
            values[key] = value.strip().strip('"')
    return values


def package_family(release: Mapping[str, str] | None = None) -> str | None:
    if release is None:
        release = read_os_release()
    if release.get("ID") == "amzn":
        # Amazon Linux 2023 identifies Fedora in ID_LIKE, but its unversioned
        # LLVM and Python packages are older than this project's supported
        # ranges.  Keep its version-aware guidance separate from generic dnf.
        return "dnf-amazon"
    identities = {release.get("ID", ""), *release.get("ID_LIKE", "").split()}
    if identities & {"debian", "ubuntu"}:
        return "apt"
    if identities & {"fedora", "rhel", "centos", "rocky", "almalinux"}:
        return "dnf"
    if identities & {"suse", "opensuse", "opensuse-leap"}:
        return "zypper"
    return None


def platform_description(release: Mapping[str, str]) -> str:
    name = (
        release.get("NAME")
        or release.get("ID")
        or platform.system()
        or "unknown"
    )
    version = release.get("VERSION_ID", "")
    codename = release.get("VERSION_CODENAME", "")
    distribution = " ".join(part for part in (name, version) if part)
    if codename:
        distribution += f" ({codename})"
    return f"{distribution}, {platform.machine() or 'unknown architecture'}"


def print_llvm_install_commands(
    family: str | None,
    release: Mapping[str, str],
) -> None:
    newest = max(LLVM_MAJORS)
    if family == "apt" and release.get("ID") in {"debian", "ubuntu"}:
        print(f"Install LLVM/Clang {newest}:")
        print("  curl -fsSLO https://apt.llvm.org/llvm.sh")
        print("  chmod +x llvm.sh")
        print(f"  sudo ./llvm.sh {newest}")
        print(
            "  sudo apt-get install -y "
            f"libclang-{newest}-dev llvm-{newest}-dev"
        )
    elif family == "dnf-amazon":
        major = AMAZON_LINUX_LLVM_MAJOR
        print(f"Install LLVM/Clang {major}:")
        print(
            "  sudo dnf install -y "
            f"clang{major} clang{major}-devel llvm{major}-devel"
        )
    elif family == "dnf":
        print("Install the distribution LLVM/Clang development packages:")
        print("  sudo dnf install -y clang clang-devel llvm-devel")
    elif family == "zypper":
        print("Install the distribution LLVM/Clang development packages:")
        print("  sudo zypper install -y clang clang-devel llvm-devel")
    else:
        print(
            "No verified LLVM/Clang install command is available for this "
            "platform; see docs/getting-started/installation.md."
        )


def print_hints(failures: Sequence[Finding]) -> None:
    keys = {finding.key for finding in failures}
    if "submodules" in keys:
        print("Fix checkout: git submodule update --init --recursive")
    release = read_os_release()
    family = package_family(release)
    mapping = {
        "apt": APT_PACKAGES,
        "dnf": DNF_PACKAGES,
        "dnf-amazon": AMAZON_DNF_PACKAGES,
        "zypper": ZYPPER_PACKAGES,
    }.get(family, {})
    packages: set[str] = set()
    if family:
        specialized_keys = set(LLVM_FINDING_KEYS)
        if family == "dnf-amazon":
            specialized_keys.update({"python", "python-headers"})
        for finding in failures:
            if finding.status != "missing":
                continue
            if finding.key in specialized_keys:
                continue
            if package_names := mapping.get(finding.key):
                packages.update(package_names.split())
    if packages:
        command = {
            "apt": "apt-get install",
            "dnf": "dnf install",
            "dnf-amazon": "dnf install",
            "zypper": "zypper install",
        }[family]
        print(f"Suggested packages: sudo {command} -y {' '.join(sorted(packages))}")
    if "mkdocs" in keys:
        print(
            "Fix MkDocs: python3 -m venv build/docs-tools && "
            "build/docs-tools/bin/python -m pip install -r docs/requirements.txt",
        )
    if "make-version" in keys:
        print(
            "Fix Make: install GNU Make 4.3 or newer and invoke this project "
            "with that executable."
        )
    if keys & LLVM_FINDING_KEYS:
        print(f"Detected platform: {platform_description(release)}")
        print(
            f"Fix LLVM/Clang: install matching {LLVM_VERSION_SPAN} development "
            "packages; versioned executables are detected automatically, or set "
            "LLVM_CONFIG and CLANGXX explicitly."
        )
        print_llvm_install_commands(family, release)
    if family == "dnf-amazon" and keys & {
        "python",
        "python-abi",
        "python-headers",
    }:
        python = AMAZON_LINUX_PYTHON
        print(
            "Amazon Linux 2023: sudo dnf install "
            f"{python} {python}-devel, then invoke make with PYTHON={python} "
            "(the default Python is too old)."
        )
    if "ptrace" in keys:
        print(
            "Fix tracing: allow ptrace in the container/sandbox, or run runtime "
            "checks on a host that permits strace.",
        )
    if "setarch-capability" in keys:
        print(
            "Fix ASLR control: allow the personality syscall, or run sanitizer "
            "checks outside the restricted container.",
        )


def positive_integer(value: str) -> int:
    try:
        number = int(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError("must be a positive integer") from error
    if number < 1:
        raise argparse.ArgumentTypeError("must be a positive integer")
    return number


def main() -> int:
    parser = argparse.ArgumentParser(
        description="validate PostGamma build tools, libraries, and host capabilities"
    )
    parser.add_argument("--root", required=True, type=Path)
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--jobs", type=positive_integer)
    parser.add_argument("--require-profile", choices=tuple(PROFILE_GROUPS))
    args = parser.parse_args()

    root = args.root.resolve()
    build_dir = args.build_dir.resolve() if args.build_dir else root / "build"
    jobs = args.jobs or positive_integer(
        os.environ.get("JOBS", str(os.cpu_count() or 1))
    )
    groups = set(GROUP_TITLES) if args.require_profile is None else set(
        PROFILE_GROUPS[args.require_profile]
    )
    reporter = Reporter()

    if "host" in groups:
        inspect_host(reporter)
    tools = inspect_tools(groups, os.environ, build_dir, reporter)
    if "checkout" in groups:
        inspect_checkout(root, tools.get("git"), reporter)
    probe_directory = locate_probe_directory(root, build_dir)
    inspect_compilers(groups, tools, os.environ, root, probe_directory, reporter)
    inspect_ast(groups, root, probe_directory, reporter)
    inspect_native(groups, tools, os.environ, probe_directory, reporter)
    inspect_runtime(groups, tools, reporter)
    inspect_python(groups, probe_directory, reporter)
    inspect_resources(groups, root, build_dir, jobs, reporter)
    inspect_environment(groups, root, os.environ, reporter)

    reporter.render(args.require_profile)
    failures = reporter.failures()
    if args.require_profile is not None and failures:
        sys.stdout.flush()
        print(
            f"Environment profile {args.require_profile!r} is incomplete; "
            "no build was started.",
        )
        print_hints(failures)
        print("Re-run the same make target after fixing the environment.")
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
