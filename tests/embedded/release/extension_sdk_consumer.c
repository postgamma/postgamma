#include "postgamma/postgamma_extension.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>


static pgmex_context
make_context(void)
{
	pgmex_context context = PGMEX_CONTEXT_INIT;

	context.phase = PGMEX_PHASE_SESSION_INITIALIZE;
	return context;
}


int
main(void)
{
	pgmex_context context = make_context();
	pgmex_descriptor descriptor;

	memset(&descriptor, 0, sizeof(descriptor));
	descriptor.struct_size = sizeof(descriptor);
	descriptor.abi_version = PGMEX_ABI_VERSION;
	descriptor.capabilities =
		PGMEX_CAP_THREAD_SAFE | PGMEX_CAP_MULTI_INSTANCE_SAFE;
	return context.struct_size == sizeof(context) &&
		context.phase == PGMEX_PHASE_SESSION_INITIALIZE &&
		descriptor.struct_size == sizeof(descriptor) &&
		descriptor.abi_version == UINT32_C(1) ? 0 : 1;
}
