#ifndef POSTGAMMA_TEST_BOOTSTRAP_EXTENSION_H
#define POSTGAMMA_TEST_BOOTSTRAP_EXTENSION_H

#include "fmgr.h"

extern const Pg_magic_struct *pgm_bootstrap_extension_magic(void);
extern void pgm_bootstrap_extension_init(void);
extern const Pg_finfo_record *pg_finfo_bootstrap_extension_answer(void);
extern Datum bootstrap_extension_answer(PG_FUNCTION_ARGS);
extern int pgm_bootstrap_extension_init_count(void);
extern int pgm_bootstrap_extension_link_probe(void);

#endif
