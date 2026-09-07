/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

#include "_native_internal.h"

#include <postgamma/postgamma_arrow.h>

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>


/* Independent consumer-side Arrow C Data definitions. */
struct ArrowSchema
{
	const char *format;
	const char *name;
	const char *metadata;
	int64_t		flags;
	int64_t		n_children;
	struct ArrowSchema **children;
	struct ArrowSchema *dictionary;
	void		(*release) (struct ArrowSchema *);
	void	   *private_data;
};


struct ArrowArray
{
	int64_t		length;
	int64_t		null_count;
	int64_t		offset;
	int64_t		n_buffers;
	int64_t		n_children;
	const void **buffers;
	struct ArrowArray **children;
	struct ArrowArray *dictionary;
	void		(*release) (struct ArrowArray *);
	void	   *private_data;
};


#define POSTGAMMA_ARROW_SCHEMA_CAPSULE "arrow_schema"
#define POSTGAMMA_ARROW_ARRAY_CAPSULE "arrow_array"
#define POSTGAMMA_USED_ARROW_SCHEMA_CAPSULE "used_arrow_schema"
#define POSTGAMMA_USED_ARROW_ARRAY_CAPSULE "used_arrow_array"


static void
arrow_schema_capsule_destructor(PyObject *capsule)
{
	struct ArrowSchema *schema = NULL;

	if (PyCapsule_IsValid(capsule, POSTGAMMA_ARROW_SCHEMA_CAPSULE))
	{
		schema = PyCapsule_GetPointer(capsule, POSTGAMMA_ARROW_SCHEMA_CAPSULE);
		if (schema != NULL && schema->release != NULL)
			schema->release(schema);
	}
	else if (PyCapsule_IsValid(capsule, POSTGAMMA_USED_ARROW_SCHEMA_CAPSULE))
		schema = PyCapsule_GetPointer(
			capsule, POSTGAMMA_USED_ARROW_SCHEMA_CAPSULE);
	if (schema == NULL)
		PyErr_Clear();
	free(schema);
}


static void
arrow_array_capsule_destructor(PyObject *capsule)
{
	struct ArrowArray *array = NULL;

	if (PyCapsule_IsValid(capsule, POSTGAMMA_ARROW_ARRAY_CAPSULE))
	{
		array = PyCapsule_GetPointer(capsule, POSTGAMMA_ARROW_ARRAY_CAPSULE);
		if (array != NULL && array->release != NULL)
			array->release(array);
	}
	else if (PyCapsule_IsValid(capsule, POSTGAMMA_USED_ARROW_ARRAY_CAPSULE))
		array = PyCapsule_GetPointer(capsule, POSTGAMMA_USED_ARROW_ARRAY_CAPSULE);
	if (array == NULL)
		PyErr_Clear();
	free(array);
}


static PyObject *
arrow_capsules_from_result(const pgm_result *result)
{
	struct ArrowSchema *schema = calloc(1, sizeof(*schema));
	struct ArrowArray *array = calloc(1, sizeof(*array));
	pgm_error  *error = NULL;
	pgm_status	status;
	PyObject   *schema_capsule;
	PyObject   *array_capsule;
	PyObject   *answer;

	if (schema == NULL || array == NULL)
	{
		free(schema);
		free(array);
		return PyErr_NoMemory();
	}
	Py_BEGIN_ALLOW_THREADS
	status = pgm_result_export_arrow(result, schema, array, &error);
	Py_END_ALLOW_THREADS
	if (status != PGM_STATUS_OK)
	{
		if (array->release != NULL)
			array->release(array);
		if (schema->release != NULL)
			schema->release(schema);
		free(array);
		free(schema);
		raise_native_error(status, error);
		return NULL;
	}
	schema_capsule = PyCapsule_New(
		schema, POSTGAMMA_ARROW_SCHEMA_CAPSULE,
		arrow_schema_capsule_destructor);
	if (schema_capsule == NULL)
	{
		if (array->release != NULL)
			array->release(array);
		free(array);
		if (schema->release != NULL)
			schema->release(schema);
		free(schema);
		return NULL;
	}
	array_capsule = PyCapsule_New(
		array, POSTGAMMA_ARROW_ARRAY_CAPSULE,
		arrow_array_capsule_destructor);
	if (array_capsule == NULL)
	{
		if (array->release != NULL)
			array->release(array);
		free(array);
		Py_DECREF(schema_capsule);
		return NULL;
	}
	answer = PyTuple_Pack(2, schema_capsule, array_capsule);
	Py_DECREF(schema_capsule);
	Py_DECREF(array_capsule);
	return answer;
}


