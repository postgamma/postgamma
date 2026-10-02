#!/usr/bin/env python3
"""Generate the public C references from Doxygen and frozen API contracts."""

from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
import xml.etree.ElementTree as ET
from collections import OrderedDict
from pathlib import Path
from typing import Any, Iterable


FUNCTION_NAME = re.compile(r"^(?:pgm|pgmex)_[a-z0-9_]+$")
HOST_SPECIAL_MACROS = {
    "PGM_ABI_VERSION_ENCODE",
    "PGM_API",
    "PGM_NO_TIMEOUT",
}


class DocumentationError(RuntimeError):
    """Raised when source declarations and documentation contracts diverge."""


def load_json(path: Path) -> dict[str, Any]:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise DocumentationError(f"cannot read {path}: {error}") from error
    if not isinstance(document, dict):
        raise DocumentationError(f"{path} must contain a JSON object")
    return document


def xml_text(element: ET.Element | None) -> str:
    if element is None:
        return ""
    return " ".join("".join(element.itertext()).split())


def markdown_cell(value: object) -> str:
    return str(value).replace("|", "\\|").replace("\n", " ")


def normalized_signature(definition: str, arguments: str) -> str:
    definition = re.sub(r"\bPGM_API\s+", "", definition)
    return " ".join((definition + arguments + ";").split())


def doxygen_config(config: Path, output: Path) -> str:
    source = config.read_text(encoding="utf-8")
    return (
        source
        + "\nOUTPUT_DIRECTORY = \""
        + str(output.resolve()).replace("\\", "/")
        + "\"\n"
    )


