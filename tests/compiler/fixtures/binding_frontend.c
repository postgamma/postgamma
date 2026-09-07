#include <stdbool.h>


static bool work_mem = false;


int
read_unrelated_frontend_work_mem(void)
{
	return work_mem ? 1 : 0;
}
