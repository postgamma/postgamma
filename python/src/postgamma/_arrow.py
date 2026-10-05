"""Dependency-free Arrow C Data adapter."""

from __future__ import annotations

from typing import Any

from ._errors import InterfaceError, NotSupportedError


class ArrowResult:
    """A one-shot Arrow struct array exported through the PyCapsule protocol.

    The PostGamma kernel does not link an Arrow runtime. Consumers such as
    PyArrow, Polars, and DataFusion take ownership through ``__arrow_c_array__``.
    """

    __slots__ = ("_capsules",)

    def __init__(self, capsules: tuple[Any, Any]) -> None:
        if not isinstance(capsules, tuple) or len(capsules) != 2:
            raise InterfaceError("native Arrow export returned an invalid capsule pair")
        self._capsules: tuple[Any, Any] | None = capsules

    @property
    def consumed(self) -> bool:
        """Return whether Arrow capsule ownership has already transferred."""

        return self._capsules is None

    def __arrow_c_array__(self, requested_schema: Any = None) -> tuple[Any, Any]:
        if requested_schema is not None:
            raise NotSupportedError(
                "PostGamma Arrow export does not perform requested-schema casting"
            )
        if self._capsules is None:
            raise InterfaceError("ArrowResult has already transferred ownership")
        capsules = self._capsules
        self._capsules = None
        return capsules
