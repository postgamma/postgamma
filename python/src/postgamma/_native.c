/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

#include "_native_internal.h"

#include <postgamma/private/instance_open_bridge.h>

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>


PyObject *PostgammaNativeError;


int
mutex_lock(pthread_mutex_t *mutex)
{
	int			status;

	/*
	 * Native close and cancel paths may hold this mutex while they run without
	 * the GIL.  A competing Python thread must therefore drop the GIL before
	 * waiting for the mutex, or the owner could never reacquire the GIL to
	 * release it.
	 */
	Py_BEGIN_ALLOW_THREADS
	status = pthread_mutex_lock(mutex);
	Py_END_ALLOW_THREADS

	if (status != 0)
	{
		PyErr_Format(PyExc_RuntimeError,
			"could not lock a native PostGamma handle: %s", strerror(status));
		return -1;
	}
	return 0;
}


void
mutex_unlock(pthread_mutex_t *mutex)
{
	int			status = pthread_mutex_unlock(mutex);

	if (status != 0)
		Py_FatalError("could not unlock a native PostGamma handle");
}


PyObject *
optional_text(const char *value)
{
	if (value == NULL || value[0] == '\0')
	{
		Py_INCREF(Py_None);
		return Py_None;
	}
	return PyUnicode_DecodeUTF8(value, (Py_ssize_t) strlen(value), "replace");
}


PyObject *
required_text(const char *value)
{
	if (value == NULL)
		value = "";
	return PyUnicode_DecodeUTF8(value, (Py_ssize_t) strlen(value), "replace");
}


static int
set_exception_attribute(PyObject *exception, const char *name, PyObject *value)
{
	int			status;

	if (value == NULL)
		return -1;
	status = PyObject_SetAttrString(exception, name, value);
	Py_DECREF(value);
	return status;
}


void
raise_native_error(pgm_status status, pgm_error *error)
{
	const char *message = error != NULL ? pgm_error_message(error) : NULL;
	const char *sqlstate = error != NULL ? pgm_error_sqlstate(error) : NULL;
	const char *severity = error != NULL ? pgm_error_severity(error) : NULL;
	const char *detail = error != NULL ? pgm_error_detail(error) : NULL;
	const char *hint = error != NULL ?
		pgm_error_field(error, PGM_DIAG_HINT) : NULL;
	PyObject   *exception;

	if (message == NULL || message[0] == '\0')
		message = pgm_status_name(status);
	exception = PyObject_CallFunction(PostgammaNativeError, "s", message);
	if (exception == NULL)
	{
		pgm_error_free(error);
		return;
	}
	if (set_exception_attribute(
			exception, "status", PyLong_FromLong((long) status)) != 0 ||
		set_exception_attribute(
			exception, "status_name", optional_text(pgm_status_name(status))) != 0 ||
		set_exception_attribute(exception, "sqlstate", optional_text(sqlstate)) != 0 ||
		set_exception_attribute(exception, "severity", optional_text(severity)) != 0 ||
		set_exception_attribute(exception, "detail", optional_text(detail)) != 0 ||
		set_exception_attribute(exception, "hint", optional_text(hint)) != 0)
	{
		Py_DECREF(exception);
		pgm_error_free(error);
		return;
	}
	PyErr_SetObject(PostgammaNativeError, exception);
	Py_DECREF(exception);
	pgm_error_free(error);
}


void
raise_simple_error(pgm_status status, const char *message)
{
	PyObject   *exception = PyObject_CallFunction(PostgammaNativeError, "s", message);

	if (exception == NULL)
		return;
	if (set_exception_attribute(
			exception, "status", PyLong_FromLong((long) status)) != 0 ||
		set_exception_attribute(
			exception, "status_name", optional_text(pgm_status_name(status))) != 0 ||
		set_exception_attribute(exception, "sqlstate", optional_text(NULL)) != 0 ||
		set_exception_attribute(exception, "severity", optional_text(NULL)) != 0 ||
		set_exception_attribute(exception, "detail", optional_text(NULL)) != 0 ||
		set_exception_attribute(exception, "hint", optional_text(NULL)) != 0)
	{
		Py_DECREF(exception);
		return;
	}
	PyErr_SetObject(PostgammaNativeError, exception);
	Py_DECREF(exception);
}


PostgammaPythonInstance *
instance_from_capsule(PyObject *capsule, bool require_open)
{
	PostgammaPythonInstance *handle = PyCapsule_GetPointer(
		capsule, POSTGAMMA_INSTANCE_CAPSULE);

	if (handle == NULL)
		return NULL;
	if (handle->owner_pid != getpid())
	{
		raise_simple_error(
			PGM_STATUS_FORKED_PROCESS,
			"PostGamma instance was inherited across fork; open a new instance in "
			"the child process");
		return NULL;
	}
	if (require_open && handle->instance == NULL)
	{
		raise_simple_error(PGM_STATUS_INVALID_ARGUMENT,
			"PostGamma instance is closed");
		return NULL;
	}
	return handle;
}


PostgammaPythonConnection *
connection_from_capsule(PyObject *capsule, bool require_open)
{
	PostgammaPythonConnection *handle = PyCapsule_GetPointer(
		capsule, POSTGAMMA_CONNECTION_CAPSULE);

	if (handle == NULL)
		return NULL;
	if (handle->owner_pid != getpid())
	{
		raise_simple_error(
			PGM_STATUS_FORKED_PROCESS,
			"PostGamma connection was inherited across fork; open a new connection "
			"in the child process");
		return NULL;
	}
	if (require_open && handle->connection == NULL)
	{
		raise_simple_error(PGM_STATUS_INVALID_ARGUMENT,
			"PostGamma connection is closed");
		return NULL;
	}
	return handle;
}


void
instance_connection_released(PyObject *capsule)
{
	PostgammaPythonInstance *instance;

	if (capsule == NULL)
		return;
	instance = PyCapsule_GetPointer(capsule, POSTGAMMA_INSTANCE_CAPSULE);
	if (instance == NULL)
	{
		PyErr_Clear();
		return;
	}
	if (instance->owner_pid == getpid())
	{
		if (pthread_mutex_lock(&instance->mutex) == 0)
		{
			if (instance->connection_count > 0)
				instance->connection_count--;
			(void) pthread_mutex_unlock(&instance->mutex);
		}
	}
}


