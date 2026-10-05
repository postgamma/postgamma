int work_mem = 4096;

struct ConfigInt
{
	int *variable;
};

struct ConfigInt ConfigureNames[] = {
#include "generated/guc_tables.inc.c"
};


int
read_backend_work_mem(void)
{
	return work_mem;
}
