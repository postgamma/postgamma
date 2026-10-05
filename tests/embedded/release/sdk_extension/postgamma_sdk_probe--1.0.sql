CREATE FUNCTION postgamma_sdk_probe_snapshot()
RETURNS text
AS 'MODULE_PATHNAME', 'postgamma_sdk_probe_snapshot'
LANGUAGE C STRICT PARALLEL RESTRICTED;

CREATE FUNCTION postgamma_sdk_probe_session_add(bigint)
RETURNS bigint
AS 'MODULE_PATHNAME', 'postgamma_sdk_probe_session_add'
LANGUAGE C STRICT PARALLEL RESTRICTED;

CREATE FUNCTION postgamma_sdk_probe_instance_add(bigint)
RETURNS bigint
AS 'MODULE_PATHNAME', 'postgamma_sdk_probe_instance_add'
LANGUAGE C STRICT PARALLEL SAFE;

CREATE FUNCTION postgamma_sdk_probe_resource()
RETURNS text
AS 'MODULE_PATHNAME', 'postgamma_sdk_probe_resource'
LANGUAGE C STRICT PARALLEL SAFE;

CREATE FUNCTION postgamma_sdk_probe_parallel_value(bigint)
RETURNS bigint
AS 'MODULE_PATHNAME', 'postgamma_sdk_probe_parallel_value'
LANGUAGE C STRICT PARALLEL SAFE;

CREATE FUNCTION postgamma_sdk_probe_parallel_calls()
RETURNS bigint
AS 'MODULE_PATHNAME', 'postgamma_sdk_probe_parallel_calls'
LANGUAGE C STRICT PARALLEL RESTRICTED;

CREATE FUNCTION postgamma_sdk_probe_descriptor_rejected(integer)
RETURNS boolean
AS 'MODULE_PATHNAME', 'postgamma_sdk_probe_descriptor_rejected'
LANGUAGE C STRICT PARALLEL RESTRICTED;