static PyObject *
native_connection_execute_arrow(PyObject *self, PyObject *args, PyObject *kwargs)
{
	static char *keywords[] = {
		"connection", "sql", "parameters", "timeout_ms", "result_format", NULL
	};
	PyObject   *capsule;
	PyObject   *sql_object;
	PyObject   *parameters_object = Py_None;
	long long	timeout_ms = 30000;
	unsigned int result_format = PGM_FORMAT_BINARY;
	PostgammaPythonConnection *handle = NULL;
	PostgammaPythonParameters parameters;
	const char *sql;
	Py_ssize_t	sql_size;
	pgm_request *request = NULL;
	pgm_result *result = NULL;
	pgm_error  *error = NULL;
	pgm_status	status = PGM_STATUS_INTERNAL_ERROR;
	uint64_t	started_ms;
	bool		python_signal = false;
	PyObject   *converted = NULL;

	(void) self;
	memset(&parameters, 0, sizeof(parameters));
	if (!PyArg_ParseTupleAndKeywords(
			args, kwargs, "OO|OLI:connection_execute_arrow", keywords,
			&capsule, &sql_object, &parameters_object, &timeout_ms,
			&result_format))
		return NULL;
	if (timeout_ms < PGM_NO_TIMEOUT || result_format > PGM_FORMAT_BINARY)
	{
		PyErr_SetString(PyExc_ValueError, "invalid Arrow execution options");
		return NULL;
	}
	if (!PyUnicode_Check(sql_object))
	{
		PyErr_SetString(PyExc_TypeError, "SQL must be str");
		return NULL;
	}
	sql = PyUnicode_AsUTF8AndSize(sql_object, &sql_size);
	if (sql == NULL)
		return NULL;
	if ((Py_ssize_t) strlen(sql) != sql_size)
	{
		PyErr_SetString(PyExc_ValueError, "SQL cannot contain a NUL byte");
		return NULL;
	}
	if (parameters_from_python(parameters_object, &parameters) != 0)
		return NULL;
	handle = connection_from_capsule(capsule, true);
	if (handle == NULL || mutex_lock(&handle->mutex) != 0)
		goto cleanup;
	if (handle->connection == NULL)
	{
		mutex_unlock(&handle->mutex);
		raise_simple_error(PGM_STATUS_INVALID_ARGUMENT,
			"PostGamma connection is closed");
		goto cleanup;
	}
	if (handle->active_request != NULL)
	{
		mutex_unlock(&handle->mutex);
		raise_simple_error(PGM_STATUS_BUSY,
			"PostGamma connection already has an active request");
		goto cleanup;
	}
	Py_BEGIN_ALLOW_THREADS
	status = pgm_execute_async(
		handle->connection, sql, parameters.parameters, parameters.count,
		(uint16_t) result_format, &request, &error);
	Py_END_ALLOW_THREADS
	if (status != PGM_STATUS_OK)
	{
		mutex_unlock(&handle->mutex);
		raise_native_error(status, error);
		error = NULL;
		goto cleanup;
	}
	handle->active_request = request;
	mutex_unlock(&handle->mutex);
	started_ms = monotonic_milliseconds();
	for (;;)
	{
		int64_t		slice = wait_slice(timeout_ms, started_ms);

		Py_BEGIN_ALLOW_THREADS
		status = pgm_request_wait(request, slice, &result, &error);
		Py_END_ALLOW_THREADS
		if (status != PGM_STATUS_TIMEOUT)
			break;
		if (timeout_ms != PGM_NO_TIMEOUT && slice == 0)
			break;
		pgm_error_free(error);
		error = NULL;
		if (PyErr_CheckSignals() != 0)
		{
			python_signal = true;
			Py_BEGIN_ALLOW_THREADS
			(void) pgm_request_cancel(request, NULL);
			Py_END_ALLOW_THREADS
			break;
		}
	}
	if (retire_connection_request(handle, request) != 0)
	{
		request = NULL;
		goto cleanup;
	}
	request = NULL;
	if (python_signal)
		goto cleanup;
	if (status != PGM_STATUS_OK)
	{
		raise_native_error(status, error);
		error = NULL;
		goto cleanup;
	}
	if (result == NULL)
	{
		PyErr_SetString(PyExc_RuntimeError,
			"PostGamma completed an Arrow request without a result");
		goto cleanup;
	}
	converted = arrow_capsules_from_result(result);

cleanup:
	if (request != NULL && handle != NULL)
		(void) retire_connection_request(handle, request);
	pgm_result_free(result);
	pgm_error_free(error);
	parameters_free(&parameters);
	return converted;
}


static PyMethodDef postgamma_arrow_methods[] = {
	{"connection_execute_arrow", _PyCFunction_CAST(native_connection_execute_arrow),
		METH_VARARGS | METH_KEYWORDS,
		"Execute one query and export an Arrow C Data struct array."},
	{NULL, NULL, 0, NULL}
};


int
postgamma_add_arrow_methods(PyObject *module)
{
	return PyModule_AddFunctions(module, postgamma_arrow_methods);
}
