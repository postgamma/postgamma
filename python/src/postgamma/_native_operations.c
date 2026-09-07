/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

#include "_native_internal.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>


typedef struct PostgammaPythonOperation
{
	pgm_operation *operation;
	PyObject   *instance_capsule;
	PyObject   *stream;
	PyObject   *callback_type;
	PyObject   *callback_value;
	PyObject   *callback_traceback;
	pid_t		owner_pid;
	pthread_mutex_t mutex;
} PostgammaPythonOperation;


static PostgammaPythonOperation *operation_from_capsule(
	PyObject *capsule, bool require_open);
static void operation_capsule_destructor(PyObject *capsule);
static void operation_release_owner(PostgammaPythonOperation *handle);
static PyObject *operation_capsule_create(
	PyObject *instance_capsule, pgm_operation *operation, PyObject *stream);
static void operation_capture_callback_error(PostgammaPythonOperation *handle);
static bool operation_restore_callback_error(PostgammaPythonOperation *handle);
static pgm_io_state operation_stream_read(
	void *user_data, void *buffer, size_t capacity, size_t *produced);
static pgm_io_state operation_stream_write(
	void *user_data, const void *data, size_t size, size_t *consumed);
static PyObject *event_to_python(pgm_event *event);


static PostgammaPythonOperation *
operation_from_capsule(PyObject *capsule, bool require_open)
{
	PostgammaPythonOperation *handle = PyCapsule_GetPointer(
		capsule, POSTGAMMA_OPERATION_CAPSULE);

	if (handle == NULL)
		return NULL;
	if (handle->owner_pid != getpid())
	{
		raise_simple_error(
			PGM_STATUS_FORKED_PROCESS,
			"PostGamma operation was inherited across fork; start it again in the "
			"child process");
		return NULL;
	}
	if (require_open && handle->operation == NULL)
	{
		raise_simple_error(PGM_STATUS_INVALID_ARGUMENT,
			"PostGamma operation is closed");
		return NULL;
	}
	return handle;
}


static void
operation_release_owner(PostgammaPythonOperation *handle)
{
	PyObject   *owner = handle->instance_capsule;
	PostgammaPythonInstance *instance;

	if (owner == NULL)
		return;
	handle->instance_capsule = NULL;
	instance = PyCapsule_GetPointer(owner, POSTGAMMA_INSTANCE_CAPSULE);
	if (instance != NULL && instance->owner_pid == getpid())
	{
		if (mutex_lock(&instance->mutex) == 0)
		{
			if (instance->operation_count > 0)
				instance->operation_count--;
			mutex_unlock(&instance->mutex);
		}
		else
			PyErr_Clear();
	}
	else
		PyErr_Clear();
	Py_DECREF(owner);
}


static void
operation_capsule_destructor(PyObject *capsule)
{
	PostgammaPythonOperation *handle = PyCapsule_GetPointer(
		capsule, POSTGAMMA_OPERATION_CAPSULE);

	if (handle == NULL)
	{
		PyErr_Clear();
		return;
	}
	if (handle->owner_pid == getpid() && handle->operation != NULL)
	{
		Py_BEGIN_ALLOW_THREADS
		(void) pgm_operation_cancel(handle->operation, NULL);
		pgm_operation_free(handle->operation);
		Py_END_ALLOW_THREADS
		handle->operation = NULL;
	}
	operation_release_owner(handle);
	Py_XDECREF(handle->stream);
	Py_XDECREF(handle->callback_type);
	Py_XDECREF(handle->callback_value);
	Py_XDECREF(handle->callback_traceback);
	(void) pthread_mutex_destroy(&handle->mutex);
	free(handle);
}


static PyObject *
operation_capsule_create(
	PyObject *instance_capsule, pgm_operation *operation, PyObject *stream)
{
	PostgammaPythonOperation *handle = calloc(1, sizeof(*handle));
	PyObject   *capsule;

	if (handle == NULL)
		return PyErr_NoMemory();
	if (pthread_mutex_init(&handle->mutex, NULL) != 0)
	{
		free(handle);
		PyErr_SetString(PyExc_RuntimeError,
			"could not initialize the native PostGamma operation lock");
		return NULL;
	}
	handle->operation = operation;
	handle->instance_capsule = instance_capsule;
	handle->stream = stream;
	handle->owner_pid = getpid();
	Py_INCREF(instance_capsule);
	Py_XINCREF(stream);
	capsule = PyCapsule_New(
		handle, POSTGAMMA_OPERATION_CAPSULE, operation_capsule_destructor);
	if (capsule != NULL)
		return capsule;
	Py_BEGIN_ALLOW_THREADS
	(void) pgm_operation_cancel(operation, NULL);
	pgm_operation_free(operation);
	Py_END_ALLOW_THREADS
	handle->operation = NULL;
	operation_release_owner(handle);
	Py_XDECREF(stream);
	(void) pthread_mutex_destroy(&handle->mutex);
	free(handle);
	return NULL;
}


