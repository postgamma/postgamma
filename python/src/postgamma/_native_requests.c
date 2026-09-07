/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

#include "_native_internal.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>


typedef struct PostgammaPythonStatement
{
	pgm_statement *statement;
	PyObject   *connection_capsule;
	pid_t		owner_pid;
	pthread_mutex_t mutex;
	size_t		request_count;
} PostgammaPythonStatement;


typedef struct PostgammaPythonRequest
{
	pgm_request *request;
	PyObject   *connection_capsule;
	PyObject   *statement_capsule;
	pid_t		owner_pid;
	pthread_mutex_t mutex;
	size_t		copy_count;
} PostgammaPythonRequest;


typedef struct PostgammaPythonCopy
{
	pgm_copy   *copy;
	PyObject   *request_capsule;
	pid_t		owner_pid;
	pthread_mutex_t mutex;
} PostgammaPythonCopy;


static PostgammaPythonStatement *statement_from_capsule(
	PyObject *capsule, bool require_open);
static PostgammaPythonRequest *request_from_capsule(
	PyObject *capsule, bool require_open);
static PostgammaPythonCopy *copy_from_capsule(
	PyObject *capsule, bool require_open);
static void statement_capsule_destructor(PyObject *capsule);
static void request_capsule_destructor(PyObject *capsule);
static void copy_capsule_destructor(PyObject *capsule);
static void statement_release_owner(PostgammaPythonStatement *handle);
static void request_release_owners(PostgammaPythonRequest *handle);
static void copy_release_owner(PostgammaPythonCopy *handle);
static PyObject *request_capsule_create(
	PyObject *connection_capsule, PyObject *statement_capsule,
	pgm_request *request);
static PyObject *copy_capsule_create_locked(
	PyObject *request_capsule, pgm_copy *copy);
static void discard_copy_capsule_locked(
	PyObject *request_capsule, PyObject *copy_capsule);
static int sql_from_python(
	PyObject *object, const char **sql, Py_ssize_t *sql_size);
static int type_oids_from_python(
	PyObject *object, uint32_t **type_oids, size_t *count);
static PyObject *statement_column_to_python(
	const pgm_statement *statement, size_t index);
static PyObject *request_result_to_python(
	PyObject *request_capsule, pgm_result **result, pgm_error **error);
static void warn_cleanup_failure(
	PyObject *capsule, const char *kind, pgm_status status, pgm_error *error);


static PostgammaPythonStatement *
statement_from_capsule(PyObject *capsule, bool require_open)
{
	PostgammaPythonStatement *handle = PyCapsule_GetPointer(
		capsule, POSTGAMMA_STATEMENT_CAPSULE);

	if (handle == NULL)
		return NULL;
	if (handle->owner_pid != getpid())
	{
		raise_simple_error(
			PGM_STATUS_FORKED_PROCESS,
			"PostGamma statement was inherited across fork; prepare it again in "
			"the child process");
		return NULL;
	}
	if (require_open && handle->statement == NULL)
	{
		raise_simple_error(PGM_STATUS_INVALID_ARGUMENT,
			"PostGamma statement is closed");
		return NULL;
	}
	return handle;
}


static PostgammaPythonRequest *
request_from_capsule(PyObject *capsule, bool require_open)
{
	PostgammaPythonRequest *handle = PyCapsule_GetPointer(
		capsule, POSTGAMMA_REQUEST_CAPSULE);

	if (handle == NULL)
		return NULL;
	if (handle->owner_pid != getpid())
	{
		raise_simple_error(
			PGM_STATUS_FORKED_PROCESS,
			"PostGamma request was inherited across fork; submit it again in the "
			"child process");
		return NULL;
	}
	if (require_open && handle->request == NULL)
	{
		raise_simple_error(PGM_STATUS_INVALID_ARGUMENT,
			"PostGamma request is closed");
		return NULL;
	}
	return handle;
}


static PostgammaPythonCopy *
copy_from_capsule(PyObject *capsule, bool require_open)
{
	PostgammaPythonCopy *handle = PyCapsule_GetPointer(
		capsule, POSTGAMMA_COPY_CAPSULE);

	if (handle == NULL)
		return NULL;
	if (handle->owner_pid != getpid())
	{
		raise_simple_error(
			PGM_STATUS_FORKED_PROCESS,
			"PostGamma COPY handle was inherited across fork; start COPY again in "
			"the child process");
		return NULL;
	}
	if (require_open && handle->copy == NULL)
	{
		raise_simple_error(PGM_STATUS_INVALID_ARGUMENT,
			"PostGamma COPY handle is closed");
		return NULL;
	}
	return handle;
}


static void
statement_release_owner(PostgammaPythonStatement *handle)
{
	PyObject   *owner = handle->connection_capsule;
	PostgammaPythonConnection *connection;

	if (owner == NULL)
		return;
	handle->connection_capsule = NULL;
	connection = PyCapsule_GetPointer(owner, POSTGAMMA_CONNECTION_CAPSULE);
	if (connection != NULL && connection->owner_pid == getpid())
	{
		if (mutex_lock(&connection->mutex) == 0)
		{
			if (connection->statement_count > 0)
				connection->statement_count--;
			mutex_unlock(&connection->mutex);
		}
		else
			PyErr_Clear();
	}
	else
		PyErr_Clear();
	Py_DECREF(owner);
}


static void
request_release_owners(PostgammaPythonRequest *handle)
{
	PyObject   *statement_owner = handle->statement_capsule;
	PyObject   *connection_owner = handle->connection_capsule;

	handle->statement_capsule = NULL;
	handle->connection_capsule = NULL;
	if (statement_owner != NULL)
	{
		PostgammaPythonStatement *statement = PyCapsule_GetPointer(
			statement_owner, POSTGAMMA_STATEMENT_CAPSULE);

		if (statement != NULL && statement->owner_pid == getpid())
		{
			if (mutex_lock(&statement->mutex) == 0)
			{
				if (statement->request_count > 0)
					statement->request_count--;
				mutex_unlock(&statement->mutex);
			}
			else
				PyErr_Clear();
		}
		else
			PyErr_Clear();
		Py_DECREF(statement_owner);
	}
	Py_XDECREF(connection_owner);
}