void
connection_release_owner(PostgammaPythonConnection *handle)
{
	PyObject   *owner = handle->instance_capsule;

	if (owner == NULL)
		return;
	handle->instance_capsule = NULL;
	instance_connection_released(owner);
	Py_DECREF(owner);
}


static void
report_destructor_failure(
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
instance_capsule_destructor(PyObject *capsule)
{
	PostgammaPythonInstance *handle = PyCapsule_GetPointer(
		capsule, POSTGAMMA_INSTANCE_CAPSULE);

	if (handle == NULL)
	{
		PyErr_Clear();
		return;
	}
	if (handle->owner_pid == getpid() &&
		(handle->connection_count != 0 || handle->operation_count != 0))
	{
		/*
		 * Every connection owns a strong reference to this capsule, so reaching
		 * zero capsule references with live connections is an internal lifetime
		 * violation rather than an ordinary host cleanup failure.
		 */
		Py_FatalError("PostGamma instance capsule has live child handles");
	}
	if (handle->owner_pid == getpid() && handle->instance != NULL)
	{
		pgm_error  *error = NULL;
		pgm_status	status;

		/*
		 * The Python layer warns when explicit close was omitted.  At this final
		 * ownership boundary, do not trade a short GC pause for an unreachable
		 * live database.  Other Python threads remain runnable while shutdown
		 * completes because no Python object is touched without the GIL.
		 */
		Py_BEGIN_ALLOW_THREADS
		status = pgm_instance_close(
			handle->instance, PGM_SHUTDOWN_FAST, PGM_NO_TIMEOUT, &error);
		Py_END_ALLOW_THREADS

		if (status == PGM_STATUS_OK)
			handle->instance = NULL;
		else
			report_destructor_failure(capsule, "instance", status, error);
		pgm_error_free(error);
	}
	(void) pthread_mutex_destroy(&handle->mutex);
	free(handle);
}


static void
connection_capsule_destructor(PyObject *capsule)
{
	PostgammaPythonConnection *handle = PyCapsule_GetPointer(
		capsule, POSTGAMMA_CONNECTION_CAPSULE);

	if (handle == NULL)
	{
		PyErr_Clear();
		return;
	}
	if (handle->owner_pid == getpid() && handle->connection != NULL)
	{
		pgm_error  *error = NULL;
		pgm_status	status;

		if (handle->statement_count != 0)
			Py_FatalError("PostGamma connection capsule has live statements");

		Py_BEGIN_ALLOW_THREADS
		if (handle->active_request != NULL)
		{
			(void) pgm_request_cancel(handle->active_request, NULL);
			pgm_request_free(handle->active_request);
			handle->active_request = NULL;
		}
		status = pgm_connection_close(
			handle->connection, PGM_NO_TIMEOUT, &error);
		Py_END_ALLOW_THREADS
		if (status == PGM_STATUS_OK)
			handle->connection = NULL;
		else
			report_destructor_failure(capsule, "connection", status, error);
		pgm_error_free(error);
	}
	connection_release_owner(handle);
	(void) pthread_mutex_destroy(&handle->mutex);
	free(handle);
}


static void
settings_free(PostgammaPythonSettings *settings)
{
	size_t		index;

	if (settings == NULL)
		return;
	for (index = 0; index < settings->count * 2; index++)
		free(settings->storage[index]);
	free(settings->storage);
	free(settings->settings);
	memset(settings, 0, sizeof(*settings));
}


static char *
duplicate_python_text(PyObject *object, const char *label)
{
	Py_ssize_t	length;
	const char *text;
	char	   *copy;

	if (!PyUnicode_Check(object))
	{
		PyErr_Format(PyExc_TypeError, "%s must be str", label);
		return NULL;
	}
	text = PyUnicode_AsUTF8AndSize(object, &length);
	if (text == NULL)
		return NULL;
	if ((size_t) length == SIZE_MAX)
	{
		PyErr_NoMemory();
		return NULL;
	}
	copy = malloc((size_t) length + 1);
	if (copy == NULL)
	{
		PyErr_NoMemory();
		return NULL;
	}
	memcpy(copy, text, (size_t) length);
	copy[length] = '\0';
	return copy;
}


static int
settings_from_python(PyObject *object, PostgammaPythonSettings *output)
{
	PyObject   *items = NULL;
	PyObject   *sequence = NULL;
	Py_ssize_t	count;
	Py_ssize_t	index;

	memset(output, 0, sizeof(*output));
	if (object == NULL || object == Py_None)
		return 0;
	if (PyDict_Check(object))
		items = PyMapping_Items(object);
	else
	{
		items = object;
		Py_INCREF(items);
	}
	if (items == NULL)
		return -1;
	sequence = PySequence_Fast(items, "settings must be a mapping or pair sequence");
	Py_DECREF(items);
	if (sequence == NULL)
		return -1;
	count = PySequence_Fast_GET_SIZE(sequence);
	if (count < 0 || (size_t) count > SIZE_MAX / sizeof(pgm_setting) ||
		(size_t) count > SIZE_MAX / (2 * sizeof(char *)))
	{
		Py_DECREF(sequence);
		PyErr_SetString(PyExc_OverflowError, "too many PostGamma settings");
		return -1;
	}
	if (count == 0)
	{
		Py_DECREF(sequence);
		return 0;
	}
	output->settings = calloc((size_t) count, sizeof(pgm_setting));
	output->storage = calloc((size_t) count * 2, sizeof(char *));
	if (output->settings == NULL || output->storage == NULL)
	{
		Py_DECREF(sequence);
		settings_free(output);
		PyErr_NoMemory();
		return -1;
	}
	output->count = (size_t) count;
	for (index = 0; index < count; index++)
	{
		PyObject   *pair = PySequence_Fast(
			PySequence_Fast_GET_ITEM(sequence, index),
			"each setting must be a name/value pair");
		char	   *name;
		char	   *value;

		if (pair == NULL)
			goto failure;
		if (PySequence_Fast_GET_SIZE(pair) != 2)
		{
			Py_DECREF(pair);
			PyErr_SetString(PyExc_ValueError,
				"each setting must contain exactly two values");
			goto failure;
		}
		name = duplicate_python_text(PySequence_Fast_GET_ITEM(pair, 0),
			"setting name");
		value = name == NULL ? NULL : duplicate_python_text(
			PySequence_Fast_GET_ITEM(pair, 1), "setting value");
		Py_DECREF(pair);
		if (name == NULL || value == NULL)
		{
			free(name);
			free(value);
			goto failure;
		}
		output->storage[index * 2] = name;
		output->storage[index * 2 + 1] = value;
		output->settings[index].name = name;
		output->settings[index].value = value;
	}
	Py_DECREF(sequence);
	return 0;

failure:
	Py_DECREF(sequence);
	settings_free(output);
	return -1;
}


void
parameters_free(PostgammaPythonParameters *parameters)
{
	size_t		index;

	if (parameters == NULL)
		return;
	for (index = 0;
		 parameters->buffer_active != NULL && parameters->buffers != NULL &&
		 index < parameters->count;
		 index++)
	{
		if (parameters->buffer_active[index])
			PyBuffer_Release(&parameters->buffers[index]);
	}
	free(parameters->parameters);
	free(parameters->buffers);
	free(parameters->buffer_active);
	memset(parameters, 0, sizeof(*parameters));
}


int
parameters_from_python(PyObject *object, PostgammaPythonParameters *output)
{
	PyObject   *sequence;
	Py_ssize_t	count;
	Py_ssize_t	index;

	memset(output, 0, sizeof(*output));
	if (object == NULL || object == Py_None)
		return 0;
	sequence = PySequence_Fast(
		object, "encoded parameters must be a sequence");
	if (sequence == NULL)
		return -1;
	count = PySequence_Fast_GET_SIZE(sequence);
	if (count < 0 || count > INT_MAX ||
		(size_t) count > SIZE_MAX / sizeof(pgm_parameter))
	{
		Py_DECREF(sequence);
		PyErr_SetString(PyExc_OverflowError, "too many query parameters");
		return -1;
	}
	if (count == 0)
	{
		Py_DECREF(sequence);
		return 0;
	}
	output->parameters = calloc((size_t) count, sizeof(pgm_parameter));
	output->buffers = calloc((size_t) count, sizeof(Py_buffer));
	output->buffer_active = calloc((size_t) count, sizeof(bool));
	if (output->parameters == NULL || output->buffers == NULL ||
		output->buffer_active == NULL)
	{
		Py_DECREF(sequence);
		parameters_free(output);
		PyErr_NoMemory();
		return -1;
	}
	output->count = (size_t) count;
	for (index = 0; index < count; index++)
	{
		PyObject   *item = PySequence_Fast(
			PySequence_Fast_GET_ITEM(sequence, index),
			"each encoded parameter must be a four-item sequence");
		PyObject   *oid_object;
		PyObject   *format_object;
		PyObject   *null_object;
		PyObject   *data_object;
		unsigned long oid;
		unsigned long format;
		int			is_null;
		pgm_parameter value = PGM_PARAMETER_INIT;

		if (item == NULL)
			goto failure;
		if (PySequence_Fast_GET_SIZE(item) != 4)
		{
			Py_DECREF(item);
			PyErr_SetString(PyExc_ValueError,
				"each encoded parameter must contain oid, format, null, and data");
			goto failure;
		}
		oid_object = PySequence_Fast_GET_ITEM(item, 0);
		format_object = PySequence_Fast_GET_ITEM(item, 1);
		null_object = PySequence_Fast_GET_ITEM(item, 2);
		data_object = PySequence_Fast_GET_ITEM(item, 3);
		oid = PyLong_AsUnsignedLong(oid_object);
		format = PyLong_AsUnsignedLong(format_object);
		is_null = PyObject_IsTrue(null_object);
		if (PyErr_Occurred() || oid > UINT32_MAX || format > PGM_FORMAT_BINARY ||
			is_null < 0)
		{
			Py_DECREF(item);
			if (!PyErr_Occurred())
				PyErr_SetString(PyExc_ValueError,
					"encoded parameter metadata is outside the C ABI range");
			goto failure;
		}
		value.type_oid = (uint32_t) oid;
		value.format = (uint16_t) format;
		value.is_null = (uint16_t) (is_null != 0);
		if (!value.is_null)
		{
			if (PyObject_GetBuffer(
					data_object, &output->buffers[index], PyBUF_SIMPLE) != 0)
			{
				Py_DECREF(item);
				goto failure;
			}
			output->buffer_active[index] = true;
			value.data = output->buffers[index].buf;
			value.size = (size_t) output->buffers[index].len;
		}
		output->parameters[index] = value;
		Py_DECREF(item);
	}
	Py_DECREF(sequence);
	return 0;

failure:
	Py_DECREF(sequence);
	parameters_free(output);
	return -1;
}


uint64_t
monotonic_milliseconds(void)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
		return UINT64_MAX;
	return (uint64_t) now.tv_sec * UINT64_C(1000) +
		(uint64_t) now.tv_nsec / UINT64_C(1000000);
}