static void
operation_capture_callback_error(PostgammaPythonOperation *handle)
{
	if (handle->callback_type != NULL)
	{
		PyErr_Clear();
		return;
	}
	PyErr_Fetch(
		&handle->callback_type, &handle->callback_value,
		&handle->callback_traceback);
	PyErr_NormalizeException(
		&handle->callback_type, &handle->callback_value,
		&handle->callback_traceback);
}


static bool
operation_restore_callback_error(PostgammaPythonOperation *handle)
{
	if (handle->callback_type == NULL)
		return false;
	PyErr_Restore(
		handle->callback_type, handle->callback_value,
		handle->callback_traceback);
	handle->callback_type = NULL;
	handle->callback_value = NULL;
	handle->callback_traceback = NULL;
	return true;
}


static pgm_io_state
operation_stream_read(
	void *user_data, void *buffer, size_t capacity, size_t *produced)
{
	PostgammaPythonOperation *handle = user_data;
	PyGILState_STATE gil;
	PyObject   *value = NULL;
	Py_buffer	view;
	pgm_io_state state = (pgm_io_state) -1;

	*produced = 0;
	gil = PyGILState_Ensure();
	value = PyObject_CallMethod(handle->stream, "read", "n", (Py_ssize_t) capacity);
	if (value == NULL)
	{
		if (PyErr_ExceptionMatches(PyExc_BlockingIOError))
		{
			PyErr_Clear();
			state = PGM_IO_AGAIN;
		}
		else
			operation_capture_callback_error(handle);
		goto done;
	}
	if (PyObject_GetBuffer(value, &view, PyBUF_SIMPLE) != 0)
	{
		operation_capture_callback_error(handle);
		goto done;
	}
	if (view.len < 0 || (size_t) view.len > capacity)
	{
		PyBuffer_Release(&view);
		PyErr_SetString(PyExc_ValueError,
			"logical restore source returned more than the requested capacity");
		operation_capture_callback_error(handle);
		goto done;
	}
	if (view.len == 0)
		state = PGM_IO_END;
	else
	{
		memcpy(buffer, view.buf, (size_t) view.len);
		*produced = (size_t) view.len;
		state = PGM_IO_PROGRESS;
	}
	PyBuffer_Release(&view);

done:
	Py_XDECREF(value);
	PyGILState_Release(gil);
	return state;
}


static pgm_io_state
operation_stream_write(
	void *user_data, const void *data, size_t size, size_t *consumed)
{
	PostgammaPythonOperation *handle = user_data;
	PyGILState_STATE gil;
	PyObject   *payload = NULL;
	PyObject   *answer = NULL;
	pgm_io_state state = (pgm_io_state) -1;

	*consumed = 0;
	gil = PyGILState_Ensure();
	if (size > (size_t) PY_SSIZE_T_MAX)
	{
		PyErr_SetString(PyExc_OverflowError,
			"logical dump fragment exceeds Python's addressable size");
		operation_capture_callback_error(handle);
		goto done;
	}
	payload = PyBytes_FromStringAndSize(data, (Py_ssize_t) size);
	if (payload == NULL)
	{
		operation_capture_callback_error(handle);
		goto done;
	}
	answer = PyObject_CallMethod(handle->stream, "write", "O", payload);
	if (answer == NULL)
	{
		if (PyErr_ExceptionMatches(PyExc_BlockingIOError))
		{
			PyErr_Clear();
			state = PGM_IO_AGAIN;
		}
		else
			operation_capture_callback_error(handle);
		goto done;
	}
	if (answer == Py_None)
		*consumed = size;
	else
	{
		unsigned long long value = PyLong_AsUnsignedLongLong(answer);

		if (PyErr_Occurred() || value > (unsigned long long) size)
		{
			if (!PyErr_Occurred())
				PyErr_SetString(PyExc_ValueError,
					"logical dump sink consumed more bytes than supplied");
			operation_capture_callback_error(handle);
			goto done;
		}
		*consumed = (size_t) value;
	}
	state = *consumed == 0 ? PGM_IO_AGAIN : PGM_IO_PROGRESS;

done:
	Py_XDECREF(answer);
	Py_XDECREF(payload);
	PyGILState_Release(gil);
	return state;
}