static void
copy_release_owner(PostgammaPythonCopy *handle)
{
	PyObject   *owner = handle->request_capsule;
	PostgammaPythonRequest *request;

	if (owner == NULL)
		return;
	handle->request_capsule = NULL;
	request = PyCapsule_GetPointer(owner, POSTGAMMA_REQUEST_CAPSULE);
	if (request != NULL && request->owner_pid == getpid())
	{
		if (mutex_lock(&request->mutex) == 0)
		{
			if (request->copy_count > 0)
				request->copy_count--;
			mutex_unlock(&request->mutex);
		}
		else
			PyErr_Clear();
	}
	else
		PyErr_Clear();
	Py_DECREF(owner);
}


static void
warn_cleanup_failure(
	PyObject *capsule, const char *kind, pgm_status status, pgm_error *error)
{
	const char *message = error != NULL ? pgm_error_message(error) : NULL;

	if (message == NULL || message[0] == '\0')
		message = pgm_status_name(status);
	if (PyErr_WarnFormat(
			PyExc_ResourceWarning, 1,
			"could not release an unclosed PostGamma %s: %s", kind, message) < 0)
		PyErr_WriteUnraisable(capsule);
}


static void
statement_capsule_destructor(PyObject *capsule)
{
	PostgammaPythonStatement *handle = PyCapsule_GetPointer(
		capsule, POSTGAMMA_STATEMENT_CAPSULE);

	if (handle == NULL)
	{
		PyErr_Clear();
		return;
	}
	if (handle->owner_pid == getpid() && handle->request_count != 0)
		Py_FatalError("PostGamma statement capsule has live requests");
	if (handle->owner_pid == getpid() && handle->statement != NULL)
	{
		pgm_error  *error = NULL;
		pgm_status	status;

		Py_BEGIN_ALLOW_THREADS
		status = pgm_statement_close(handle->statement, &error);
		Py_END_ALLOW_THREADS
		if (status == PGM_STATUS_OK)
			handle->statement = NULL;
		else
			warn_cleanup_failure(capsule, "statement", status, error);
		pgm_error_free(error);
	}
	statement_release_owner(handle);
	(void) pthread_mutex_destroy(&handle->mutex);
	free(handle);
}


static void
request_capsule_destructor(PyObject *capsule)
{
	PostgammaPythonRequest *handle = PyCapsule_GetPointer(
		capsule, POSTGAMMA_REQUEST_CAPSULE);

	if (handle == NULL)
	{
		PyErr_Clear();
		return;
	}
	if (handle->owner_pid == getpid() && handle->copy_count != 0)
		Py_FatalError("PostGamma request capsule has a live COPY handle");
	if (handle->owner_pid == getpid() && handle->request != NULL)
	{
		PostgammaPythonConnection *connection = PyCapsule_GetPointer(
			handle->connection_capsule, POSTGAMMA_CONNECTION_CAPSULE);

		if (connection != NULL)
			(void) retire_connection_request(connection, handle->request);
		else
		{
			PyErr_Clear();
			Py_BEGIN_ALLOW_THREADS
			pgm_request_free(handle->request);
			Py_END_ALLOW_THREADS
		}
		handle->request = NULL;
	}
	request_release_owners(handle);
	(void) pthread_mutex_destroy(&handle->mutex);
	free(handle);
}


static void
copy_capsule_destructor(PyObject *capsule)
{
	PostgammaPythonCopy *handle = PyCapsule_GetPointer(
		capsule, POSTGAMMA_COPY_CAPSULE);

	if (handle == NULL)
	{
		PyErr_Clear();
		return;
	}
	if (handle->owner_pid == getpid() && handle->copy != NULL)
	{
		pgm_error  *error = NULL;
		pgm_status	status;

		Py_BEGIN_ALLOW_THREADS
		(void) pgm_copy_abort(
			handle->copy, "Python COPY handle was abandoned",
			strlen("Python COPY handle was abandoned"), NULL);
		status = pgm_copy_close(handle->copy, PGM_NO_TIMEOUT, &error);
		Py_END_ALLOW_THREADS
		if (status == PGM_STATUS_OK)
			handle->copy = NULL;
		else
			warn_cleanup_failure(capsule, "COPY handle", status, error);
		pgm_error_free(error);
	}
	copy_release_owner(handle);
	(void) pthread_mutex_destroy(&handle->mutex);
	free(handle);
}


static PyObject *
request_capsule_create(
	PyObject *connection_capsule, PyObject *statement_capsule,
	pgm_request *request)
{
	PostgammaPythonRequest *handle;
	PostgammaPythonConnection *connection;
	PyObject   *capsule;

	handle = calloc(1, sizeof(*handle));
	if (handle == NULL)
		return PyErr_NoMemory();
	if (pthread_mutex_init(&handle->mutex, NULL) != 0)
	{
		free(handle);
		PyErr_SetString(PyExc_RuntimeError,
			"could not initialize the native PostGamma request lock");
		return NULL;
	}
	handle->request = request;
	handle->owner_pid = getpid();
	handle->connection_capsule = connection_capsule;
	Py_INCREF(connection_capsule);
	if (statement_capsule != NULL)
	{
		PostgammaPythonStatement *statement = PyCapsule_GetPointer(
			statement_capsule, POSTGAMMA_STATEMENT_CAPSULE);

		if (statement == NULL || mutex_lock(&statement->mutex) != 0)
			goto failure;
		statement->request_count++;
		mutex_unlock(&statement->mutex);
		handle->statement_capsule = statement_capsule;
		Py_INCREF(statement_capsule);
	}
	capsule = PyCapsule_New(
		handle, POSTGAMMA_REQUEST_CAPSULE, request_capsule_destructor);
	if (capsule != NULL)
		return capsule;

failure:
	connection = PyCapsule_GetPointer(
		connection_capsule, POSTGAMMA_CONNECTION_CAPSULE);
	if (connection != NULL)
		(void) retire_connection_request(connection, request);
	else
	{
		PyErr_Clear();
		pgm_request_free(request);
	}
	handle->request = NULL;
	request_release_owners(handle);
	(void) pthread_mutex_destroy(&handle->mutex);
	free(handle);
	return NULL;
}