int
retire_connection_request(
	PostgammaPythonConnection *handle, pgm_request *request)
{
	int			lock_status;
	int			unlock_status = 0;

	/*
	 * A canceling Python thread may own this mutex while it reacquires the
	 * GIL after pgm_request_cancel().  Wait without the GIL to avoid reversing
	 * those two locks.
	 */
	Py_BEGIN_ALLOW_THREADS
	lock_status = pthread_mutex_lock(&handle->mutex);
	if (lock_status == 0)
	{
		pgm_request_free(request);
		if (handle->active_request == request)
			handle->active_request = NULL;
		unlock_status = pthread_mutex_unlock(&handle->mutex);
	}
	Py_END_ALLOW_THREADS
	if (lock_status != 0 || unlock_status != 0)
	{
		int			status = lock_status != 0 ? lock_status : unlock_status;

		PyErr_Format(PyExc_RuntimeError,
			"could not retire a native PostGamma request: %s", strerror(status));
		return -1;
	}
	return 0;
}


PyObject *
build_column(const pgm_result *result, size_t index)
{
	pgm_column	column = PGM_COLUMN_INIT;
	pgm_error  *error = NULL;
	pgm_status	status = pgm_result_column(result, index, &column, &error);
	PyObject   *name;
	PyObject   *value;

	if (status != PGM_STATUS_OK)
	{
		raise_native_error(status, error);
		return NULL;
	}
	name = optional_text(column.name);
	if (name == NULL)
		return NULL;
	value = Py_BuildValue(
		"(NIiIiiI)", name, (unsigned int) column.table_oid,
		(int) column.table_column, (unsigned int) column.type_oid,
		(int) column.type_size, (int) column.type_modifier,
		(unsigned int) column.format);
	return value;
}


