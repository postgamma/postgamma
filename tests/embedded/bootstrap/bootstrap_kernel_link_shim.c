#include "postgres.h"

#include "postmaster/postmaster.h"

const char *progname = "postgamma-bootstrap-link-probe";

DispatchOption
parse_dispatch_option(const char *name)
{
	(void) name;
	return DISPATCH_POSTMASTER;
}