static PyObject *
copy_capsule_create_locked(PyObject *request_capsule, pgm_copy *copy)
{
	/* The caller owns request->mutex while publishing the child handle. */
	PostgammaPythonRequest *request = request_from_capsule(
		request_capsule, true);
	PostgammaPythonCopy *handle;
	PyObject   *capsule;

	if (request == NULL)
		return NULL;
	handle = calloc(1, sizeof(*handle));
	if (handle == NULL)
		return PyErr_NoMemory();
	if (pthread_mutex_init(&handle->mutex, NULL) != 0)
	{
		free(handle);
		PyErr_SetString(PyExc_RuntimeError,
			"could not initialize the native PostGamma COPY lock");
		return NULL;
	}
	handle->copy = copy;
	handle->owner_pid = getpid();
	handle->request_capsule = request_capsule;
	Py_INCREF(request_capsule);
	request->copy_count++;
	capsule = PyCapsule_New(handle, POSTGAMMA_COPY_CAPSULE, copy_capsule_destructor);
	if (capsule != NULL)
		return capsule;
	request->copy_count--;
	Py_DECREF(request_capsule);
	(void) pthread_mutex_destroy(&handle->mutex);
	free(handle);
	return NULL;
}


static void
discard_copy_capsule_locked(
	PyObject *request_capsule, PyObject *copy_capsule)
{
	PostgammaPythonRequest *request = PyCapsule_GetPointer(
		request_capsule, POSTGAMMA_REQUEST_CAPSULE);
	PostgammaPythonCopy *copy = PyCapsule_GetPointer(
		copy_capsule, POSTGAMMA_COPY_CAPSULE);
	PyObject   *owner;

	if (request == NULL || copy == NULL)
	{
		PyErr_Clear();
		return;
	}
	owner = copy->request_capsule;
	copy->request_capsule = NULL;
	if (request->copy_count > 0)
		request->copy_count--;
	if (copy->copy != NULL)
	{
		Py_BEGIN_ALLOW_THREADS
		(void) pgm_copy_abort(
			copy->copy, "Python allocation failure",
			strlen("Python allocation failure"), NULL);
		(void) pgm_copy_close(copy->copy, PGM_NO_TIMEOUT, NULL);
		Py_END_ALLOW_THREADS
		copy->copy = NULL;
	}
	Py_XDECREF(owner);
}


static int
sql_from_python(PyObject *object, const char **sql, Py_ssize_t *sql_size)
{
	if (!PyUnicode_Check(object))
	{
		PyErr_SetString(PyExc_TypeError, "SQL must be str");
		return -1;
	}
	*sql = PyUnicode_AsUTF8AndSize(object, sql_size);
	if (*sql == NULL)
		return -1;
	if ((Py_ssize_t) strlen(*sql) != *sql_size)
	{
		PyErr_SetString(PyExc_ValueError, "SQL cannot contain a NUL byte");
		return -1;
	}
	return 0;
}


static int
type_oids_from_python(PyObject *object, uint32_t **type_oids, size_t *count)
{
	PyObject   *sequence;
	Py_ssize_t	length;

	*type_oids = NULL;
	*count = 0;
	if (object == NULL || object == Py_None)
		return 0;
	sequence = PySequence_Fast(object, "parameter_type_oids must be a sequence");
	if (sequence == NULL)
		return -1;
	length = PySequence_Fast_GET_SIZE(sequence);
	if (length < 0 || (size_t) length > SIZE_MAX / sizeof(uint32_t))
	{
		Py_DECREF(sequence);
		PyErr_SetString(PyExc_OverflowError, "too many parameter type OIDs");
		return -1;
	}
	if (length > 0)
	{
		*type_oids = calloc((size_t) length, sizeof(uint32_t));
		if (*type_oids == NULL)
		{
			Py_DECREF(sequence);
			PyErr_NoMemory();
			return -1;
		}
	}
	for (Py_ssize_t index = 0; index < length; index++)
	{
		unsigned long value = PyLong_AsUnsignedLong(
			PySequence_Fast_GET_ITEM(sequence, index));

		if (PyErr_Occurred() || value > UINT32_MAX)
		{
			free(*type_oids);
			*type_oids = NULL;
			Py_DECREF(sequence);
			if (!PyErr_Occurred())
				PyErr_SetString(PyExc_ValueError,
					"parameter type OID exceeds uint32");
			return -1;
		}
		(*type_oids)[index] = (uint32_t) value;
	}
	*count = (size_t) length;
	Py_DECREF(sequence);
	return 0;
}


static PyObject *
statement_column_to_python(const pgm_statement *statement, size_t index)
{
	pgm_column	column = PGM_COLUMN_INIT;
	pgm_error  *error = NULL;
	pgm_status	status = pgm_statement_column(statement, index, &column, &error);
	PyObject   *name;

	if (status != PGM_STATUS_OK)
	{
		raise_native_error(status, error);
		return NULL;
	}
	name = optional_text(column.name);
	if (name == NULL)
		return NULL;
	return Py_BuildValue(
		"(NIiIiiI)", name, (unsigned int) column.table_oid,
		(int) column.table_column, (unsigned int) column.type_oid,
		(int) column.type_size, (int) column.type_modifier,
		(unsigned int) column.format);
}


static PyObject *
request_result_to_python(
	PyObject *request_capsule, pgm_result **result, pgm_error **error)
{
	pgm_result_status kind = pgm_result_kind(*result);
	PyObject   *document;

	if (kind == PGM_RESULT_ERROR)
	{
		pgm_status	status = *error != NULL ?
			pgm_error_status(*error) : PGM_STATUS_POSTGRES_ERROR;

		pgm_result_free(*result);
		*result = NULL;
		raise_native_error(status, *error);
		*error = NULL;
		return NULL;
	}
	document = result_to_python(*result);
	if (document == NULL)
		return NULL;
	if (kind == PGM_RESULT_COPY_IN || kind == PGM_RESULT_COPY_OUT)
	{
		pgm_copy   *copy = NULL;
		pgm_error  *copy_error = NULL;
		pgm_status	status = pgm_result_take_copy(result, &copy, &copy_error);
		PyObject   *copy_capsule;

		if (status != PGM_STATUS_OK)
		{
			Py_DECREF(document);
			raise_native_error(status, copy_error);
			return NULL;
		}
		copy_capsule = copy_capsule_create_locked(request_capsule, copy);
		if (copy_capsule == NULL)
		{
			(void) pgm_copy_abort(copy, "Python allocation failure",
				strlen("Python allocation failure"), NULL);
			(void) pgm_copy_close(copy, PGM_NO_TIMEOUT, NULL);
			Py_DECREF(document);
			return NULL;
		}
		if (PyDict_SetItemString(document, "copy", copy_capsule) != 0)
		{
			discard_copy_capsule_locked(request_capsule, copy_capsule);
			Py_DECREF(copy_capsule);
			Py_DECREF(document);
			return NULL;
		}
		Py_DECREF(copy_capsule);
	}
	else
	{
		pgm_result_free(*result);
		*result = NULL;
	}
	return document;
}