static PyObject *
build_raw_value(const pgm_result *result, size_t row, size_t column)
{
	pgm_value_view view = PGM_VALUE_VIEW_INIT;
	pgm_error  *error = NULL;
	pgm_status	status = pgm_result_value(result, row, column, &view, &error);
	PyObject   *data;

	if (status != PGM_STATUS_OK)
	{
		raise_native_error(status, error);
		return NULL;
	}
	if (view.is_null)
	{
		Py_INCREF(Py_None);
		return Py_None;
	}
	if (view.size > (size_t) PY_SSIZE_T_MAX)
	{
		PyErr_SetString(PyExc_OverflowError,
			"PostGamma result value exceeds Python's addressable size");
		return NULL;
	}
	data = PyBytes_FromStringAndSize(view.data, (Py_ssize_t) view.size);
	if (data == NULL)
		return NULL;
	return Py_BuildValue(
		"(IIN)", (unsigned int) view.type_oid, (unsigned int) view.format, data);
}


PyObject *
result_to_python(const pgm_result *result)
{
	size_t		column_count = pgm_result_column_count(result);
	size_t		row_count = pgm_result_row_count(result);
	const char *command = pgm_result_command_status(result);
	PyObject   *columns;
	PyObject   *rows;
	PyObject   *command_object;
	PyObject   *document;
	size_t		column_index;
	size_t		row_index;

	if (column_count > (size_t) PY_SSIZE_T_MAX ||
		row_count > (size_t) PY_SSIZE_T_MAX)
	{
		PyErr_SetString(PyExc_OverflowError,
			"PostGamma result dimensions exceed Python's addressable size");
		return NULL;
	}
	columns = PyTuple_New((Py_ssize_t) column_count);
	rows = PyList_New((Py_ssize_t) row_count);
	command_object = optional_text(command);
	if (columns == NULL || rows == NULL || command_object == NULL)
	{
		Py_XDECREF(columns);
		Py_XDECREF(rows);
		Py_XDECREF(command_object);
		return NULL;
	}
	for (column_index = 0; column_index < column_count; column_index++)
	{
		PyObject   *column = build_column(result, column_index);

		if (column == NULL)
			goto failure;
		PyTuple_SET_ITEM(columns, (Py_ssize_t) column_index, column);
	}
	for (row_index = 0; row_index < row_count; row_index++)
	{
		PyObject   *row = PyTuple_New((Py_ssize_t) column_count);

		if (row == NULL)
			goto failure;
		for (column_index = 0; column_index < column_count; column_index++)
		{
			PyObject   *value = build_raw_value(
				result, row_index, column_index);

			if (value == NULL)
			{
				Py_DECREF(row);
				goto failure;
			}
			PyTuple_SET_ITEM(row, (Py_ssize_t) column_index, value);
		}
		PyList_SET_ITEM(rows, (Py_ssize_t) row_index, row);
	}
	document = Py_BuildValue(
		"{s:i,s:N,s:N,s:N}",
		"kind", (int) pgm_result_kind(result),
		"command_status", command_object,
		"columns", columns,
		"rows", rows);
	return document;

failure:
	Py_DECREF(columns);
	Py_DECREF(rows);
	Py_DECREF(command_object);
	return NULL;
}


static PyObject *
native_metadata(PyObject *self, PyObject *unused)
{
	(void) self;
	(void) unused;
	return Py_BuildValue(
		"{s:I,s:K,s:s,s:s}",
		"abi_version", (unsigned int) pgm_abi_version(),
		"capabilities", (unsigned long long) pgm_capabilities(),
		"postgresql_version", pgm_postgresql_version(),
		"build_id", pgm_build_id());
}


static PyObject *
native_instance_open(PyObject *self, PyObject *args, PyObject *kwargs)
{
	static char *keywords[] = {
		"path", "create", "executable_path", "resource_root", "settings",
		"worker_count", "result_buffer_limit", "maximum_value_size",
		"event_queue_capacity", "create_new", NULL
	};
	const char *path;
	const char *executable_path = NULL;
	const char *resource_root = NULL;
	int			create = 1;
	PyObject   *settings_object = Py_None;
	unsigned int worker_count = 4;
	unsigned long long result_buffer_limit = 0;
	unsigned long long maximum_value_size = 0;
	unsigned long long event_queue_capacity = 64;
	int			create_new = 0;
	PostgammaPythonSettings settings;
	PostgammaPythonInstance *handle;
	pgm_instance_options options = PGM_INSTANCE_OPTIONS_INIT;
	pgm_error  *error = NULL;
	pgm_status	status;
	PyObject   *capsule;

	(void) self;
	if (!PyArg_ParseTupleAndKeywords(
			args, kwargs, "s|pzzOIKKKp:instance_open", keywords,
			&path, &create, &executable_path, &resource_root, &settings_object,
			&worker_count, &result_buffer_limit, &maximum_value_size,
			&event_queue_capacity, &create_new))
		return NULL;
	if (event_queue_capacity > SIZE_MAX || result_buffer_limit > SIZE_MAX ||
		maximum_value_size > SIZE_MAX)
	{
		PyErr_SetString(PyExc_OverflowError,
			"PostGamma instance size option exceeds the native platform range");
		return NULL;
	}
	if (settings_from_python(settings_object, &settings) != 0)
		return NULL;
	handle = calloc(1, sizeof(*handle));
	if (handle == NULL)
	{
		settings_free(&settings);
		return PyErr_NoMemory();
	}
	if (pthread_mutex_init(&handle->mutex, NULL) != 0)
	{
		free(handle);
		settings_free(&settings);
		PyErr_SetString(PyExc_RuntimeError,
			"could not initialize the native PostGamma instance lock");
		return NULL;
	}
	options.path = path;
	options.create = (uint32_t) (create != 0);
	options.executable_path = executable_path;
	options.resource_root = resource_root;
	options.settings = settings.settings;
	options.setting_count = settings.count;
	options.executor_worker_count = worker_count;
	options.result_buffer_limit = (size_t) result_buffer_limit;
	options.maximum_value_size = (size_t) maximum_value_size;
	options.event_queue_capacity = (size_t) event_queue_capacity;
	if (create_new != 0)
		options.reserved = POSTGAMMA_PRIVATE_INSTANCE_OPEN_REQUIRE_MISSING;
	Py_BEGIN_ALLOW_THREADS
	status = pgm_instance_open(&options, &handle->instance, &error);
	Py_END_ALLOW_THREADS
	settings_free(&settings);
	if (status != PGM_STATUS_OK)
	{
		(void) pthread_mutex_destroy(&handle->mutex);
		free(handle);
		raise_native_error(status, error);
		return NULL;
	}
	handle->owner_pid = getpid();
	capsule = PyCapsule_New(
		handle, POSTGAMMA_INSTANCE_CAPSULE, instance_capsule_destructor);
	if (capsule == NULL)
	{
		Py_BEGIN_ALLOW_THREADS
		(void) pgm_instance_close(
			handle->instance, PGM_SHUTDOWN_FAST, PGM_NO_TIMEOUT, NULL);
		Py_END_ALLOW_THREADS
		(void) pthread_mutex_destroy(&handle->mutex);
		free(handle);
		return NULL;
	}
	return capsule;
}