static PyObject *
event_to_python(pgm_event *event)
{
	pgm_event_kind kind = pgm_event_type(event);
	pgm_connection_id connection = pgm_event_connection(event);
	pgm_request_id request = pgm_event_request(event);
	pgm_error  *error = NULL;
	pgm_status	status = PGM_STATUS_OK;
	PyObject   *document = NULL;

	switch (kind)
	{
		case PGM_EVENT_LOG:
		{
			pgm_log_record record = PGM_LOG_RECORD_INIT;

			status = pgm_event_log(event, &record, &error);
			if (status == PGM_STATUS_OK)
				document = Py_BuildValue(
					"{s:i,s:K,s:K,s:i,s:N,s:N,s:N,s:N}",
					"kind", (int) kind,
					"connection_id", (unsigned long long) connection,
					"request_id", (unsigned long long) request,
					"virtual_backend_pid", record.virtual_backend_pid,
					"severity", required_text(record.severity),
					"sqlstate", required_text(record.sqlstate),
					"message", required_text(record.message),
					"detail", required_text(record.detail));
			break;
		}
		case PGM_EVENT_NOTICE:
		{
			pgm_notice notice = PGM_NOTICE_INIT;

			status = pgm_event_notice(event, &notice, &error);
			if (status == PGM_STATUS_OK)
				document = Py_BuildValue(
					"{s:i,s:K,s:K,s:N,s:N,s:N,s:N,s:N}",
					"kind", (int) kind,
					"connection_id", (unsigned long long) connection,
					"request_id", (unsigned long long) request,
					"severity", required_text(notice.severity),
					"sqlstate", required_text(notice.sqlstate),
					"message", required_text(notice.message),
					"detail", required_text(notice.detail),
					"hint", required_text(notice.hint));
			break;
		}
		case PGM_EVENT_NOTIFICATION:
		{
			pgm_notification notification = PGM_NOTIFICATION_INIT;

			status = pgm_event_notification(event, &notification, &error);
			if (status == PGM_STATUS_OK)
				document = Py_BuildValue(
					"{s:i,s:K,s:K,s:i,s:N,s:N}",
					"kind", (int) kind,
					"connection_id", (unsigned long long) connection,
					"request_id", (unsigned long long) request,
					"virtual_backend_pid",
						notification.virtual_backend_pid,
					"channel", required_text(notification.channel),
					"payload", required_text(notification.payload));
			break;
		}
		case PGM_EVENT_OVERFLOW:
		{
			pgm_event_overflow_record overflow = PGM_EVENT_OVERFLOW_RECORD_INIT;

			status = pgm_event_overflow(event, &overflow, &error);
			if (status == PGM_STATUS_OK)
				document = Py_BuildValue(
					"{s:i,s:K,s:K,s:i,s:K}",
					"kind", (int) kind,
					"connection_id", (unsigned long long) connection,
					"request_id", (unsigned long long) request,
					"dropped_kind", (int) overflow.dropped_kind,
					"dropped_count",
					(unsigned long long) overflow.dropped_count);
			break;
		}
		default:
			status = PGM_STATUS_INTERNAL_ERROR;
			break;
	}
	if (status != PGM_STATUS_OK)
		raise_native_error(status, error);
	return document;
}


static PyObject *
native_instance_waitable(PyObject *self, PyObject *capsule)
{
	PostgammaPythonInstance *handle = instance_from_capsule(capsule, true);
	pgm_error  *error = NULL;
	pgm_status	status;
	int			descriptor = -1;

	(void) self;
	if (handle == NULL || mutex_lock(&handle->mutex) != 0)
		return NULL;
	status = pgm_instance_waitable(handle->instance, &descriptor, &error);
	mutex_unlock(&handle->mutex);
	if (status != PGM_STATUS_OK)
	{
		raise_native_error(status, error);
		return NULL;
	}
	return PyLong_FromLong(descriptor);
}


static PyObject *
native_instance_next_event(PyObject *self, PyObject *args, PyObject *kwargs)
{
	static char *keywords[] = {"instance", "timeout_ms", NULL};
	PyObject   *capsule;
	long long	timeout_ms = 0;
	PostgammaPythonInstance *handle;
	pgm_event  *event = NULL;
	pgm_availability availability = PGM_AVAILABILITY_AGAIN;
	pgm_error  *error = NULL;
	pgm_status	status;
	PyObject   *document;

	(void) self;
	if (!PyArg_ParseTupleAndKeywords(
			args, kwargs, "O|L:instance_next_event", keywords,
			&capsule, &timeout_ms))
		return NULL;
	if (timeout_ms < PGM_NO_TIMEOUT)
	{
		PyErr_SetString(PyExc_ValueError, "invalid event wait timeout");
		return NULL;
	}
	handle = instance_from_capsule(capsule, true);
	if (handle == NULL || mutex_lock(&handle->mutex) != 0)
		return NULL;
	Py_BEGIN_ALLOW_THREADS
	status = pgm_instance_next_event(
		handle->instance, (int64_t) timeout_ms, &event, &availability, &error);
	Py_END_ALLOW_THREADS
	mutex_unlock(&handle->mutex);
	if (status != PGM_STATUS_OK)
	{
		raise_native_error(status, error);
		return NULL;
	}
	if (availability != PGM_AVAILABILITY_READY)
		Py_RETURN_NONE;
	document = event_to_python(event);
	pgm_event_free(event);
	return document;
}