static PyObject *
native_request_start(PyObject *self, PyObject *args, PyObject *kwargs)
{
	static char *keywords[] = {
		"connection", "sql", "parameters", "chunk_rows", "result_format",
		"result_buffer_limit", "script", NULL
	};
	PyObject   *connection_capsule;
	PyObject   *sql_object;
	PyObject   *parameters_object = Py_None;
	unsigned int chunk_rows = 0;
	unsigned int result_format = PGM_FORMAT_TEXT;
	unsigned long long result_buffer_limit = 0;
	int			script = 0;
	PostgammaPythonConnection *connection;
	PostgammaPythonParameters parameters;
	const char *sql;
	Py_ssize_t	sql_size;
	pgm_request *request = NULL;
	pgm_error  *error = NULL;
	pgm_status	status;

	(void) self;
	if (!PyArg_ParseTupleAndKeywords(
			args, kwargs, "OO|OIIKp:request_start", keywords,
			&connection_capsule, &sql_object, &parameters_object, &chunk_rows,
			&result_format, &result_buffer_limit, &script))
		return NULL;
	if (result_format > PGM_FORMAT_BINARY ||
		result_buffer_limit > (unsigned long long) SIZE_MAX)
	{
		PyErr_SetString(PyExc_ValueError, "invalid request delivery options");
		return NULL;
	}
	if (sql_from_python(sql_object, &sql, &sql_size) != 0 ||
		parameters_from_python(parameters_object, &parameters) != 0)
		return NULL;
	if (script && (parameters.count != 0 || result_format != PGM_FORMAT_TEXT))
	{
		parameters_free(&parameters);
		PyErr_SetString(PyExc_ValueError,
			"script requests do not accept parameters or binary results");
		return NULL;
	}
	connection = connection_from_capsule(connection_capsule, true);
	if (connection == NULL)
	{
		parameters_free(&parameters);
		return NULL;
	}
	if (mutex_lock(&connection->mutex) != 0)
	{
		parameters_free(&parameters);
		return NULL;
	}
	if (connection->active_request != NULL)
	{
		mutex_unlock(&connection->mutex);
		parameters_free(&parameters);
		raise_simple_error(PGM_STATUS_BUSY,
			"PostGamma connection already has an active request");
		return NULL;
	}
	Py_BEGIN_ALLOW_THREADS
	if (script)
	{
		pgm_script_options options = PGM_SCRIPT_OPTIONS_INIT;

		options.delivery_mode = chunk_rows == 0 ?
			PGM_DELIVERY_MATERIALIZED : PGM_DELIVERY_CHUNKED;
		options.target_chunk_rows = chunk_rows;
		options.result_buffer_limit = (size_t) result_buffer_limit;
		status = pgm_execute_script_async(
			connection->connection, sql, (size_t) sql_size, &options,
			&request, &error);
	}
	else
	{
		pgm_execute_options options = PGM_EXECUTE_OPTIONS_INIT;

		options.parameters = parameters.parameters;
		options.parameter_count = parameters.count;
		options.result_format = (uint16_t) result_format;
		options.delivery_mode = chunk_rows == 0 ?
			PGM_DELIVERY_MATERIALIZED : PGM_DELIVERY_CHUNKED;
		options.target_chunk_rows = chunk_rows;
		options.result_buffer_limit = (size_t) result_buffer_limit;
		status = pgm_execute_async_ex(
			connection->connection, sql, (size_t) sql_size, &options,
			&request, &error);
	}
	Py_END_ALLOW_THREADS
	parameters_free(&parameters);
	if (status != PGM_STATUS_OK)
	{
		mutex_unlock(&connection->mutex);
		raise_native_error(status, error);
		return NULL;
	}
	connection->active_request = request;
	mutex_unlock(&connection->mutex);
	return request_capsule_create(connection_capsule, NULL, request);
}


static PyObject *
native_request_next(PyObject *self, PyObject *args, PyObject *kwargs)
{
	static char *keywords[] = {"request", "timeout_ms", NULL};
	PyObject   *capsule;
	long long	timeout_ms = 0;
	PostgammaPythonRequest *handle;
	pgm_result *result = NULL;
	pgm_error  *error = NULL;
	pgm_availability availability = PGM_AVAILABILITY_AGAIN;
	pgm_status	status;
	PyObject   *document = NULL;
	PyObject   *answer;

	(void) self;
	if (!PyArg_ParseTupleAndKeywords(
			args, kwargs, "O|L:request_next", keywords, &capsule, &timeout_ms))
		return NULL;
	if (timeout_ms < PGM_NO_TIMEOUT)
	{
		PyErr_SetString(PyExc_ValueError, "invalid request timeout");
		return NULL;
	}
	handle = request_from_capsule(capsule, true);
	if (handle == NULL || mutex_lock(&handle->mutex) != 0)
		return NULL;
	Py_BEGIN_ALLOW_THREADS
	status = pgm_request_next_result(
		handle->request, (int64_t) timeout_ms, &result, &availability, &error);
	Py_END_ALLOW_THREADS
	if (status != PGM_STATUS_OK)
	{
		mutex_unlock(&handle->mutex);
		raise_native_error(status, error);
		return NULL;
	}
	if (availability == PGM_AVAILABILITY_READY)
		document = request_result_to_python(capsule, &result, &error);
	else
	{
		Py_INCREF(Py_None);
		document = Py_None;
	}
	pgm_result_free(result);
	pgm_error_free(error);
	mutex_unlock(&handle->mutex);
	if (document == NULL)
		return NULL;
	answer = Py_BuildValue("(iN)", (int) availability, document);
	return answer;
}