static PyObject *
native_instance_close(PyObject *self, PyObject *args, PyObject *kwargs)
{
	static char *keywords[] = {"instance", "mode", "timeout_ms", NULL};
	PyObject   *capsule;
	int			mode = PGM_SHUTDOWN_FAST;
	long long	timeout_ms = 30000;
	PostgammaPythonInstance *handle;
	pgm_error  *error = NULL;
	pgm_status	status;

	(void) self;
	if (!PyArg_ParseTupleAndKeywords(
			args, kwargs, "O|iL:instance_close", keywords,
			&capsule, &mode, &timeout_ms))
		return NULL;
	handle = instance_from_capsule(capsule, false);
	if (handle == NULL)
		return NULL;
	if (mode < PGM_SHUTDOWN_SMART || mode > PGM_SHUTDOWN_IMMEDIATE ||
		timeout_ms < PGM_NO_TIMEOUT)
	{
		PyErr_SetString(PyExc_ValueError, "invalid PostGamma shutdown options");
		return NULL;
	}
	if (mutex_lock(&handle->mutex) != 0)
		return NULL;
	if (handle->instance == NULL)
	{
		mutex_unlock(&handle->mutex);
		Py_RETURN_NONE;
	}
	if (handle->connection_count != 0 || handle->operation_count != 0)
	{
		mutex_unlock(&handle->mutex);
		raise_simple_error(PGM_STATUS_BUSY,
			"close all PostGamma connections and operations before closing the "
			"instance");
		return NULL;
	}
	Py_BEGIN_ALLOW_THREADS
	status = pgm_instance_close(
		handle->instance, (pgm_shutdown_mode) mode, (int64_t) timeout_ms,
		&error);
	Py_END_ALLOW_THREADS
	if (status == PGM_STATUS_OK)
		handle->instance = NULL;
	mutex_unlock(&handle->mutex);
	if (status != PGM_STATUS_OK)
	{
		raise_native_error(status, error);
		return NULL;
	}
	Py_RETURN_NONE;
}


static PyObject *
native_connection_open(PyObject *self, PyObject *args, PyObject *kwargs)
{
	static char *keywords[] = {
		"instance", "user", "database", "application_name", "settings", NULL
	};
	PyObject   *instance_capsule;
	const char *user = NULL;
	const char *database = NULL;
	const char *application_name = NULL;
	PyObject   *settings_object = Py_None;
	PostgammaPythonInstance *instance;
	PostgammaPythonConnection *handle;
	PostgammaPythonSettings settings;
	pgm_connection_options options = PGM_CONNECTION_OPTIONS_INIT;
	pgm_error  *error = NULL;
	pgm_status	status;
	PyObject   *capsule;

	(void) self;
	if (!PyArg_ParseTupleAndKeywords(
			args, kwargs, "O|zzzO:connection_open", keywords,
			&instance_capsule, &user, &database, &application_name,
			&settings_object))
		return NULL;
	instance = instance_from_capsule(instance_capsule, true);
	if (instance == NULL)
		return NULL;
	if (settings_from_python(settings_object, &settings) != 0)
		return NULL;
	handle = calloc(1, sizeof(*handle));
	if (handle == NULL)
	{
		settings_free(&settings);
		return PyErr_NoMemory();
	}
	if (pthread_mutex_init(&handle->mutex, NULL) != 0)
	{
		free(handle);
		settings_free(&settings);
		PyErr_SetString(PyExc_RuntimeError,
			"could not initialize the native PostGamma connection lock");
		return NULL;
	}
	if (mutex_lock(&instance->mutex) != 0)
		goto failure;
	if (instance->instance == NULL)
	{
		mutex_unlock(&instance->mutex);
		raise_simple_error(PGM_STATUS_INVALID_ARGUMENT,
			"PostGamma instance is closed");
		goto failure;
	}
	options.user = user;
	options.database = database;
	options.application_name = application_name;
	options.settings = settings.settings;
	options.setting_count = settings.count;
	Py_BEGIN_ALLOW_THREADS
	status = pgm_connection_open(
		instance->instance, &options, &handle->connection, &error);
	Py_END_ALLOW_THREADS
	if (status == PGM_STATUS_OK)
		instance->connection_count++;
	mutex_unlock(&instance->mutex);
	settings_free(&settings);
	if (status != PGM_STATUS_OK)
	{
		(void) pthread_mutex_destroy(&handle->mutex);
		free(handle);
		raise_native_error(status, error);
		return NULL;
	}
	handle->owner_pid = getpid();
	handle->instance_capsule = instance_capsule;
	Py_INCREF(instance_capsule);
	capsule = PyCapsule_New(
		handle, POSTGAMMA_CONNECTION_CAPSULE, connection_capsule_destructor);
	if (capsule == NULL)
	{
		Py_BEGIN_ALLOW_THREADS
		(void) pgm_connection_close(
			handle->connection, PGM_NO_TIMEOUT, NULL);
		Py_END_ALLOW_THREADS
		handle->connection = NULL;
		connection_release_owner(handle);
		(void) pthread_mutex_destroy(&handle->mutex);
		free(handle);
		return NULL;
	}
	return capsule;

failure:
	settings_free(&settings);
	(void) pthread_mutex_destroy(&handle->mutex);
	free(handle);
	return NULL;
}