static PyObject *
native_connection_status(PyObject *self, PyObject *capsule)
{
	PostgammaPythonConnection *handle = connection_from_capsule(capsule, true);
	pgm_connection_status_snapshot snapshot =
		PGM_CONNECTION_STATUS_SNAPSHOT_INIT;
	pgm_error  *error = NULL;
	pgm_status	status;

	(void) self;
	if (handle == NULL || mutex_lock(&handle->mutex) != 0)
		return NULL;
	status = pgm_connection_get_status(
		handle->connection, &snapshot, &error);
	mutex_unlock(&handle->mutex);
	if (status != PGM_STATUS_OK)
	{
		raise_native_error(status, error);
		return NULL;
	}
	return Py_BuildValue(
		"{s:i,s:I,s:I,s:i,s:K,s:K}",
		"transaction_status", (int) snapshot.transaction_status,
		"pin_reasons", (unsigned int) snapshot.pin_reasons,
		"carrier_retained", (unsigned int) snapshot.carrier_retained,
		"virtual_backend_pid", snapshot.virtual_backend_pid,
		"connection_id", (unsigned long long) snapshot.connection_id,
		"active_request_id", (unsigned long long) snapshot.active_request_id);
}


static PyObject *
native_bundled_extensions(PyObject *self, PyObject *unused)
{
	size_t		count = pgm_bundled_extension_count();
	PyObject   *result;

	(void) self;
	(void) unused;
	if (count > (size_t) PY_SSIZE_T_MAX)
	{
		PyErr_SetString(PyExc_OverflowError,
			"bundled extension count exceeds Python's addressable size");
		return NULL;
	}
	result = PyTuple_New((Py_ssize_t) count);
	if (result == NULL)
		return NULL;
	for (size_t index = 0; index < count; index++)
	{
		pgm_bundled_extension_info info = PGM_BUNDLED_EXTENSION_INFO_INIT;
		pgm_status	status = pgm_bundled_extension_get(index, &info);
		PyObject   *item;

		if (status != PGM_STATUS_OK)
		{
			Py_DECREF(result);
			raise_native_error(status, NULL);
			return NULL;
		}
		item = Py_BuildValue(
			"{s:N,s:N,s:N,s:I,s:I,s:K}",
			"id", required_text(info.id),
			"sql_name", required_text(info.sql_name),
			"version", required_text(info.version),
			"postgresql_major", (unsigned int) info.postgresql_major,
			"sdk_abi_version", (unsigned int) info.sdk_abi_version,
			"capabilities", (unsigned long long) info.capabilities);
		if (item == NULL)
		{
			Py_DECREF(result);
			return NULL;
		}
		PyTuple_SET_ITEM(result, (Py_ssize_t) index, item);
	}
	return result;
}


static PyObject *
native_operation_checkpoint(PyObject *self, PyObject *capsule)
{
	PostgammaPythonInstance *instance = instance_from_capsule(capsule, true);
	pgm_checkpoint_options options = PGM_CHECKPOINT_OPTIONS_INIT;
	pgm_operation *operation = NULL;
	pgm_error  *error = NULL;
	pgm_status	status;

	(void) self;
	if (instance == NULL || mutex_lock(&instance->mutex) != 0)
		return NULL;
	Py_BEGIN_ALLOW_THREADS
	status = pgm_instance_checkpoint_async(
		instance->instance, &options, &operation, &error);
	Py_END_ALLOW_THREADS
	if (status == PGM_STATUS_OK)
		instance->operation_count++;
	mutex_unlock(&instance->mutex);
	if (status != PGM_STATUS_OK)
	{
		raise_native_error(status, error);
		return NULL;
	}
	return operation_capsule_create(capsule, operation, NULL);
}


static PyObject *
native_operation_maintenance(PyObject *self, PyObject *args, PyObject *kwargs)
{
	static char *keywords[] = {"instance", "kind", "database", "user", NULL};
	PyObject   *capsule;
	int			kind;
	const char *database = NULL;
	const char *user = NULL;
	PostgammaPythonInstance *instance;
	pgm_maintenance_options options = PGM_MAINTENANCE_OPTIONS_INIT;
	pgm_operation *operation = NULL;
	pgm_error  *error = NULL;
	pgm_status	status;

	(void) self;
	if (!PyArg_ParseTupleAndKeywords(
			args, kwargs, "Oi|zz:operation_maintenance", keywords,
			&capsule, &kind, &database, &user))
		return NULL;
	if (kind < PGM_MAINTENANCE_VACUUM ||
		kind > PGM_MAINTENANCE_REINDEX_DATABASE)
	{
		PyErr_SetString(PyExc_ValueError, "invalid maintenance kind");
		return NULL;
	}
	instance = instance_from_capsule(capsule, true);
	if (instance == NULL || mutex_lock(&instance->mutex) != 0)
		return NULL;
	options.kind = (pgm_maintenance_kind) kind;
	options.database = database;
	options.user = user;
	Py_BEGIN_ALLOW_THREADS
	status = pgm_instance_maintenance_async(
		instance->instance, &options, &operation, &error);
	Py_END_ALLOW_THREADS
	if (status == PGM_STATUS_OK)
		instance->operation_count++;
	mutex_unlock(&instance->mutex);
	if (status != PGM_STATUS_OK)
	{
		raise_native_error(status, error);
		return NULL;
	}
	return operation_capsule_create(capsule, operation, NULL);
}


