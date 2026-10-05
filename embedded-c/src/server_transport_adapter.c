/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

#include "postgamma/private/server_transport_adapter.h"

#include <errno.h>
#include <string.h>


static int memory_status_to_errno(PostgammaMemoryStatus status);
static int retain_transport(
	void *transport, uint64_t generation, void **retained_transport);
static int release_transport(void **transport, uint64_t generation);
static int set_transport_notify(
	void *transport, uint64_t generation,
	PostgammaKernelTransportNotifyFunction notify, void *notify_argument);
static int read_transport(
	void *transport, uint64_t generation, void *buffer, size_t length,
	size_t *transferred);
static int write_transport(
	void *transport, uint64_t generation, const void *buffer, size_t length,
	size_t *transferred);
static int ready_transport(
	void *transport, uint64_t generation, uint32_t *events);
static int half_close_transport(void *transport, uint64_t generation);
static int set_session_status(
	void *transport, uint64_t generation, uint32_t pin_reasons,
	uint32_t carrier_retained);
static int get_result_policy(
	void *transport, uint64_t generation,
	PostgammaKernelResultPolicy *policy);


static const PostgammaKernelServerTransportOps MemoryServerTransportOps =
{
	.struct_size = sizeof(PostgammaKernelServerTransportOps),
	.abi_version = POSTGAMMA_EMBEDDED_KERNEL_ABI_VERSION,
	.retain = retain_transport,
	.release = release_transport,
	.set_notify = set_transport_notify,
	.read = read_transport,
	.write = write_transport,
	.ready = ready_transport,
	.half_close_write = half_close_transport,
	.set_session_status = set_session_status,
	.get_result_policy = get_result_policy,
};


int
postgamma_memory_server_connect_init(
	uint64_t generation, PostgammaMemoryEndpoint *backend_endpoint,
	PostgammaKernelConnectRequest *request)
{
	PostgammaMemoryTelemetry telemetry;
	PostgammaMemoryStatus status;

	if (generation == 0 || backend_endpoint == NULL || request == NULL)
		return EINVAL;
	status = postgamma_memory_endpoint_snapshot(
		backend_endpoint, generation, &telemetry);
	if (status != POSTGAMMA_MEMORY_STATUS_OK)
		return memory_status_to_errno(status);
	memset(request, 0, sizeof(*request));
	request->struct_size = sizeof(*request);
	request->abi_version = POSTGAMMA_EMBEDDED_KERNEL_ABI_VERSION;
	request->generation = generation;
	request->transport_ops = &MemoryServerTransportOps;
	request->transport = backend_endpoint;
	return 0;
}


static int
memory_status_to_errno(PostgammaMemoryStatus status)
{
	switch (status)
	{
		case POSTGAMMA_MEMORY_STATUS_OK:
		case POSTGAMMA_MEMORY_STATUS_PROGRESS:
		case POSTGAMMA_MEMORY_STATUS_EOF:
			return 0;
		case POSTGAMMA_MEMORY_STATUS_RETRY:
			return EAGAIN;
		case POSTGAMMA_MEMORY_STATUS_TIMEOUT:
			return ETIMEDOUT;
		case POSTGAMMA_MEMORY_STATUS_CANCELLED:
			return ECANCELED;
		case POSTGAMMA_MEMORY_STATUS_STALE_GENERATION:
			return ESTALE;
		case POSTGAMMA_MEMORY_STATUS_ABORTED:
			return ECONNRESET;
		case POSTGAMMA_MEMORY_STATUS_PEER_CLOSED:
		case POSTGAMMA_MEMORY_STATUS_LOCAL_CLOSED:
			return EPIPE;
		case POSTGAMMA_MEMORY_STATUS_INVALID_ARGUMENT:
			return EINVAL;
		case POSTGAMMA_MEMORY_STATUS_NO_MEMORY:
			return ENOMEM;
		case POSTGAMMA_MEMORY_STATUS_OVERFLOW:
			return EOVERFLOW;
		case POSTGAMMA_MEMORY_STATUS_CONTRACT_VIOLATION:
			return EPROTO;
		case POSTGAMMA_MEMORY_STATUS_INTERNAL_ERROR:
			return EIO;
	}
	return EIO;
}


