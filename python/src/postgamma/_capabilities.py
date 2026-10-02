"""Stable capability and pin-reason vocabularies."""

from __future__ import annotations

from enum import IntFlag

from . import _native


class Capability(IntFlag):
    """Optional capabilities advertised by the loaded PostGamma kernel."""

    PREPARED_STATEMENTS = _native.CAP_PREPARED_STATEMENTS
    CHUNKED_RESULTS = _native.CAP_CHUNKED_RESULTS
    COPY_IN = _native.CAP_COPY_IN
    COPY_OUT = _native.CAP_COPY_OUT
    ARROW_C_DATA = _native.CAP_ARROW_C_DATA
    NOTIFICATIONS = _native.CAP_NOTIFICATIONS
    REQUEST_NOTICES = _native.CAP_REQUEST_NOTICES
    STATUS_TELEMETRY = _native.CAP_STATUS_TELEMETRY
    INSTANCE_EVENTS = _native.CAP_INSTANCE_EVENTS
    MANAGEMENT_OPERATIONS = _native.CAP_MANAGEMENT_OPERATIONS
    MULTIPLE_INSTANCES = _native.CAP_MULTIPLE_INSTANCES
    NATIVE_EXTENSION_LOADING = _native.CAP_NATIVE_EXTENSION_LOADING
    LOGICAL_BACKUP = _native.CAP_LOGICAL_BACKUP
    LOGICAL_RESTORE = _native.CAP_LOGICAL_RESTORE
    PHYSICAL_BACKUP = _native.CAP_PHYSICAL_BACKUP
    MAINTENANCE = _native.CAP_MAINTENANCE
    BUNDLED_EXTENSIONS = _native.CAP_BUNDLED_EXTENSIONS


class PinReason(IntFlag):
    """Reasons a logical session currently retains one executor worker."""

    NONE = 0
    TRANSACTION = _native.PIN_TRANSACTION
    ADVISORY_LOCK = _native.PIN_ADVISORY_LOCK
    PORTAL = _native.PIN_PORTAL
    COPY = _native.PIN_COPY
    OTHER = _native.PIN_OTHER