static PyObject *
native_connection_close(PyObject *self, PyObject *args, PyObject *kwargs)
{
	static char *keywords[] = {"connection", "timeout_ms", NULL};
	PyObject   *capsule;
	long long	timeout_ms = 30000;
	PostgammaPythonConnection *handle;
	pgm_error  *error = NULL;
	pgm_status	status;

	(void) self;
	if (!PyArg_ParseTupleAndKeywords(
			args, kwargs, "O|L:connection_close", keywords,
			&capsule, &timeout_ms))
		return NULL;
	handle = connection_from_capsule(capsule, false);
	if (handle == NULL)
		return NULL;
	if (timeout_ms < PGM_NO_TIMEOUT)
	{
		PyErr_SetString(PyExc_ValueError, "invalid PostGamma close timeout");
		return NULL;
	}
	if (mutex_lock(&handle->mutex) != 0)
		return NULL;
	if (handle->connection == NULL)
	{
		mutex_unlock(&handle->mutex);
		Py_RETURN_NONE;
	}
	if (handle->active_request != NULL)
	{
		mutex_unlock(&handle->mutex);
		raise_simple_error(PGM_STATUS_BUSY,
			"cannot close a PostGamma connection while a request is active");
		return NULL;
	}
	if (handle->statement_count != 0)
	{
		mutex_unlock(&handle->mutex);
		raise_simple_error(PGM_STATUS_BUSY,
			"close all prepared statements before closing the connection");
		return NULL;
	}
	Py_BEGIN_ALLOW_THREADS
	status = pgm_connection_close(
		handle->connection, (int64_t) timeout_ms, &error);
	Py_END_ALLOW_THREADS
	if (status == PGM_STATUS_OK)
		handle->connection = NULL;
	mutex_unlock(&handle->mutex);
	if (status != PGM_STATUS_OK)
	{
		raise_native_error(status, error);
		return NULL;
	}
	connection_release_owner(handle);
	Py_RETURN_NONE;
}


int64_t
wait_slice(long long timeout_ms, uint64_t started_ms)
{
	uint64_t	now;
	uint64_t	elapsed;
	uint64_t	remaining;

	if (timeout_ms == PGM_NO_TIMEOUT)
		return POSTGAMMA_WAIT_SLICE_MS;
	if (timeout_ms == 0)
		return 0;
	now = monotonic_milliseconds();
	if (now == UINT64_MAX || now < started_ms)
		return 0;
	elapsed = now - started_ms;
	if (elapsed >= (uint64_t) timeout_ms)
		return 0;
	remaining = (uint64_t) timeout_ms - elapsed;
	return (int64_t) (remaining < (uint64_t) POSTGAMMA_WAIT_SLICE_MS ?
		remaining : (uint64_t) POSTGAMMA_WAIT_SLICE_MS);
}


static PyObject *
native_connection_execute(PyObject *self, PyObject *args, PyObject *kwargs)
{
	static char *keywords[] = {
		"connection", "sql", "parameters", "timeout_ms", NULL
	};
	PyObject   *capsule;
	PyObject   *sql_object;
	PyObject   *parameters_object = Py_None;
	long long	timeout_ms = 30000;
	PostgammaPythonConnection *handle;
	PostgammaPythonParameters parameters;
	const char *sql;
	Py_ssize_t	sql_size;
	pgm_request *request = NULL;
	pgm_result *result = NULL;
	pgm_error  *error = NULL;
	pgm_status	status;
	uint64_t	started_ms;
	bool		python_signal = false;
	PyObject   *converted = NULL;

	(void) self;
	if (!PyArg_ParseTupleAndKeywords(
			args, kwargs, "OO|OL:connection_execute", keywords,
			&capsule, &sql_object, &parameters_object, &timeout_ms))
		return NULL;
	if (timeout_ms < PGM_NO_TIMEOUT)
	{
		PyErr_SetString(PyExc_ValueError, "invalid PostGamma query timeout");
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
	if (handle == NULL)
		goto cleanup;
	if (mutex_lock(&handle->mutex) != 0)
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
		PGM_FORMAT_TEXT, &request, &error);
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
			"PostGamma completed a request without a result");
		goto cleanup;
	}
	converted = result_to_python(result);

cleanup:
	if (request != NULL)
	{
		if (handle != NULL)
			(void) retire_connection_request(handle, request);
	}
	pgm_result_free(result);
	pgm_error_free(error);
	parameters_free(&parameters);
	return converted;
}


static PyObject *
native_connection_cancel(PyObject *self, PyObject *capsule)
{
	PostgammaPythonConnection *handle;
	pgm_error  *error = NULL;
	pgm_status	status;
	pgm_request *request;

	(void) self;
	handle = connection_from_capsule(capsule, true);
	if (handle == NULL)
		return NULL;
	if (mutex_lock(&handle->mutex) != 0)
		return NULL;
	request = handle->active_request;
	if (request == NULL)
	{
		mutex_unlock(&handle->mutex);
		Py_RETURN_FALSE;
	}
	Py_BEGIN_ALLOW_THREADS
	status = pgm_request_cancel(request, &error);
	Py_END_ALLOW_THREADS
	mutex_unlock(&handle->mutex);
	if (status != PGM_STATUS_OK)
	{
		raise_native_error(status, error);
		return NULL;
	}
	Py_RETURN_TRUE;
}