static PyObject *
native_request_state(PyObject *self, PyObject *capsule, bool progress)
{
	PostgammaPythonRequest *handle = request_from_capsule(capsule, true);
	pgm_request_state state = PGM_REQUEST_PENDING;
	pgm_error  *error = NULL;
	pgm_status	status;

	(void) self;
	if (handle == NULL || mutex_lock(&handle->mutex) != 0)
		return NULL;
	Py_BEGIN_ALLOW_THREADS
	status = progress ?
		pgm_request_progress(handle->request, &state, &error) :
		pgm_request_poll(handle->request, &state, &error);
	Py_END_ALLOW_THREADS
	mutex_unlock(&handle->mutex);
	if (status != PGM_STATUS_OK)
	{
		raise_native_error(status, error);
		return NULL;
	}
	return PyLong_FromLong((long) state);
}


static PyObject *
native_request_poll(PyObject *self, PyObject *capsule)
{
	return native_request_state(self, capsule, false);
}


static PyObject *
native_request_progress(PyObject *self, PyObject *capsule)
{
	return native_request_state(self, capsule, true);
}


static PyObject *
native_request_waitable(PyObject *self, PyObject *capsule)
{
	PostgammaPythonRequest *handle = request_from_capsule(capsule, true);
	pgm_error  *error = NULL;
	pgm_status	status;
	int			descriptor = -1;

	(void) self;
	if (handle == NULL || mutex_lock(&handle->mutex) != 0)
		return NULL;
	status = pgm_request_waitable(handle->request, &descriptor, &error);
	mutex_unlock(&handle->mutex);
	if (status != PGM_STATUS_OK)
	{
		raise_native_error(status, error);
		return NULL;
	}
	return PyLong_FromLong(descriptor);
}


static PyObject *
native_request_cancel(PyObject *self, PyObject *capsule)
{
	PostgammaPythonRequest *handle = request_from_capsule(capsule, true);
	PostgammaPythonConnection *connection;
	pgm_error  *error = NULL;
	pgm_status	status;

	(void) self;
	if (handle == NULL)
		return NULL;
	connection = connection_from_capsule(handle->connection_capsule, true);
	if (connection == NULL || mutex_lock(&connection->mutex) != 0)
		return NULL;
	if (connection->active_request != handle->request)
	{
		mutex_unlock(&connection->mutex);
		Py_RETURN_FALSE;
	}
	Py_BEGIN_ALLOW_THREADS
	status = pgm_request_cancel(handle->request, &error);
	Py_END_ALLOW_THREADS
	mutex_unlock(&connection->mutex);
	if (status != PGM_STATUS_OK)
	{
		raise_native_error(status, error);
		return NULL;
	}
	Py_RETURN_TRUE;
}


static PyObject *
native_request_identity(PyObject *self, PyObject *capsule)
{
	PostgammaPythonRequest *handle = request_from_capsule(capsule, true);
	pgm_request_id identity;

	(void) self;
	if (handle == NULL || mutex_lock(&handle->mutex) != 0)
		return NULL;
	identity = pgm_request_identity(handle->request);
	mutex_unlock(&handle->mutex);
	return PyLong_FromUnsignedLongLong((unsigned long long) identity);
}


static PyObject *
native_request_close(PyObject *self, PyObject *capsule)
{
	PostgammaPythonRequest *handle = request_from_capsule(capsule, false);
	PostgammaPythonConnection *connection;

	(void) self;
	if (handle == NULL || mutex_lock(&handle->mutex) != 0)
		return NULL;
	if (handle->request == NULL)
	{
		mutex_unlock(&handle->mutex);
		Py_RETURN_NONE;
	}
	if (handle->copy_count != 0)
	{
		mutex_unlock(&handle->mutex);
		raise_simple_error(PGM_STATUS_BUSY,
			"close the active COPY handle before closing its request");
		return NULL;
	}
	connection = connection_from_capsule(handle->connection_capsule, false);
	if (connection == NULL)
	{
		mutex_unlock(&handle->mutex);
		return NULL;
	}
	if (retire_connection_request(connection, handle->request) != 0)
	{
		mutex_unlock(&handle->mutex);
		return NULL;
	}
	handle->request = NULL;
	mutex_unlock(&handle->mutex);
	request_release_owners(handle);
	Py_RETURN_NONE;
}


static PyObject *
native_statement_prepare(PyObject *self, PyObject *args, PyObject *kwargs)
{
	static char *keywords[] = {
		"connection", "sql", "parameter_type_oids", NULL
	};
	PyObject   *connection_capsule;
	PyObject   *sql_object;
	PyObject   *types_object = Py_None;
	PostgammaPythonConnection *connection;
	PostgammaPythonStatement *handle;
	pgm_prepare_options options = PGM_PREPARE_OPTIONS_INIT;
	uint32_t   *type_oids = NULL;
	size_t		type_count = 0;
	const char *sql;
	Py_ssize_t	sql_size;
	pgm_error  *error = NULL;
	pgm_status	status;
	PyObject   *capsule;

	(void) self;
	if (!PyArg_ParseTupleAndKeywords(
			args, kwargs, "OO|O:statement_prepare", keywords,
			&connection_capsule, &sql_object, &types_object))
		return NULL;
	if (sql_from_python(sql_object, &sql, &sql_size) != 0 ||
		type_oids_from_python(types_object, &type_oids, &type_count) != 0)
		return NULL;
	connection = connection_from_capsule(connection_capsule, true);
	if (connection == NULL || mutex_lock(&connection->mutex) != 0)
	{
		free(type_oids);
		return NULL;
	}
	if (connection->active_request != NULL)
	{
		mutex_unlock(&connection->mutex);
		free(type_oids);
		raise_simple_error(PGM_STATUS_BUSY,
			"cannot prepare while another request is active");
		return NULL;
	}
	handle = calloc(1, sizeof(*handle));
	if (handle == NULL)
	{
		mutex_unlock(&connection->mutex);
		free(type_oids);
		return PyErr_NoMemory();
	}
	if (pthread_mutex_init(&handle->mutex, NULL) != 0)
	{
		mutex_unlock(&connection->mutex);
		free(type_oids);
		free(handle);
		PyErr_SetString(PyExc_RuntimeError,
			"could not initialize the native PostGamma statement lock");
		return NULL;
	}
	options.parameter_type_oids = type_oids;
	options.parameter_count = type_count;
	Py_BEGIN_ALLOW_THREADS
	status = pgm_statement_prepare(
		connection->connection, sql, (size_t) sql_size, &options,
		&handle->statement, &error);
	Py_END_ALLOW_THREADS
	free(type_oids);
	if (status == PGM_STATUS_OK)
		connection->statement_count++;
	mutex_unlock(&connection->mutex);
	if (status != PGM_STATUS_OK)
	{
		(void) pthread_mutex_destroy(&handle->mutex);
		free(handle);
		raise_native_error(status, error);
		return NULL;
	}
	handle->owner_pid = getpid();
	handle->connection_capsule = connection_capsule;
	Py_INCREF(connection_capsule);
	capsule = PyCapsule_New(
		handle, POSTGAMMA_STATEMENT_CAPSULE, statement_capsule_destructor);
	if (capsule != NULL)
		return capsule;
	(void) pgm_statement_close(handle->statement, NULL);
	handle->statement = NULL;
	statement_release_owner(handle);
	(void) pthread_mutex_destroy(&handle->mutex);
	free(handle);
	return NULL;
}


