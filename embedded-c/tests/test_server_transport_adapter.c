#include "postgamma/private/server_transport_adapter.h"

#include <assert.h>
#include <errno.h>
#include <string.h>


#define TEST_GENERATION UINT64_C(9101)


static void
record_notification(void *argument, uint32_t events)
{
	unsigned int *count = argument;

	assert((events & ~POSTGAMMA_KERNEL_TRANSPORT_ALL) == 0);
	(*count)++;
}


int
main(void)
{
	PostgammaMemoryEndpoint *frontend = NULL;
	PostgammaMemoryEndpoint *backend = NULL;
	PostgammaKernelConnectRequest request =
		POSTGAMMA_KERNEL_CONNECT_REQUEST_INIT;
	PostgammaMemoryResultPolicy memory_policy =
		POSTGAMMA_MEMORY_RESULT_POLICY_INIT;
	PostgammaKernelResultPolicy kernel_policy =
		POSTGAMMA_KERNEL_RESULT_POLICY_INIT;
	void	   *retained = NULL;
	unsigned int notifications = 0;
	unsigned char buffer[8] = {0};
	size_t		transferred = 0;

	assert(postgamma_memory_transport_create(
		TEST_GENERATION, sizeof(buffer), &frontend, &backend) ==
		POSTGAMMA_MEMORY_STATUS_OK);
	assert(postgamma_memory_server_connect_init(
		TEST_GENERATION, backend, &request) == 0);
	assert(request.struct_size == sizeof(request));
	assert(request.abi_version == POSTGAMMA_EMBEDDED_KERNEL_ABI_VERSION);
	assert(request.generation == TEST_GENERATION);
	assert(request.transport == backend && request.transport_ops != NULL);
	assert(request.transport_ops->struct_size ==
		   sizeof(PostgammaKernelServerTransportOps));
	memory_policy.request_generation = UINT64_C(7);
	memory_policy.delivery_mode = POSTGAMMA_MEMORY_DELIVERY_CHUNKED;
	memory_policy.target_chunk_rows = UINT32_C(2);
	memory_policy.result_buffer_limit = 2048;
	memory_policy.maximum_value_size = 1024;
	assert(postgamma_memory_endpoint_set_result_policy(
		frontend, TEST_GENERATION, &memory_policy) ==
		POSTGAMMA_MEMORY_STATUS_OK);
	assert(request.transport_ops->get_result_policy(
		request.transport, TEST_GENERATION, &kernel_policy) == 0);
	assert(kernel_policy.request_generation == memory_policy.request_generation);
	assert(kernel_policy.delivery_mode == memory_policy.delivery_mode);
	assert(kernel_policy.target_chunk_rows == memory_policy.target_chunk_rows);
	assert(kernel_policy.result_buffer_limit == memory_policy.result_buffer_limit);
	assert(kernel_policy.maximum_value_size == memory_policy.maximum_value_size);
	assert(request.transport_ops->retain(
		request.transport, TEST_GENERATION, &retained) == 0);
	assert(retained == backend);
	assert(request.transport_ops->set_notify(
		retained, TEST_GENERATION, record_notification,
		&notifications) == 0);
	assert(notifications == 1);
	assert(postgamma_memory_endpoint_write(
		frontend, TEST_GENERATION, "query", 5).status ==
		POSTGAMMA_MEMORY_STATUS_PROGRESS);
	assert(notifications == 2);
	assert(request.transport_ops->read(
		retained, TEST_GENERATION, buffer, sizeof(buffer),
		&transferred) == 0);
	assert(transferred == 5 && memcmp(buffer, "query", 5) == 0);
	assert(request.transport_ops->read(
		retained, TEST_GENERATION, buffer, sizeof(buffer),
		&transferred) == EAGAIN);
	assert(request.transport_ops->write(
		retained, TEST_GENERATION, "result", 6, &transferred) == 0);
	assert(transferred == 6);
	assert(postgamma_memory_endpoint_read(
		frontend, TEST_GENERATION, buffer, sizeof(buffer)).bytes == 6);
	assert(memcmp(buffer, "result", 6) == 0);
	assert(request.transport_ops->half_close_write(
		retained, TEST_GENERATION) == 0);
	assert(request.transport_ops->set_notify(
		retained, TEST_GENERATION, NULL, NULL) == 0);
	assert(request.transport_ops->release(
		&retained, TEST_GENERATION) == 0);
	assert(retained == NULL);
	assert(postgamma_memory_endpoint_release(
		&backend, TEST_GENERATION) == POSTGAMMA_MEMORY_STATUS_OK);
	assert(postgamma_memory_endpoint_release(
		&frontend, TEST_GENERATION) == POSTGAMMA_MEMORY_STATUS_OK);
	return 0;
}
