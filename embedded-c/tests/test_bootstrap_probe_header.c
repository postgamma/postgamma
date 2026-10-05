#include "postgamma/private/bootstrap_probe.h"

int
main(void)
{
	const pgm_bootstrap_probe_result result = {
		.status = PGM_BOOTSTRAP_PROBE_NOT_RUN,
		.checks = 0,
		.dynamic_load_attempts = 0,
		.init_calls = 0,
		.kernel_symbol_address = 0,
		.detail = "not run"
	};

	return result.status == PGM_BOOTSTRAP_PROBE_NOT_RUN ? 0 : 1;
}