static PyObject *
native_statement_describe(PyObject *self, PyObject *capsule)
{
	PostgammaPythonStatement *handle = statement_from_capsule(capsule, true);
	pgm_statement_description description = PGM_STATEMENT_DESCRIPTION_INIT;
	pgm_error  *error = NULL;
	pgm_status	status;
	PyObject   *types;
	PyObject   *columns;
	PyObject   *document;

	(void) self;
	if (handle == NULL || mutex_lock(&handle->mutex) != 0)
		return NULL;
	status = pgm_statement_describe(handle->statement, &description, &error);
	if (status != PGM_STATUS_OK)
	{
		mutex_unlock(&handle->mutex);
		raise_native_error(status, error);
		return NULL;
	}
	if (description.parameter_count > (size_t) PY_SSIZE_T_MAX ||
		description.column_count > (size_t) PY_SSIZE_T_MAX)
	{
		mutex_unlock(&handle->mutex);
		PyErr_SetString(PyExc_OverflowError,
			"statement description exceeds Python's addressable size");
		return NULL;
	}
	types = PyTuple_New((Py_ssize_t) description.parameter_count);
	columns = PyTuple_New((Py_ssize_t) description.column_count);
	if (types == NULL || columns == NULL)
		goto failure;
	for (size_t index = 0; index < description.parameter_count; index++)
	{
		uint32_t	oid = 0;

		status = pgm_statement_parameter_type(
			handle->statement, index, &oid, &error);
		if (status != PGM_STATUS_OK)
		{
			raise_native_error(status, error);
			error = NULL;
			goto failure;
		}
		{
			PyObject   *type_oid = PyLong_FromUnsignedLong((unsigned long) oid);

			if (type_oid == NULL)
				goto failure;
			PyTuple_SET_ITEM(types, (Py_ssize_t) index, type_oid);
		}
	}
	for (size_t index = 0; index < description.column_count; index++)
	{
		PyObject   *column = statement_column_to_python(handle->statement, index);

		if (column == NULL)
			goto failure;
		PyTuple_SET_ITEM(columns, (Py_ssize_t) index, column);
	}
	mutex_unlock(&handle->mutex);
	document = Py_BuildValue("{s:N,s:N}",
		"parameter_type_oids", types, "columns", columns);
	return document;

failure:
	mutex_unlock(&handle->mutex);
	Py_XDECREF(types);
	Py_XDECREF(columns);
	return NULL;
}


static PyObject *
native_statement_execute(PyObject *self, PyObject *args, PyObject *kwargs)
{
	static char *keywords[] = {
		"statement", "parameters", "chunk_rows", "result_format",
		"result_buffer_limit", NULL
	};
	PyObject   *statement_capsule;
	PyObject   *parameters_object = Py_None;
	unsigned int chunk_rows = 0;
	unsigned int result_format = PGM_FORMAT_TEXT;
	unsigned long long result_buffer_limit = 0;
	PostgammaPythonStatement *statement;
	PostgammaPythonConnection *connection;
	PostgammaPythonParameters parameters;
	pgm_execute_options options = PGM_EXECUTE_OPTIONS_INIT;
	pgm_request *request = NULL;
	pgm_error  *error = NULL;
	pgm_status	status;

	(void) self;
	if (!PyArg_ParseTupleAndKeywords(
			args, kwargs, "O|OIIK:statement_execute", keywords,
			&statement_capsule, &parameters_object, &chunk_rows,
			&result_format, &result_buffer_limit))
		return NULL;
	if (result_format > PGM_FORMAT_BINARY ||
		result_buffer_limit > (unsigned long long) SIZE_MAX)
	{
		PyErr_SetString(PyExc_ValueError, "invalid statement delivery options");
		return NULL;
	}
	statement = statement_from_capsule(statement_capsule, true);
	if (statement == NULL ||
		parameters_from_python(parameters_object, &parameters) != 0)
		return NULL;
	connection = connection_from_capsule(statement->connection_capsule, true);
	if (connection == NULL || mutex_lock(&connection->mutex) != 0)
	{
		parameters_free(&parameters);
		return NULL;
	}
	if (connection->active_request != NULL)
	{
		mutex_unlock(&connection->mutex);
		parameters_free(&parameters);
		raise_simple_error(PGM_STATUS_BUSY,
			"PostGamma connection already has an active request");
		return NULL;
	}
	options.parameters = parameters.parameters;
	options.parameter_count = parameters.count;
	options.result_format = (uint16_t) result_format;
	options.delivery_mode = chunk_rows == 0 ?
		PGM_DELIVERY_MATERIALIZED : PGM_DELIVERY_CHUNKED;
	options.target_chunk_rows = chunk_rows;
	options.result_buffer_limit = (size_t) result_buffer_limit;
	Py_BEGIN_ALLOW_THREADS
	status = pgm_statement_execute_async(
		statement->statement, &options, &request, &error);
	Py_END_ALLOW_THREADS
	parameters_free(&parameters);
	if (status != PGM_STATUS_OK)
	{
		mutex_unlock(&connection->mutex);
		raise_native_error(status, error);
		return NULL;
	}
	connection->active_request = request;
	mutex_unlock(&connection->mutex);
	return request_capsule_create(
		statement->connection_capsule, statement_capsule, request);
}