static PyObject *
native_operation_logical(PyObject *self, PyObject *args, PyObject *kwargs, bool dump)
{
	static char *keywords[] = {
		"instance", "stream", "flags", "database", "user",
		"channel_capacity", "progress_quantum", NULL
	};
	PyObject   *capsule;
	PyObject   *stream;
	unsigned int flags = 0;
	const char *database = NULL;
	const char *user = NULL;
	unsigned long long channel_capacity = 256U * 1024U;
	unsigned long long progress_quantum = 64U * 1024U;
	PostgammaPythonInstance *instance;
	PostgammaPythonOperation *handle;
	pgm_operation *operation = NULL;
	pgm_error  *error = NULL;
	pgm_status	status;
	PyObject   *result;

	(void) self;
	if (!PyArg_ParseTupleAndKeywords(
			args, kwargs, "OO|IzzKK:operation_logical", keywords,
			&capsule, &stream, &flags, &database, &user,
			&channel_capacity, &progress_quantum))
		return NULL;
	if ((flags & ~PGM_LOGICAL_FLAGS_ALL) != 0 || channel_capacity == 0 ||
		progress_quantum == 0 || channel_capacity > (unsigned long long) SIZE_MAX ||
		progress_quantum > (unsigned long long) SIZE_MAX)
	{
		PyErr_SetString(PyExc_ValueError, "invalid logical operation options");
		return NULL;
	}
	{
		int			has_method = PyObject_HasAttrString(
			stream, dump ? "write" : "read");

		if (has_method < 0)
			return NULL;
		if (has_method == 0)
		{
			PyErr_Format(PyExc_TypeError, "logical %s stream must provide %s()",
				dump ? "dump" : "restore", dump ? "write" : "read");
			return NULL;
		}
	}
	instance = instance_from_capsule(capsule, true);
	if (instance == NULL || mutex_lock(&instance->mutex) != 0)
		return NULL;
	handle = calloc(1, sizeof(*handle));
	if (handle == NULL)
	{
		mutex_unlock(&instance->mutex);
		return PyErr_NoMemory();
	}
	if (pthread_mutex_init(&handle->mutex, NULL) != 0)
	{
		mutex_unlock(&instance->mutex);
		free(handle);
		PyErr_SetString(PyExc_RuntimeError,
			"could not initialize the native PostGamma operation lock");
		return NULL;
	}
	handle->stream = stream;
	if (dump)
	{
		pgm_logical_dump_options options = PGM_LOGICAL_DUMP_OPTIONS_INIT;

		options.flags = flags;
		options.database = database;
		options.user = user;
		options.channel_capacity = (size_t) channel_capacity;
		options.progress_quantum = (size_t) progress_quantum;
		options.write = operation_stream_write;
		options.user_data = handle;
		Py_BEGIN_ALLOW_THREADS
		status = pgm_instance_logical_dump_async(
			instance->instance, &options, &operation, &error);
		Py_END_ALLOW_THREADS
	}
	else
	{
		pgm_logical_restore_options options = PGM_LOGICAL_RESTORE_OPTIONS_INIT;

		options.flags = flags;
		options.database = database;
		options.user = user;
		options.channel_capacity = (size_t) channel_capacity;
		options.progress_quantum = (size_t) progress_quantum;
		options.read = operation_stream_read;
		options.user_data = handle;
		Py_BEGIN_ALLOW_THREADS
		status = pgm_instance_logical_restore_async(
			instance->instance, &options, &operation, &error);
		Py_END_ALLOW_THREADS
	}
	if (status == PGM_STATUS_OK)
		instance->operation_count++;
	mutex_unlock(&instance->mutex);
	if (status != PGM_STATUS_OK)
	{
		(void) pthread_mutex_destroy(&handle->mutex);
		free(handle);
		raise_native_error(status, error);
		return NULL;
	}
	handle->operation = operation;
	handle->instance_capsule = capsule;
	handle->owner_pid = getpid();
	Py_INCREF(capsule);
	Py_INCREF(stream);
	result = PyCapsule_New(
		handle, POSTGAMMA_OPERATION_CAPSULE, operation_capsule_destructor);
	if (result == NULL)
	{
		Py_BEGIN_ALLOW_THREADS
		(void) pgm_operation_cancel(operation, NULL);
		pgm_operation_free(operation);
		Py_END_ALLOW_THREADS
		handle->operation = NULL;
		operation_release_owner(handle);
		Py_DECREF(stream);
		(void) pthread_mutex_destroy(&handle->mutex);
		free(handle);
		return NULL;
	}
	return result;
}