static PyObject *
native_connection_transaction_status(PyObject *self, PyObject *capsule)
{
	PostgammaPythonConnection *handle;
	pgm_transaction_status transaction_status = PGM_TRANSACTION_UNKNOWN;
	pgm_error  *error = NULL;
	pgm_status	status;

	(void) self;
	handle = connection_from_capsule(capsule, true);
	if (handle == NULL)
		return NULL;
	if (mutex_lock(&handle->mutex) != 0)
		return NULL;
	if (handle->active_request != NULL)
	{
		mutex_unlock(&handle->mutex);
		raise_simple_error(PGM_STATUS_BUSY,
			"cannot inspect transaction state while a request is active");
		return NULL;
	}
	Py_BEGIN_ALLOW_THREADS
	status = pgm_connection_transaction_status(
		handle->connection, &transaction_status, &error);
	Py_END_ALLOW_THREADS
	mutex_unlock(&handle->mutex);
	if (status != PGM_STATUS_OK)
	{
		raise_native_error(status, error);
		return NULL;
	}
	return PyLong_FromLong((long) transaction_status);
}


static PyObject *
native_connection_identity(PyObject *self, PyObject *capsule)
{
	PostgammaPythonConnection *handle;
	pgm_connection_id identity;

	(void) self;
	handle = connection_from_capsule(capsule, true);
	if (handle == NULL)
		return NULL;
	identity = pgm_connection_identity(handle->connection);
	return PyLong_FromUnsignedLongLong((unsigned long long) identity);
}


static PyObject *
native_connection_parameter_status(PyObject *self, PyObject *args)
{
	PyObject   *capsule;
	const char *name;
	PostgammaPythonConnection *handle;
	const char *value = NULL;
	pgm_error  *error = NULL;
	pgm_status	status;
	PyObject   *result;

	(void) self;
	if (!PyArg_ParseTuple(args, "Os:connection_parameter_status", &capsule, &name))
		return NULL;
	handle = connection_from_capsule(capsule, true);
	if (handle == NULL)
		return NULL;
	if (mutex_lock(&handle->mutex) != 0)
		return NULL;
	if (handle->active_request != NULL)
	{
		mutex_unlock(&handle->mutex);
		raise_simple_error(PGM_STATUS_BUSY,
			"cannot inspect parameters while a request is active");
		return NULL;
	}
	status = pgm_connection_parameter_status(
		handle->connection, name, &value, &error);
	if (status == PGM_STATUS_OK)
		result = optional_text(value);
	else
		result = NULL;
	mutex_unlock(&handle->mutex);
	if (status != PGM_STATUS_OK)
		raise_native_error(status, error);
	return result;
}


static PyObject *
native_instance_telemetry(PyObject *self, PyObject *capsule)
{
	PostgammaPythonInstance *handle;
	pgm_instance_telemetry telemetry = PGM_INSTANCE_TELEMETRY_INIT;
	pgm_error  *error = NULL;
	pgm_status	status;

	(void) self;
	handle = instance_from_capsule(capsule, true);
	if (handle == NULL)
		return NULL;
	if (mutex_lock(&handle->mutex) != 0)
		return NULL;
	status = pgm_instance_get_telemetry(handle->instance, &telemetry, &error);
	mutex_unlock(&handle->mutex);
	if (status != PGM_STATUS_OK)
	{
		raise_native_error(status, error);
		return NULL;
	}
	return Py_BuildValue(
		"{s:I,s:K,s:K,s:K,s:K,s:K,s:K,s:K,s:K,s:K,s:K,s:K,s:K,s:K,s:K,"
		"s:K,s:K,s:K,s:K}",
		"executor_worker_count", telemetry.executor_worker_count,
		"connection_count", (unsigned long long) telemetry.connection_count,
		"active_request_count", (unsigned long long) telemetry.active_request_count,
		"running_session_count", (unsigned long long) telemetry.running_session_count,
		"pinned_session_count", (unsigned long long) telemetry.pinned_session_count,
		"runnable_session_count", (unsigned long long) telemetry.runnable_session_count,
		"queued_request_count", (unsigned long long) telemetry.queued_request_count,
		"parallel_tokens_in_use", (unsigned long long) telemetry.parallel_tokens_in_use,
		"request_count", (unsigned long long) telemetry.request_count,
		"completed_request_count", (unsigned long long) telemetry.completed_request_count,
		"canceled_request_count", (unsigned long long) telemetry.canceled_request_count,
		"failed_request_count", (unsigned long long) telemetry.failed_request_count,
		"execution_token_rejections",
		(unsigned long long) telemetry.execution_token_rejections,
		"queue_wait_ns_max", (unsigned long long) telemetry.queue_wait_ns_max,
		"event_queue_depth", (unsigned long long) telemetry.event_queue_depth,
		"event_queue_capacity", (unsigned long long) telemetry.event_queue_capacity,
		"dropped_log_count", (unsigned long long) telemetry.dropped_log_count,
		"dropped_notice_count", (unsigned long long) telemetry.dropped_notice_count,
		"dropped_notification_count",
		(unsigned long long) telemetry.dropped_notification_count);
}


static PyMethodDef postgamma_native_methods[] = {
	{"metadata", native_metadata, METH_NOARGS,
		"Return the embedded library identity and capability mask."},
	{"instance_open", _PyCFunction_CAST(native_instance_open),
		METH_VARARGS | METH_KEYWORDS, "Open or create an in-process instance."},
	{"instance_close", _PyCFunction_CAST(native_instance_close),
		METH_VARARGS | METH_KEYWORDS, "Close an in-process instance."},
	{"instance_telemetry", native_instance_telemetry, METH_O,
		"Return one instance telemetry snapshot."},
	{"connection_open", _PyCFunction_CAST(native_connection_open),
		METH_VARARGS | METH_KEYWORDS, "Open an in-memory protocol connection."},
	{"connection_close", _PyCFunction_CAST(native_connection_close),
		METH_VARARGS | METH_KEYWORDS, "Close an in-memory protocol connection."},
	{"connection_execute", _PyCFunction_CAST(native_connection_execute),
		METH_VARARGS | METH_KEYWORDS, "Execute one extended-protocol statement."},
	{"connection_cancel", native_connection_cancel, METH_O,
		"Cancel the active request on a connection."},
	{"connection_transaction_status", native_connection_transaction_status,
		METH_O, "Return the PostgreSQL transaction status."},
	{"connection_identity", native_connection_identity, METH_O,
		"Return the logical connection identifier."},
	{"connection_parameter_status", native_connection_parameter_status,
		METH_VARARGS, "Return one PostgreSQL connection parameter."},
	{NULL, NULL, 0, NULL}
};