static PyObject *
native_statement_close(PyObject *self, PyObject *capsule)
{
	PostgammaPythonStatement *handle = statement_from_capsule(capsule, false);
	pgm_error  *error = NULL;
	pgm_status	status;

	(void) self;
	if (handle == NULL || mutex_lock(&handle->mutex) != 0)
		return NULL;
	if (handle->statement == NULL)
	{
		mutex_unlock(&handle->mutex);
		Py_RETURN_NONE;
	}
	if (handle->request_count != 0)
	{
		mutex_unlock(&handle->mutex);
		raise_simple_error(PGM_STATUS_BUSY,
			"close all statement requests before closing the statement");
		return NULL;
	}
	Py_BEGIN_ALLOW_THREADS
	status = pgm_statement_close(handle->statement, &error);
	Py_END_ALLOW_THREADS
	if (status == PGM_STATUS_OK)
		handle->statement = NULL;
	mutex_unlock(&handle->mutex);
	if (status != PGM_STATUS_OK)
	{
		raise_native_error(status, error);
		return NULL;
	}
	statement_release_owner(handle);
	Py_RETURN_NONE;
}


static PyObject *
native_copy_write(PyObject *self, PyObject *args)
{
	PyObject   *capsule;
	PyObject   *data_object;
	PostgammaPythonCopy *handle;
	Py_buffer	data;
	size_t		consumed = 0;
	pgm_io_state state = PGM_IO_AGAIN;
	pgm_error  *error = NULL;
	pgm_status	status;

	(void) self;
	if (!PyArg_ParseTuple(args, "OO:copy_write", &capsule, &data_object))
		return NULL;
	if (PyObject_GetBuffer(data_object, &data, PyBUF_SIMPLE) != 0)
		return NULL;
	handle = copy_from_capsule(capsule, true);
	if (handle == NULL || mutex_lock(&handle->mutex) != 0)
	{
		PyBuffer_Release(&data);
		return NULL;
	}
	Py_BEGIN_ALLOW_THREADS
	status = pgm_copy_write(
		handle->copy, data.buf, (size_t) data.len, &consumed, &state, &error);
	Py_END_ALLOW_THREADS
	mutex_unlock(&handle->mutex);
	PyBuffer_Release(&data);
	if (status != PGM_STATUS_OK)
	{
		raise_native_error(status, error);
		return NULL;
	}
	return Py_BuildValue("(Ki)", (unsigned long long) consumed, (int) state);
}


static PyObject *
native_copy_finish(PyObject *self, PyObject *args, PyObject *kwargs)
{
	static char *keywords[] = {"copy", "failure_message", NULL};
	PyObject   *capsule;
	PyObject   *message_object = Py_None;
	PostgammaPythonCopy *handle;
	const char *message = NULL;
	Py_ssize_t	message_size = 0;
	pgm_io_state state = PGM_IO_AGAIN;
	pgm_error  *error = NULL;
	pgm_status	status;

	(void) self;
	if (!PyArg_ParseTupleAndKeywords(
			args, kwargs, "O|O:copy_finish", keywords,
			&capsule, &message_object))
		return NULL;
	if (message_object != Py_None &&
		sql_from_python(message_object, &message, &message_size) != 0)
		return NULL;
	handle = copy_from_capsule(capsule, true);
	if (handle == NULL || mutex_lock(&handle->mutex) != 0)
		return NULL;
	Py_BEGIN_ALLOW_THREADS
	status = pgm_copy_finish(
		handle->copy, message, (size_t) message_size, &state, &error);
	Py_END_ALLOW_THREADS
	mutex_unlock(&handle->mutex);
	if (status != PGM_STATUS_OK)
	{
		raise_native_error(status, error);
		return NULL;
	}
	return PyLong_FromLong((long) state);
}


static PyObject *
native_copy_read(PyObject *self, PyObject *args, PyObject *kwargs)
{
	static char *keywords[] = {"copy", "capacity", NULL};
	PyObject   *capsule;
	Py_ssize_t	capacity = 65536;
	PostgammaPythonCopy *handle;
	char	   *buffer;
	size_t		produced = 0;
	pgm_io_state state = PGM_IO_AGAIN;
	pgm_error  *error = NULL;
	pgm_status	status;
	PyObject   *data;

	(void) self;
	if (!PyArg_ParseTupleAndKeywords(
			args, kwargs, "O|n:copy_read", keywords, &capsule, &capacity))
		return NULL;
	if (capacity <= 0)
	{
		PyErr_SetString(PyExc_ValueError, "COPY read capacity must be positive");
		return NULL;
	}
	buffer = PyMem_Malloc((size_t) capacity);
	if (buffer == NULL)
		return PyErr_NoMemory();
	handle = copy_from_capsule(capsule, true);
	if (handle == NULL || mutex_lock(&handle->mutex) != 0)
	{
		PyMem_Free(buffer);
		return NULL;
	}
	Py_BEGIN_ALLOW_THREADS
	status = pgm_copy_read(
		handle->copy, buffer, (size_t) capacity, &produced, &state, &error);
	Py_END_ALLOW_THREADS
	mutex_unlock(&handle->mutex);
	if (status != PGM_STATUS_OK)
	{
		PyMem_Free(buffer);
		raise_native_error(status, error);
		return NULL;
	}
	data = PyBytes_FromStringAndSize(buffer, (Py_ssize_t) produced);
	PyMem_Free(buffer);
	if (data == NULL)
		return NULL;
	return Py_BuildValue("(Ni)", data, (int) state);
}


static PyObject *
native_copy_abort(PyObject *self, PyObject *args, PyObject *kwargs)
{
	static char *keywords[] = {"copy", "message", NULL};
	PyObject   *capsule;
	PyObject   *message_object = Py_None;
	PostgammaPythonCopy *handle;
	const char *message = NULL;
	Py_ssize_t	message_size = 0;
	pgm_error  *error = NULL;
	pgm_status	status;

	(void) self;
	if (!PyArg_ParseTupleAndKeywords(
			args, kwargs, "O|O:copy_abort", keywords,
			&capsule, &message_object))
		return NULL;
	if (message_object != Py_None &&
		sql_from_python(message_object, &message, &message_size) != 0)
		return NULL;
	handle = copy_from_capsule(capsule, true);
	if (handle == NULL || mutex_lock(&handle->mutex) != 0)
		return NULL;
	Py_BEGIN_ALLOW_THREADS
	status = pgm_copy_abort(
		handle->copy, message, (size_t) message_size, &error);
	Py_END_ALLOW_THREADS
	mutex_unlock(&handle->mutex);
	if (status != PGM_STATUS_OK)
	{
		raise_native_error(status, error);
		return NULL;
	}
	Py_RETURN_NONE;
}


