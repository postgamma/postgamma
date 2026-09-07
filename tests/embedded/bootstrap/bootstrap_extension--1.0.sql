CREATE FUNCTION bootstrap_extension_answer()
RETURNS integer
AS 'MODULE_PATHNAME', 'bootstrap_extension_answer'
LANGUAGE C STRICT;
