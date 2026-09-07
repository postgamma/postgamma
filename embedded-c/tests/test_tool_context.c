#include "postgamma/private/tool_context.h"

#include <assert.h>
#include <errno.h>
#include <stddef.h>


typedef struct CleanupFixture
{
	PostgammaToolContext *context;
	void	   *kernel_context;
	int			order[2];
	size_t		count;
} CleanupFixture;


static int
record_second_cleanup(void *argument)
{
	CleanupFixture *fixture = argument;

	fixture->order[fixture->count++] = 2;
	return 0;
}


static int
record_first_cleanup(void *argument)
{
	CleanupFixture *fixture = argument;

	fixture->order[fixture->count++] = 1;
	return postgamma_tool_context_clear_kernel_context(
		fixture->context, fixture->kernel_context);
}


int
main(void)
{
	PostgammaToolOptions options = {
		.generation = 7,
		.kind = POSTGAMMA_TOOL_BOOTSTRAP,
		.data_directory = "/tmp/postgamma-tool-data",
		.resource_root = "/tmp/postgamma-tool-resources",
		.bootstrap_input = "/tmp/postgamma-tool-input.bki",
	};
	PostgammaToolContext *context = NULL;
	PostgammaToolContext *previous = NULL;
	PostgammaToolTelemetry telemetry;
	CleanupFixture fixture;
	int			kernel_marker;

	assert(postgamma_tool_context_create(NULL, &context) == EINVAL);
	assert(postgamma_tool_context_create(&options, &context) == 0);
	fixture = (CleanupFixture) {
		.context = context,
		.kernel_context = &kernel_marker,
	};
	assert(postgamma_tool_context_bind(context, &previous) == 0);
	assert(previous == NULL);
	assert(postgamma_tool_context_current() == context);
	assert(postgamma_tool_context_set_kernel_context(
		context, fixture.kernel_context) == 0);
	assert(postgamma_tool_context_register_cleanup(
		context, record_first_cleanup, &fixture) == 0);
	assert(postgamma_tool_context_register_cleanup(
		context, record_second_cleanup, &fixture) == 0);
	assert(postgamma_tool_context_begin(context) == 0);
	assert(postgamma_tool_context_record_exit(context, 0) == 0);
	assert(postgamma_tool_context_unwind(context) == 0);
	assert(fixture.count == 2);
	assert(fixture.order[0] == 2);
	assert(fixture.order[1] == 1);
	assert(postgamma_tool_context_telemetry(context, &telemetry) == 0);
	assert(telemetry.phase == POSTGAMMA_TOOL_PHASE_COMPLETE);
	assert(telemetry.generation == options.generation);
	assert(telemetry.cleanup_registered == 2);
	assert(telemetry.cleanup_completed == 2);
	assert(telemetry.cleanup_remaining == 0);
	assert(telemetry.active_kernel_contexts == 0);
	assert(postgamma_tool_context_restore(context, previous) == 0);
	assert(postgamma_tool_context_current() == NULL);
	assert(postgamma_tool_context_destroy(context) == 0);
	return 0;
}