static struct PyModuleDef postgamma_native_module = {
	PyModuleDef_HEAD_INIT,
	"_native",
	"Native CPython boundary for the PostGamma embedded C ABI.",
	-1,
	postgamma_native_methods,
	NULL,
	NULL,
	NULL,
	NULL
};


PyMODINIT_FUNC
PyInit__native(void)
{
	uint32_t	loaded_abi = pgm_abi_version();
	PyObject   *module;

	if ((loaded_abi >> 16) != PGM_ABI_VERSION_MAJOR ||
		loaded_abi < PGM_ABI_VERSION)
	{
		PyErr_Format(PyExc_ImportError,
			"PostGamma C ABI mismatch: extension requires %u.%u and loaded %u.%u",
			(unsigned int) PGM_ABI_VERSION_MAJOR,
			(unsigned int) PGM_ABI_VERSION_MINOR,
			(unsigned int) (loaded_abi >> 16),
			(unsigned int) (loaded_abi & UINT32_C(0xffff)));
		return NULL;
	}
	module = PyModule_Create(&postgamma_native_module);

	if (module == NULL)
		return NULL;
	PostgammaNativeError = PyErr_NewException(
		"postgamma._native.NativeError", PyExc_RuntimeError, NULL);
	if (PostgammaNativeError == NULL)
	{
		Py_DECREF(module);
		return NULL;
	}
	Py_INCREF(PostgammaNativeError);
	if (PyModule_AddObject(module, "NativeError", PostgammaNativeError) != 0)
	{
		Py_DECREF(PostgammaNativeError);
		Py_DECREF(PostgammaNativeError);
		Py_DECREF(module);
		return NULL;
	}
	if (PyModule_AddIntConstant(module, "SHUTDOWN_SMART", PGM_SHUTDOWN_SMART) != 0 ||
		PyModule_AddIntConstant(module, "SHUTDOWN_FAST", PGM_SHUTDOWN_FAST) != 0 ||
		PyModule_AddIntConstant(
			module, "SHUTDOWN_IMMEDIATE", PGM_SHUTDOWN_IMMEDIATE) != 0 ||
		PyModule_AddIntConstant(module, "TRANSACTION_IDLE", PGM_TRANSACTION_IDLE) != 0 ||
		PyModule_AddIntConstant(
			module, "TRANSACTION_ACTIVE", PGM_TRANSACTION_ACTIVE) != 0 ||
		PyModule_AddIntConstant(
			module, "TRANSACTION_INTRANS", PGM_TRANSACTION_INTRANS) != 0 ||
		PyModule_AddIntConstant(
			module, "TRANSACTION_INERROR", PGM_TRANSACTION_INERROR) != 0 ||
		PyModule_AddIntConstant(
			module, "RESULT_COMMAND_OK", PGM_RESULT_COMMAND_OK) != 0 ||
		PyModule_AddIntConstant(
			module, "RESULT_TUPLES_OK", PGM_RESULT_TUPLES_OK) != 0 ||
		PyModule_AddIntConstant(
			module, "RESULT_EMPTY_QUERY", PGM_RESULT_EMPTY_QUERY) != 0 ||
		PyModule_AddIntConstant(module, "PGM_STATUS_OK", PGM_STATUS_OK) != 0 ||
		PyModule_AddIntConstant(
			module, "PGM_STATUS_INVALID_ARGUMENT", PGM_STATUS_INVALID_ARGUMENT) != 0 ||
		PyModule_AddIntConstant(module, "PGM_STATUS_BUSY", PGM_STATUS_BUSY) != 0 ||
		PyModule_AddIntConstant(
			module, "PGM_STATUS_TIMEOUT", PGM_STATUS_TIMEOUT) != 0 ||
		PyModule_AddIntConstant(
			module, "PGM_STATUS_CANCELED", PGM_STATUS_CANCELED) != 0 ||
		PyModule_AddIntConstant(
			module, "PGM_STATUS_IO_ERROR", PGM_STATUS_IO_ERROR) != 0 ||
		PyModule_AddIntConstant(
			module, "PGM_STATUS_POSTGRES_ERROR", PGM_STATUS_POSTGRES_ERROR) != 0 ||
		PyModule_AddIntConstant(
			module, "PGM_STATUS_CONNECTION_FAILED", PGM_STATUS_CONNECTION_FAILED) != 0 ||
		PyModule_AddIntConstant(
			module, "PGM_STATUS_INSTANCE_FAILED", PGM_STATUS_INSTANCE_FAILED) != 0 ||
		PyModule_AddIntConstant(
			module, "PGM_STATUS_FORKED_PROCESS", PGM_STATUS_FORKED_PROCESS) != 0 ||
		PyModule_AddIntConstant(
			module, "PGM_STATUS_VERSION_MISMATCH", PGM_STATUS_VERSION_MISMATCH) != 0 ||
		PyModule_AddIntConstant(
			module, "PGM_STATUS_UNSUPPORTED", PGM_STATUS_UNSUPPORTED) != 0 ||
		PyModule_AddIntConstant(
			module, "PGM_STATUS_OUT_OF_MEMORY", PGM_STATUS_OUT_OF_MEMORY) != 0 ||
		PyModule_AddIntConstant(
			module, "PGM_STATUS_INTERNAL_ERROR", PGM_STATUS_INTERNAL_ERROR) != 0 ||
		PyModule_AddIntConstant(
			module, "PGM_STATUS_REENTRANT_CALL", PGM_STATUS_REENTRANT_CALL) != 0)
	{
		Py_DECREF(module);
		return NULL;
	}
	if (postgamma_add_request_methods(module) != 0 ||
		postgamma_add_operation_methods(module) != 0 ||
		postgamma_add_arrow_methods(module) != 0)
	{
		Py_DECREF(module);
		return NULL;
	}
	return module;
}
