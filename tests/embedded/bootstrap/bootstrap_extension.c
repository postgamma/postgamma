#include "postgres.h"

#include "fmgr.h"
#include "utils/palloc.h"

#include "bootstrap_extension.h"

#undef PG_MAGIC_FUNCTION_NAME
#define PG_MAGIC_FUNCTION_NAME pgm_bootstrap_extension_magic

PG_MODULE_MAGIC;
PG_FUNCTION_INFO_V1(bootstrap_extension_answer);

static int bootstrap_init_count;

void *(*const pgm_bootstrap_extension_palloc_link_root)(Size) = palloc;

void
_PG_init(void)
{
	bootstrap_init_count++;
}

Datum
bootstrap_extension_answer(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT32(42);
}

int
pgm_bootstrap_extension_init_count(void)
{
	return bootstrap_init_count;
}

int
pgm_bootstrap_extension_link_probe(void)
{
	return pgm_bootstrap_extension_palloc_link_root != NULL &&
		PG_MAJORVERSION_NUM == 19;
}