static PyObject *
native_operation_logical_dump(PyObject *self, PyObject *args, PyObject *kwargs)
{
	return native_operation_logical(self, args, kwargs, true);
}


static PyObject *
native_operation_logical_restore(PyObject *self, PyObject *args, PyObject *kwargs)
{
	return native_operation_logical(self, args, kwargs, false);
}


static PyObject *
native_operation_progress(PyObject *self, PyObject *capsule)
{
	PostgammaPythonOperation *handle = operation_from_capsule(capsule, true);
	pgm_operation_state state = PGM_OPERATION_PENDING;
	pgm_error  *error = NULL;
	pgm_status	status;

	(void) self;
	if (handle == NULL || mutex_lock(&handle->mutex) != 0)
		return NULL;
	Py_BEGIN_ALLOW_THREADS
	status = pgm_operation_progress(handle->operation, &state, &error);
	Py_END_ALLOW_THREADS
	if (operation_restore_callback_error(handle))
	{
		pgm_error_free(error);
		mutex_unlock(&handle->mutex);
		return NULL;
	}
	mutex_unlock(&handle->mutex);
	if (status != PGM_STATUS_OK)
	{
		raise_native_error(status, error);
		return NULL;
	}
	return PyLong_FromLong((long) state);
}


static PyObject *
native_operation_snapshot(PyObject *self, PyObject *capsule)
{
	PostgammaPythonOperation *handle = operation_from_capsule(capsule, true);
	pgm_operation_progress_snapshot snapshot =
		PGM_OPERATION_PROGRESS_SNAPSHOT_INIT;
	pgm_error  *error = NULL;
	pgm_status	status;

	(void) self;
	if (handle == NULL || mutex_lock(&handle->mutex) != 0)
		return NULL;
	status = pgm_operation_get_progress(handle->operation, &snapshot, &error);
	mutex_unlock(&handle->mutex);
	if (status != PGM_STATUS_OK)
	{
		raise_native_error(status, error);
		return NULL;
	}
	return Py_BuildValue(
		"{s:i,s:i,s:i,s:K,s:K,s:K,s:K,s:K}",
		"kind", (int) snapshot.kind,
		"state", (int) snapshot.state,
		"phase", (int) snapshot.phase,
		"bytes_received", (unsigned long long) snapshot.bytes_received,
		"bytes_produced", (unsigned long long) snapshot.bytes_produced,
		"total_bytes", (unsigned long long) snapshot.total_bytes,
		"objects_completed", (unsigned long long) snapshot.objects_completed,
		"objects_total", (unsigned long long) snapshot.objects_total);
}


static PyObject *
native_operation_waitable(PyObject *self, PyObject *capsule)
{
	PostgammaPythonOperation *handle = operation_from_capsule(capsule, true);
	pgm_error  *error = NULL;
	pgm_status	status;
	int			descriptor = -1;

	(void) self;
	if (handle == NULL || mutex_lock(&handle->mutex) != 0)
		return NULL;
	status = pgm_operation_waitable(handle->operation, &descriptor, &error);
	mutex_unlock(&handle->mutex);
	if (status != PGM_STATUS_OK)
	{
		raise_native_error(status, error);
		return NULL;
	}
	return PyLong_FromLong(descriptor);
}


