/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef POSTGAMMA_PYTHON_NATIVE_INTERNAL_H
#define POSTGAMMA_PYTHON_NATIVE_INTERNAL_H

#define PY_SSIZE_T_CLEAN
#define _POSIX_C_SOURCE 200809L

#include <Python.h>

#include <postgamma/postgamma.h>

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>


#define POSTGAMMA_INSTANCE_CAPSULE "postgamma.instance"
#define POSTGAMMA_CONNECTION_CAPSULE "postgamma.connection"
#define POSTGAMMA_REQUEST_CAPSULE "postgamma.request"
#define POSTGAMMA_STATEMENT_CAPSULE "postgamma.statement"
#define POSTGAMMA_COPY_CAPSULE "postgamma.copy"
#define POSTGAMMA_OPERATION_CAPSULE "postgamma.operation"
#define POSTGAMMA_WAIT_SLICE_MS INT64_C(50)


typedef struct PostgammaPythonInstance
{
	pgm_instance *instance;
	pid_t		owner_pid;
	pthread_mutex_t mutex;
	size_t		connection_count;
	size_t		operation_count;
} PostgammaPythonInstance;


typedef struct PostgammaPythonConnection
{
	pgm_connection *connection;
	pgm_request *active_request;
	PyObject   *instance_capsule;
	pid_t		owner_pid;
	pthread_mutex_t mutex;
	size_t		statement_count;
} PostgammaPythonConnection;


typedef struct PostgammaPythonSettings
{
	pgm_setting *settings;
	char	  **storage;
	size_t		count;
} PostgammaPythonSettings;


typedef struct PostgammaPythonParameters
{
	pgm_parameter *parameters;
	Py_buffer  *buffers;
	bool	   *buffer_active;
	size_t		count;
} PostgammaPythonParameters;


extern PyObject *PostgammaNativeError;

int mutex_lock(pthread_mutex_t *mutex);
void mutex_unlock(pthread_mutex_t *mutex);
PyObject *optional_text(const char *value);
PyObject *required_text(const char *value);
void raise_native_error(pgm_status status, pgm_error *error);
void raise_simple_error(pgm_status status, const char *message);
PostgammaPythonInstance *instance_from_capsule(
	PyObject *capsule, bool require_open);
PostgammaPythonConnection *connection_from_capsule(
	PyObject *capsule, bool require_open);
void instance_connection_released(PyObject *capsule);
void connection_release_owner(PostgammaPythonConnection *handle);
int parameters_from_python(
	PyObject *object, PostgammaPythonParameters *output);
void parameters_free(PostgammaPythonParameters *parameters);
uint64_t monotonic_milliseconds(void);
int64_t wait_slice(long long timeout_ms, uint64_t started_ms);
int retire_connection_request(
	PostgammaPythonConnection *handle, pgm_request *request);
PyObject *build_column(const pgm_result *result, size_t index);
PyObject *result_to_python(const pgm_result *result);

int postgamma_add_request_methods(PyObject *module);
int postgamma_add_operation_methods(PyObject *module);
int postgamma_add_arrow_methods(PyObject *module);

#endif /* POSTGAMMA_PYTHON_NATIVE_INTERNAL_H */
