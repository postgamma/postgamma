"""SQL-aware adaptation of PEP 249 numeric parameter markers."""

from __future__ import annotations


def rewrite_numeric_parameters(operation: str) -> str:
    """Translate ``:1`` markers to PostgreSQL ``$1`` markers.

    Markers inside quoted identifiers, string literals, dollar-quoted bodies,
    line comments, and nested block comments are left untouched.  PostgreSQL
    casts such as ``value::int`` are therefore unambiguous.  Ordinary string
    literals follow PostgreSQL's supported default
    ``standard_conforming_strings = on``; only ``E'...'`` literals interpret
    backslash escapes.
    """

    if not isinstance(operation, str):
        raise TypeError("SQL operation must be a string")

    result: list[str] = []
    length = len(operation)
    position = 0
    while position < length:
        character = operation[position]

        if character == "'":
            position = _copy_quoted(
                operation,
                position,
                "'",
                result,
                backslash=_has_escape_string_prefix(operation, position),
            )
            continue
        if character == '"':
            position = _copy_quoted(operation, position, '"', result, backslash=False)
            continue
        if operation.startswith("--", position):
            end = operation.find("\n", position + 2)
            if end < 0:
                result.append(operation[position:])
                break
            result.append(operation[position : end + 1])
            position = end + 1
            continue
        if operation.startswith("/*", position):
            position = _copy_block_comment(operation, position, result)
            continue
        if character == "$":
            delimiter = _dollar_quote_delimiter(operation, position)
            if delimiter is not None:
                end = operation.find(delimiter, position + len(delimiter))
                if end < 0:
                    result.append(operation[position:])
                    break
                end += len(delimiter)
                result.append(operation[position:end])
                position = end
                continue
        if character == ":" and position + 1 < length:
            cursor = position + 1
            if operation[cursor].isdigit() and (
                position == 0 or operation[position - 1] != ":"
            ):
                while cursor < length and operation[cursor].isdigit():
                    cursor += 1
                index = operation[position + 1 : cursor]
                if index.startswith("0"):
                    raise ValueError("PEP 249 numeric parameters are one-based")
                result.extend(("$", index))
                position = cursor
                continue

        result.append(character)
        position += 1

    return "".join(result)


def is_transaction_control(operation: str) -> bool:
    """Return whether SQL starts with an explicit transaction-control command."""

    if not isinstance(operation, str):
        return False
    position = _skip_ignored(operation, 0)
    first, position = _read_keyword(operation, position)
    if first in {
        "ABORT",
        "BEGIN",
        "COMMIT",
        "END",
        "RELEASE",
        "ROLLBACK",
        "SAVEPOINT",
    }:
        return True
    if first not in {"PREPARE", "START"}:
        return False
    second, _position = _read_keyword(operation, _skip_ignored(operation, position))
    return second == "TRANSACTION"


def _has_escape_string_prefix(operation: str, quote_position: int) -> bool:
    if quote_position == 0 or operation[quote_position - 1] not in "Ee":
        return False
    if quote_position == 1:
        return True
    previous = operation[quote_position - 2]
    return not (previous.isalnum() or previous in "_$")


def _copy_quoted(
    operation: str,
    position: int,
    delimiter: str,
    result: list[str],
    *,
    backslash: bool,
) -> int:
    start = position
    position += 1
    length = len(operation)
    while position < length:
        character = operation[position]
        if backslash and character == "\\" and position + 1 < length:
            position += 2
            continue
        if character == delimiter:
            if position + 1 < length and operation[position + 1] == delimiter:
                position += 2
                continue
            position += 1
            break
        position += 1
    result.append(operation[start:position])
    return position


def _copy_block_comment(operation: str, position: int, result: list[str]) -> int:
    start = position
    position = _block_comment_end(operation, position)
    result.append(operation[start:position])
    return position


def _block_comment_end(operation: str, position: int) -> int:
    position += 2
    depth = 1
    length = len(operation)
    while position < length and depth:
        if operation.startswith("/*", position):
            depth += 1
            position += 2
        elif operation.startswith("*/", position):
            depth -= 1
            position += 2
        else:
            position += 1
    return position


def _skip_ignored(operation: str, position: int) -> int:
    length = len(operation)
    while position < length:
        if operation[position].isspace():
            position += 1
            continue
        if operation.startswith("--", position):
            newline = operation.find("\n", position + 2)
            return length if newline < 0 else _skip_ignored(operation, newline + 1)
        if operation.startswith("/*", position):
            position = _block_comment_end(operation, position)
            continue
        break
    return position


def _read_keyword(operation: str, position: int) -> tuple[str | None, int]:
    start = position
    length = len(operation)
    while position < length and (
        operation[position].isalnum() or operation[position] in "_$"
    ):
        position += 1
    if position == start:
        return None, position
    return operation[start:position].upper(), position


def _dollar_quote_delimiter(operation: str, position: int) -> str | None:
    cursor = position + 1
    length = len(operation)
    if cursor < length and operation[cursor] == "$":
        return "$$"
    if cursor >= length or not (
        operation[cursor].isalpha() or operation[cursor] == "_"
    ):
        return None
    cursor += 1
    while cursor < length and (
        operation[cursor].isalnum() or operation[cursor] == "_"
    ):
        cursor += 1
    if cursor < length and operation[cursor] == "$":
        return operation[position : cursor + 1]
    return None
