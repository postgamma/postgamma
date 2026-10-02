#!/usr/bin/env python3
"""Validate the tested-byte promotion and manylinux workflow contract."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
from pathlib import Path

from check_python_wheel import WheelCheckError, load_release_baseline
from workflow_contract import (
    WorkflowContractError,
    argv,
    mapping,
    option,
    read_workflow,
    require,
    shell_commands,
)

REPORT_KIND = "postgamma.python-release-workflow-contract"
EXPECTED_PYTHON_TAGS = ["cp310", "cp311", "cp312", "cp313", "cp314"]
EXPECTED_POLICY = "manylinux_2_17_x86_64"
EXPECTED_IMAGE = (
    "quay.io/pypa/manylinux2014_x86_64:2026.05.07-2@"
    "sha256:5f253c89349fc699eb1060afb163fe26c2053681813b627611e2ba0572369fe0"
)
EXPECTED_GNU_MAKE = "4.4.1"
EXPECTED_GNU_MAKE_SHA256 = (
    "dd16fb1d67bfab79a72f5e8390735c49e3e8e70b4945a15ab1f81ddb78658fb3"
)
EXPECTED_MICROMAMBA = "2.8.1-0"
EXPECTED_MICROMAMBA_SHA256 = (
    "9689782d863c05a1bf5d2d371ba527104e7a4eb4310c1637d8653b751aed9c82"
)
EXPECTED_TOOLCHAIN_LOCK = "buildsys/toolchains/llvm22-linux-64.lock"
EXPECTED_CFLAGS = "-O2 -fPIC -march=x86-64 -mtune=generic"
WHEEL_DIAGNOSTICS_ARTIFACT = "wheel-diagnostics-${{ matrix.python_tag }}"


def validate_toolchain_lock(path: Path) -> dict[str, object]:
    content = path.read_text(encoding="utf-8")
    lines = [
        line.strip()
        for line in content.splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    ]
    if not lines or lines[0] != "@EXPLICIT":
        raise WorkflowContractError("LLVM toolchain lock must be explicit")
    entries = lines[1:]
    if not entries or len(entries) != len(set(entries)):
        raise WorkflowContractError(
            "LLVM toolchain lock must contain unique package entries"
        )
    entry_pattern = re.compile(
        r"^https://conda\.anaconda\.org/conda-forge/"
        r"(?:linux-64|noarch)/[^#]+#[0-9a-f]{64}$"
    )
    if not all(entry_pattern.fullmatch(entry) for entry in entries):
        raise WorkflowContractError("LLVM toolchain lock contains an unpinned package")
    required = (
        "/clang-22.1.8-",
        "/clangxx-22.1.8-",
        "/clangdev-22.1.8-",
        "/llvmdev-22.1.8-",
        "/libclang-22.1.8-",
        "/sysroot_linux-64-2.17-",
    )
    missing = [
        marker for marker in required if not any(marker in item for item in entries)
    ]
    if missing:
        raise WorkflowContractError(
            "LLVM toolchain lock is incomplete: " + ", ".join(missing)
        )
    return {
        "sha256": hashlib.sha256(content.encode()).hexdigest(),
        "package_count": len(entries),
        "llvm_version": "22.1.8",
        "sysroot": "glibc-2.17",
    }


def condition(value: object) -> str:
    text = str(value).strip()
    if text.startswith("${{") and text.endswith("}}"):
        text = text[3:-2].strip()
    # Whitespace outside string literals does not change an Actions expression.
    return re.sub(r"\s+(?=(?:[^']*'[^']*')*[^']*$)", "", text)


def steps(job: dict[str, object], label: str) -> list[dict[str, object]]:
    result = job.get("steps")
    require(isinstance(result, list) and bool(result), f"{label} must have steps")
    for step in result:
        mapping(step, f"{label} step")
        require(
            ("run" in step) != ("uses" in step),
            f"{label} step must run or use an action",
        )
        require(
            step.get("continue-on-error", False) is False,
            f"{label} steps must propagate failures",
        )
        # Only the separate diagnostic upload may run on failure. Required
        # build, verification and release artifact steps retain success().
        failure_diagnostics = (
            str(step.get("uses", "")).startswith("actions/upload-artifact@")
            and mapping(step.get("with", {}), label).get("name")
            == WHEEL_DIAGNOSTICS_ARTIFACT
        )
        require(
            condition(step.get("if", "success()"))
            == ("failure()" if failure_diagnostics else "success()"),
            f"{label} required steps must not be conditional",
        )
        if "run" in step:
            require(isinstance(step["run"], str), f"{label} run must be shell text")
            require(
                step.get("shell", "bash")
                in ("bash", "bash --noprofile --norc -e -o pipefail {0}"),
                f"{label} run must use Bash with failure propagation",
            )
    return result


def job_commands(job: dict[str, object], label: str) -> list[list[str]]:
    return [
        command
        for step in steps(job, label)
        if "run" in step
        for command in shell_commands(step["run"])
    ]


def commands_named(
    commands: list[list[str]], program: str, *prefix: str
) -> list[list[str]]:
    matches = []
    for command in commands:
        words = argv(command)
        if not words:
            continue
        name = Path(words[0]).name
        matches_program = name == program
        if program == "python":
            matches_program = (
                bool(re.fullmatch(r"python(?:[0-9.]+)?", name))
                or words[0] == "$python_executable"
            )
        if matches_program and words[1 : 1 + len(prefix)] == list(prefix):
            matches.append(words)
    return matches


def command_required(
    commands: list[list[str]], program: str, *prefix: str, label: str
) -> list[str]:
    found = commands_named(commands, program, *prefix)
    require(
        len(found) == 1, f"{label}: expected one {program} {' '.join(prefix)} command"
    )
    return found[0]


def require_options(command: list[str], expected: dict[str, str], label: str) -> None:
    for name, value in expected.items():
        require(option(command, name) == [value], f"{label}: {name} must be {value}")


def require_action(
    job: dict[str, object], action: str, values: dict[str, str], label: str
) -> dict[str, object]:
    matches = [
        step
        for step in steps(job, label)
        if str(step.get("uses", "")).startswith(action + "@")
        and all(
            str(mapping(step.get("with", {}), label).get(key, "")).rstrip("/")
            == value.rstrip("/")
            for key, value in values.items()
        )
    ]
    require(len(matches) == 1, f"{label}: missing or duplicated {action} with {values}")
    return matches[0]


def validate_jobs(
    document: dict[str, object],
    dependencies: dict[str, set[str]],
    writers: set[str],
    *,
    pypi: bool = False,
) -> dict[str, dict[str, object]]:
    require(
        document.get("permissions") == {"contents": "read"},
        "workflow permissions must default to contents: read",
    )
    jobs = mapping(document.get("jobs"), "jobs")
    require(set(jobs) == set(dependencies), "release workflow job inventory changed")
    for name, job in jobs.items():
        mapping(job, name)
        require(
            "container" not in job,
            "Node-based Actions must run on the host, outside manylinux jobs",
        )
        require(
            job.get("continue-on-error", False) is False,
            f"{name} must propagate failures",
        )
        expected_permissions = {"contents": "write" if name in writers else "read"}
        if name == "dispatch-pypi":
            expected_permissions["actions"] = "write"
        if pypi:
            expected_permissions["id-token"] = "write"
        require(
            job.get("permissions", document["permissions"]) == expected_permissions,
            f"{name} has incorrect permissions: expected {expected_permissions}",
        )
        needs = job.get("needs", [])
        if isinstance(needs, str):
            needs = [needs]
        require(
            isinstance(needs, list)
            and all(isinstance(item, str) for item in needs)
            and len(needs) == len(set(needs))
            and set(needs) == dependencies[name],
            f"{name} dependencies must be {sorted(dependencies[name])}",
        )
        expected_if = "success()"
        if name == "stage-github-release":
            expected_if = (
                "github.event_name == 'push' && startsWith(github.ref, 'refs/tags/v')"
            )
        if name == "dispatch-pypi" or pypi:
            expected_if = "vars.POSTGAMMA_PYPI_PUBLISH_ENABLED == 'true'"
        if pypi:
            expected_if += " && startsWith(github.ref, 'refs/tags/v')"
        require(
            condition(job.get("if", "success()")) == condition(expected_if),
            f"{name} has incorrect execution condition",
        )
        if name != "wheels":
            require("strategy" not in job, f"{name} must run once without a matrix")
        steps(job, name)
    return jobs


def environment_name(job: dict[str, object]) -> object:
    environment = job.get("environment")
    return environment.get("name") if isinstance(environment, dict) else environment


def validate_pypi_workflow(path: Path) -> dict[str, object]:
    document = read_workflow(path)
    triggers = mapping(document.get("on"), "PyPI triggers")
    require(
        set(triggers) == {"workflow_dispatch", "release"}
        and triggers["workflow_dispatch"] in (None, {})
        and triggers["release"] == {"types": ["published"]},
        "PyPI workflow must support explicit dispatch and release.published",
    )
    jobs = validate_jobs(document, {"publish": set()}, set(), pypi=True)
    job = jobs["publish"]
    require(
        environment_name(job) == "pypi",
        "PyPI publication must use the pypi environment",
    )
    require_action(
        job, "actions/checkout", {"ref": "${{ github.ref }}"}, "PyPI tag checkout"
    )
    require(
        mapping(job.get("env"), "PyPI environment").get("RELEASE_TAG")
        == "${{ github.ref_name }}",
        "PyPI release tag must match the triggering tag",
    )
    require(
        all("RELEASE_TAG" not in step.get("env", {}) for step in steps(job, "publish")),
        "PyPI steps must not override the release tag",
    )
    for scope in (document, job, *steps(job, "publish")):
        fields = {
            **mapping(scope.get("env", {}), "PyPI env"),
            **mapping(scope.get("with", {}), "PyPI action inputs"),
        }
        require(
            not any(
                marker in str(fields)
                for marker in ("TWINE_PASSWORD", "secrets.PYPI", "__token__")
            ),
            "PyPI workflow must use OIDC instead of a stored upload token",
        )
    commands = job_commands(job, "publish")
    require(
        not any(
            word.startswith("TWINE_PASSWORD=")
            or "secrets.PYPI" in word
            or "__token__" in word
            for command in commands
            for word in command
        ),
        "PyPI workflow must use OIDC instead of a stored upload token",
    )
    version = command_required(
        commands, "python", "buildsys/product_version.py", label="canonical version"
    )
    require_options(
        version, {"--root": ".", "--tag": "$RELEASE_TAG"}, "canonical version"
    )
    published = command_required(
        commands, "gh", "release", "view", label="published GitHub release"
    )
    require(
        published[3:4] == ["$RELEASE_TAG"],
        "published GitHub release must match the triggering tag",
    )
    require_options(
        published,
        {"--repo": "$GITHUB_REPOSITORY", "--json": "isDraft", "--jq": ".isDraft"},
        "published GitHub release",
    )
    publication_gate = command_required(
        commands, "test", "$is_draft", "=", "false", label="published release gate"
    )
    downloads = commands_named(commands, "gh", "release", "download")
    require(
        len(downloads) == 2,
        "PyPI workflow must download release wheels and their manifest",
    )
    require(
        any(
            option(cmd, "--pattern") == ["*.whl"] and option(cmd, "--dir") == ["dist"]
            for cmd in downloads
        ),
        "PyPI release wheel download is missing",
    )
    require(
        any(
            set(option(cmd, "--pattern")) == {"release-manifest.json", "SHA256SUMS"}
            and option(cmd, "--dir") == ["release-metadata"]
            for cmd in downloads
        ),
        "PyPI release manifest download is missing",
    )
    for download in downloads:
        require(
            download[3:4] == ["$RELEASE_TAG"],
            "PyPI downloads must use the published tag",
        )
        require_options(download, {"--repo": "$GITHUB_REPOSITORY"}, "PyPI download")
    verify = command_required(
        commands,
        "python",
        "buildsys/prepare_release_assets.py",
        "verify-wheels",
        label="exact wheel verification",
    )
    require_options(
        verify,
        {"--bundle": "dist", "--tag": "$RELEASE_TAG"},
        "exact wheel verification",
    )
    ordered_commands = [argv(cmd) for cmd in commands]
    require(
        ordered_commands.index(version)
        < ordered_commands.index(published)
        < ordered_commands.index(publication_gate)
        < min(ordered_commands.index(download) for download in downloads)
        and max(ordered_commands.index(download) for download in downloads)
        < ordered_commands.index(verify),
        "PyPI must validate publication before downloading and verifying wheels",
    )
    publish = require_action(
        job,
        "pypa/gh-action-pypi-publish",
        {"packages-dir": "dist/"},
        "Trusted Publishing",
    )
    require(
        publish["uses"] == "pypa/gh-action-pypi-publish@release/v1",
        "Trusted Publishing action changed",
    )
    require(
        set(publish.get("with", {})) == {"packages-dir"},
        "Trusted Publishing must not use a stored upload token",
    )
    ordered = steps(job, "publish")
    require(
        ordered[-1] is publish, "PyPI publication must follow all verification steps"
    )
    return {
        "workflow_sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
        "triggers": ["workflow_dispatch", "release.published"],
        "published_tag_required": True,
        "environment": "pypi",
        "trusted_publishing": True,
        "owner_switch_required": True,
        "exact_github_release_wheels": True,
        "stored_upload_token": False,
    }


def validate_build_commands(kernel: list[list[str]], wheels: list[list[str]]) -> None:
    for label, commands in (("kernel", kernel), ("wheels", wheels)):
        docker = command_required(
            commands, "docker", "run", label=f"{label} manylinux container"
        )
        require(
            EXPECTED_IMAGE in docker
            and {"--rm", "--init", "--interactive"} <= set(docker),
            f"{label} must use the pinned manylinux image",
        )
        require_options(
            docker,
            {
                "--volume": "$GITHUB_WORKSPACE:$GITHUB_WORKSPACE",
                "--workdir": "$GITHUB_WORKSPACE",
            },
            f"{label} container",
        )
        useradd = command_required(
            commands, "useradd", label=f"{label} workspace owner"
        )
        require_options(useradd, {"--uid": "$(stat -c %u $GITHUB_WORKSPACE)"}, label)
        require(
            "--create-home" in useradd and useradd[-1] == "postgamma",
            f"{label} workspace owner changed",
        )
        runusers = [
            cmd
            for cmd in commands
            if cmd[:1] == ["runuser"] and option(cmd, "--user") == ["postgamma"]
        ]
        require(len(runusers) >= 2, f"{label} must execute as an unprivileged user")
    urls = [word for cmd in commands_named(kernel, "curl") for word in cmd]
    require(
        "https://ftp.gnu.org/gnu/make/make-${GNU_MAKE_VERSION}.tar.gz" in urls,
        "GNU Make source URL changed",
    )
    require(
        "https://github.com/mamba-org/micromamba-releases/releases/download/${MICROMAMBA_VERSION}/micromamba-linux-64"
        in urls,
        "micromamba source URL changed",
    )
    for variable in ("GNU_MAKE_SHA256", "MICROMAMBA_SHA256"):
        verified = any(
            argv(before)[:1] == ["echo"]
            and any("${" + variable + "}" in word for word in before[1:])
            and argv(after) == ["sha256sum", "--check"]
            for before, after in zip(kernel, kernel[1:])
        )
        require(
            verified,
            f"{variable}: GNU Make checksum verification or micromamba verification is missing",
        )
    configure = command_required(kernel, "configure", label="GNU Make install prefix")
    require_options(
        configure, {"--prefix": "/opt/postgamma-tools"}, "GNU Make install prefix"
    )
    exports = {word for cmd in kernel if cmd[:1] == ["export"] for word in cmd[1:]}
    require(
        {
            "PATH=/opt/postgamma-tools/bin:$PATH",
            "LLVM_CONFIG=/opt/postgamma-llvm/bin/llvm-config",
            "CLANGXX=/opt/postgamma-llvm/bin/clang++",
        }
        <= exports,
        "isolated GNU Make and LLVM toolchain exports are missing",
    )
    mamba = command_required(
        kernel, "$micromamba", "create", label="explicit LLVM toolchain"
    )
    require_options(
        mamba,
        {"--file": EXPECTED_TOOLCHAIN_LOCK, "--prefix": "/opt/postgamma-llvm"},
        "LLVM toolchain",
    )
    build = command_required(
        kernel, "make", "static-sdk-package", label="single static kernel build"
    )
    require(
        {"CC=gcc", "CXX=g++", "CFLAGS=$RELEASE_CFLAGS"} <= set(build),
        "release compiler flags or product compilers changed",
    )
    find = command_required(
        kernel, "find", "python-manylinux-kernel", label="kernel archive"
    )
    sort = command_required(kernel, "sort", label="kernel archive")
    archives = [cmd for cmd in commands_named(kernel, "tar") if "--null" in cmd]
    gzip = command_required(kernel, "gzip", label="kernel archive")
    require(
        "-print0" in find
        and "-z" in sort
        and len(archives) == 1
        and "--no-recursion" in archives[0]
        and gzip[1:] == ["-n", ">", "python-manylinux-kernel.tar.gz"]
        and not any(
            word.startswith("--sort")
            for cmd in commands_named(kernel, "tar")
            for word in cmd
        ),
        "kernel archive must use deterministic packaging supported by manylinux tar",
    )
    require(
        any(cmd[:2] == ["LC_ALL=C", "sort"] for cmd in kernel),
        "kernel archive sort must use LC_ALL=C",
    )
    wheel_exports = {
        word for cmd in wheels if cmd[:1] == ["export"] for word in cmd[1:]
    }
    require(
        {"CC=gcc", "CFLAGS=$RELEASE_CFLAGS"} <= wheel_exports,
        "wheel release compiler flags changed",
    )
    runner = command_required(
        wheels,
        "python",
        "buildsys/run_python_release.py",
        label="shared wheel release gate",
    )
    require(
        runner[0] == "$python_executable",
        "release runner must use the matrix interpreter",
    )
    require_options(
        runner,
        {
            "--static-library": "build/python-manylinux-kernel/libpostgamma_python_abi1.a",
            "--link-options": "build/python-manylinux-kernel/postgamma-static-libs.txt",
            "--static-receipt": "build/python-manylinux-kernel/static-library-link.json",
            "--resource-root": "build/python-manylinux-kernel/resource-pack",
            "--output-dir": "$release_root",
            "--platform": "$MANYLINUX_POLICY",
        },
        "shared wheel release gate",
    )
    require(
        ["python_executable=/opt/python/${PYTHON_TAG}-${PYTHON_TAG}/bin/python"]
        in wheels
        and ["release_root=build/python-release/${PYTHON_TAG}"] in wheels,
        "wheel interpreter and output must follow the matrix tag",
    )
    for script in (
        "python/build_backend.py",
        "buildsys/repair_python_wheel.py",
        "buildsys/check_python_wheel.py",
    ):
        require(
            not commands_named(wheels, "python", script),
            "wheel stages must run only through the shared release runner",
        )


def check(
    path: Path,
    baseline_path: Path,
    pypi_path: Path,
    toolchain_lock_path: Path,
) -> dict[str, object]:
    document = read_workflow(path)
    baseline = load_release_baseline(baseline_path)
    pypi = validate_pypi_workflow(pypi_path)
    expected_toolchain_lock = (
        baseline_path.resolve().parents[2] / EXPECTED_TOOLCHAIN_LOCK
    )
    require(
        toolchain_lock_path.resolve() == expected_toolchain_lock,
        "workflow contract must validate the repository LLVM toolchain lock",
    )
    toolchain = validate_toolchain_lock(toolchain_lock_path)
    require(
        baseline["python"]["tags"] == EXPECTED_PYTHON_TAGS
        and baseline["python"]["manylinux_policy"] == EXPECTED_POLICY,
        "workflow constants differ from the Python release baseline",
    )
    triggers = mapping(document.get("on"), "release triggers")
    require(
        set(triggers) == {"workflow_dispatch", "push"}
        and triggers["workflow_dispatch"] in (None, {})
        and triggers["push"] == {"tags": ["v*"]},
        "release workflow must support manual dispatch and version tags",
    )
    environment = mapping(document.get("env"), "release environment")
    expected_env = {
        "MANYLINUX_POLICY": (EXPECTED_POLICY, "manylinux policy"),
        "RELEASE_CFLAGS": (EXPECTED_CFLAGS, "generic x86-64 flags"),
        "SOURCE_DATE_EPOCH": (
            str(baseline["wheel"]["source_date_epoch"]), "ZIP-compatible source date epoch"
        ),
        "GNU_MAKE_VERSION": (EXPECTED_GNU_MAKE, "pinned GNU Make"),
        "GNU_MAKE_SHA256": (EXPECTED_GNU_MAKE_SHA256, "pinned GNU Make checksum"),
        "MICROMAMBA_VERSION": (EXPECTED_MICROMAMBA, "pinned micromamba"),
        "MICROMAMBA_SHA256": (EXPECTED_MICROMAMBA_SHA256, "pinned micromamba checksum"),
    }
    for name, (value, label) in expected_env.items():
        require(
            str(environment.get(name)) == value, f"release environment: {label} changed"
        )
    jobs = validate_jobs(
        document,
        {
            "version": set(),
            "kernel": {"version"},
            "documentation": {"version"},
            "wheels": {"version", "kernel"},
            "release-gate": {"version", "kernel", "documentation", "wheels"},
            "stage-github-release": {"version", "release-gate"},
            "publish-github-release": {"version", "stage-github-release"},
            "dispatch-pypi": {"version", "publish-github-release"},
        },
        {"stage-github-release", "publish-github-release"},
    )
    for name, job in jobs.items():
        for scope in (job, *steps(job, name)):
            overrides = mapping(scope.get("env", {}), f"{name} environment")
            for key, (value, label) in expected_env.items():
                require(
                    str(overrides.get(key, value)) == value, f"{name} overrides {label}"
                )
    strategy = mapping(jobs["wheels"].get("strategy"), "wheel strategy")
    matrix = mapping(strategy.get("matrix"), "CPython matrix")
    declared_tags = matrix.get("python_tag")
    require(
        set(matrix) == {"python_tag"}
        and isinstance(declared_tags, list)
        and all(isinstance(tag, str) for tag in declared_tags)
        and len(declared_tags) == len(EXPECTED_PYTHON_TAGS)
        and set(declared_tags) == set(EXPECTED_PYTHON_TAGS),
        f"Python release matrix must contain exactly {EXPECTED_PYTHON_TAGS}",
    )
    require(
        strategy.get("fail-fast") is False,
        "Python release matrix must collect every interpreter result",
    )
    tags = list(EXPECTED_PYTHON_TAGS)
    commands = {name: job_commands(job, name) for name, job in jobs.items()}
    for name, entries in commands.items():
        require(
            not commands_named(entries, "twine", "upload")
            and not any(
                "gh-action-pypi-publish" in str(step.get("uses", ""))
                for step in jobs[name]["steps"]
            ),
            "GitHub release workflow must not publish directly to a package index",
        )
        require(
            not any(
                word.startswith("POSTGAMMA_LIBRARY=")
                or "libpostgamma_python_abi1.so" in word
                for cmd in entries
                for word in cmd
            ),
            "release must not package a PostGamma shared library",
        )
    command_required(
        commands["version"],
        "python",
        "buildsys/product_version.py",
        label="canonical version",
    )
    validate_build_commands(commands["kernel"], commands["wheels"])
    command_required(
        commands["documentation"],
        "make",
        "docs-check",
        label="strict release documentation",
    )
    aggregate = command_required(
        commands["release-gate"],
        "python",
        "buildsys/check_python_release.py",
        label="matrix aggregation",
    )
    require_options(
        aggregate,
        {
            "--baseline": "manifests/api/python-api-v1.json",
            "--platform": "$MANYLINUX_POLICY",
            "--output": "build/python-release-evidence.json",
        },
        "matrix aggregation",
    )
    require(
        sorted(option(aggregate, "--python-tag")) == sorted(tags),
        "matrix aggregation omits an interpreter",
    )
    promote = command_required(
        commands["release-gate"],
        "python",
        "buildsys/prepare_release_assets.py",
        "prepare",
        label="tested-byte promotion",
    )
    require_options(
        promote,
        {
            "--python-evidence": "build/python-release-evidence.json",
            "--output": "build/release-assets",
        },
        "tested-byte promotion",
    )
    require(
        [argv(cmd) for cmd in commands["release-gate"]].index(aggregate)
        < [argv(cmd) for cmd in commands["release-gate"]].index(promote),
        "promotion must follow matrix aggregation",
    )
    require_action(
        jobs["kernel"],
        "actions/upload-artifact",
        {
            "name": "python-manylinux-kernel",
            "path": "build/python-manylinux-kernel.tar.gz",
        },
        "kernel archive upload",
    )
    require_action(
        jobs["wheels"],
        "actions/download-artifact",
        {"name": "python-manylinux-kernel", "path": "build"},
        "kernel reuse",
    )
    require_action(
        jobs["wheels"],
        "actions/upload-artifact",
        {"name": "python-wheel-${{ matrix.python_tag }}"},
        "wheel evidence upload",
    )
    diagnostics = require_action(
        jobs["wheels"],
        "actions/upload-artifact",
        {"name": WHEEL_DIAGNOSTICS_ARTIFACT, "if-no-files-found": "warn"},
        "failed wheel diagnostics",
    )
    require(
        set(str(diagnostics["with"].get("path", "")).splitlines())
        == {
            "build/python-release/${{ matrix.python_tag }}/reports/",
            "build/python-release/${{ matrix.python_tag }}/check/*.stdout",
            "build/python-release/${{ matrix.python_tag }}/check/*.stderr",
        },
        "failed wheel diagnostics must preserve checker output and release logs",
    )
    require_action(
        jobs["release-gate"],
        "actions/download-artifact",
        {"pattern": "python-wheel-*", "path": "build/python-release-artifacts"},
        "matrix evidence download",
    )
    bundle = "postgamma-release-${{ needs.version.outputs.version }}"
    require_action(
        jobs["release-gate"],
        "actions/upload-artifact",
        {"name": bundle, "path": "build/release-assets/"},
        "release bundle upload",
    )
    require_action(
        jobs["stage-github-release"],
        "actions/download-artifact",
        {"name": bundle, "path": "build/release-assets"},
        "tested release bundle download",
    )
    create = command_required(
        commands["stage-github-release"],
        "gh",
        "release",
        "create",
        label="draft release",
    )
    draft_flags = {"--verify-tag", "--draft", "--generate-notes"}
    flags_bound = draft_flags <= set(create) or any(
        len(cmd) > 2
        and cmd[0].endswith("=")
        and cmd[1] == "("
        and draft_flags <= set(cmd[2:])
        and "${" + cmd[0][:-1] + "[@]}" in create
        for cmd in commands["stage-github-release"]
    )
    require(
        flags_bound and "build/release-assets/*" in create,
        "draft release must upload the tested bundle with verified draft flags",
    )
    require(
        environment_name(jobs["publish-github-release"]) == "github-release",
        "publication must use the protected github-release environment",
    )
    publish = command_required(
        commands["publish-github-release"],
        "gh",
        "release",
        "edit",
        label="GitHub release publication",
    )
    require_options(publish, {"--draft": "false"}, "GitHub release publication")
    dispatch_job = jobs["dispatch-pypi"]
    dispatch_steps = steps(dispatch_job, "PyPI dispatch")
    require(
        len(dispatch_steps) == 1
        and dispatch_steps[0].get("env")
        == {
            "GH_TOKEN": "${{ github.token }}",
            "RELEASE_TAG": "${{ needs.version.outputs.tag }}",
        },
        "PyPI dispatch must use GITHUB_TOKEN and the validated release tag",
    )
    dispatch = command_required(
        commands["dispatch-pypi"],
        "gh", "workflow", "run", "pypi.yml",
        label="explicit PyPI dispatch",
    )
    require_options(
        dispatch,
        {"--repo": "$GITHUB_REPOSITORY", "--ref": "$RELEASE_TAG"},
        "explicit PyPI dispatch",
    )
    return {
        "schema_version": 1,
        "kind": REPORT_KIND,
        "status": "pass",
        "validation_scope": "workflow_structure_and_declared_commands",
        "workflow_sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
        "manylinux_policy": EXPECTED_POLICY,
        "container_image": EXPECTED_IMAGE,
        "glibc_floor": "2.17",
        "cpu_baseline": "x86-64-v1",
        "gnu_make_version": EXPECTED_GNU_MAKE,
        "gnu_make_source_sha256": EXPECTED_GNU_MAKE_SHA256,
        "toolchain": toolchain,
        "python_tags": tags,
        "kernel_build_count": 1,
        "kernel_linkage": "static",
        "separate_kernel_library_in_wheel": False,
        "wheel_build_count": len(tags),
        "clean_install_required": True,
        "failed_wheel_diagnostics_preserved": True,
        "exact_tested_byte_promotion": True,
        "github_release_declared": True,
        "release_environment": "github-release",
        "draft_before_approval": True,
        "explicit_pypi_dispatch_after_publication": True,
        "strict_documentation_required": True,
        "build_jobs_publish_authorized": False,
        "pypi": pypi,
        "publish_authorized": False,
        "execution_evidence_required": "postgamma.python-release-evidence",
        "release_baseline_sha256": hashlib.sha256(
            baseline_path.read_bytes()
        ).hexdigest(),
        "release": baseline["release"],
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--workflow", required=True, type=Path)
    parser.add_argument("--pypi-workflow", required=True, type=Path)
    parser.add_argument("--baseline", required=True, type=Path)
    parser.add_argument("--toolchain-lock", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    try:
        report = check(
            args.workflow.resolve(),
            args.baseline.resolve(),
            args.pypi_workflow.resolve(),
            args.toolchain_lock.resolve(),
        )
    except (OSError, WheelCheckError, WorkflowContractError) as error:
        parser.error(str(error))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    print(
        "Python release workflow structure: pass; artifact execution evidence is still required"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
