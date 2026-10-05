/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef POSTGAMMA_ARROW_H
#define POSTGAMMA_ARROW_H

/*
 * Optional Arrow C Data adapter for PostGamma embedded.
 *
 * The core postgamma.h header does not depend on Arrow.  A consumer includes
 * the Arrow C Data definitions separately when it needs concrete ArrowSchema
 * and ArrowArray storage.  Export never consumes or mutates the source result.
 */

#include "postgamma/postgamma.h"

#ifdef __cplusplus
extern "C" {
#endif

struct ArrowSchema;
struct ArrowArray;

PGM_API pgm_status pgm_result_export_arrow(
	const pgm_result *result,
	struct ArrowSchema *schema,
	struct ArrowArray *array,
	pgm_error **error);

#ifdef __cplusplus
}
#endif

#endif /* POSTGAMMA_ARROW_H */