static PyObject *
native_copy_close(PyObject *self, PyObject *args, PyObject *kwargs)
{
	static char *keywords[] = {"copy", "timeout_ms", NULL};
	PyObject   *capsule;
	long long	timeout_ms = 30000;
	PostgammaPythonCopy *handle;
	pgm_error  *error = NULL;
	pgm_status	status;

	(void) self;
	if (!PyArg_ParseTupleAndKeywords(
			args, kwargs, "O|L:copy_close", keywords, &capsule, &timeout_ms))
		return NULL;
	if (timeout_ms < PGM_NO_TIMEOUT)
	{
		PyErr_SetString(PyExc_ValueError, "invalid COPY close timeout");
		return NULL;
	}
	handle = copy_from_capsule(capsule, false);
	if (handle == NULL || mutex_lock(&handle->mutex) != 0)
		return NULL;
	if (handle->copy == NULL)
	{
		mutex_unlock(&handle->mutex);
		Py_RETURN_NONE;
	}
	Py_BEGIN_ALLOW_THREADS
	status = pgm_copy_close(handle->copy, (int64_t) timeout_ms, &error);
	Py_END_ALLOW_THREADS
	if (status == PGM_STATUS_OK)
		handle->copy = NULL;
	mutex_unlock(&handle->mutex);
	if (status != PGM_STATUS_OK)
	{
		raise_native_error(status, error);
		return NULL;
	}
	copy_release_owner(handle);
	Py_RETURN_NONE;
}


static PyMethodDef postgamma_request_methods[] = {
	{"request_start", _PyCFunction_CAST(native_request_start),
		METH_VARARGS | METH_KEYWORDS, "Start a caller-driven request."},
	{"request_next", _PyCFunction_CAST(native_request_next),
		METH_VARARGS | METH_KEYWORDS, "Take the next ordered request result."},
	{"request_poll", native_request_poll, METH_O,
		"Return a request state without advancing it."},
	{"request_progress", native_request_progress, METH_O,
		"Advance a caller-driven request without waiting."},
	{"request_waitable", native_request_waitable, METH_O,
		"Return the level-triggered request waitable descriptor."},
	{"request_cancel", native_request_cancel, METH_O,
		"Cancel a caller-driven request."},
	{"request_identity", native_request_identity, METH_O,
		"Return the request identifier."},
	{"request_close", native_request_close, METH_O,
		"Release a request after closing any COPY child."},
	{"statement_prepare", _PyCFunction_CAST(native_statement_prepare),
		METH_VARARGS | METH_KEYWORDS, "Prepare one statement."},
	{"statement_describe", native_statement_describe, METH_O,
		"Describe a prepared statement."},
	{"statement_execute", _PyCFunction_CAST(native_statement_execute),
		METH_VARARGS | METH_KEYWORDS, "Execute a prepared statement."},
	{"statement_close", native_statement_close, METH_O,
		"Close a prepared statement."},
	{"copy_write", native_copy_write, METH_VARARGS,
		"Write one partial COPY IN buffer."},
	{"copy_finish", _PyCFunction_CAST(native_copy_finish),
		METH_VARARGS | METH_KEYWORDS, "Finish COPY IN."},
	{"copy_read", _PyCFunction_CAST(native_copy_read),
		METH_VARARGS | METH_KEYWORDS, "Read one partial COPY OUT buffer."},
	{"copy_abort", _PyCFunction_CAST(native_copy_abort),
		METH_VARARGS | METH_KEYWORDS, "Abort COPY."},
	{"copy_close", _PyCFunction_CAST(native_copy_close),
		METH_VARARGS | METH_KEYWORDS, "Close COPY and release its request."},
	{NULL, NULL, 0, NULL}
};


int
postgamma_add_request_methods(PyObject *module)
{
	if (PyModule_AddFunctions(module, postgamma_request_methods) != 0)
		return -1;
	if (PyModule_AddIntConstant(
			module, "AVAILABILITY_READY", PGM_AVAILABILITY_READY) != 0 ||
		PyModule_AddIntConstant(
			module, "AVAILABILITY_AGAIN", PGM_AVAILABILITY_AGAIN) != 0 ||
		PyModule_AddIntConstant(
			module, "AVAILABILITY_END", PGM_AVAILABILITY_END) != 0 ||
		PyModule_AddIntConstant(module, "IO_PROGRESS", PGM_IO_PROGRESS) != 0 ||
		PyModule_AddIntConstant(module, "IO_AGAIN", PGM_IO_AGAIN) != 0 ||
		PyModule_AddIntConstant(module, "IO_END", PGM_IO_END) != 0 ||
		PyModule_AddIntConstant(
			module, "REQUEST_PENDING", PGM_REQUEST_PENDING) != 0 ||
		PyModule_AddIntConstant(
			module, "REQUEST_RUNNING", PGM_REQUEST_RUNNING) != 0 ||
		PyModule_AddIntConstant(
			module, "REQUEST_COMPLETED", PGM_REQUEST_COMPLETED) != 0 ||
		PyModule_AddIntConstant(
			module, "REQUEST_CANCELED", PGM_REQUEST_CANCELED) != 0 ||
		PyModule_AddIntConstant(
			module, "REQUEST_FAILED", PGM_REQUEST_FAILED) != 0 ||
		PyModule_AddIntConstant(module, "RESULT_COPY_IN", PGM_RESULT_COPY_IN) != 0 ||
		PyModule_AddIntConstant(
			module, "RESULT_COPY_OUT", PGM_RESULT_COPY_OUT) != 0 ||
		PyModule_AddIntConstant(
			module, "RESULT_TUPLES_CHUNK", PGM_RESULT_TUPLES_CHUNK) != 0 ||
		PyModule_AddIntConstant(module, "FORMAT_TEXT", PGM_FORMAT_TEXT) != 0 ||
		PyModule_AddIntConstant(module, "FORMAT_BINARY", PGM_FORMAT_BINARY) != 0)
		return -1;
	return 0;
}
