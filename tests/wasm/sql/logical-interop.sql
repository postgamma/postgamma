CREATE SCHEMA interop;
CREATE TYPE interop.phase AS ENUM ('ready', 'done');
CREATE SEQUENCE interop.sequence START 83;
CREATE TABLE interop.values(id bigint PRIMARY KEY, exact numeric, bytes bytea, value text, missing text, phase interop.phase);
INSERT INTO interop.values VALUES (9223372036854775807,12345678901234567890.123456789,decode('007f80ff','hex'),'雪 🐘',NULL,'ready');
CREATE VIEW interop.view AS SELECT * FROM interop.values;
CREATE FUNCTION interop.plus_one(integer) RETURNS integer LANGUAGE sql IMMUTABLE AS 'SELECT $1+1';