static int
retain_transport(
	void *transport, uint64_t generation, void **retained_transport)
{
	PostgammaMemoryEndpoint *retained = NULL;
	PostgammaMemoryStatus status;

	if (retained_transport == NULL)
		return EINVAL;
	*retained_transport = NULL;
	status = postgamma_memory_endpoint_retain(
		transport, generation, &retained);
	if (status == POSTGAMMA_MEMORY_STATUS_OK)
		*retained_transport = retained;
	return memory_status_to_errno(status);
}


static int
release_transport(void **transport, uint64_t generation)
{
	PostgammaMemoryEndpoint *endpoint;
	PostgammaMemoryStatus status;

	if (transport == NULL || *transport == NULL)
		return EINVAL;
	endpoint = *transport;
	status = postgamma_memory_endpoint_release(&endpoint, generation);
	if (status == POSTGAMMA_MEMORY_STATUS_OK)
		*transport = NULL;
	return memory_status_to_errno(status);
}


static int
set_transport_notify(
	void *transport, uint64_t generation,
	PostgammaKernelTransportNotifyFunction notify, void *notify_argument)
{
	return memory_status_to_errno(postgamma_memory_endpoint_set_notify(
		transport, generation, notify, notify_argument));
}


static int
read_transport(
	void *transport, uint64_t generation, void *buffer, size_t length,
	size_t *transferred)
{
	PostgammaMemoryIoResult result;

	if (transferred == NULL)
		return EINVAL;
	*transferred = 0;
	result = postgamma_memory_endpoint_read(
		transport, generation, buffer, length);
	if (result.status == POSTGAMMA_MEMORY_STATUS_PROGRESS)
		*transferred = result.bytes;
	return memory_status_to_errno(result.status);
}


static int
write_transport(
	void *transport, uint64_t generation, const void *buffer, size_t length,
	size_t *transferred)
{
	PostgammaMemoryIoResult result;

	if (transferred == NULL)
		return EINVAL;
	*transferred = 0;
	result = postgamma_memory_endpoint_write(
		transport, generation, buffer, length);
	if (result.status == POSTGAMMA_MEMORY_STATUS_PROGRESS)
		*transferred = result.bytes;
	return memory_status_to_errno(result.status);
}


static int
ready_transport(void *transport, uint64_t generation, uint32_t *events)
{
	return memory_status_to_errno(postgamma_memory_endpoint_ready(
		transport, generation, events));
}


static int
half_close_transport(void *transport, uint64_t generation)
{
	return memory_status_to_errno(postgamma_memory_endpoint_half_close_write(
		transport, generation));
}


static int
set_session_status(
	void *transport, uint64_t generation, uint32_t pin_reasons,
	uint32_t carrier_retained)
{
	return memory_status_to_errno(postgamma_memory_endpoint_set_session_status(
		transport, generation, pin_reasons, carrier_retained));
}


static int
get_result_policy(
	void *transport, uint64_t generation,
	PostgammaKernelResultPolicy *policy)
{
	PostgammaMemoryResultPolicy memory_policy =
		POSTGAMMA_MEMORY_RESULT_POLICY_INIT;
	PostgammaMemoryStatus status;

	if (policy == NULL)
		return EINVAL;
	status = postgamma_memory_endpoint_get_result_policy(
		transport, generation, &memory_policy);
	if (status != POSTGAMMA_MEMORY_STATUS_OK)
		return memory_status_to_errno(status);
	policy->request_generation = memory_policy.request_generation;
	policy->delivery_mode = memory_policy.delivery_mode;
	policy->target_chunk_rows = memory_policy.target_chunk_rows;
	policy->result_buffer_limit = memory_policy.result_buffer_limit;
	policy->maximum_value_size = memory_policy.maximum_value_size;
	return 0;
}