def run_doxygen(root: Path, executable: str, config: Path, output: Path) -> Path:
    command = shutil.which(executable) if "/" not in executable else executable
    if command is None:
        raise DocumentationError(
            f"Doxygen executable {executable!r} was not found; install doxygen"
        )
    if output.exists():
        shutil.rmtree(output)
    output.mkdir(parents=True)
    result = subprocess.run(
        (command, "-"),
        cwd=root,
        input=doxygen_config(config, output),
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if result.returncode != 0:
        detail = (result.stderr or result.stdout).strip()
        raise DocumentationError(f"Doxygen failed: {detail}")
    xml = output / "xml"
    if not (xml / "index.xml").is_file():
        raise DocumentationError("Doxygen did not produce its XML index")
    return xml


def xml_documents(directory: Path) -> Iterable[ET.Element]:
    for path in sorted(directory.glob("*.xml")):
        try:
            yield ET.parse(path).getroot()
        except ET.ParseError as error:
            raise DocumentationError(f"invalid Doxygen XML {path}: {error}") from error


def collect_functions(directory: Path) -> dict[str, str]:
    functions: dict[str, str] = {}
    for root in xml_documents(directory):
        for member in root.findall(".//memberdef[@kind='function']"):
            name = xml_text(member.find("name"))
            if not FUNCTION_NAME.fullmatch(name):
                continue
            signature = normalized_signature(
                xml_text(member.find("definition")),
                xml_text(member.find("argsstring")),
            )
            previous = functions.setdefault(name, signature)
            if previous != signature:
                raise DocumentationError(
                    f"Doxygen produced conflicting declarations for {name}"
                )
    return functions


def collect_typedefs(directory: Path, prefix: str) -> dict[str, str]:
    typedefs: dict[str, str] = {}
    for root in xml_documents(directory):
        for member in root.findall(".//memberdef[@kind='typedef']"):
            name = xml_text(member.find("name"))
            if not name.startswith(prefix):
                continue
            definition = xml_text(member.find("definition"))
            # Doxygen includes a function-pointer typedef's argument list in
            # ``definition`` and repeats it in ``argsstring``.  The definition
            # is complete for both ordinary and function-pointer typedefs.
            typedefs.setdefault(name, " ".join((definition + ";").split()))
    return typedefs


def collect_structs(directory: Path, prefix: str) -> dict[str, list[tuple[str, str]]]:
    structures: dict[str, list[tuple[str, str]]] = {}
    for root in xml_documents(directory):
        for compound in root.findall(".//compounddef[@kind='struct']"):
            name = xml_text(compound.find("compoundname"))
            if not name.startswith(prefix):
                continue
            fields: list[tuple[str, str]] = []
            for member in compound.findall(".//memberdef[@kind='variable']"):
                field_name = xml_text(member.find("name"))
                field_type = xml_text(member.find("type"))
                if field_name:
                    fields.append((field_name, field_type))
            structures[name] = fields
    return structures


def collect_defines(directory: Path, prefix: str) -> dict[str, str]:
    defines: dict[str, str] = {}
    for root in xml_documents(directory):
        for member in root.findall(".//memberdef[@kind='define']"):
            name = xml_text(member.find("name"))
            if not name.startswith(prefix):
                continue
            initializer = xml_text(member.find("initializer"))
            defines.setdefault(name, initializer)
    return defines


def exact_names(entries: object, label: str) -> list[str]:
    if not isinstance(entries, list):
        raise DocumentationError(f"{label} must be a list")
    names: list[str] = []
    for entry in entries:
        if not isinstance(entry, dict) or not isinstance(entry.get("name"), str):
            raise DocumentationError(f"{label} contains an invalid entry")
        names.append(entry["name"])
    if len(names) != len(set(names)):
        raise DocumentationError(f"{label} contains duplicate names")
    return names


def validate_summary_map(
    summaries: object, expected: set[str], label: str
) -> dict[str, str]:
    if not isinstance(summaries, dict) or not all(
        isinstance(name, str) and isinstance(summary, str) and summary.strip()
        for name, summary in summaries.items()
    ):
        raise DocumentationError(f"{label} must be a nonempty string map")
    names = set(summaries)
    if names != expected:
        raise DocumentationError(
            f"{label} mismatch: missing={sorted(expected - names)}, "
            f"stale={sorted(names - expected)}"
        )
    return {name: summaries[name].strip() for name in summaries}


def title_from_identifier(value: str) -> str:
    return value.replace("_", " ").strip().title()


def render_constants(registry: object) -> list[str]:
    if not isinstance(registry, list):
        raise DocumentationError("numeric_registry must be a list")
    output = ["## Constants", ""]
    for group in registry:
        if not isinstance(group, dict) or not isinstance(group.get("group"), str):
            raise DocumentationError("numeric_registry contains an invalid group")
        values = group.get("values")
        if not isinstance(values, dict) or not values:
            raise DocumentationError("numeric_registry group has no values")
        output.extend(
            [
                f"### {title_from_identifier(group['group'])}",
                "",
                "| Name | Value |",
                "| --- | ---: |",
            ]
        )
        for name, value in values.items():
            output.append(f"| `{markdown_cell(name)}` | `{markdown_cell(value)}` |")
        if group.get("bitmask") is True:
            output.extend(["", "Values in this group may be combined as a bit mask."])
        output.append("")
    return output


def numeric_names(registry: object) -> set[str]:
    if not isinstance(registry, list):
        raise DocumentationError("numeric_registry must be a list")
    names: set[str] = set()
    for group in registry:
        if not isinstance(group, dict) or not isinstance(group.get("values"), dict):
            raise DocumentationError("numeric_registry contains an invalid group")
        group_names = set(group["values"])
        if names & group_names:
            raise DocumentationError("numeric_registry contains duplicate names")
        names.update(group_names)
    return names


def validate_host_macros(
    registry: object,
    structures: dict[str, list[tuple[str, str]]],
    defines: dict[str, str],
) -> dict[str, int]:
    numerics = numeric_names(registry)
    initializers = {name for name in defines if name.endswith("_INIT")}
    required_initializers = {
        name.upper() + "_INIT"
        for name, fields in structures.items()
        if fields and fields[0][0] == "struct_size"
    }
    missing = (numerics | required_initializers | HOST_SPECIAL_MACROS) - set(defines)
    unclassified = set(defines) - (
        numerics | initializers | HOST_SPECIAL_MACROS
    )
    if missing or unclassified:
        raise DocumentationError(
            "public C macro coverage mismatch: "
            f"missing={sorted(missing)}, unclassified={sorted(unclassified)}"
        )
    stale_initializers = initializers - required_initializers
    if stale_initializers:
        raise DocumentationError(
            "public C initializer has no extensible structure: "
            + ", ".join(sorted(stale_initializers))
        )
    return {
        "numeric_count": len(numerics),
        "initializer_count": len(initializers),
        "special_count": len(HOST_SPECIAL_MACROS),
        "total_count": len(defines),
    }


def render_common_macros() -> list[str]:
    return [
        "## Common macros",
        "",
        "| Macro | Contract |",
        "| --- | --- |",
        "| `PGM_ABI_VERSION_ENCODE(major, minor)` | Encode an ABI major and "
        "minor in the same form as `PGM_ABI_VERSION`. |",
        "| `PGM_NO_TIMEOUT` | Select no deadline for a C API timeout measured "
        "in milliseconds. |",
        "| `PGM_API` | Portable public-symbol declaration annotation; host "
        "code must not redefine it. |",
        "",
    ]


def render_handles(handles: object) -> list[str]:
    if not isinstance(handles, list):
        raise DocumentationError("handles must be a list")
    output = [
        "## Opaque handles",
        "",
        "| Handle | Parent | Constructors | Destructor |",
        "| --- | --- | --- | --- |",
    ]
    for handle in handles:
        if not isinstance(handle, dict):
            raise DocumentationError("handles contains an invalid entry")
        constructors = handle.get("constructors")
        if not isinstance(constructors, list):
            raise DocumentationError("handle constructors must be a list")
        output.append(
            "| `{name}` | {parent} | {constructors} | `{destructor}` |".format(
                name=markdown_cell(handle.get("name")),
                parent=(
                    f"`{markdown_cell(handle['parent'])}`"
                    if handle.get("parent") is not None
                    else "Library"
                ),
                constructors=", ".join(f"`{markdown_cell(item)}`" for item in constructors),
                destructor=markdown_cell(handle.get("destructor")),
            )
        )
    output.append("")
    return output


def render_typedefs(typedefs: dict[str, str], excluded: set[str]) -> list[str]:
    selected = [item for item in sorted(typedefs.items()) if item[0] not in excluded]
    if not selected:
        return []
    output = ["## Public typedefs", ""]
    for name, definition in selected:
        output.extend([f"### `{name}`", "", "```c", definition, "```", ""])
    return output


def render_structures(
    structures: dict[str, list[tuple[str, str]]],
    defines: dict[str, str],
    introduction: str,
) -> list[str]:
    output = [
        "## Public structures",
        "",
        introduction,
        "",
    ]
    for name, fields in sorted(structures.items()):
        output.extend([f"### `{name}`", ""])
        macro = name.upper() + "_INIT"
        if macro in defines:
            output.extend([f"Initializer: `{macro}`", ""])
        output.extend(["| Field | Type |", "| --- | --- |"])
        for field_name, field_type in fields:
            output.append(
                f"| `{markdown_cell(field_name)}` | `{markdown_cell(field_type)}` |"
            )
        output.append("")
    return output


def render_functions(
    policy: dict[str, Any],
    signatures: dict[str, str],
    summaries: dict[str, str],
) -> list[str]:
    profiles = policy.get("profiles")
    entries = policy.get("functions")
    if not isinstance(profiles, dict) or not isinstance(entries, list):
        raise DocumentationError("public API policy is incomplete")
    groups: OrderedDict[str, list[dict[str, Any]]] = OrderedDict()
    for entry in entries:
        if not isinstance(entry, dict) or not isinstance(entry.get("object"), str):
            raise DocumentationError("function policy contains an invalid entry")
        groups.setdefault(entry["object"], []).append(entry)

    output = ["## Functions", ""]
    for object_name, functions in groups.items():
        title = (
            "Library metadata"
            if object_name == "library"
            else title_from_identifier(object_name)
        )
        output.extend([f"### {title}", ""])
        for entry in functions:
            name = entry["name"]
            profile = profiles.get(entry.get("profile"))
            if not isinstance(profile, dict):
                raise DocumentationError(f"{name} has no valid contract profile")
            output.extend(
                [
                    f"#### `{name}`",
                    "",
                    "```c",
                    signatures[name],
                    "```",
                    "",
                    summaries[name],
                    "",
                    f"Introduced in `{entry['phase']}`. Contract profile: "
                    f"`{entry['profile']}`.",
                    "",
                    "| Property | Contract |",
                    "| --- | --- |",
                ]
            )
            for key in ("ownership", "blocking", "thread_safety", "callback_context", "errors"):
                value = profile.get(key)
                if not isinstance(value, str) or not value:
                    raise DocumentationError(
                        f"contract profile {entry['profile']} lacks {key}"
                    )
                output.append(
                    f"| {title_from_identifier(key)} | {markdown_cell(value)} |"
                )
            output.append("")
    return output


def render_host_reference(
    policy: dict[str, Any],
    signatures: dict[str, str],
    summaries: dict[str, str],
    typedefs: dict[str, str],
    structures: dict[str, list[tuple[str, str]]],
    defines: dict[str, str],
) -> str:
    opaque = {entry["name"] for entry in policy["handles"]}
    lines = [
        "<!-- Generated by buildsys/generate_c_api_docs.py; do not edit. -->",
        "",
        "The loaded ABI is version 1.3 and embeds PostgreSQL 19. Borrowed views "
        "and handle ownership follow the contracts below.",
        "",
    ]
    lines.extend(render_handles(policy.get("handles")))
    lines.extend(render_constants(policy.get("numeric_registry")))
    lines.extend(render_common_macros())
    lines.extend(render_typedefs(typedefs, opaque))
    lines.extend(
        render_structures(
            structures,
            defines,
            "Every extensible public structure must be initialized with its "
            "matching `PGM_*_INIT` macro before use.",
        )
    )
    lines.extend(render_functions(policy, signatures, summaries))
    return "\n".join(lines).rstrip() + "\n"


def render_extension_reference(
    signatures: dict[str, str],
    summaries: dict[str, str],
    typedefs: dict[str, str],
    structures: dict[str, list[tuple[str, str]]],
    defines: dict[str, str],
) -> str:
    lines = [
        "<!-- Generated by buildsys/generate_c_api_docs.py; do not edit. -->",
        "",
        "SDK ABI v1 applies only to extensions selected and compiled into the "
        "PostGamma kernel.",
        "",
        "## Constants",
        "",
        "| Name | Definition |",
        "| --- | --- |",
    ]
    for name, value in sorted(defines.items()):
        if name.endswith("_INIT"):
            continue
        lines.append(f"| `{markdown_cell(name)}` | `{markdown_cell(value)}` |")
    lines.append("")
    lines.extend(render_typedefs(typedefs, set()))
    lines.extend(
        render_structures(
            structures,
            defines,
            "Use the listed initializer whenever the SDK provides one. "
            "Immutable extension descriptors are initialized field by field.",
        )
    )
    lines.extend(["## Functions", ""])
    for name in sorted(summaries):
        lines.extend(
            [
                f"### `{name}`",
                "",
                "```c",
                signatures[name],
                "```",
                "",
                summaries[name],
                "",
            ]
        )
    return "\n".join(lines).rstrip() + "\n"


def write_text(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")


def generate(args: argparse.Namespace) -> dict[str, Any]:
    root = args.root.resolve()
    policy = load_json((root / args.policy).resolve())
    descriptions = load_json((root / args.descriptions).resolve())
    if policy.get("kind") != "postgamma.c-public-api-contract":
        raise DocumentationError("unsupported public C API policy")
    if descriptions.get("kind") != "postgamma.c-api-documentation":
        raise DocumentationError("unsupported C documentation inventory")

    expected = set(exact_names(policy.get("functions"), "C function policy"))
    summaries = validate_summary_map(
        descriptions.get("functions"), expected, "C function summaries"
    )
    extension_summaries = validate_summary_map(
        descriptions.get("extension_functions"),
        set(descriptions.get("extension_functions", {})),
        "extension function summaries",
    )

    doxygen_output = (root / args.doxygen_output).resolve()
    xml = run_doxygen(
        root,
        args.doxygen,
        (root / args.doxyfile).resolve(),
        doxygen_output,
    )
    declarations = collect_functions(xml)
    declared_host = {name for name in declarations if name.startswith("pgm_")}
    declared_extension = {
        name for name in declarations if name.startswith("pgmex_")
    }
    if declared_host != expected:
        raise DocumentationError(
            "Doxygen/public C function mismatch: "
            f"missing={sorted(expected - declared_host)}, "
            f"extra={sorted(declared_host - expected)}"
        )
    if declared_extension != set(extension_summaries):
        raise DocumentationError(
            "Doxygen/extension function mismatch: "
            f"missing={sorted(set(extension_summaries) - declared_extension)}, "
            f"extra={sorted(declared_extension - set(extension_summaries))}"
        )

    host_typedefs = collect_typedefs(xml, "pgm_")
    host_structures = collect_structs(xml, "pgm_")
    host_defines = collect_defines(xml, "PGM_")
    extension_typedefs = collect_typedefs(xml, "pgmex_")
    extension_structures = collect_structs(xml, "pgmex_")
    extension_defines = collect_defines(xml, "PGMEX_")
    macro_counts = validate_host_macros(
        policy.get("numeric_registry"), host_structures, host_defines
    )

    host = render_host_reference(
        policy,
        declarations,
        summaries,
        host_typedefs,
        host_structures,
        host_defines,
    )
    extension = render_extension_reference(
        declarations,
        extension_summaries,
        extension_typedefs,
        extension_structures,
        extension_defines,
    )
    output = (root / args.output_dir).resolve()
    write_text(output / "c-api.md", host)
    write_text(output / "extension-api.md", extension)
    report = {
        "schema_version": 1,
        "kind": "postgamma.c-api-documentation-evidence",
        "status": "pass",
        "host_function_count": len(expected),
        "extension_function_count": len(extension_summaries),
        "host_typedef_count": len(host_typedefs),
        "host_structure_count": len(host_structures),
        "host_macro_count": macro_counts["total_count"],
        "host_numeric_macro_count": macro_counts["numeric_count"],
        "host_initializer_macro_count": macro_counts["initializer_count"],
        "extension_typedef_count": len(extension_typedefs),
        "extension_structure_count": len(extension_structures),
        "extension_macro_count": len(extension_defines),
    }
    write_text(
        output / "c-api.json",
        json.dumps(report, indent=2, sort_keys=True) + "\n",
    )
    return report


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True, type=Path)
    parser.add_argument("--doxygen", default="doxygen")
    parser.add_argument("--doxyfile", default="docs/Doxyfile", type=Path)
    parser.add_argument(
        "--policy", default="manifests/api/c-public-api.json", type=Path
    )
    parser.add_argument(
        "--descriptions", default="docs/reference/c-api.json", type=Path
    )
    parser.add_argument(
        "--doxygen-output", default="build/docs/doxygen", type=Path
    )
    parser.add_argument(
        "--output-dir", default="build/docs/generated", type=Path
    )
    args = parser.parse_args()
    try:
        report = generate(args)
    except DocumentationError as error:
        parser.error(str(error))
    print(
        "C documentation: "
        f"{report['host_function_count']} host and "
        f"{report['extension_function_count']} extension functions"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