static PyObject *
native_operation_cancel(PyObject *self, PyObject *capsule)
{
	PostgammaPythonOperation *handle = operation_from_capsule(capsule, true);
	pgm_error  *error = NULL;
	pgm_status	status;

	(void) self;
	if (handle == NULL || mutex_lock(&handle->mutex) != 0)
		return NULL;
	Py_BEGIN_ALLOW_THREADS
	status = pgm_operation_cancel(handle->operation, &error);
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
native_operation_close(PyObject *self, PyObject *capsule)
{
	PostgammaPythonOperation *handle = operation_from_capsule(capsule, false);

	(void) self;
	if (handle == NULL || mutex_lock(&handle->mutex) != 0)
		return NULL;
	if (handle->operation != NULL)
	{
		Py_BEGIN_ALLOW_THREADS
		pgm_operation_free(handle->operation);
		Py_END_ALLOW_THREADS
		handle->operation = NULL;
	}
	mutex_unlock(&handle->mutex);
	operation_release_owner(handle);
	Py_CLEAR(handle->stream);
	Py_RETURN_NONE;
}


static PyMethodDef postgamma_operation_methods[] = {
	{"instance_waitable", native_instance_waitable, METH_O,
		"Return the instance event waitable descriptor."},
	{"instance_next_event", _PyCFunction_CAST(native_instance_next_event),
		METH_VARARGS | METH_KEYWORDS, "Take one owned instance event."},
	{"connection_status", native_connection_status, METH_O,
		"Return a connection scheduling and pinning snapshot."},
	{"bundled_extensions", native_bundled_extensions, METH_NOARGS,
		"Return reviewed bundled extension metadata."},
	{"operation_checkpoint", native_operation_checkpoint, METH_O,
		"Start an in-process checkpoint operation."},
	{"operation_maintenance", _PyCFunction_CAST(native_operation_maintenance),
		METH_VARARGS | METH_KEYWORDS, "Start an in-process maintenance operation."},
	{"operation_logical_dump", _PyCFunction_CAST(native_operation_logical_dump),
		METH_VARARGS | METH_KEYWORDS, "Start an in-process logical dump."},
	{"operation_logical_restore",
		_PyCFunction_CAST(native_operation_logical_restore),
		METH_VARARGS | METH_KEYWORDS, "Start an in-process logical restore."},
	{"operation_progress", native_operation_progress, METH_O,
		"Advance one caller-driven management operation."},
	{"operation_snapshot", native_operation_snapshot, METH_O,
		"Return one management operation progress snapshot."},
	{"operation_waitable", native_operation_waitable, METH_O,
		"Return a management operation waitable descriptor."},
	{"operation_cancel", native_operation_cancel, METH_O,
		"Cancel a management operation."},
	{"operation_close", native_operation_close, METH_O,
		"Release a management operation."},
	{NULL, NULL, 0, NULL}
};


int
postgamma_add_operation_methods(PyObject *module)
{
	if (PyModule_AddFunctions(module, postgamma_operation_methods) != 0)
		return -1;
	if (PyModule_AddIntConstant(module, "EVENT_LOG", PGM_EVENT_LOG) != 0 ||
		PyModule_AddIntConstant(module, "EVENT_NOTICE", PGM_EVENT_NOTICE) != 0 ||
		PyModule_AddIntConstant(
			module, "EVENT_NOTIFICATION", PGM_EVENT_NOTIFICATION) != 0 ||
		PyModule_AddIntConstant(module, "EVENT_OVERFLOW", PGM_EVENT_OVERFLOW) != 0 ||
		PyModule_AddIntConstant(
			module, "PIN_TRANSACTION", (long) PGM_PIN_TRANSACTION) != 0 ||
		PyModule_AddIntConstant(
			module, "PIN_ADVISORY_LOCK", (long) PGM_PIN_ADVISORY_LOCK) != 0 ||
		PyModule_AddIntConstant(module, "PIN_PORTAL", (long) PGM_PIN_PORTAL) != 0 ||
		PyModule_AddIntConstant(module, "PIN_COPY", (long) PGM_PIN_COPY) != 0 ||
		PyModule_AddIntConstant(module, "PIN_OTHER", (long) PGM_PIN_OTHER) != 0 ||
		PyModule_AddIntConstant(
			module, "CAP_PREPARED_STATEMENTS",
			(long) PGM_CAP_PREPARED_STATEMENTS) != 0 ||
		PyModule_AddIntConstant(
			module, "CAP_CHUNKED_RESULTS", (long) PGM_CAP_CHUNKED_RESULTS) != 0 ||
		PyModule_AddIntConstant(module, "CAP_COPY_IN", (long) PGM_CAP_COPY_IN) != 0 ||
		PyModule_AddIntConstant(
			module, "CAP_COPY_OUT", (long) PGM_CAP_COPY_OUT) != 0 ||
		PyModule_AddIntConstant(
			module, "CAP_ARROW_C_DATA", (long) PGM_CAP_ARROW_C_DATA) != 0 ||
		PyModule_AddIntConstant(
			module, "CAP_NOTIFICATIONS", (long) PGM_CAP_NOTIFICATIONS) != 0 ||
		PyModule_AddIntConstant(
			module, "CAP_REQUEST_NOTICES", (long) PGM_CAP_REQUEST_NOTICES) != 0 ||
		PyModule_AddIntConstant(
			module, "CAP_STATUS_TELEMETRY", (long) PGM_CAP_STATUS_TELEMETRY) != 0 ||
		PyModule_AddIntConstant(
			module, "CAP_INSTANCE_EVENTS", (long) PGM_CAP_INSTANCE_EVENTS) != 0 ||
		PyModule_AddIntConstant(
			module, "CAP_MANAGEMENT_OPERATIONS",
			(long) PGM_CAP_MANAGEMENT_OPERATIONS) != 0 ||
		PyModule_AddIntConstant(
			module, "CAP_MULTIPLE_INSTANCES",
			(long) PGM_CAP_MULTIPLE_INSTANCES) != 0 ||
		PyModule_AddIntConstant(
			module, "CAP_NATIVE_EXTENSION_LOADING",
			(long) PGM_CAP_NATIVE_EXTENSION_LOADING) != 0 ||
		PyModule_AddIntConstant(
			module, "CAP_LOGICAL_BACKUP", (long) PGM_CAP_LOGICAL_BACKUP) != 0 ||
		PyModule_AddIntConstant(
			module, "CAP_LOGICAL_RESTORE", (long) PGM_CAP_LOGICAL_RESTORE) != 0 ||
		PyModule_AddIntConstant(
			module, "CAP_PHYSICAL_BACKUP", (long) PGM_CAP_PHYSICAL_BACKUP) != 0 ||
		PyModule_AddIntConstant(
			module, "CAP_MAINTENANCE", (long) PGM_CAP_MAINTENANCE) != 0 ||
		PyModule_AddIntConstant(
			module, "CAP_BUNDLED_EXTENSIONS",
			(long) PGM_CAP_BUNDLED_EXTENSIONS) != 0 ||
		PyModule_AddIntConstant(
			module, "OPERATION_PENDING", PGM_OPERATION_PENDING) != 0 ||
		PyModule_AddIntConstant(
			module, "OPERATION_RUNNING", PGM_OPERATION_RUNNING) != 0 ||
		PyModule_AddIntConstant(
			module, "OPERATION_COMPLETED", PGM_OPERATION_COMPLETED) != 0 ||
		PyModule_AddIntConstant(
			module, "OPERATION_CANCELED", PGM_OPERATION_CANCELED) != 0 ||
		PyModule_AddIntConstant(
			module, "OPERATION_FAILED", PGM_OPERATION_FAILED) != 0 ||
		PyModule_AddIntConstant(
			module, "OPERATION_KIND_CHECKPOINT", PGM_OPERATION_CHECKPOINT) != 0 ||
		PyModule_AddIntConstant(
			module, "OPERATION_KIND_LOGICAL_DUMP", PGM_OPERATION_LOGICAL_DUMP) != 0 ||
		PyModule_AddIntConstant(
			module, "OPERATION_KIND_LOGICAL_RESTORE",
			PGM_OPERATION_LOGICAL_RESTORE) != 0 ||
		PyModule_AddIntConstant(
			module, "OPERATION_KIND_MAINTENANCE", PGM_OPERATION_MAINTENANCE) != 0 ||
		PyModule_AddIntConstant(
			module, "OPERATION_PHASE_PENDING", PGM_OPERATION_PHASE_PENDING) != 0 ||
		PyModule_AddIntConstant(
			module, "OPERATION_PHASE_STARTING", PGM_OPERATION_PHASE_STARTING) != 0 ||
		PyModule_AddIntConstant(
			module, "OPERATION_PHASE_DATABASE", PGM_OPERATION_PHASE_DATABASE) != 0 ||
		PyModule_AddIntConstant(
			module, "OPERATION_PHASE_ARCHIVE_IO",
			PGM_OPERATION_PHASE_ARCHIVE_IO) != 0 ||
		PyModule_AddIntConstant(
			module, "OPERATION_PHASE_HOST_IO", PGM_OPERATION_PHASE_HOST_IO) != 0 ||
		PyModule_AddIntConstant(
			module, "OPERATION_PHASE_FINALIZING",
			PGM_OPERATION_PHASE_FINALIZING) != 0 ||
		PyModule_AddIntConstant(
			module, "OPERATION_PHASE_TERMINAL", PGM_OPERATION_PHASE_TERMINAL) != 0 ||
		PyModule_AddIntConstant(
			module, "MAINTENANCE_VACUUM", PGM_MAINTENANCE_VACUUM) != 0 ||
		PyModule_AddIntConstant(
			module, "MAINTENANCE_ANALYZE", PGM_MAINTENANCE_ANALYZE) != 0 ||
		PyModule_AddIntConstant(
			module, "MAINTENANCE_VACUUM_ANALYZE",
			PGM_MAINTENANCE_VACUUM_ANALYZE) != 0 ||
		PyModule_AddIntConstant(
			module, "MAINTENANCE_REINDEX_DATABASE",
			PGM_MAINTENANCE_REINDEX_DATABASE) != 0 ||
		PyModule_AddIntConstant(
			module, "LOGICAL_SCHEMA_ONLY", PGM_LOGICAL_SCHEMA_ONLY) != 0 ||
		PyModule_AddIntConstant(
			module, "LOGICAL_DATA_ONLY", PGM_LOGICAL_DATA_ONLY) != 0 ||
		PyModule_AddIntConstant(module, "LOGICAL_CLEAN", PGM_LOGICAL_CLEAN) != 0 ||
		PyModule_AddIntConstant(module, "LOGICAL_CREATE", PGM_LOGICAL_CREATE) != 0 ||
		PyModule_AddIntConstant(
			module, "LOGICAL_NO_OWNER", PGM_LOGICAL_NO_OWNER) != 0 ||
		PyModule_AddIntConstant(
			module, "LOGICAL_NO_PRIVILEGES", PGM_LOGICAL_NO_PRIVILEGES) != 0)
		return -1;
	return 0;
}
